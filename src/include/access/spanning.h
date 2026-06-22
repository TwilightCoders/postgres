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

#include "access/itup.h"
#include "nodes/execnodes.h"
#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/relcache.h"

extern void ExecInsertSpanningIndexTuples(TupleTableSlot *slot,
										  ItemPointer tupleid,
										  Relation partition,
										  EState *estate);
extern void ProgresqlReleasePartitionCache(EState *estate);

/*
 * spanning_lock.c — the cross-partition "value lock".
 *
 * Stock btree enforces uniqueness under concurrency by holding the write lock
 * on the leaf page the key belongs to: any other inserter of the same key must
 * take the same page lock, so the check-and-insert is serialized.  A spanning
 * index breaks that invariant -- the same USER key inserted into two different
 * partitions forms two different full keys (userkey, partseq_a) and
 * (userkey, partseq_b) that can sit on different btree pages -- so the page
 * lock no longer serializes them.  SpanningLockUserKey restores the invariant
 * by taking a short-duration heavyweight lock keyed on (index, hash(userkey))
 * around the check-and-insert; SpanningUnlockUserKey releases it once the new
 * entry is physically in the tree (whereupon it serves as the SnapshotDirty
 * conflict marker for the next inserter, exactly as in stock btree).
 */
extern void SpanningLockUserKey(Relation indexRel, IndexTuple itup,
								LOCKTAG *locktag);
extern void SpanningUnlockUserKey(const LOCKTAG *locktag);

/* spanning_ddl.c — partition-lifecycle maintenance (ATTACH/DETACH/DROP/TRUNCATE) */
extern void progresql_clean_spanning_indexes_for_partition(Relation partRel,
														   bool drop_map);
extern void progresql_backfill_spanning_indexes_for_attached_partition(Relation attachrel);
extern void progresql_rebuild_spanning_for_rewritten_partition(Oid relid);
extern void BuildSpanningIndexFromPartitions(Relation rel, Oid indexRelationId);
/* remap a spanning index's root-relative user-key attnums to a leaf, by name */
extern void spanning_remap_keyatts_to_leaf(IndexInfo *idxInfo,
										   const AttrNumber *rootKeyAtts,
										   Oid rootOid, Oid leafOid);

/* spanning_relcache.c — leaf->root resolution + HOT-blocking attrs (E7) */
extern List *progresql_spanning_ancestors(Oid relid);
extern void progresql_add_spanning_hotblocking_attrs(Relation relation,
													 Bitmapset **hotblockingattrs);
extern bool progresql_leaf_has_spanning_ancestor(Relation relation);
extern bool RelationHasSpanningAncestor(Relation relation);
/* true if relation has a spanning (GLOBAL) index of its own (FK referenced-side) */
extern bool RelationHasSpanningIndex(Relation relation);

#endif							/* SPANNING_H */
