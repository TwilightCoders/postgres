/*-------------------------------------------------------------------------
 *
 * spanning.h
 *	  ProgreSQL: public entry points for cross-partition ("spanning")
 *	  unique-index maintenance.  Implementation in src/backend/access/spanning/.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spanning.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPANNING_H
#define SPANNING_H

#include "nodes/execnodes.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

extern void ExecInsertSpanningIndexTuples(TupleTableSlot *slot,
										  ItemPointer tupleid,
										  Relation partition,
										  EState *estate,
										  ResultRelInfo *resultRelInfo);
extern void ProgresqlReleasePartitionCache(EState *estate);

/* spanning_ddl.c — partition-lifecycle maintenance (ATTACH/DETACH/DROP/TRUNCATE) */
extern void progresql_clean_spanning_indexes_for_partition(Relation partRel,
														   bool drop_map);
extern void progresql_backfill_spanning_indexes_for_attached_partition(Relation attachrel);
extern void BuildSpanningIndexFromPartitions(Relation rel, Oid indexRelationId);

#endif							/* SPANNING_H */
