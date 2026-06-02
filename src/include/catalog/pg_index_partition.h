/*-------------------------------------------------------------------------
 *
 * pg_index_partition.h
 *	  definition of the "index partition map" system catalog
 *	  (pg_index_partition)
 *
 * ProgreSQL: this catalog maps a spanning index's index-local partition
 * sequence number (partseq) to the partition relation it identifies.  A
 * spanning index stores partseq --- not the partition's tableoid --- as the
 * trailing discriminator key column, so this catalog is the authoritative
 * (index, partseq) -> partition resolution used when an index entry must be
 * traced back to the heap that owns its tuple.  partseq is index-local,
 * stable for the life of the partition's membership, and never reused, which
 * is what makes spanning indexes survive OID reuse and pg_upgrade.
 *
 * At this point the catalog is defined but not yet populated or consulted by
 * any code path; allocation (writer) and resolution (reader) are added in
 * later increments of the C1 work.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_index_partition.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INDEX_PARTITION_H
#define PG_INDEX_PARTITION_H

#include "catalog/genbki.h"
#include "catalog/pg_index_partition_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_index_partition definition.  cpp turns this into
 *		typedef struct FormData_pg_index_partition
 * ----------------
 */
CATALOG(pg_index_partition,560,IndexPartitionRelationId)
{
	Oid			indpartidxid BKI_LOOKUP(pg_class);	/* the spanning index */
	int32		indpartseq;		/* index-local partition sequence number */
	Oid			indpartrelid BKI_LOOKUP(pg_class);	/* the partition relation */
} FormData_pg_index_partition;

/* ----------------
 *		Form_pg_index_partition corresponds to a pointer to a tuple with
 *		the format of pg_index_partition relation.
 * ----------------
 */
typedef FormData_pg_index_partition *Form_pg_index_partition;

DECLARE_UNIQUE_INDEX_PKEY(pg_index_partition_idxid_seq_index, 561, IndexPartitionIdxidSeqIndexId, pg_index_partition, btree(indpartidxid oid_ops, indpartseq int4_ops));
DECLARE_UNIQUE_INDEX(pg_index_partition_idxid_relid_index, 562, IndexPartitionIdxidRelidIndexId, pg_index_partition, btree(indpartidxid oid_ops, indpartrelid oid_ops));

MAKE_SYSCACHE(INDEXPARTITIONSEQ, pg_index_partition_idxid_seq_index, 16);
MAKE_SYSCACHE(INDEXPARTITIONREL, pg_index_partition_idxid_relid_index, 16);

extern int32 SpanningGetOrAllocPartseq(Relation spanningIndex, Oid partitionOid);
extern int32 SpanningLookupPartseqByRelid(Relation spanningIndex, Oid partitionOid);
extern Oid	SpanningResolvePartseqRelid(Relation spanningIndex, int32 partseq);
extern Oid	SpanningResolvePartseqRelidByOid(Oid spanningIndexOid, int32 partseq);
extern void RemoveSpanningPartitionMapForIndex(Oid spanningIndexOid);
extern void RemoveSpanningPartitionMapForPartition(Oid partitionOid);

#endif							/* PG_INDEX_PARTITION_H */
