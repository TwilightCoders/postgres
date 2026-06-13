/*-------------------------------------------------------------------------
 *
 * pg_spanning_drainq.h
 *	  definition of the "spanning-index drain queue" system catalog
 *	  (pg_spanning_drainq)
 *
 * ProgreSQL: this catalog is the durable work queue for deferred spanning-index
 * VACUUM (the DHR design --- Defer-Heap-Reap).  A leaf-partition VACUUM no
 * longer eagerly scans the whole spanning index to retire its dead entries
 * (which was O(N) per leaf, O(N^2) per sweep); instead it leaves the dead heap
 * line pointers at LP_DEAD (un-reaped, so they cannot be reused) and records a
 * single row here saying "partseq P of spanning index I has pending dead
 * entries".  A later coalesced drain scans the spanning index ONCE, retires all
 * queued partseqs' entries, then reaps the now-safe LP_DEAD slots --- collapsing
 * N independent full scans into one (O(N^2) -> O(N)).
 *
 * The queue deliberately does NOT store the dead TID list.  The authoritative
 * set of TIDs to delete is re-derived at drain time from the partition heap's
 * current LP_DEAD line pointers, so the queue is tiny: at most one row per
 * (spanning index, partseq), i.e. O(#partitions), regardless of dead-tuple
 * volume.  sdq_ndead and sdq_enqueue_xid carry only the volume and age signals
 * the drain trigger needs.
 *
 * As of this increment the catalog is defined and cleaned up with its owning
 * index, but not yet populated or consulted by any vacuum path; the enqueue,
 * drain, and trigger machinery land in later increments of the E5 work.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_spanning_drainq.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SPANNING_DRAINQ_H
#define PG_SPANNING_DRAINQ_H

#include "catalog/genbki.h"
#include "catalog/pg_spanning_drainq_d.h"	/* IWYU pragma: export */
#include "nodes/pg_list.h"

/* ----------------
 *		pg_spanning_drainq definition.  cpp turns this into
 *		typedef struct FormData_pg_spanning_drainq
 * ----------------
 */
CATALOG(pg_spanning_drainq,563,SpanningDrainqRelationId)
{
	Oid			sdq_idxid BKI_LOOKUP(pg_class);	/* the spanning index */
	int32		sdq_partseq;	/* index-local partseq with pending dead entries */

	/*
	 * Oldest enqueue witness for this (index, partseq): the xid of the leaf
	 * vacuum that first created this pending row since the last drain.  Used as
	 * an age signal for the time-based drain trigger and to order drain
	 * retirement.  A 32-bit TransactionId is sufficient and idiomatic (cf.
	 * pg_class.relfrozenxid): drainq rows are transient --- drained well within
	 * one xid epoch by the age trigger --- so they are compared with the
	 * standard wraparound-aware TransactionIdPrecedes, never raw arithmetic.
	 */
	TransactionId sdq_enqueue_xid;

	/*
	 * Approximate count of dead TIDs enqueued for this (index, partseq) since
	 * the last drain.  Accumulated in place by re-enqueue; drives the
	 * threshold-based drain trigger.  Not authoritative for deletion --- the
	 * drain re-derives the actual kill set from live LP_DEAD heap state.
	 */
	int64		sdq_ndead;
} FormData_pg_spanning_drainq;

/* ----------------
 *		Form_pg_spanning_drainq corresponds to a pointer to a tuple with
 *		the format of pg_spanning_drainq relation.
 * ----------------
 */
typedef FormData_pg_spanning_drainq *Form_pg_spanning_drainq;

DECLARE_UNIQUE_INDEX_PKEY(pg_spanning_drainq_idxid_seq_index, 564, SpanningDrainqIdxidSeqIndexId, pg_spanning_drainq, btree(sdq_idxid oid_ops, sdq_partseq int4_ops));

MAKE_SYSCACHE(SPANNINGDRAINQ, pg_spanning_drainq_idxid_seq_index, 16);

extern void SpanningDrainqEnqueue(Relation spanningIndex, int32 partseq,
								  int64 ndead);
extern List *SpanningDrainqListDirty(Oid spanningIndexOid);
extern List *SpanningDrainqListAllIndexes(void);
extern void SpanningDrainqDeleteList(Oid spanningIndexOid, List *partseqs);
extern void RemoveSpanningDrainqForPartseq(Oid spanningIndexOid, int32 partseq);
extern void RemoveSpanningDrainqForIndex(Oid spanningIndexOid);

#endif							/* PG_SPANNING_DRAINQ_H */
