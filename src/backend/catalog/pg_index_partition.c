/*-------------------------------------------------------------------------
 *
 * pg_index_partition.c
 *	  routines to support manipulation of the pg_index_partition catalog
 *
 * ProgreSQL: pg_index_partition maps a spanning index's index-local
 * partition sequence number (partseq) to the partition relation it
 * identifies.  partseq is allocated once, when a partition first joins a
 * spanning index's domain (at index build, REINDEX, or partition ATTACH),
 * recorded here, and never reused or recomputed thereafter.  Because partseq
 * is index-local and stable, spanning index entries that key on it survive
 * partition OID reuse and pg_upgrade --- the failure modes inherent to the
 * previous tableoid-based discriminator.
 *
 * This module provides the writer (get-or-allocate) and the reader
 * (resolver).  As of increment C the catalog is populated alongside the
 * still-tableoid-keyed index entries (dual-tracked); increment D switches the
 * stored discriminator to partseq and begins consuming the resolver on the
 * read path.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_index_partition.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_index_partition.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * spanning_lookup_partseq
 *		Return the partseq already assigned to (spanningIndexOid, partitionOid),
 *		or -1 if the partition is not yet mapped to this spanning index.
 *
 * Uses the (indpartidxid, indpartrelid) syscache; this is the same lookup the
 * writer needs to stay idempotent and the lookup callers will use to translate
 * a partition relation to its partseq before writing an index entry.
 */
static int32
spanning_lookup_partseq(Oid spanningIndexOid, Oid partitionOid)
{
	HeapTuple	tup;
	int32		partseq = -1;

	tup = SearchSysCache2(INDEXPARTITIONREL,
						  ObjectIdGetDatum(spanningIndexOid),
						  ObjectIdGetDatum(partitionOid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_index_partition form = (Form_pg_index_partition) GETSTRUCT(tup);

		partseq = form->indpartseq;
		ReleaseSysCache(tup);
	}

	return partseq;
}

/*
 * spanning_max_partseq
 *		Return the largest partseq currently assigned to this spanning index,
 *		or 0 if it has no partitions mapped yet.
 *
 * Scans pg_index_partition via the (indpartidxid, indpartseq) index.  A plain
 * forward scan suffices --- the maps are small (one row per partition) --- and
 * a systable scan (rather than the syscache) guarantees we observe rows this
 * transaction inserted earlier in a multi-partition build, provided the caller
 * has issued CommandCounterIncrement after each insert (see
 * SpanningGetOrAllocPartseq).
 */
static int32
spanning_max_partseq(Relation catalog, Oid spanningIndexOid)
{
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	int32		maxseq = 0;

	ScanKeyInit(&skey,
				Anum_pg_index_partition_indpartidxid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spanningIndexOid));

	scan = systable_beginscan(catalog, IndexPartitionIdxidSeqIndexId,
							  true, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_index_partition form = (Form_pg_index_partition) GETSTRUCT(tup);

		if (form->indpartseq > maxseq)
			maxseq = form->indpartseq;
	}

	systable_endscan(scan);

	return maxseq;
}

/*
 * SpanningLookupPartseqByRelid
 *		Exposed read-only lookup: return the partseq mapping spanningIndex to
 *		partitionOid, or -1 if the partition is not in this index's map.  Used
 *		by the executor to stamp each per-statement cache entry with its
 *		partseq.  Never allocates.
 */
int32
SpanningLookupPartseqByRelid(Relation spanningIndex, Oid partitionOid)
{
	return spanning_lookup_partseq(RelationGetRelid(spanningIndex),
								   partitionOid);
}

/*
 * SpanningGetOrAllocPartseq
 *		Return the partseq for (spanningIndex, partitionOid), allocating and
 *		recording a new one on first sight.
 *
 * If the partition is already mapped (e.g. during REINDEX, or a re-run of the
 * build), its existing partseq is returned unchanged --- partseq must be
 * stable across rebuilds so that stored index entries keep resolving to the
 * right partition.  Otherwise a fresh partseq = max+1 is allocated and a
 * pg_index_partition row is inserted.  partseq numbers are monotonic per
 * spanning index and are never reused, even after DETACH/DROP removes a row.
 *
 * Caller must hold a lock on the spanning index that serializes concurrent
 * joiners (index build and ATTACH both do); the unique (indpartidxid,
 * indpartrelid) index is the backstop, turning any missed serialization into a
 * duplicate-key error rather than a duplicate partseq.  Not safe under the
 * weaker locking of CONCURRENTLY, which is unsupported for spanning indexes.
 */
int32
SpanningGetOrAllocPartseq(Relation spanningIndex, Oid partitionOid)
{
	Oid			spanningIndexOid = RelationGetRelid(spanningIndex);
	Relation	catalog;
	int32		partseq;
	Datum		values[Natts_pg_index_partition];
	bool		nulls[Natts_pg_index_partition];
	HeapTuple	tup;

	/* Already mapped?  Reuse the existing partseq (idempotent / REINDEX). */
	partseq = spanning_lookup_partseq(spanningIndexOid, partitionOid);
	if (partseq >= 0)
		return partseq;

	catalog = table_open(IndexPartitionRelationId, RowExclusiveLock);

	partseq = spanning_max_partseq(catalog, spanningIndexOid) + 1;

	memset(nulls, 0, sizeof(nulls));
	values[Anum_pg_index_partition_indpartidxid - 1] =
		ObjectIdGetDatum(spanningIndexOid);
	values[Anum_pg_index_partition_indpartseq - 1] = Int32GetDatum(partseq);
	values[Anum_pg_index_partition_indpartrelid - 1] =
		ObjectIdGetDatum(partitionOid);

	tup = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	CatalogTupleInsert(catalog, tup);
	heap_freetuple(tup);

	table_close(catalog, RowExclusiveLock);

	/*
	 * Make the new row visible to the next max/lookup scan in this same
	 * transaction (e.g. the next partition of a multi-partition build).
	 */
	CommandCounterIncrement();

	return partseq;
}

/*
 * SpanningResolvePartseqRelid
 *		Map a stored partseq back to the partition relation it identifies, for
 *		a given spanning index.  Returns InvalidOid if no such mapping exists
 *		(e.g. the partition has been detached and its row removed).
 *
 * This is the read-side primitive: increment D calls it from the uniqueness
 * check and from VACUUM to turn a partseq read out of an index tuple back into
 * the heap that owns the referenced TID.  Uses the (indpartidxid, indpartseq)
 * syscache.
 */
Oid
SpanningResolvePartseqRelidByOid(Oid spanningIndexOid, int32 partseq)
{
	HeapTuple	tup;
	Oid			partitionOid = InvalidOid;

	tup = SearchSysCache2(INDEXPARTITIONSEQ,
						  ObjectIdGetDatum(spanningIndexOid),
						  Int32GetDatum(partseq));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_index_partition form = (Form_pg_index_partition) GETSTRUCT(tup);

		partitionOid = form->indpartrelid;
		ReleaseSysCache(tup);
	}

	return partitionOid;
}

Oid
SpanningResolvePartseqRelid(Relation spanningIndex, int32 partseq)
{
	return SpanningResolvePartseqRelidByOid(RelationGetRelid(spanningIndex),
										   partseq);
}

/*
 * spanning_delete_partition_rows
 *		Delete every pg_index_partition row matching one column == targetOid,
 *		scanning by the supplied index.  Shared implementation for the two
 *		drop-cleanup entry points below.
 */
static void
spanning_delete_partition_rows(AttrNumber keyAttno, Oid indexId, Oid targetOid)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;

	catalog = table_open(IndexPartitionRelationId, RowExclusiveLock);

	ScanKeyInit(&skey, keyAttno, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(targetOid));

	scan = systable_beginscan(catalog, indexId, true, NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(catalog, &tup->t_self);

	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
}

/*
 * RemoveSpanningPartitionMapForIndex
 *		Drop all pg_index_partition rows belonging to a spanning index.  Called
 *		from index_drop so the partseq map does not outlive the index (which
 *		would leave dangling indpartidxid references to a recycled pg_class OID
 *		--- the very failure mode partseq exists to avoid).
 */
void
RemoveSpanningPartitionMapForIndex(Oid spanningIndexOid)
{
	spanning_delete_partition_rows(Anum_pg_index_partition_indpartidxid,
								   IndexPartitionIdxidSeqIndexId,
								   spanningIndexOid);
}

/*
 * RemoveSpanningPartitionMapForPartition
 *		Drop every pg_index_partition row that maps any spanning index to this
 *		partition.  Called when a partition is dropped or detached, so a stale
 *		indpartrelid cannot dangle (or, worse, alias a future relation that
 *		reuses the OID).
 */
void
RemoveSpanningPartitionMapForPartition(Oid partitionOid)
{
	spanning_delete_partition_rows(Anum_pg_index_partition_indpartrelid,
								   IndexPartitionIdxidRelidIndexId,
								   partitionOid);
}
