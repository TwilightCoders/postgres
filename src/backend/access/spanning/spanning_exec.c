/*-------------------------------------------------------------------------
 *
 * spanning_exec.c
 *	  ProgreSQL: executor-side maintenance of cross-partition ("spanning")
 *	  unique indexes.
 *
 * Holds the per-statement partition cache and ExecInsertSpanningIndexTuples,
 * the hook nodeModifyTable calls after a leaf insert / in-partition update to
 * maintain the root's spanning index.  Kept out of execIndexing.c so the
 * incision into the core executor stays a thin call plus one EState field.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/spanning/spanning_exec.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/partition.h"
#include "catalog/pg_index_partition.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/multirangetypes.h"
#include "utils/rangetypes.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "access/spanning.h"

/*
 * ProgreSQL spanning-index per-statement cache.
 *
 * For every leaf partition that this statement inserts/updates rows in, we
 * cache: the open ancestor partitioned relations (AccessShareLock), the open
 * spanning indexes on those ancestors (RowExclusiveLock), and a precomputed
 * IndexInfo per spanning index.  The cache is built lazily on the first
 * tuple landing in a given partition and torn down by FreeExecutorState.
 *
 * Without this cache, every inserted tuple paid for: a pg_inherits scan
 * (get_partition_ancestors), a relcache lookup + lock acquire on each
 * ancestor, RelationGetIndexList, and an index_open + BuildIndexInfo +
 * index_close for *every* index on every ancestor (not just spanning ones).
 * For a deeply partitioned table with several non-spanning indexes per root,
 * that is 100s of relcache/lock-manager hits per row.
 */
typedef struct ProgresqlSpanningEntry
{
	Relation	parentRel;		/* root the spanning index lives on
								 * (AccessShareLock held) */
	Relation	indexRel;		/* the spanning index (RowExclusiveLock) */
	IndexInfo  *indexInfo;
	int			discrimKeyPos;	/* 0-based position of the trailing discriminator
								 * column in indexInfo->ii_IndexAttrNumbers */
	int32		partseq;		/* this partition's index-local partseq from
								 * pg_index_partition; -1 if unmapped.  Stored as
								 * the trailing discriminator key on write. */
} ProgresqlSpanningEntry;

typedef struct ProgresqlPartitionCacheEntry
{
	Oid			partOid;		/* hash key */
	int			nEntries;
	ProgresqlSpanningEntry *entries;
} ProgresqlPartitionCacheEntry;

static ProgresqlPartitionCacheEntry *
progresql_build_partition_cache_entry(EState *estate, Relation partition)
{
	ProgresqlPartitionCacheEntry *pe;
	List	   *ancestors;
	ListCell   *lc;
	MemoryContext oldcxt;
	List	   *entries = NIL;
	bool		found;

	if (estate->es_progresql_partition_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(ProgresqlPartitionCacheEntry);
		ctl.hcxt = estate->es_query_cxt;
		estate->es_progresql_partition_cache =
			hash_create("ProgreSQL partition spanning-index cache",
						32, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	pe = (ProgresqlPartitionCacheEntry *)
		hash_search(estate->es_progresql_partition_cache,
					&RelationGetRelid(partition),
					HASH_ENTER, &found);
	if (found)
		return pe;

	pe->nEntries = 0;
	pe->entries = NULL;

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Resolve this leaf's spanning roots by walking the full pg_inherits tree
	 * upward (declarative partitions and/or table inheritance, any depth), not
	 * just declarative partition ancestry.
	 */
	ancestors = progresql_spanning_ancestors(RelationGetRelid(partition));
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
			Oid			indexOid = lfirst_oid(il);
			Relation	indexRel;
			ProgresqlSpanningEntry *se;

			indexRel = index_open(indexOid, RowExclusiveLock);
			if (!RelationIsSpanning(indexRel))
			{
				index_close(indexRel, RowExclusiveLock);
				continue;
			}

			se = (ProgresqlSpanningEntry *)
				palloc(sizeof(ProgresqlSpanningEntry));
			se->indexRel = indexRel;
			se->indexInfo = BuildIndexInfo(indexRel);
			se->discrimKeyPos = se->indexInfo->ii_NumIndexKeyAttrs - 1;

			/*
			 * The index's key attnums are relative to the root the index lives
			 * on; remap them to this leaf by column name so FormIndexDatum reads
			 * the right columns when the leaf orders them differently (a
			 * reordered inheritance child, or a partition with a divergent
			 * layout).  Done once per cached (leaf, index) entry.
			 */
			{
				AttrNumber	rootKeyAtts[INDEX_MAX_KEYS];

				memcpy(rootKeyAtts, se->indexInfo->ii_IndexAttrNumbers,
					   se->indexInfo->ii_NumIndexKeyAttrs * sizeof(AttrNumber));
				spanning_remap_keyatts_to_leaf(se->indexInfo, rootKeyAtts,
											   parentOid,
											   RelationGetRelid(partition));
			}
			/*
			 * Stamp the entry with this partition's index-local partseq; the
			 * write path below stores it as the trailing discriminator key.
			 */
			se->partseq = SpanningLookupPartseqByRelid(indexRel,
													  RelationGetRelid(partition));
			/*
			 * Take an independent table_open for each entry so cleanup can
			 * pair each open with a close.  Lock-manager fast path makes
			 * repeat opens of the same OID effectively free.
			 */
			se->parentRel = table_open(parentOid, AccessShareLock);
			entries = lappend(entries, se);
		}

		list_free(indexoidlist);
		table_close(parentRel, AccessShareLock);
	}
	list_free(ancestors);

	pe->nEntries = list_length(entries);
	if (pe->nEntries > 0)
	{
		int			i = 0;
		ListCell   *lc2;

		pe->entries = (ProgresqlSpanningEntry *)
			palloc(sizeof(ProgresqlSpanningEntry) * pe->nEntries);
		foreach(lc2, entries)
		{
			ProgresqlSpanningEntry *src = (ProgresqlSpanningEntry *) lfirst(lc2);

			pe->entries[i++] = *src;
			pfree(src);
		}
		list_free(entries);
	}

	MemoryContextSwitchTo(oldcxt);
	return pe;
}

void
ProgresqlReleasePartitionCache(EState *estate)
{
	HASH_SEQ_STATUS scan;
	ProgresqlPartitionCacheEntry *pe;

	if (estate->es_progresql_partition_cache == NULL)
		return;

	hash_seq_init(&scan, estate->es_progresql_partition_cache);
	while ((pe = hash_seq_search(&scan)) != NULL)
	{
		for (int i = 0; i < pe->nEntries; i++)
		{
			ProgresqlSpanningEntry *se = &pe->entries[i];

			if (se->indexRel)
				index_close(se->indexRel, RowExclusiveLock);
			if (se->parentRel)
				table_close(se->parentRel, AccessShareLock);
		}
	}

	hash_destroy(estate->es_progresql_partition_cache);
}

/*
 * ExecInsertSpanningIndexTuples
 *
 * Insert an entry for the given tuple into every ProgreSQL spanning index on
 * the partitioned root above this leaf partition.
 *
 * Callers invoke this exactly where a leaf-local index would receive a new
 * entry for a new physical tuple version: on INSERT, and on a COLD (non-HOT)
 * UPDATE.  A HOT UPDATE must NOT call this -- its existing spanning entry stays
 * valid because heap_hot_search_buffer follows the on-page HOT chain from the
 * original root TID to the live tuple, and inserting a duplicate-key entry would
 * self-conflict.  (Spanning-key changes are forced cold via HOT-blocking on
 * leaves, so they too arrive here as cold updates.)
 *
 * slot       - the just-inserted/updated tuple slot (in the partition)
 * tupleid    - the physical TID of the new tuple in the partition
 * partition  - the leaf partition relation
 * estate     - executor estate
 */
void
ExecInsertSpanningIndexTuples(TupleTableSlot *slot,
							   ItemPointer tupleid,
							   Relation partition,
							   EState *estate)
{
	ProgresqlPartitionCacheEntry *pe;
	Oid			partOid = RelationGetRelid(partition);
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * Fast path for the do-no-harm case: a leaf with no spanning index on any
	 * ancestor has nothing to maintain.  RelationHasSpanningAncestor is a cached
	 * relcache predicate (reset on relcache rebuild, which a spanning-index
	 * add/drop triggers -- #42), so this is a single field read after the first
	 * call per backend.  It avoids building the per-statement partition cache
	 * entry -- whose miss path does get_partition_ancestors + table_open(root) +
	 * a pg_index walk -- for every inserted row of an ordinary partitioned
	 * table (each single-row INSERT statement is a fresh estate -> cache miss).
	 */
	if (!RelationHasSpanningAncestor(partition))
		return;

	pe = progresql_build_partition_cache_entry(estate, partition);
	if (pe->nEntries == 0)
		return;

	for (int i = 0; i < pe->nEntries; i++)
	{
		ProgresqlSpanningEntry *se = &pe->entries[i];

		FormIndexDatum(se->indexInfo, slot, estate, values, isnull);

		/*
		 * Override the trailing discriminator column with this partition's
		 * index-local partseq (resolved when the cache entry was built).  A
		 * negative value means the partition was never mapped, which should be
		 * impossible once the spanning index is built/backfilled.
		 */
		if (se->partseq < 0)
			elog(ERROR, "spanning index \"%s\" has no partseq for partition %u",
				 RelationGetRelationName(se->indexRel), partOid);
		values[se->discrimKeyPos] = Int32GetDatum(se->partseq);
		isnull[se->discrimKeyPos] = false;

		index_insert(se->indexRel,
					 values,
					 isnull,
					 tupleid,
					 se->parentRel,
					 UNIQUE_CHECK_YES,
					 false,
					 se->indexInfo);
	}
}
