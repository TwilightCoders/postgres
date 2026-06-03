/*-------------------------------------------------------------------------
 *
 * pg_spanning_drainq.c
 *	  routines to support manipulation of the pg_spanning_drainq catalog
 *
 * ProgreSQL: pg_spanning_drainq is the durable work queue for deferred
 * spanning-index VACUUM (the DHR design).  A leaf-partition VACUUM enqueues a
 * row here --- one per (spanning index, partseq) with pending dead heap entries
 * --- instead of eagerly scanning the whole spanning index, and a later
 * coalesced drain retires all queued entries in a single scan.  See
 * pg_spanning_drainq.h for the full rationale.
 *
 * This module currently provides only the index-drop cleanup; the enqueue
 * (writer), the dirty-set reader, and the drain itself land in later increments
 * of the E5 work, alongside the vacuum and autovacuum changes that exercise
 * them.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_spanning_drainq.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_spanning_drainq.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * SpanningDrainqEnqueue
 *		Record that partseq P of spanningIndex has ndead newly-dead heap entries
 *		pending a coalesced drain.  UPSERT on the (sdq_idxid, sdq_partseq) key:
 *		if the row already exists, accumulate sdq_ndead in place; otherwise insert
 *		a fresh row witnessed by this (vacuum) transaction's xid.
 *
 * The pre-existing row always carries the OLDER enqueue witness --- it was
 * written by a strictly earlier transaction (a partition maps to one partseq and
 * is vacuumed by one transaction at a time, so re-enqueue of the same
 * (index, partseq) only happens in a later run) --- so we deliberately leave
 * sdq_enqueue_xid untouched on update: it is already the oldest, which is exactly
 * the age signal the drain trigger wants.
 *
 * Caller holds at least AccessShareLock on spanningIndex (to read its relcache
 * entry / partseq); the RowExclusiveLock taken here on the queue catalog plus the
 * unique PK index serialize concurrent enqueues, and different partitions of the
 * same root touch different rows so they do not conflict.  Forces assignment of a
 * top-level xid (this is a catalog write, which needs one anyway), making that
 * xid the durable witness for the LP_DEAD marks this vacuum produced.
 */
void
SpanningDrainqEnqueue(Relation spanningIndex, int32 partseq, int64 ndead)
{
	Oid			spanningIndexOid = RelationGetRelid(spanningIndex);
	Relation	catalog;
	HeapTuple	tup;

	Assert(RelationIsSpanning(spanningIndex));
	Assert(partseq >= 0);

	catalog = table_open(SpanningDrainqRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(SPANNINGDRAINQ,
							  ObjectIdGetDatum(spanningIndexOid),
							  Int32GetDatum(partseq));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_spanning_drainq form = (Form_pg_spanning_drainq) GETSTRUCT(tup);

		/* Accumulate the dead count; keep the (older) existing witness xid. */
		form->sdq_ndead += ndead;
		CatalogTupleUpdate(catalog, &tup->t_self, tup);
		heap_freetuple(tup);
	}
	else
	{
		Datum		values[Natts_pg_spanning_drainq];
		bool		nulls[Natts_pg_spanning_drainq];

		memset(nulls, 0, sizeof(nulls));
		values[Anum_pg_spanning_drainq_sdq_idxid - 1] =
			ObjectIdGetDatum(spanningIndexOid);
		values[Anum_pg_spanning_drainq_sdq_partseq - 1] = Int32GetDatum(partseq);
		values[Anum_pg_spanning_drainq_sdq_enqueue_xid - 1] =
			TransactionIdGetDatum(GetTopTransactionId());
		values[Anum_pg_spanning_drainq_sdq_ndead - 1] = Int64GetDatum(ndead);

		tup = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
		CatalogTupleInsert(catalog, tup);
		heap_freetuple(tup);
	}

	table_close(catalog, RowExclusiveLock);
}

/*
 * SpanningDrainqListDirty
 *		Return the list of partseqs (as a List of int) that currently have
 *		pending un-drained rows for this spanning index.  This is the drain's
 *		snapshot of "which partitions need draining".
 *
 * Scans by the leading column of the PK index.  The caller (the drain) holds
 * ShareUpdateExclusiveLock on the spanning index, so no concurrent drain runs;
 * vacuum only ever ADDS rows (enqueue), never removes, so every partseq in the
 * returned snapshot remains valid for the duration of the drain.
 */
List *
SpanningDrainqListDirty(Oid spanningIndexOid)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *result = NIL;

	catalog = table_open(SpanningDrainqRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_spanning_drainq_sdq_idxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));

	scan = systable_beginscan(catalog, SpanningDrainqIdxidSeqIndexId,
							  true, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_spanning_drainq form = (Form_pg_spanning_drainq) GETSTRUCT(tup);

		result = lappend_int(result, form->sdq_partseq);
	}

	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	return result;
}

/*
 * SpanningDrainqDeleteList
 *		Delete the (spanningIndexOid, partseq) rows for each partseq in the list.
 *		Called by the drain after it has retired those partitions' spanning
 *		entries and reaped their heap slots.
 *
 * Deletes only the listed partseqs --- never the whole index's rows --- so a
 * partition that enqueued during this drain (a partseq not in the snapshot) is
 * left for the next drain.  Each listed partseq's row is stable because the
 * drain holds that partition's lock, blocking the only writer (vacuum enqueue).
 */
void
SpanningDrainqDeleteList(Oid spanningIndexOid, List *partseqs)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;

	if (partseqs == NIL)
		return;

	catalog = table_open(SpanningDrainqRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_spanning_drainq_sdq_idxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));

	scan = systable_beginscan(catalog, SpanningDrainqIdxidSeqIndexId,
							  true, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_spanning_drainq form = (Form_pg_spanning_drainq) GETSTRUCT(tup);

		if (list_member_int(partseqs, form->sdq_partseq))
			CatalogTupleDelete(catalog, &tup->t_self);
	}

	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
}

/*
 * RemoveSpanningDrainqForPartseq
 *		Drop the pg_spanning_drainq row for one (spanning index, partseq), if
 *		present.  Called from the DETACH/DROP partition-cleanup path so a
 *		departing partition's pending-drain obligation does not outlive its
 *		membership.  Otherwise the orphaned (idxid, partseq) row would describe
 *		a partition that is gone: a later drain would resolve it to no partition
 *		(harmless skip), but the row would leak until the index itself is
 *		dropped.  This is a transactional catalog delete --- it rolls back with
 *		an aborted DETACH, unlike the deferred LP_DEAD retirement.
 *
 * No-op if the partition had no pending drain row (the common case).  Scans the
 * (sdq_idxid, sdq_partseq) primary-key index with both keys.
 */
void
RemoveSpanningDrainqForPartseq(Oid spanningIndexOid, int32 partseq)
{
	Relation	catalog;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	tup;

	catalog = table_open(SpanningDrainqRelationId, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_pg_spanning_drainq_sdq_idxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));
	ScanKeyInit(&skey[1],
				Anum_pg_spanning_drainq_sdq_partseq,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(partseq));

	scan = systable_beginscan(catalog, SpanningDrainqIdxidSeqIndexId,
							  true, NULL, 2, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(catalog, &tup->t_self);

	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
}

/*
 * RemoveSpanningDrainqForIndex
 *		Drop all pg_spanning_drainq rows belonging to a spanning index.  Called
 *		from index_drop so the drain queue does not outlive the index it
 *		references (which would leave dangling sdq_idxid references to a recycled
 *		pg_class OID, and an undrainable orphan obligation).
 *
 * Scans by the leading column of the (sdq_idxid, sdq_partseq) primary-key
 * index, which serves "all rows for this index" without a dedicated
 * single-column index.
 */
void
RemoveSpanningDrainqForIndex(Oid spanningIndexOid)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;

	catalog = table_open(SpanningDrainqRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_spanning_drainq_sdq_idxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));

	scan = systable_beginscan(catalog, SpanningDrainqIdxidSeqIndexId,
							  true, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(catalog, &tup->t_self);

	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
}
