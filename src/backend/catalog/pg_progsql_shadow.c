/*-------------------------------------------------------------------------
 *
 * pg_progsql_shadow.c
 *	  routines to support manipulation of the pg_progsql_shadow catalog
 *
 * This catalog records the association between a unique/primary-key
 * constraint on a partitioned+inherited table ("root") and a shadow heap
 * table that enforces cross-partition uniqueness.  See
 * src/include/catalog/pg_progsql_shadow.h for the row format.
 *
 * Shadow tables are created via direct heap_create_with_catalog +
 * index_create calls (mirroring the pattern used for TOAST tables in
 * src/backend/catalog/toasting.c) and live in the same namespace as the
 * root relation.  No SPI is involved, and no allowSystemTableMods escape
 * hatch is required.
 *
 * pg_upgrade compatibility:
 *   - Shadow heap tables (_pss_<indexOid>) are regular heap relations in user
 *     namespace with OIDs >= FirstNormalObjectId.  pg_upgrade's info.c query
 *     includes all such relations unconditionally; they are migrated exactly
 *     like any other user table — relfilenode copied, indexes rebuilt.
 *   - pg_progsql_shadow itself is a BKI-bootstrapped system catalog in
 *     pg_catalog.  pg_upgrade treats it as a catalog relation: the new cluster
 *     is initialized with an empty copy via initdb, then pg_upgrade's catalog
 *     copy pass (relfilenumber.c swap_catalog_files) replaces the new-cluster
 *     file with a pg_restore-generated file containing the old-cluster rows.
 *     No special-casing in pg_upgrade source is required.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_progsql_shadow.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_progsql_shadow.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CreateProgsqlShadowEntry
 *		Insert a row into pg_progsql_shadow.
 *
 * rootrelid is the partitioned+inherited table whose constraint this entry
 * describes; conid is the pg_constraint OID for the PK/UNIQUE constraint;
 * shadowid is the OID of the shadow heap table; keyidxid is the OID of the
 * shadow's UNIQUE index on the constraint key columns; childidxid is the
 * OID of the shadow's index on pss_child_relid.
 *
 * Returns the OID of the new pg_progsql_shadow row.
 */
Oid
CreateProgsqlShadowEntry(Oid rootrelid, Oid conid, Oid shadowid,
						 Oid keyidxid, Oid childidxid)
{
	Relation	pg_progsql_shadow;
	Datum		values[Natts_pg_progsql_shadow];
	bool		nulls[Natts_pg_progsql_shadow];
	HeapTuple	tup;
	Oid			newoid;
	ObjectAddress myself;
	ObjectAddress referenced;

	pg_progsql_shadow = table_open(ProgsqlShadowRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	newoid = GetNewOidWithIndex(pg_progsql_shadow,
								ProgsqlShadowOidIndexId,
								Anum_pg_progsql_shadow_oid);
	values[Anum_pg_progsql_shadow_oid - 1] = ObjectIdGetDatum(newoid);
	values[Anum_pg_progsql_shadow_pssrootrelid - 1] = ObjectIdGetDatum(rootrelid);
	values[Anum_pg_progsql_shadow_pssconid - 1] = ObjectIdGetDatum(conid);
	values[Anum_pg_progsql_shadow_pssshadowid - 1] = ObjectIdGetDatum(shadowid);
	values[Anum_pg_progsql_shadow_psskeyidxid - 1] = ObjectIdGetDatum(keyidxid);
	values[Anum_pg_progsql_shadow_psschildidxid - 1] = ObjectIdGetDatum(childidxid);

	tup = heap_form_tuple(RelationGetDescr(pg_progsql_shadow), values, nulls);

	CatalogTupleInsert(pg_progsql_shadow, tup);
	heap_freetuple(tup);

	/*
	 * Record dependencies: the shadow entry depends on both the constraint
	 * and the shadow relation (INTERNAL so that dropping either cascades
	 * cleanly), and on the root relation (AUTO).
	 */
	ObjectAddressSet(myself, ProgsqlShadowRelationId, newoid);

	ObjectAddressSet(referenced, ConstraintRelationId, conid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);

	ObjectAddressSet(referenced, RelationRelationId, shadowid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);

	ObjectAddressSet(referenced, RelationRelationId, rootrelid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	table_close(pg_progsql_shadow, RowExclusiveLock);

	return newoid;
}


/*
 * LookupProgsqlShadow
 *		Return the shadow-table OID for a given constraint, or InvalidOid
 *		if none exists.
 *
 * If missing_ok is false, an error is raised when no row is found.
 */
Oid
LookupProgsqlShadow(Oid conid, bool missing_ok)
{
	HeapTuple	tup;
	Oid			shadowid;

	tup = SearchSysCache1(PROGSQLSHADOWCONID, ObjectIdGetDatum(conid));
	if (!HeapTupleIsValid(tup))
	{
		if (missing_ok)
			return InvalidOid;
		elog(ERROR, "no pg_progsql_shadow entry for constraint %u", conid);
	}

	shadowid = ((Form_pg_progsql_shadow) GETSTRUCT(tup))->pssshadowid;
	ReleaseSysCache(tup);

	return shadowid;
}


/*
 * GetProgsqlShadowsForRoot
 *		Return a list of shadow-table OIDs for all shadow entries whose
 *		root relation is rootrelid.  Returns NIL if none.
 *
 * The returned list is palloc'd in the current memory context.
 */
List *
GetProgsqlShadowsForRoot(Oid rootrelid)
{
	Relation	pg_progsql_shadow;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *result = NIL;

	pg_progsql_shadow = table_open(ProgsqlShadowRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_progsql_shadow_pssrootrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rootrelid));

	scan = systable_beginscan(pg_progsql_shadow,
							  ProgsqlShadowRootrelidIndexId, true,
							  NULL, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_progsql_shadow form = (Form_pg_progsql_shadow) GETSTRUCT(tup);

		result = lappend_oid(result, form->pssshadowid);
	}

	systable_endscan(scan);
	table_close(pg_progsql_shadow, AccessShareLock);

	return result;
}


/*
 * LookupProgsqlShadowForRoot
 *		Return the (first) shadow-table OID registered for the given root
 *		relation, or InvalidOid if none exists.
 *
 * Multi-constraint shadow support is a future enhancement; for now we
 * return the first entry in the rootrelid index.
 */
Oid
LookupProgsqlShadowForRoot(Oid rootrelid)
{
	Oid			shadowOid = InvalidOid;
	Oid			keyIdx;
	Oid			childIdx;

	(void) LookupProgsqlShadowDescForRoot(rootrelid,
										  &shadowOid, &keyIdx, &childIdx);
	return shadowOid;
}


/*
 * LookupProgsqlShadowDescForRoot
 *		Look up the shadow-table OID and the OIDs of its key/child indexes
 *		for the given root relation.  Returns true if a row was found and
 *		fills in *shadowOid, *keyIndexOid, *childIndexOid; returns false
 *		otherwise (output pointers are set to InvalidOid).
 */
bool
LookupProgsqlShadowDescForRoot(Oid rootrelid,
							   Oid *shadowOid,
							   Oid *keyIndexOid,
							   Oid *childIndexOid)
{
	Relation	pg_progsql_shadow;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	if (shadowOid)
		*shadowOid = InvalidOid;
	if (keyIndexOid)
		*keyIndexOid = InvalidOid;
	if (childIndexOid)
		*childIndexOid = InvalidOid;

	pg_progsql_shadow = table_open(ProgsqlShadowRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_progsql_shadow_pssrootrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rootrelid));

	scan = systable_beginscan(pg_progsql_shadow,
							  ProgsqlShadowRootrelidIndexId, true,
							  NULL, 1, key);

	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		Form_pg_progsql_shadow form = (Form_pg_progsql_shadow) GETSTRUCT(tup);

		if (shadowOid)
			*shadowOid = form->pssshadowid;
		if (keyIndexOid)
			*keyIndexOid = form->psskeyidxid;
		if (childIndexOid)
			*childIndexOid = form->psschildidxid;
		found = true;
	}

	systable_endscan(scan);
	table_close(pg_progsql_shadow, AccessShareLock);

	return found;
}


/*
 * CreateProgsqlShadowTable
 *		Create the shadow heap table that enforces cross-partition uniqueness
 *		for a PK/UNIQUE constraint on a partitioned+inherited root relation,
 *		and register the mapping in pg_progsql_shadow.
 *
 * rootrelid is the root (partitioned+inherited) relation.
 * indexOid is the OID of the just-created unique/PK index on the root.
 * constraintOid is the pg_constraint OID for that same constraint.
 *
 * The shadow table is named "_pss_<indexOid>" and lives in the same
 * namespace as the root relation.  Its columns are the key columns of the
 * index (same types) plus a pss_child_relid OID column.  We create two
 * indexes: a UNIQUE btree on the key columns ("_pss_<indexOid>_key"), which
 * enforces the cross-partition uniqueness; and a non-unique btree on
 * pss_child_relid ("_pss_<indexOid>_child") used for partition-scoped
 * deletes during TRUNCATE of a single partition.
 *
 * Returns the pg_class OID of the shadow table.
 */
Oid
CreateProgsqlShadowTable(Oid rootrelid, Oid indexOid, Oid constraintOid)
{
	Relation	rootRel;
	Relation	indexRel;
	Relation	shadowRel;
	Form_pg_index indexForm;
	TupleDesc	indexTupdesc;
	TupleDesc	tupdesc;
	Oid			nspOid;
	Oid			ownerid;
	Oid			tablespace;
	Oid			shadowOid;
	char		shadowName[NAMEDATALEN];
	char		keyIdxName[NAMEDATALEN];
	char		childIdxName[NAMEDATALEN];
	int			nkeys;
	int			i;
	IndexInfo  *keyIndexInfo;
	IndexInfo  *childIndexInfo;
	Oid		   *keyCollations;
	Oid		   *keyOpclasses;
	int16	   *keyColoptions;
	List	   *keyColNames = NIL;
	uint16		keyConstrFlags = 0;
	Oid			keyIndexOid;
	Oid			childIndexOid;
	bool		condeferrable = false;
	bool		condeferred = false;

	/* Open the root relation to obtain namespace/owner/tablespace. */
	rootRel = table_open(rootrelid, AccessShareLock);
	nspOid = RelationGetNamespace(rootRel);
	ownerid = rootRel->rd_rel->relowner;
	tablespace = rootRel->rd_rel->reltablespace;
	table_close(rootRel, AccessShareLock);

	/* Open the index to enumerate its key columns. */
	indexRel = index_open(indexOid, AccessShareLock);
	indexForm = indexRel->rd_index;
	indexTupdesc = RelationGetDescr(indexRel);
	nkeys = indexForm->indnkeyatts;

	/*
	 * Build the shadow table's TupleDesc: nkeys key columns followed by
	 * pss_child_relid (OID).
	 */
	tupdesc = CreateTemplateTupleDesc(nkeys + 1);

	keyCollations = (Oid *) palloc0(nkeys * sizeof(Oid));
	keyOpclasses = (Oid *) palloc0(nkeys * sizeof(Oid));
	keyColoptions = (int16 *) palloc0(nkeys * sizeof(int16));

	for (i = 0; i < nkeys; i++)
	{
		Form_pg_attribute iatt = TupleDescAttr(indexTupdesc, i);
		AttrNumber	rootAttno = indexForm->indkey.values[i];
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;

		if (rootAttno <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("progsql shadow does not support expression index columns")));

		/*
		 * The index relation's tuple descriptor is the authoritative source
		 * for the key column's type, typmod, and collation.
		 */
		atttypid = iatt->atttypid;
		atttypmod = iatt->atttypmod;
		attcollation = iatt->attcollation;

		TupleDescInitEntry(tupdesc, (AttrNumber) (i + 1),
						   NameStr(iatt->attname),
						   atttypid, atttypmod, 0);
		TupleDescAttr(tupdesc, i)->attnotnull = true;
		TupleDescInitEntryCollation(tupdesc, (AttrNumber) (i + 1),
									attcollation);

		/*
		 * Use the type's default btree opclass for the shadow's UNIQUE index.
		 * The shadow only has to enforce equality semantics for the key, and
		 * the default btree opclass does that for any indexable type.
		 */
		keyCollations[i] = attcollation;
		keyOpclasses[i] = GetDefaultOpClass(atttypid, BTREE_AM_OID);
		if (!OidIsValid(keyOpclasses[i]))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"btree\"",
							format_type_be(atttypid))));

		keyColoptions[i] = 0;

		keyColNames = lappend(keyColNames, pstrdup(NameStr(iatt->attname)));
	}

	TupleDescInitEntry(tupdesc, (AttrNumber) (nkeys + 1),
					   "pss_child_relid", OIDOID, -1, 0);
	TupleDescAttr(tupdesc, nkeys)->attnotnull = true;

	/*
	 * Mirror the original constraint's deferrability.  If the constraint
	 * was defined as DEFERRABLE [INITIALLY DEFERRED], the shadow's UNIQUE
	 * constraint inherits the same flags so the violation timing matches.
	 */
	{
		HeapTuple	conTup;

		conTup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
		if (HeapTupleIsValid(conTup))
		{
			Form_pg_constraint conForm = (Form_pg_constraint) GETSTRUCT(conTup);

			condeferrable = conForm->condeferrable;
			condeferred = conForm->condeferred;
			ReleaseSysCache(conTup);
		}
	}
	if (condeferrable)
		keyConstrFlags |= INDEX_CONSTR_CREATE_DEFERRABLE;
	if (condeferred)
		keyConstrFlags |= INDEX_CONSTR_CREATE_INIT_DEFERRED;

	index_close(indexRel, AccessShareLock);

	/* Construct names. */
	snprintf(shadowName, sizeof(shadowName), "_pss_%u", indexOid);
	snprintf(keyIdxName, sizeof(keyIdxName), "_pss_%u_key", indexOid);
	snprintf(childIdxName, sizeof(childIdxName), "_pss_%u_child", indexOid);

	/*
	 * Create the shadow heap.  Owner/namespace/tablespace come from the root
	 * relation.  is_internal=true so that drops cascade cleanly via the
	 * dependencies recorded by CreateProgsqlShadowEntry.
	 */
	shadowOid = heap_create_with_catalog(shadowName,
										 nspOid,
										 tablespace,
										 InvalidOid,	/* relid */
										 InvalidOid,	/* reltypeid */
										 InvalidOid,	/* reloftypeid */
										 ownerid,
										 HEAP_TABLE_AM_OID,
										 tupdesc,
										 NIL,	/* cooked_constraints */
										 RELKIND_RELATION,
										 RELPERSISTENCE_PERMANENT,
										 false, /* shared_relation */
										 false, /* mapped_relation */
										 ONCOMMIT_NOOP,
										 (Datum) 0, /* reloptions */
										 false, /* use_user_acl */
										 false, /* allow_system_table_mods */
										 true,	/* is_internal */
										 InvalidOid,	/* relrewrite */
										 NULL); /* typaddress */
	Assert(OidIsValid(shadowOid));

	/* Make the new heap visible. */
	CommandCounterIncrement();

	/* Shadow tables must not be independently replicated. */
	{
		Relation	pg_class_rel;
		HeapTuple	classTup;
		Form_pg_class classForm;

		pg_class_rel = table_open(RelationRelationId, RowExclusiveLock);
		classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(shadowOid));
		if (!HeapTupleIsValid(classTup))
			elog(ERROR, "cache lookup failed for relation %u", shadowOid);
		classForm = (Form_pg_class) GETSTRUCT(classTup);
		classForm->relreplident = REPLICA_IDENTITY_NOTHING;
		CatalogTupleUpdate(pg_class_rel, &classTup->t_self, classTup);
		heap_freetuple(classTup);
		table_close(pg_class_rel, RowExclusiveLock);
	}

	/* Open the new heap to feed it to index_create. */
	shadowRel = table_open(shadowOid, ShareLock);

	/* Construct IndexInfo for the UNIQUE key index. */
	keyIndexInfo = makeNode(IndexInfo);
	keyIndexInfo->ii_NumIndexAttrs = nkeys;
	keyIndexInfo->ii_NumIndexKeyAttrs = nkeys;
	for (i = 0; i < nkeys; i++)
		keyIndexInfo->ii_IndexAttrNumbers[i] = (AttrNumber) (i + 1);
	keyIndexInfo->ii_Expressions = NIL;
	keyIndexInfo->ii_ExpressionsState = NIL;
	keyIndexInfo->ii_Predicate = NIL;
	keyIndexInfo->ii_PredicateState = NULL;
	keyIndexInfo->ii_ExclusionOps = NULL;
	keyIndexInfo->ii_ExclusionProcs = NULL;
	keyIndexInfo->ii_ExclusionStrats = NULL;
	keyIndexInfo->ii_Unique = true;
	keyIndexInfo->ii_NullsNotDistinct = false;
	keyIndexInfo->ii_ReadyForInserts = true;
	keyIndexInfo->ii_CheckedUnchanged = false;
	keyIndexInfo->ii_IndexUnchanged = false;
	keyIndexInfo->ii_Concurrent = false;
	keyIndexInfo->ii_BrokenHotChain = false;
	keyIndexInfo->ii_ParallelWorkers = max_parallel_maintenance_workers;
	keyIndexInfo->ii_Am = BTREE_AM_OID;
	keyIndexInfo->ii_AmCache = NULL;
	keyIndexInfo->ii_Context = CurrentMemoryContext;

	keyIndexOid = index_create(shadowRel, keyIdxName,
							   InvalidOid, /* indexRelationId */
							   InvalidOid, /* parentIndexRelid */
							   InvalidOid, /* parentConstraintId */
							   InvalidOid, /* relFileNumber */
							   keyIndexInfo,
							   keyColNames,
							   BTREE_AM_OID,
							   tablespace,
							   keyCollations,
							   keyOpclasses,
							   NULL, /* opclassOptions */
							   keyColoptions,
							   NULL, /* stattargets */
							   (Datum) 0,
							   INDEX_CREATE_ADD_CONSTRAINT,
							   keyConstrFlags,
							   false, /* allow_system_table_mods */
							   true,  /* is_internal */
							   NULL); /* constraintId */

	/* Construct IndexInfo for the non-unique pss_child_relid index. */
	childIndexInfo = makeNode(IndexInfo);
	childIndexInfo->ii_NumIndexAttrs = 1;
	childIndexInfo->ii_NumIndexKeyAttrs = 1;
	childIndexInfo->ii_IndexAttrNumbers[0] = (AttrNumber) (nkeys + 1);
	childIndexInfo->ii_Expressions = NIL;
	childIndexInfo->ii_ExpressionsState = NIL;
	childIndexInfo->ii_Predicate = NIL;
	childIndexInfo->ii_PredicateState = NULL;
	childIndexInfo->ii_ExclusionOps = NULL;
	childIndexInfo->ii_ExclusionProcs = NULL;
	childIndexInfo->ii_ExclusionStrats = NULL;
	childIndexInfo->ii_Unique = false;
	childIndexInfo->ii_NullsNotDistinct = false;
	childIndexInfo->ii_ReadyForInserts = true;
	childIndexInfo->ii_CheckedUnchanged = false;
	childIndexInfo->ii_IndexUnchanged = false;
	childIndexInfo->ii_Concurrent = false;
	childIndexInfo->ii_BrokenHotChain = false;
	childIndexInfo->ii_ParallelWorkers = max_parallel_maintenance_workers;
	childIndexInfo->ii_Am = BTREE_AM_OID;
	childIndexInfo->ii_AmCache = NULL;
	childIndexInfo->ii_Context = CurrentMemoryContext;

	{
		Oid			childCollations[1];
		Oid			childOpclasses[1];
		int16		childColoptions[1];

		childCollations[0] = InvalidOid;
		childOpclasses[0] = OID_BTREE_OPS_OID;
		childColoptions[0] = 0;

		childIndexOid = index_create(shadowRel, childIdxName,
									 InvalidOid,
									 InvalidOid,
									 InvalidOid,
									 InvalidOid,
									 childIndexInfo,
									 list_make1("pss_child_relid"),
									 BTREE_AM_OID,
									 tablespace,
									 childCollations,
									 childOpclasses,
									 NULL,
									 childColoptions,
									 NULL,
									 (Datum) 0,
									 0, /* flags: no constraint */
									 0, /* constr_flags */
									 false,
									 true,
									 NULL);
	}

	table_close(shadowRel, NoLock);

	/*
	 * Make all of the new objects visible before we record the catalog
	 * mapping.  Subsequent code (e.g. INSERT into the shadow table) needs to
	 * see the new heap and indexes via the relcache.
	 */
	CommandCounterIncrement();

	/* Register the mapping in pg_progsql_shadow. */
	CreateProgsqlShadowEntry(rootrelid, constraintOid, shadowOid,
							 keyIndexOid, childIndexOid);

	pfree(keyCollations);
	pfree(keyOpclasses);
	pfree(keyColoptions);

	return shadowOid;
}
