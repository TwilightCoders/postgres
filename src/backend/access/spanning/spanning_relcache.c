/*-------------------------------------------------------------------------
 *
 * spanning_relcache.c
 *	  ProgreSQL: relcache-side HOT-blocking support for cross-partition
 *	  ("spanning") unique indexes (the E7 correctness fix).
 *
 * A spanning index lives on the partitioned root, so a leaf has no local index
 * on the spanning key and an UPDATE changing that key would wrongly be treated
 * as HOT.  These helpers, called from RelationGetIndexAttrBitmap, add the
 * spanning key columns to the leaf's hot-blocking attribute set.  Catalog scans
 * only, so they are safe on the cached relcache path.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/spanning/spanning_relcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_index.h"
#include "nodes/bitmapset.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "access/spanning.h"

/*
 * progresql_add_spanning_hotblocking_attrs
 *
 * ProgreSQL: a spanning (GLOBAL) index lives on a partitioned ROOT, not on the
 * leaf partitions that hold the rows.  A leaf therefore has no *local* index on
 * the spanning key columns, so without this an UPDATE that changes a spanning
 * key on a leaf would be considered HOT-safe (the leaf's own index set does not
 * mention those columns).  A HOT update does not maintain the leaf's indexes and
 * keeps the old line pointer live as a redirect, which would leave the old
 * spanning-index entry pointing at a still-live heap chain -- a stale entry that
 * vacuum can never reclaim and that causes false cross-partition uniqueness
 * conflicts.
 *
 * To prevent that, the spanning index's user-key columns must BLOCK HOT on the
 * leaf, exactly as a real local index on those columns would.  We add them to
 * the leaf's hot-blocking attribute set so a spanning-key change forces a
 * non-HOT update: the old heap tuple dies normally, its spanning entry becomes
 * reclaimable, and the liveness probe no longer mistakes it for a live row.
 *
 * Done with catalog scans only (no relation/index opens) so it is safe to call
 * from this cached relcache path; the result is memoized in rd_hotblockingattr.
 */
void
progresql_add_spanning_hotblocking_attrs(Relation relation,
										 Bitmapset **hotblockingattrs)
{
	Oid			leafOid = RelationGetRelid(relation);
	List	   *ancestors;
	ListCell   *lc;
	Relation	pg_index_rel;

	if (!relation->rd_rel->relispartition)
		return;

	ancestors = get_partition_ancestors(leafOid);
	if (ancestors == NIL)
		return;

	pg_index_rel = table_open(IndexRelationId, AccessShareLock);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;

		ScanKeyInit(&skey, Anum_pg_index_indrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(parentOid));
		scan = systable_beginscan(pg_index_rel, IndexIndrelidIndexId, true,
								  NULL, 1, &skey);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_index pgidx = (Form_pg_index) GETSTRUCT(tup);
			int			nuser;
			int			i;

			if (!IndexFormIsSpanning(pgidx))
				continue;

			/*
			 * The leading indnuniqatts columns are the user-visible unique key;
			 * the trailing column is the partseq discriminator (a system column
			 * not derived from any user column) and must be skipped.
			 */
			nuser = pgidx->indnuniqatts;
			for (i = 0; i < nuser; i++)
			{
				AttrNumber	rootattno = pgidx->indkey.values[i];
				char	   *attname;
				AttrNumber	leafattno;

				if (rootattno <= 0)		/* expression/system column: skip */
					continue;

				/*
				 * Map root attribute -> leaf attribute by name (robust to
				 * attribute-number divergence across the partition tree).
				 */
				attname = get_attname(parentOid, rootattno, true);
				if (attname == NULL)
					continue;
				leafattno = get_attnum(leafOid, attname);
				if (leafattno == InvalidAttrNumber)
					continue;

				*hotblockingattrs =
					bms_add_member(*hotblockingattrs,
								   leafattno - FirstLowInvalidHeapAttributeNumber);
			}
		}
		systable_endscan(scan);
	}

	table_close(pg_index_rel, AccessShareLock);
	list_free(ancestors);
}

/*
 * progresql_leaf_has_spanning_ancestor
 *
 * Cheap predicate: true if relation is a leaf partition that has at least one
 * spanning (GLOBAL) index on an ancestor root.  Used to keep
 * RelationGetIndexAttrBitmap from taking its "no local indexes" fast-path
 * exits for such a leaf, since its hot-blocking set is non-empty even though it
 * owns no local index.  Catalog scans only; early-exits on first match.
 */
bool
progresql_leaf_has_spanning_ancestor(Relation relation)
{
	List	   *ancestors;
	ListCell   *lc;
	Relation	pg_index_rel;
	bool		found = false;

	if (!relation->rd_rel->relispartition)
		return false;

	ancestors = get_partition_ancestors(RelationGetRelid(relation));
	if (ancestors == NIL)
		return false;

	pg_index_rel = table_open(IndexRelationId, AccessShareLock);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;

		ScanKeyInit(&skey, Anum_pg_index_indrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(parentOid));
		scan = systable_beginscan(pg_index_rel, IndexIndrelidIndexId, true,
								  NULL, 1, &skey);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			if (IndexFormIsSpanning((Form_pg_index) GETSTRUCT(tup)))
			{
				found = true;
				break;
			}
		}
		systable_endscan(scan);
		if (found)
			break;
	}

	table_close(pg_index_rel, AccessShareLock);
	list_free(ancestors);
	return found;
}
