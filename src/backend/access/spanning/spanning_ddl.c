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
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/partition.h"
#include "catalog/pg_index_partition.h"
#include "catalog/pg_spanning_drainq.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "partitioning/partdesc.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "access/spanning.h"

/*
 * Deferred, abort-safe retirement of a partition's spanning-index entries.
 *
 * Retirement marks the matching index entries LP_DEAD (see
 * spanning_retire_entries below).  An LP_DEAD page hint is NOT transactional:
 * once set it survives a ROLLBACK.  Doing it inline during DETACH / DROP /
 * TRUNCATE was therefore unsafe -- if the command's transaction aborted, the
 * catalog changes rolled back (partition stays attached, rows return) but the
 * hints did not, leaving a live partition's keys invisible to
 * _bt_check_unique.  Cross-partition uniqueness was then silently unenforced
 * (a duplicate could be inserted into another partition).
 *
 * Fix: queue (indexOid, partseq) during the command and perform the actual
 * LP_DEAD marking from an XACT_EVENT_PRE_COMMIT callback, so it happens only
 * if the transaction commits.  An aborting (sub)transaction discards its
 * queued entries, leaving the index untouched.
 *
 * Caveats (both conservative -- they over-enforce or no-op, never corrupt):
 *  - Marking is deferred to commit, so within the SAME transaction a DETACH/DROP
 *    leaves the departed partition's entries present-but-unresolvable until the
 *    pre-commit pass.  _bt_check_unique skips an entry whose partseq no longer
 *    resolves (nbtinsert.c) rather than probing the storage-less root, so a
 *    same-transaction reinsert of the freed key succeeds.
 *  - PREPARE TRANSACTION discards the queue (a committed-prepared detach leaves
 *    stale-but-live entries that VACUUM reclaims); like a lost LP_DEAD hint
 *    after a crash, the only effect is a spurious conflict, not lost rows.
 */
typedef struct SpanningPendingRetire
{
	Oid			indexOid;
	int32		partseq;
	int			nestLevel;		/* subxact nest level at enqueue time */
} SpanningPendingRetire;

/* pending list lives in TopTransactionContext; reset at end of every xact */
static List *spanning_pending_retire = NIL;
static bool spanning_retire_cb_registered = false;

static void spanning_retire_entries(Oid indexOid, int32 partseq);

/*
 * XACT callback: mark queued entries LP_DEAD at pre-commit; discard on abort.
 */
static void
spanning_retire_xact_cb(XactEvent event, void *arg)
{
	ListCell   *lc;

	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			foreach(lc, spanning_pending_retire)
			{
				SpanningPendingRetire *p = (SpanningPendingRetire *) lfirst(lc);

				spanning_retire_entries(p->indexOid, p->partseq);
			}
			spanning_pending_retire = NIL;	/* freed with TopTransactionContext */
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/* discard without marking (see caveats above) */
			spanning_pending_retire = NIL;
			break;

		default:
			break;
	}
}

/*
 * Sub-XACT callback: a rolled-back subtransaction must drop the entries it
 * queued, otherwise a savepoint rollback of a DETACH would still retire the
 * partition's entries at top-level commit.
 */
static void
spanning_retire_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB && spanning_pending_retire != NIL)
	{
		int			abortLevel = GetCurrentTransactionNestLevel();
		List	   *kept = NIL;
		ListCell   *lc;

		foreach(lc, spanning_pending_retire)
		{
			SpanningPendingRetire *p = (SpanningPendingRetire *) lfirst(lc);

			/* keep only entries queued at a shallower (surviving) level */
			if (p->nestLevel < abortLevel)
				kept = lappend(kept, p);
		}
		spanning_pending_retire = kept;
	}
}

/*
 * Queue one (spanning index, partseq) pair for pre-commit retirement.
 */
static void
spanning_queue_retire(Oid indexOid, int32 partseq)
{
	MemoryContext old;
	SpanningPendingRetire *p;

	if (!spanning_retire_cb_registered)
	{
		RegisterXactCallback(spanning_retire_xact_cb, NULL);
		RegisterSubXactCallback(spanning_retire_subxact_cb, NULL);
		spanning_retire_cb_registered = true;
	}

	old = MemoryContextSwitchTo(TopTransactionContext);
	p = (SpanningPendingRetire *) palloc(sizeof(SpanningPendingRetire));
	p->indexOid = indexOid;
	p->partseq = partseq;
	p->nestLevel = GetCurrentTransactionNestLevel();
	spanning_pending_retire = lappend(spanning_pending_retire, p);
	MemoryContextSwitchTo(old);
}

/*
 * Mark LP_DEAD every entry in the given spanning index whose trailing partseq
 * key equals partseq.  Tolerates the index (or its root) having been dropped
 * in the same committing transaction (e.g. DROP of the whole partitioned root).
 */
static void
spanning_retire_entries(Oid indexOid, int32 partseq)
{
	Relation		idxRel;
	Relation		rootRel;
	IndexScanDesc	scan;
	ScanKeyData		skey;

	idxRel = try_index_open(indexOid, RowExclusiveLock);
	if (idxRel == NULL)
		return;					/* index dropped in this txn */

	if (!RelationIsSpanning(idxRel))
	{
		index_close(idxRel, RowExclusiveLock);
		return;
	}

	/*
	 * The spanning index lives on the partitioned root; open it as the
	 * heapRelation for index_beginscan.  The root has no table AM
	 * (rd_tableam == NULL); index_beginscan skips table_index_fetch_begin in
	 * that case and we only call index_getnext_tid (TID-only), so
	 * xs_heapfetch is never dereferenced.
	 */
	rootRel = try_table_open(idxRel->rd_index->indrelid, AccessShareLock);
	if (rootRel == NULL)
	{
		index_close(idxRel, RowExclusiveLock);
		return;
	}

	ScanKeyInit(&skey,
				idxRel->rd_index->indnkeyatts,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(partseq));

	scan = index_beginscan(rootRel, idxRel, SnapshotAny, NULL, 1, 0);
	index_rescan(scan, &skey, 1, NULL, 0);

	while (index_getnext_tid(scan, ForwardScanDirection) != NULL)
	{
		/*
		 * Mark this entry LP_DEAD on the next index_getnext_tid call (or on
		 * index_endscan).  _bt_check_unique skips LP_DEAD items, so future
		 * inserts won't see these retired entries as conflicts; they're
		 * physically reclaimed when the page is next modified or VACUUMed.
		 */
		scan->kill_prior_tuple = true;
	}

	index_endscan(scan);
	table_close(rootRel, AccessShareLock);
	index_close(idxRel, RowExclusiveLock);
}

/*
 * Clean all ProgreSQL spanning index entries that reference partRel.
 * Called after a partition's heap storage is gone (TRUNCATE, DROP, or
 * DETACH); any stale entries in spanning indexes on ancestor partitioned
 * roots are removed so they don't dangle to a now-invalid heap TID.
 *
 * Implementation: resolve each spanning index's partseq for this partition
 * and queue (indexOid, partseq) for pre-commit retirement (see
 * spanning_queue_retire / spanning_retire_entries).  Retirement marks the
 * matching entries LP_DEAD; _bt_check_unique skips LP_DEAD items
 * (nbtinsert.c, the !ItemIdIsDead test in the leaf scan loop), so future
 * uniqueness checks correctly ignore them.  Physical removal happens lazily
 * via btree's simple/bottom-up deletion passes when pages are next modified,
 * or via VACUUM.
 *
 * The marking is deferred to commit because an LP_DEAD page hint is not
 * transactional: doing it here would leave a live partition's keys retired
 * if the command's transaction later rolled back.  Filtering by
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
			 * still exists (drop_map removes it only after this loop).  If the
			 * partition was never mapped to this index there is nothing to
			 * clean.
			 */
			partseq = SpanningLookupPartseqByRelid(idxRel, partOid);
			index_close(idxRel, RowExclusiveLock);
			if (partseq < 0)
				continue;

			/*
			 * Queue the LP_DEAD marking for pre-commit rather than doing it
			 * inline: the marking is a non-transactional page hint and must
			 * not survive a rollback of this command (spanning_queue_retire).
			 */
			spanning_queue_retire(indexOid, partseq);

			/*
			 * For DROP/DETACH, also reap this partition's deferred-drain queue
			 * row so the obligation does not outlive its membership.  (A
			 * transactional catalog delete --- safe inline, unlike the LP_DEAD
			 * hint above.)  TRUNCATE keeps the partition, so it keeps its row.
			 */
			if (drop_map)
				RemoveSpanningDrainqForPartseq(indexOid, partseq);
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

/*
 * BuildSpanningIndexFromPartitions
 *
 * After creating a ProgreSQL spanning index on a partitioned root, populate
 * it with entries for all existing tuples in all leaf child partitions.
 * Without this step the spanning index is empty after ALTER TABLE ADD
 * CONSTRAINT on a non-empty table, so cross-partition uniqueness checks
 * miss pre-existing rows.
 *
 * For each live tuple in each partition we compute the index key
 * (user_columns... + tableoid) via FormIndexDatum and call index_insert with
 * UNIQUE_CHECK_YES so that pre-existing duplicates are caught.
 */
void
BuildSpanningIndexFromPartitions(Relation rel, Oid indexRelationId)
{
	Relation	idxRel;
	IndexInfo  *idxInfo;
	PartitionDesc partdesc;
	Snapshot	snapshot;
	int			pi;

	idxRel = index_open(indexRelationId, RowExclusiveLock);
	idxInfo = BuildIndexInfo(idxRel);

	partdesc = RelationGetPartitionDesc(rel, true);

	/*
	 * Push the transaction snapshot as active so that heap visibility checks
	 * inside table_scan_getnextslot (and the btree uniqueness check path) can
	 * satisfy the snapshot active_count assertions.  DDL commands do not
	 * push a snapshot automatically, so we must do it explicitly here.
	 */
	snapshot = GetTransactionSnapshot();
	PushActiveSnapshot(snapshot);

	for (pi = 0; pi < partdesc->nparts; pi++)
	{
		Oid			partOid = partdesc->oids[pi];
		Relation	partRel;
		TupleTableSlot *slot;
		TableScanDesc scan;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		int32		partseq;

		partRel = table_open(partOid, AccessShareLock);

		/*
		 * Spanning indexes do not support multi-level partitioning: partdesc
		 * holds only the root's DIRECT partitions, so a sub-partitioned child's
		 * grandchild leaves are never visited here --- they would silently be
		 * omitted from the index (their rows unenforced for uniqueness) and
		 * could never get a partseq.  Reject it at build time rather than
		 * corrupt-by-omission.
		 */
		if (partRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create a spanning (GLOBAL) index on a multi-level partitioned table"),
					 errdetail("Partition \"%s\" is itself partitioned; spanning indexes require every partition to be a leaf.",
							   RelationGetRelationName(partRel))));

		/*
		 * ProgreSQL C1: this partition's index-local partseq (get-or-allocate;
		 * stable across rebuilds).  It is the trailing discriminator stored in
		 * each spanning index entry below.
		 */
		partseq = SpanningGetOrAllocPartseq(idxRel, partOid);

		slot = table_slot_create(partRel, NULL);
		scan = table_beginscan(partRel, GetActiveSnapshot(), 0, NULL);

		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			int			k;

			FormIndexDatum(idxInfo, slot, NULL, values, isnull);

			/*
			 * The trailing key column is this partition's index-local
			 * partseq, the stable discriminator that resolves back to the
			 * partition via pg_index_partition.
			 */
			k = idxInfo->ii_NumIndexKeyAttrs - 1;
			values[k] = Int32GetDatum(partseq);
			isnull[k] = false;

			/*
			 * Pass rel (the partitioned root) as heapRelation, not partRel.
			 * The root has rd_tableam == NULL, which signals nbtinsert.c to
			 * skip the heap-liveness deletion passes (_bt_simpledel_pass and
			 * _bt_bottomupdel_pass) that cannot safely process TIDs spread
			 * across multiple partitions.
			 */
			index_insert(idxRel, values, isnull, &slot->tts_tid,
						 rel, UNIQUE_CHECK_YES, false, idxInfo);
		}

		table_endscan(scan);
		ExecDropSingleTupleTableSlot(slot);
		table_close(partRel, AccessShareLock);
	}

	PopActiveSnapshot();

	index_close(idxRel, RowExclusiveLock);
}
