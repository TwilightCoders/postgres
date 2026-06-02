/*-------------------------------------------------------------------------
 *
 * spanning_ddl.c
 *	  ProgreSQL: partition-lifecycle maintenance for cross-partition
 *	  ("spanning") unique indexes.
 *
 * The ATTACH backfill and the DROP/DETACH/TRUNCATE cleanup of spanning index
 * entries, called as thin hooks from tablecmds.c.  Kept out of tablecmds.c so
 * the incision there stays the call sites plus the inline DDL guards.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/spanning/spanning_ddl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/partition.h"
#include "catalog/pg_index_partition.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "access/spanning.h"

/*
 * Clean all ProgreSQL spanning index entries that reference partRel.
 * Called after a partition's heap storage is gone (TRUNCATE, DROP, or
 * DETACH); any stale entries in spanning indexes on ancestor partitioned
 * roots are removed so they don't dangle to a now-invalid heap TID.
 *
 * Implementation: scan the spanning index for entries whose trailing
 * tableoid key column equals partOid, and set kill_prior_tuple on each
 * match so the btree marks the index entry LP_DEAD.  _bt_check_unique
 * skips LP_DEAD items (nbtinsert.c, the !ItemIdIsDead test in the leaf
 * scan loop), so future uniqueness checks correctly ignore them.
 * Physical removal happens lazily via btree's simple/bottom-up deletion
 * passes when pages are next modified, or via VACUUM.
 *
 * This is the BUG-A-safe replacement for the prior bulk_delete-based
 * cleanup, which collided across partitions whenever heap TIDs (e.g.
 * (0,1), the first row of every partition) were shared.  Filtering by
 * (key, partseq) via the index scan key is exact: no false positives,
 * no false negatives.
 *
 * drop_map: see the prototype in tablecmds.h.  DROP/DETACH pass true (retire
 * the partseq mapping); TRUNCATE passes false (the partition stays attached
 * and must keep its partseq).
 */
void
progresql_clean_spanning_indexes_for_partition(Relation partRel, bool drop_map)
{
	List	   *ancestors;
	ListCell   *lc;
	Oid			partOid = RelationGetRelid(partRel);

	/* Only leaf partitions that are also inheritance children need cleaning. */
	if (!partRel->rd_rel->relispartition)
		return;

	ancestors = get_partition_ancestors(partOid);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		Relation	parentRel;
		List	   *indexoidlist;
		ListCell   *il;

		parentRel = table_open(parentOid, AccessShareLock);
		indexoidlist = RelationGetIndexList(parentRel);

		foreach(il, indexoidlist)
		{
			Oid				indexOid = lfirst_oid(il);
			Relation		idxRel;
			IndexScanDesc	scan;
			ScanKeyData		skey;
			int32			partseq;

			idxRel = index_open(indexOid, RowExclusiveLock);

			/* Only spanning indexes. */
			if (!RelationIsSpanning(idxRel))
			{
				index_close(idxRel, RowExclusiveLock);
				continue;
			}

			/*
			 * Resolve this partition's index-local partseq while its map row
			 * still exists (we delete the map rows only after this loop).  If
			 * the partition was never mapped to this index there is nothing to
			 * clean.
			 */
			partseq = SpanningLookupPartseqByRelid(idxRel, partOid);
			if (partseq < 0)
			{
				index_close(idxRel, RowExclusiveLock);
				continue;
			}

			/* Scan for entries whose trailing partseq matches this partition. */
			ScanKeyInit(&skey,
						idxRel->rd_index->indnkeyatts,
						BTEqualStrategyNumber,
						F_INT4EQ,
						Int32GetDatum(partseq));

			/*
			 * Pass parentRel as heapRelation.  The partitioned root has no
			 * table AM (rd_tableam == NULL); index_beginscan skips
			 * table_index_fetch_begin in that case.  We only call
			 * index_getnext_tid (TID-only), never index_getnext (heap fetch),
			 * so xs_heapfetch is never dereferenced.
			 */
			scan = index_beginscan(parentRel, idxRel, SnapshotAny, NULL,
								   1, 0);
			index_rescan(scan, &skey, 1, NULL, 0);

			while (index_getnext_tid(scan, ForwardScanDirection) != NULL)
			{
				/*
				 * Mark this entry LP_DEAD on the next index_getnext_tid call
				 * (or on index_endscan).  btree's _bt_check_unique skips
				 * LP_DEAD items, so future inserts won't see these stale
				 * entries as conflicts; they're physically reclaimed when
				 * the page is next modified or VACUUMed.
				 */
				scan->kill_prior_tuple = true;
			}

			index_endscan(scan);
			index_close(idxRel, RowExclusiveLock);
		}

		list_free(indexoidlist);
		table_close(parentRel, AccessShareLock);
	}

	list_free(ancestors);

	/*
	 * C1: for DROP/DETACH, now that every spanning index's entries for this
	 * partition have been retired, drop the partition's partseq map rows.  This
	 * runs LAST so the per-index partseq lookups above still see the map;
	 * deleting it earlier would leave the entry scans unable to find the
	 * partseq.  Removing the rows keeps indpartrelid from dangling (or aliasing
	 * a future OID reuse).
	 *
	 * TRUNCATE passes drop_map=false: the partition stays attached, so its
	 * partseq must persist for subsequent inserts to be discriminated.
	 */
	if (drop_map)
		RemoveSpanningPartitionMapForPartition(partOid);
}

/*
 * progresql_backfill_spanning_indexes_for_attached_partition
 *
 * After ATTACH PARTITION, walk all spanning indexes on partitioned-root
 * ancestors of attachrel and insert (user_columns..., attachrel_oid) keys
 * for each existing row in attachrel.  Without this step, BUG C: rows
 * already in the attached partition are absent from the spanning index,
 * so cross-partition uniqueness checks miss them.
 *
 * Called from ATExecAttachPartition immediately after
 * AttachPartitionEnsureIndexes, while pg_inherits already has the new
 * row so get_partition_ancestors can find ancestor roots.
 */
void
progresql_backfill_spanning_indexes_for_attached_partition(Relation attachrel)
{
	List	   *ancestors;
	ListCell   *lc;
	Oid			attachOid = RelationGetRelid(attachrel);

	if (attachrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;					/* sub-partitioned: handled by leaf ATTACH */

	ancestors = get_partition_ancestors(attachOid);

	foreach(lc, ancestors)
	{
		Oid			parentOid = lfirst_oid(lc);
		Relation	parentRel;
		List	   *indexoidlist;
		ListCell   *il;

		parentRel = table_open(parentOid, AccessShareLock);

		/* Spanning indexes only live on PARTITION BY roots. */
		if (parentRel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			table_close(parentRel, AccessShareLock);
			continue;
		}

		indexoidlist = RelationGetIndexList(parentRel);

		foreach(il, indexoidlist)
		{
			Oid			indexOid = lfirst_oid(il);
			Relation	idxRel;
			IndexInfo  *idxInfo;
			TupleTableSlot *slot;
			TableScanDesc scan;
			Datum		values[INDEX_MAX_KEYS];
			bool		isnull[INDEX_MAX_KEYS];
			Snapshot	snapshot;
			int32		partseq;

			idxRel = index_open(indexOid, RowExclusiveLock);

			if (!RelationIsSpanning(idxRel))
			{
				index_close(idxRel, RowExclusiveLock);
				continue;
			}

			idxInfo = BuildIndexInfo(idxRel);

			/*
			 * ProgreSQL C1: the attaching partition's index-local partseq
			 * (get-or-allocate).  It is the trailing discriminator stored in
			 * each backfilled spanning index entry below.
			 */
			partseq = SpanningGetOrAllocPartseq(idxRel, attachOid);

			snapshot = GetTransactionSnapshot();
			PushActiveSnapshot(snapshot);

			slot = table_slot_create(attachrel, NULL);
			scan = table_beginscan(attachrel, GetActiveSnapshot(), 0, NULL);

			while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
			{
				int			k;

				FormIndexDatum(idxInfo, slot, NULL, values, isnull);

				/* Trailing key column: the attaching partition's partseq. */
				k = idxInfo->ii_NumIndexKeyAttrs - 1;
				values[k] = Int32GetDatum(partseq);
				isnull[k] = false;

				/*
				 * Pass parentRel as heapRelation: the partitioned root has
				 * rd_tableam == NULL, which signals nbtinsert.c to skip
				 * heap-liveness deletion passes that would be unsafe across
				 * partitions.  UNIQUE_CHECK_YES so any pre-existing
				 * cross-partition duplicates are reported.
				 */
				index_insert(idxRel, values, isnull, &slot->tts_tid,
							 parentRel, UNIQUE_CHECK_YES, false, idxInfo);
			}

			table_endscan(scan);
			ExecDropSingleTupleTableSlot(slot);
			PopActiveSnapshot();
			index_close(idxRel, RowExclusiveLock);
		}

		list_free(indexoidlist);
		table_close(parentRel, AccessShareLock);
	}

	list_free(ancestors);
}
