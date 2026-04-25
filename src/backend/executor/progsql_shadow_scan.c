/*-------------------------------------------------------------------------
 *
 * progsql_shadow_scan.c
 *	  CustomScan provider that routes point lookups on a partitioned+
 *	  inherited root relation through the ProgreSQL shadow key index.
 *
 * The shadow heap table maps each PK/UNIQUE key value to the OID of the
 * partition (pss_child_relid) that owns the row.  For a WHERE clause that
 * equates a shadow-covered key column to a constant or parameter, this
 * lets the planner short-circuit a full partition sweep with an O(log n)
 * index probe followed by a single-tuple fetch from the owning partition.
 *
 * Integration points:
 *	 - progsql_shadow_scan_init() is called once per backend from
 *	   InitPostgres() to register the CustomScanMethods.
 *	 - progsql_add_shadow_paths() is called twice per plan: once from
 *	   set_rel_pathlist() (for non-partitioned shadow-covered tables), and
 *	   again from apply_scanjoin_target_to_paths() after the planner zaps
 *	   and rebuilds the partitioned rel's pathlist.  Either call is a
 *	   no-op for irrelevant relations.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/progsql_shadow_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_progsql_shadow.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/progsql_shadow_scan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* ------------------------------------------------------------------ */
/* Execution state                                                    */
/* ------------------------------------------------------------------ */

typedef struct ProgsqlShadowScanState
{
	CustomScanState css;

	/* Shadow resources */
	Oid			shadowOid;
	Oid			shadowKeyIdx;
	Relation	shadowRel;
	Relation	shadowIdxRel;
	IndexScanDesc shadowScan;
	TupleTableSlot *shadowSlot;

	/* Shadow-side scan keys, one per equality the planner captured */
	ScanKeyData skeys[INDEX_MAX_KEYS];
	int			nskeys;

	/* Per-key metadata needed to build the partition probe */
	int16		shadowAttnos[INDEX_MAX_KEYS];
	Oid			keyTypes[INDEX_MAX_KEYS];	/* shadow col types */
	Datum		keyValues[INDEX_MAX_KEYS];	/* evaluated RHS values */
	bool		keyNulls[INDEX_MAX_KEYS];

	AttrNumber	partAttnos[INDEX_MAX_KEYS]; /* partition's attnums for keys */

	/* Current partition state */
	Oid			currentPartOid;
	Relation	partRel;
	TableScanDesc partScan;
	TupleTableSlot *partSlot;

	/* Metadata cached from custom_private */
	int			nkeys;			/* logical key columns of the PK */
	Oid			pkIndexOid;		/* original PK index OID -- for attno map */

	bool		done;

	/* EXPLAIN ANALYZE counters */
	int64		probes;			/* total index probes into shadow */
	int64		hits;			/* shadow rows that led to a partition fetch */
} ProgsqlShadowScanState;


/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static Plan *progsql_plan_shadow_scan(PlannerInfo *root, RelOptInfo *rel,
									  struct CustomPath *best_path,
									  List *tlist, List *clauses,
									  List *custom_plans);
static Node *progsql_create_shadow_scan_state(CustomScan *cscan);
static void progsql_shadow_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *progsql_shadow_exec(CustomScanState *node);
static void progsql_shadow_end(CustomScanState *node);
static void progsql_shadow_rescan(CustomScanState *node);
static void progsql_shadow_explain(CustomScanState *node, List *ancestors,
								   ExplainState *es);

static void progsql_close_current_partition(ProgsqlShadowScanState *psss);
static AttrNumber progsql_map_root_attno_to_part(Relation partRel, Relation rootRel,
												 AttrNumber rootAttno);


/* ------------------------------------------------------------------ */
/* Method tables                                                      */
/* ------------------------------------------------------------------ */

static const CustomPathMethods progsql_shadow_path_methods = {
	.CustomName = "ProgsqlShadowScan",
	.PlanCustomPath = progsql_plan_shadow_scan,
};

static const CustomScanMethods progsql_shadow_scan_methods = {
	.CustomName = "ProgsqlShadowScan",
	.CreateCustomScanState = progsql_create_shadow_scan_state,
};

static const CustomExecMethods progsql_shadow_exec_methods = {
	.CustomName = "ProgsqlShadowScan",
	.BeginCustomScan = progsql_shadow_begin,
	.ExecCustomScan = progsql_shadow_exec,
	.EndCustomScan = progsql_shadow_end,
	.ReScanCustomScan = progsql_shadow_rescan,
	.ExplainCustomScan = progsql_shadow_explain,
};


/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

void
progsql_shadow_scan_init(void)
{
	static bool registered = false;

	if (registered)
		return;

	RegisterCustomScanMethods(&progsql_shadow_scan_methods);
	registered = true;
}


/* ------------------------------------------------------------------ */
/* Path generation                                                    */
/* ------------------------------------------------------------------ */

/*
 * Look up the shadow catalog row for the given root relation and return
 * the shadow heap, its key index, and the PK's own index.  Returns true
 * on hit, false otherwise.
 */
static bool
progsql_lookup_shadow_desc(Oid rootrelid, Oid *shadowOid, Oid *keyIdxOid,
						   Oid *pkIdxOid)
{
	Oid			childIdx;

	if (!LookupProgsqlShadowDescForRoot(rootrelid,
										shadowOid, keyIdxOid, &childIdx))
		return false;

	/*
	 * Resolve the PK index OID from pg_progsql_shadow indirectly: the
	 * constraint that owns the shadow maps to the PK index.  We read the
	 * row again via the conid index to retrieve pssconid and then look up
	 * conindid.  For a single-entry scan, open the catalog relation once.
	 */
	{
		Relation	catrel;
		ScanKeyData key[1];
		SysScanDesc scan;
		HeapTuple	tup;
		bool		found = false;

		catrel = table_open(ProgsqlShadowRelationId, AccessShareLock);

		ScanKeyInit(&key[0],
					Anum_pg_progsql_shadow_pssrootrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(rootrelid));

		scan = systable_beginscan(catrel,
								  ProgsqlShadowRootrelidIndexId, true,
								  NULL, 1, key);

		tup = systable_getnext(scan);
		if (HeapTupleIsValid(tup))
		{
			Form_pg_progsql_shadow form = (Form_pg_progsql_shadow) GETSTRUCT(tup);
			HeapTuple	conTup;

			conTup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(form->pssconid));
			if (HeapTupleIsValid(conTup))
			{
				Form_pg_constraint conForm = (Form_pg_constraint) GETSTRUCT(conTup);

				*pkIdxOid = conForm->conindid;
				found = true;
				ReleaseSysCache(conTup);
			}
		}
		systable_endscan(scan);
		table_close(catrel, AccessShareLock);

		if (!found)
			return false;
	}

	return true;
}

/*
 * Check whether the given expression is (or strips down to) a simple
 * non-volatile value that is safe to evaluate once in BeginCustomScan.
 * We accept Const and Param and everything that has already been folded.
 */
static bool
progsql_is_safe_rhs(Node *expr, Index rti)
{
	/* Must not reference this relation */
	if (bms_is_member(rti, pull_varnos(NULL, expr)))
		return false;
	if (contain_volatile_functions(expr))
		return false;
	return true;
}

/*
 * Walk the rel's baserestrictinfo looking for equality clauses whose Var
 * side is a column covered by the shadow's key index.  Produces two
 * parallel lists:
 *	 - *eqExprs     : RHS expressions (go through nestloop param rewrite)
 *	 - *shadowAtnos : IntList of shadow attno for each eqExpr (index in
 *					  the shadow heap, 1-based)
 * Returns the number of matched clauses.
 */
static int
progsql_collect_key_equalities(PlannerInfo *root, RelOptInfo *rel,
							   Index rti, Relation pkIdxRel,
							   List **eqExprs, List **shadowAttnos)
{
	ListCell   *lc;
	int			matched = 0;
	bool	   *covered;
	int			nkeyatts;

	*eqExprs = NIL;
	*shadowAttnos = NIL;

	nkeyatts = pkIdxRel->rd_index->indnkeyatts;
	covered = (bool *) palloc0(sizeof(bool) * nkeyatts);

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *opexpr;
		Node	   *leftop;
		Node	   *rightop;
		Var		   *keyvar = NULL;
		Node	   *rhs = NULL;

		if (!is_opclause(rinfo->clause))
			continue;

		opexpr = (OpExpr *) rinfo->clause;
		if (list_length(opexpr->args) != 2)
			continue;

		leftop = (Node *) linitial(opexpr->args);
		rightop = (Node *) lsecond(opexpr->args);

		/* Strip RelabelType wrappers so IsA(Var) checks succeed */
		if (leftop && IsA(leftop, RelabelType))
			leftop = (Node *) ((RelabelType *) leftop)->arg;
		if (rightop && IsA(rightop, RelabelType))
			rightop = (Node *) ((RelabelType *) rightop)->arg;

		if (IsA(leftop, Var) && ((Var *) leftop)->varno == rti)
		{
			keyvar = (Var *) leftop;
			rhs = rightop;
		}
		else if (IsA(rightop, Var) && ((Var *) rightop)->varno == rti)
		{
			keyvar = (Var *) rightop;
			rhs = leftop;
		}
		else
			continue;

		/* Only equality operators */
		if (!op_mergejoinable(opexpr->opno, exprType((Node *) keyvar)))
			continue;

		/* The RHS must not reference our rel */
		if (!progsql_is_safe_rhs(rhs, rti))
			continue;

		/*
		 * See if keyvar->varattno is one of the index-key columns.  indkey
		 * values are 1-based root table attnos; we want the position (0-based
		 * index column) and therefore shadow attno = position + 1.
		 */
		for (int j = 0; j < nkeyatts; j++)
		{
			if (pkIdxRel->rd_index->indkey.values[j] == keyvar->varattno)
			{
				if (covered[j])
					break;		/* already have this key column */

				covered[j] = true;
				*eqExprs = lappend(*eqExprs, rhs);
				*shadowAttnos = lappend_int(*shadowAttnos, j + 1);
				matched++;
				break;
			}
		}
	}

	pfree(covered);
	return matched;
}

/*
 * progsql_add_shadow_paths
 *	  Consider adding a shadow-backed CustomPath for this relation.
 */
void
progsql_add_shadow_paths(PlannerInfo *root, RelOptInfo *rel, Index rti,
						 RangeTblEntry *rte)
{
	Oid			shadowOid;
	Oid			keyIdxOid;
	Oid			pkIdxOid;
	Relation	pkIdxRel;
	List	   *eqExprs = NIL;
	List	   *shadowAttnos = NIL;
	CustomPath *cpath;
	int			nmatched;
	Cost		startup_cost;
	Cost		total_cost;
	double		rows;

	if (rel->reloptkind != RELOPT_BASEREL)
		return;
	if (rte->rtekind != RTE_RELATION)
		return;
	if (rte->relkind != RELKIND_PARTITIONED_TABLE)
		return;

	/*
	 * Don't intercept UPDATE/DELETE/MERGE target relations.  Those plans
	 * require row-identity columns (varno = ROWID_VAR) in the scan
	 * targetlist, which the shadow path cannot produce -- createplan's
	 * setrefs.c will assert on them.  The planner recognises the target
	 * relation both via parse->resultRelation and root->all_result_relids.
	 */
	if (root->parse->commandType != CMD_SELECT)
	{
		if (root->parse->resultRelation == (int) rti)
			return;
		if (bms_is_member(rti, root->all_result_relids))
			return;
	}

	if (!progsql_lookup_shadow_desc(rte->relid, &shadowOid, &keyIdxOid,
									&pkIdxOid))
		return;

	pkIdxRel = index_open(pkIdxOid, AccessShareLock);

	nmatched = progsql_collect_key_equalities(root, rel, rti, pkIdxRel,
											  &eqExprs, &shadowAttnos);
	if (nmatched == 0)
	{
		index_close(pkIdxRel, AccessShareLock);
		return;
	}

	/*
	 * For correctness the planner's index selectivity won't prefer this
	 * path unless we cost it attractively.  We model the plan as two index
	 * probes (shadow key index + single-tuple lookup on one partition) of
	 * ~log2(N) cost each.  The row estimate is the product of per-key
	 * selectivities, which the planner may already have computed, but for
	 * a unique lookup we can safely cap it at one.
	 */
	/*
	 * Use actual shadow table stats when available.  reltuples == -1 means
	 * no ANALYZE has run yet; fall back to safe constants in that case.
	 */
	{
		double		shadowTuples;
		double		shadowPages;
		double		log2_rows;
		Relation	shadowRelForStats;

		shadowRelForStats = table_open(shadowOid, AccessShareLock);
		shadowTuples = shadowRelForStats->rd_rel->reltuples;
		shadowPages = shadowRelForStats->rd_rel->relpages;
		table_close(shadowRelForStats, AccessShareLock);

		if (shadowTuples < 1)
			shadowTuples = 1000.0;	/* pre-ANALYZE default */
		if (shadowPages < 1)
			shadowPages = 10.0;

		log2_rows = ceil(log(shadowTuples + 1) / log(2.0));
		startup_cost = 0.0;
		total_cost = random_page_cost * log2_rows	/* shadow idx descent */
					 + cpu_index_tuple_cost			/* shadow tuple eval */
					 + random_page_cost				/* partition heap page */
					 + cpu_tuple_cost				/* partition tuple eval */
					 + cpu_operator_cost * 2 * nmatched;	/* key comparisons */
		rows = 1.0;
	}

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rows;
	cpath->path.disabled_nodes = 0;
	cpath->path.startup_cost = startup_cost;
	cpath->path.total_cost = total_cost;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_restrictinfo = NIL;
	/*
	 * custom_private carries planner-time metadata (OIDs, attno map) that
	 * does NOT need nestloop param rewrite.  custom_exprs carries the RHS
	 * expressions, which createplan.c substitutes for us.
	 */
	cpath->custom_private = list_make3(list_make3_oid(shadowOid,
													  keyIdxOid,
													  pkIdxOid),
									   shadowAttnos,
									   NIL /* reserved */);
	/* custom_exprs is set on the Plan, not the Path.  Stash here for now. */
	cpath->custom_private = lappend(cpath->custom_private, eqExprs);
	cpath->methods = &progsql_shadow_path_methods;

	index_close(pkIdxRel, AccessShareLock);

	add_path(rel, (Path *) cpath);
}


/* ------------------------------------------------------------------ */
/* Plan generation                                                    */
/* ------------------------------------------------------------------ */

static Plan *
progsql_plan_shadow_scan(PlannerInfo *root, RelOptInfo *rel,
						 struct CustomPath *best_path,
						 List *tlist, List *clauses,
						 List *custom_plans)
{
	CustomScan *cscan;
	List	   *private_ = best_path->custom_private;
	List	   *eqExprs;
	List	   *metaHead;
	List	   *shadowAttnos;

	Assert(list_length(private_) == 4);

	metaHead = (List *) linitial(private_);
	shadowAttnos = (List *) lsecond(private_);
	eqExprs = (List *) lfourth(private_);

	cscan = makeNode(CustomScan);
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.plan.lefttree = NULL;
	cscan->scan.plan.righttree = NULL;
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	/*
	 * Repackage private metadata.  Private carries OIDs and attnos; exprs
	 * (the RHS values) live in custom_exprs so they get nestloop-param
	 * substitution in createplan.c.
	 */
	cscan->custom_private = list_make2(metaHead, shadowAttnos);
	cscan->custom_exprs = eqExprs;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_relids = NULL;
	cscan->methods = &progsql_shadow_scan_methods;

	return &cscan->scan.plan;
}


/* ------------------------------------------------------------------ */
/* Executor: state creation                                           */
/* ------------------------------------------------------------------ */

static Node *
progsql_create_shadow_scan_state(CustomScan *cscan)
{
	ProgsqlShadowScanState *psss = palloc0_object(ProgsqlShadowScanState);

	NodeSetTag(psss, T_CustomScanState);
	psss->css.flags = cscan->flags;
	psss->css.methods = &progsql_shadow_exec_methods;
	psss->done = false;
	psss->currentPartOid = InvalidOid;
	psss->partRel = NULL;
	psss->partScan = NULL;
	psss->partSlot = NULL;
	psss->shadowRel = NULL;
	psss->shadowIdxRel = NULL;
	psss->shadowScan = NULL;
	psss->shadowSlot = NULL;

	return (Node *) psss;
}


/* ------------------------------------------------------------------ */
/* Executor: BeginCustomScan                                          */
/* ------------------------------------------------------------------ */

static void
progsql_shadow_begin(CustomScanState *node, EState *estate, int eflags)
{
	ProgsqlShadowScanState *psss = (ProgsqlShadowScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *private_ = cscan->custom_private;
	List	   *metaHead;
	List	   *shadowAttnos;
	ListCell   *lc1,
			   *lc2;
	int			i;
	ExprContext *econtext;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	Assert(list_length(private_) == 2);

	metaHead = (List *) linitial(private_);
	shadowAttnos = (List *) lsecond(private_);

	psss->shadowOid = list_nth_oid(metaHead, 0);
	psss->shadowKeyIdx = list_nth_oid(metaHead, 1);
	psss->pkIndexOid = list_nth_oid(metaHead, 2);
	psss->nkeys = list_length(shadowAttnos);

	/* Evaluate each RHS expression once and cache it */
	econtext = node->ss.ps.ps_ExprContext;
	if (econtext == NULL)
	{
		ExecAssignExprContext(estate, &node->ss.ps);
		econtext = node->ss.ps.ps_ExprContext;
	}

	psss->shadowRel = table_open(psss->shadowOid, AccessShareLock);
	psss->shadowIdxRel = index_open(psss->shadowKeyIdx, AccessShareLock);
	psss->shadowSlot = table_slot_create(psss->shadowRel, NULL);

	/* Build a slot for the chosen partition; size determined later */
	psss->partSlot = NULL;

	i = 0;
	forboth(lc1, cscan->custom_exprs, lc2, shadowAttnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		int			attno = lfirst_int(lc2);
		ExprState  *state;
		Datum		value;
		bool		isnull;
		AttrNumber	sattr;
		Oid			typid;
		Oid			opfamily;
		Oid			opcintype;
		StrategyNumber strategy;
		Oid			strategy_op;
		Oid			strategy_proc;
		RegProcedure strategy_regproc;

		if (i >= INDEX_MAX_KEYS)
			elog(ERROR, "progsql_shadow_scan: too many key columns");

		state = ExecInitExpr(expr, &node->ss.ps);
		value = ExecEvalExprSwitchContext(state, econtext, &isnull);

		sattr = (AttrNumber) attno;
		typid = TupleDescAttr(RelationGetDescr(psss->shadowRel),
							  sattr - 1)->atttypid;

		/*
		 * Look up the default btree equality op info for the key column
		 * type (the shadow uses the default btree opclass for each key
		 * column, so this round-trips exactly).
		 */
		opfamily = psss->shadowIdxRel->rd_opfamily[sattr - 1];
		opcintype = psss->shadowIdxRel->rd_opcintype[sattr - 1];
		strategy = BTEqualStrategyNumber;

		strategy_op = get_opfamily_member(opfamily, opcintype, opcintype,
										  strategy);
		if (!OidIsValid(strategy_op))
			elog(ERROR, "missing opfamily entry for shadow key column");
		strategy_proc = get_opcode(strategy_op);
		strategy_regproc = (RegProcedure) strategy_proc;

		ScanKeyEntryInitialize(&psss->skeys[i],
							   isnull ? SK_ISNULL : 0,
							   sattr,
							   strategy,
							   opcintype,
							   psss->shadowIdxRel->rd_indcollation[sattr - 1],
							   strategy_regproc,
							   value);

		psss->shadowAttnos[i] = sattr;
		psss->keyTypes[i] = typid;
		psss->keyValues[i] = value;
		psss->keyNulls[i] = isnull;
		i++;
	}
	psss->nskeys = i;

	/* If any key is NULL, nothing can match; mark done. */
	for (int k = 0; k < psss->nskeys; k++)
	{
		if (psss->keyNulls[k])
		{
			psss->done = true;
			break;
		}
	}

	if (!psss->done)
	{
		psss->shadowScan = index_beginscan(psss->shadowRel,
										   psss->shadowIdxRel,
										   estate->es_snapshot,
										   NULL,
										   psss->nskeys, 0,
										   SO_NONE);
		index_rescan(psss->shadowScan, psss->skeys, psss->nskeys, NULL, 0);
	}
}


/* ------------------------------------------------------------------ */
/* Executor: helpers                                                  */
/* ------------------------------------------------------------------ */

static void
progsql_close_current_partition(ProgsqlShadowScanState *psss)
{
	if (psss->partScan)
	{
		table_endscan(psss->partScan);
		psss->partScan = NULL;
	}
	if (psss->partSlot)
	{
		ExecDropSingleTupleTableSlot(psss->partSlot);
		psss->partSlot = NULL;
	}
	if (psss->partRel)
	{
		table_close(psss->partRel, AccessShareLock);
		psss->partRel = NULL;
	}
	psss->currentPartOid = InvalidOid;
}

/*
 * The shadow stores key columns at shadow attnums 1..n in PK-index order.
 * Partitions inherit the root table's schema, so the root attnum (from
 * the PK index) is also the partition attnum for that column.
 */
static AttrNumber
progsql_map_root_attno_to_part(Relation partRel, Relation rootRel,
							   AttrNumber rootAttno)
{
	(void) partRel;
	(void) rootRel;
	return rootAttno;
}



/* ------------------------------------------------------------------ */
/* Executor: ExecCustomScan                                           */
/* ------------------------------------------------------------------ */

static TupleTableSlot *
progsql_shadow_exec(CustomScanState *node)
{
	ProgsqlShadowScanState *psss = (ProgsqlShadowScanState *) node;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *resultSlot = node->ss.ss_ScanTupleSlot;
	Relation	rootRel = node->ss.ss_currentRelation;
	EState	   *estate = node->ss.ps.state;
	Relation	pkIdxRel;

	if (psss->done)
		return NULL;

	pkIdxRel = index_open(psss->pkIndexOid, AccessShareLock);

	for (;;)
	{
		Oid			childOid;
		Datum		childDatum;
		bool		childNull;
		AttrNumber	childAttno;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Get the next shadow tuple that matches.
		 */
		psss->probes++;
		if (!index_getnext_slot(psss->shadowScan, ForwardScanDirection,
								psss->shadowSlot))
		{
			psss->done = true;
			index_close(pkIdxRel, AccessShareLock);
			return NULL;
		}

		/* Extract pss_child_relid -- always the last column */
		childAttno = (AttrNumber) RelationGetDescr(psss->shadowRel)->natts;
		childDatum = slot_getattr(psss->shadowSlot, childAttno, &childNull);
		if (childNull)
			continue;			/* shouldn't happen; defensively skip */
		childOid = DatumGetObjectId(childDatum);

		/*
		 * Switch partition if needed.
		 */
		if (childOid != psss->currentPartOid)
		{
			progsql_close_current_partition(psss);

			psss->partRel = table_open(childOid, AccessShareLock);
			psss->currentPartOid = childOid;
			psss->partSlot = table_slot_create(psss->partRel, NULL);
		}

		/*
		 * Build a partition-side scan: equality on every key column.  We
		 * use a heap scan with quals rather than index probing because the
		 * shadow already narrowed us to a single (expected) matching row.
		 */
		{
			ScanKeyData pskeys[INDEX_MAX_KEYS];
			int			nps;
			bool		abort_scan = false;

			nps = 0;
			for (int k = 0; k < psss->nskeys; k++)
			{
				AttrNumber	rootAttno;
				Oid			typid;
				Oid			eq_opr;
				RegProcedure eq_proc;

				rootAttno = pkIdxRel->rd_index->indkey.values[psss->shadowAttnos[k] - 1];
				typid = psss->keyTypes[k];

				eq_opr = get_opfamily_member(psss->shadowIdxRel->rd_opfamily[psss->shadowAttnos[k] - 1],
											 typid, typid,
											 BTEqualStrategyNumber);
				if (!OidIsValid(eq_opr))
				{
					abort_scan = true;
					break;
				}
				eq_proc = (RegProcedure) get_opcode(eq_opr);

				ScanKeyInit(&pskeys[nps],
							progsql_map_root_attno_to_part(psss->partRel,
														   rootRel,
														   rootAttno),
							BTEqualStrategyNumber,
							eq_proc,
							psss->keyValues[k]);
				nps++;
			}
			if (abort_scan)
				continue;

			if (psss->partScan)
			{
				table_endscan(psss->partScan);
				psss->partScan = NULL;
			}
			psss->partScan = table_beginscan(psss->partRel,
											 estate->es_snapshot,
											 nps, pskeys, 0);

			if (table_scan_getnextslot(psss->partScan,
									   ForwardScanDirection,
									   psss->partSlot))
			{
				psss->hits++;

				/*
				 * Project the partition tuple through the scan target.
				 */
				ExecClearTuple(resultSlot);
				{
					TupleDesc	td = RelationGetDescr(psss->partRel);
					TupleDesc	resTd = resultSlot->tts_tupleDescriptor;
					bool		use_transfer = (td->natts == resTd->natts);

					if (use_transfer)
					{
						slot_getallattrs(psss->partSlot);
						for (int a = 0; a < resTd->natts; a++)
						{
							resultSlot->tts_values[a] =
								psss->partSlot->tts_values[a];
							resultSlot->tts_isnull[a] =
								psss->partSlot->tts_isnull[a];
						}
						ExecStoreVirtualTuple(resultSlot);
					}
					else
					{
						/* Fallback: copy then store */
						HeapTuple	t = ExecCopySlotHeapTuple(psss->partSlot);

						ExecStoreHeapTuple(t, resultSlot, true);
					}
				}

				/*
				 * Apply planner qual if one is present (extras beyond our
				 * captured key equalities).
				 */
				if (node->ss.ps.qual)
				{
					econtext->ecxt_scantuple = resultSlot;
					if (!ExecQual(node->ss.ps.qual, econtext))
						continue;
				}

				if (node->ss.ps.ps_ProjInfo)
				{
					econtext->ecxt_scantuple = resultSlot;
					index_close(pkIdxRel, AccessShareLock);
					return ExecProject(node->ss.ps.ps_ProjInfo);
				}

				index_close(pkIdxRel, AccessShareLock);
				return resultSlot;
			}
			/* else: partition didn't actually contain the row; loop back. */
		}
	}
}


/* ------------------------------------------------------------------ */
/* Executor: EndCustomScan / ReScan                                   */
/* ------------------------------------------------------------------ */

static void
progsql_shadow_end(CustomScanState *node)
{
	ProgsqlShadowScanState *psss = (ProgsqlShadowScanState *) node;

	progsql_close_current_partition(psss);

	if (psss->shadowScan)
	{
		index_endscan(psss->shadowScan);
		psss->shadowScan = NULL;
	}
	if (psss->shadowSlot)
	{
		ExecDropSingleTupleTableSlot(psss->shadowSlot);
		psss->shadowSlot = NULL;
	}
	if (psss->shadowIdxRel)
	{
		index_close(psss->shadowIdxRel, AccessShareLock);
		psss->shadowIdxRel = NULL;
	}
	if (psss->shadowRel)
	{
		table_close(psss->shadowRel, AccessShareLock);
		psss->shadowRel = NULL;
	}
}

static void
progsql_shadow_rescan(CustomScanState *node)
{
	ProgsqlShadowScanState *psss = (ProgsqlShadowScanState *) node;

	progsql_close_current_partition(psss);

	if (psss->shadowScan)
	{
		if (!psss->done)
			index_rescan(psss->shadowScan, psss->skeys, psss->nskeys,
						 NULL, 0);
	}
	psss->done = false;
	for (int k = 0; k < psss->nskeys; k++)
		if (psss->keyNulls[k])
		{
			psss->done = true;
			break;
		}
}


/* ------------------------------------------------------------------ */
/* Executor: ExplainCustomScan                                        */
/* ------------------------------------------------------------------ */

static void
progsql_shadow_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ProgsqlShadowScanState *psss = (ProgsqlShadowScanState *) node;
	char		buf[NAMEDATALEN * 2 + 32];

	if (psss->shadowRel != NULL)
	{
		const char *shadowName = RelationGetRelationName(psss->shadowRel);
		const char *keyIdxName = RelationGetRelationName(psss->shadowIdxRel);

		snprintf(buf, sizeof(buf), "%s via %s", shadowName, keyIdxName);
		ExplainPropertyText("Shadow Index", buf, es);
	}
	if (es->analyze)
	{
		ExplainPropertyInteger("Shadow Probes", NULL, psss->probes, es);
		ExplainPropertyInteger("Shadow Hits", NULL, psss->hits, es);
	}
}
