/*-------------------------------------------------------------------------
 *
 * pg_spanning_seq.h
 *	  definition of the "spanning-index partseq counter" system catalog
 *	  (pg_spanning_seq)
 *
 * ProgreSQL: this catalog holds the persistent per-spanning-index high-water
 * mark for partseq allocation.  A spanning index stores partseq --- an
 * index-local partition discriminator --- as the trailing key column of every
 * index entry, and pg_index_partition maps (index, partseq) -> partition.  For
 * those stored discriminators to stay unambiguous, a partseq must NEVER be
 * reused: once partition P leaves the index (DETACH/DROP), its number must not
 * be handed to a later joiner, or a surviving-but-stale entry keyed on that
 * number could silently re-resolve to the wrong partition.
 *
 * The original allocator derived "next partseq" as MAX(indpartseq) + 1 over the
 * *surviving* pg_index_partition rows.  Detaching the highest-numbered partition
 * lowered that maximum, so the next joiner reused the freed number --- the root
 * cause of several spanning-index corruption modes.  This catalog replaces that
 * scan with a durable monotonic counter: one row per spanning index recording
 * spseqnext, the next partseq to hand out.  Allocation reads spseqnext, returns
 * it, and bumps the row; DETACH/DROP never touches it.
 *
 * The counter row is created lazily when a spanning index's first partition is
 * allocated (so the first partseq is 1, spseqnext becomes 2) and dropped with
 * the index (RemoveSpanningSeqForIndex, from index_drop).  pg_upgrade preserves
 * it verbatim alongside the partseq map; an ordinary logical dump/restore omits
 * it, because that path rebuilds the index from scratch and re-allocates a fresh,
 * compacted partseq space (no stale entries exist to be aliased).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_spanning_seq.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SPANNING_SEQ_H
#define PG_SPANNING_SEQ_H

#include "catalog/genbki.h"
#include "catalog/pg_spanning_seq_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_spanning_seq definition.  cpp turns this into
 *		typedef struct FormData_pg_spanning_seq
 * ----------------
 */
CATALOG(pg_spanning_seq,565,SpanningSeqRelationId)
{
	Oid			spseqidxid BKI_LOOKUP(pg_class);	/* the spanning index */
	int32		spseqnext;		/* next partseq to allocate (monotonic) */
} FormData_pg_spanning_seq;

/* ----------------
 *		Form_pg_spanning_seq corresponds to a pointer to a tuple with
 *		the format of pg_spanning_seq relation.
 * ----------------
 */
typedef FormData_pg_spanning_seq *Form_pg_spanning_seq;

DECLARE_UNIQUE_INDEX_PKEY(pg_spanning_seq_idxid_index, 566, SpanningSeqIdxidIndexId, pg_spanning_seq, btree(spseqidxid oid_ops));

extern int32 SpanningSeqNextval(Oid spanningIndexOid);
extern void RemoveSpanningSeqForIndex(Oid spanningIndexOid);

#endif							/* PG_SPANNING_SEQ_H */
