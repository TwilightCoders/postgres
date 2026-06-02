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
#include "catalog/indexing.h"
#include "catalog/pg_spanning_drainq.h"
#include "utils/fmgroids.h"

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
