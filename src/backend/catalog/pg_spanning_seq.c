/*-------------------------------------------------------------------------
 *
 * pg_spanning_seq.c
 *	  routines to support manipulation of the pg_spanning_seq catalog
 *
 * ProgreSQL: pg_spanning_seq is the durable, per-spanning-index partseq
 * counter that guarantees partseq numbers are never reused.  Allocation
 * (SpanningSeqNextval) reads the stored "next" value, returns it, and bumps the
 * row; the counter is created lazily on the first partition (so the first
 * partseq is 1) and dropped with the index (RemoveSpanningSeqForIndex).  See
 * pg_spanning_seq.h for the full rationale.
 *
 * The previous allocator computed the next partseq as MAX(indpartseq) + 1 over
 * surviving pg_index_partition rows, which reused a number whenever the
 * highest-numbered partition was detached/dropped.  This counter removes that
 * reuse: DETACH/DROP deletes only the map row, never the counter, so a later
 * joiner always gets a strictly larger number than any partition that ever
 * belonged to the index.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_spanning_seq.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_spanning_seq.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

/*
 * SpanningSeqNextval
 *		Allocate and return the next partseq for a spanning index, advancing the
 *		persistent counter so the value is never handed out again.
 *
 * The very first allocation for an index lazily creates the counter row and
 * returns 1 (recording spseqnext = 2).  Thereafter the stored spseqnext is
 * returned and the row bumped by one.  Because DETACH/DROP removes pg_index_
 * partition rows but never this counter, the returned value strictly exceeds the
 * partseq of every partition that has ever belonged to this index --- which is
 * exactly the no-reuse invariant spanning correctness depends on.
 *
 * A systable scan on the (spseqidxid) unique index locates the row; the
 * RowExclusiveLock on the catalog plus that unique index serialize concurrent
 * first-time creators (the caller already holds a lock on the spanning index
 * that serializes joiners --- index build and ATTACH both do).  A
 * CommandCounterIncrement makes the bumped value visible to the next allocation
 * in the same transaction (e.g. a multi-partition build).
 */
int32
SpanningSeqNextval(Oid spanningIndexOid)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	int32		partseq;

	catalog = table_open(SpanningSeqRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_spanning_seq_spseqidxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));

	scan = systable_beginscan(catalog, SpanningSeqIdxidIndexId,
							  true, NULL, 1, &skey);

	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		Form_pg_spanning_seq form = (Form_pg_spanning_seq) GETSTRUCT(tup);
		HeapTuple	newtup;
		Datum		values[Natts_pg_spanning_seq];
		bool		nulls[Natts_pg_spanning_seq];
		bool		replace[Natts_pg_spanning_seq];

		partseq = form->spseqnext;

		/*
		 * Guard the int4 partseq space.  Wrapping past INT_MAX would store a
		 * negative "next" and reintroduce reuse; refuse instead.  Unreachable
		 * in practice (it takes 2^31 attaches to one index), but cheap to hold.
		 */
		if (partseq == PG_INT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("spanning index partseq space exhausted")));

		memset(nulls, 0, sizeof(nulls));
		memset(replace, 0, sizeof(replace));
		values[Anum_pg_spanning_seq_spseqnext - 1] = Int32GetDatum(partseq + 1);
		replace[Anum_pg_spanning_seq_spseqnext - 1] = true;

		newtup = heap_modify_tuple(tup, RelationGetDescr(catalog),
								   values, nulls, replace);
		CatalogTupleUpdate(catalog, &newtup->t_self, newtup);
		heap_freetuple(newtup);
	}
	else
	{
		Datum		values[Natts_pg_spanning_seq];
		bool		nulls[Natts_pg_spanning_seq];

		/* First partition for this index: partseq 1, next becomes 2. */
		partseq = 1;

		memset(nulls, 0, sizeof(nulls));
		values[Anum_pg_spanning_seq_spseqidxid - 1] =
			ObjectIdGetDatum(spanningIndexOid);
		values[Anum_pg_spanning_seq_spseqnext - 1] = Int32GetDatum(partseq + 1);

		tup = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
		CatalogTupleInsert(catalog, tup);
		heap_freetuple(tup);
	}

	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);

	/* Make the new counter value visible to the next allocation this txn. */
	CommandCounterIncrement();

	return partseq;
}

/*
 * RemoveSpanningSeqForIndex
 *		Drop the pg_spanning_seq counter row belonging to a spanning index.
 *		Called from index_drop so the counter does not outlive the index it
 *		references (which would leave a dangling spseqidxid against a recycled
 *		pg_class OID).  No-op for ordinary indexes, which have no row here.
 */
void
RemoveSpanningSeqForIndex(Oid spanningIndexOid)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;

	catalog = table_open(SpanningSeqRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_spanning_seq_spseqidxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));

	scan = systable_beginscan(catalog, SpanningSeqIdxidIndexId,
							  true, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(catalog, &tup->t_self);

	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
}
