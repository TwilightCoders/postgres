/*-------------------------------------------------------------------------
 *
 * spanning_lock.c
 *	  ProgreSQL: the cross-partition "value lock" for spanning unique indexes.
 *
 * Stock btree enforces uniqueness under concurrency by holding the write lock
 * on the single leaf page a key belongs to: every would-be inserter of that
 * key must take the same page lock, so the check-and-insert is serialized and
 * the first inserter's (possibly still in-progress) entry becomes the
 * SnapshotDirty conflict marker that makes the next inserter wait
 * (see the invariant documented in _bt_doinsert, nbtinsert.c).
 *
 * A spanning index breaks that invariant.  Its entries sort by
 * (user_cols..., partseq), so the SAME user key inserted into two DIFFERENT
 * leaf partitions forms two DIFFERENT full keys -- (userkey, partseq_a) and
 * (userkey, partseq_b) -- which can live on different btree pages.  Two such
 * inserters then take different page locks and never serialize, so both can
 * pass the cross-partition uniqueness probe and both insert: a silent
 * duplicate.
 *
 * SpanningLockUserKey restores the invariant one level up: a short-duration
 * heavyweight lock keyed on (database, index, hash(user_cols)), taken before
 * the btree descent and released once the new entry is physically in the tree.
 * Because it is keyed on the user columns only (NOT partseq), all inserters of
 * the same user key contend on it regardless of which partition/page they
 * target.  It is the cross-partition analogue of the leaf-page write lock, and
 * is modelled on SpeculativeInsertionLockAcquire (lmgr.c) -- a heavyweight lock
 * held around a physical operation rather than to end of transaction.
 *
 * LOCK ORDERING INVARIANT: the spanning-key value lock is ALWAYS taken before
 * any btree buffer lock.  Callers must acquire it before _bt_search; never the
 * other way around.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/spanning/spanning_lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/itup.h"
#include "access/tupdesc.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "utils/rel.h"
#include "utils/typcache.h"

#include "access/spanning.h"

/*
 * Hash the leading nuniqs user-key columns of an index tuple (excluding the
 * trailing partseq) into a single uint32, using each column type's hash opclass
 * support proc.  Equal user keys MUST hash equal -- that is what makes two
 * inserts of the same key into different partitions contend on the same lock --
 * which the type's hash proc guarantees (it is the same machinery hash indexes
 * and hash partitioning use, and is collation-aware for collatable types).
 *
 * A key-column type with no hash opclass falls back to a constant: all keys for
 * the index then serialize on a single lock (correct -- it can only
 * over-serialize, never miss a conflict, since the real check is still the
 * full-key comparison plus the SnapshotDirty heap probe).  Every type usable in
 * a UNIQUE/PK key has a hash opclass, so this path is effectively unreachable.
 */
static uint32
spanning_userkey_hash(Relation indexRel, IndexTuple itup)
{
	TupleDesc	tupdesc = RelationGetDescr(indexRel);
	int			nuniqs = IndexRelationGetNumberOfUniqueAttributes(indexRel);
	uint32		hash = 0;

	for (int i = 0; i < nuniqs; i++)
	{
		bool		isnull;
		Datum		val = index_getattr(itup, i + 1, tupdesc, &isnull);
		uint32		colhash;

		if (isnull)
			colhash = 0;
		else
		{
			Oid			typid = TupleDescAttr(tupdesc, i)->atttypid;
			Oid			collid = indexRel->rd_indcollation[i];
			TypeCacheEntry *typentry;

			typentry = lookup_type_cache(typid, TYPECACHE_HASH_PROC_FINFO);
			if (!OidIsValid(typentry->hash_proc_finfo.fn_oid))
				colhash = 0;	/* no hash opclass: serialize all keys */
			else
				colhash = DatumGetUInt32(FunctionCall1Coll(&typentry->hash_proc_finfo,
														   collid, val));
		}

		/* order-sensitive combine: PostgreSQL's canonical 32-bit mixer */
		hash = hash_combine(hash, colhash);
	}

	return hash;
}

/*
 * Acquire the spanning value lock for the user key carried by itup in the
 * spanning index indexRel, filling *locktag for the matching release.  The lock
 * is a transaction-scoped exclusive heavyweight lock; ExclusiveLock
 * self-conflicts, so at most one inserter of a given user key is in the
 * check-and-insert critical section at a time.  It is short-duration: the
 * caller releases it with SpanningUnlockUserKey the moment the new entry is
 * physically in the tree, whereupon that entry serves as the conflict marker
 * for the next inserter.  (On error the aborting transaction releases it.)
 */
void
SpanningLockUserKey(Relation indexRel, IndexTuple itup, LOCKTAG *locktag)
{
	uint32		keyhash = spanning_userkey_hash(indexRel, itup);

	SET_LOCKTAG_SPANNING_KEY(*locktag, MyDatabaseId,
							 RelationGetRelid(indexRel), keyhash);

	(void) LockAcquire(locktag, ExclusiveLock, false, false);
}

/*
 * Release a lock taken by SpanningLockUserKey.
 */
void
SpanningUnlockUserKey(const LOCKTAG *locktag)
{
	LockRelease(locktag, ExclusiveLock, false);
}
