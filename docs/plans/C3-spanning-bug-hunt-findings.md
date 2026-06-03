# C3 — Spanning-index correctness findings (adversarial hunt + verification)

> Produced 2026-06-02 during the C3 trust-testing pass.  An adversarial,
> read-only multi-agent hunt (8 failure-mode lenses; 3 emitted structured
> output, 5 did analysis without structured results) generated bug hypotheses;
> each item below was then **verified by a live repro** against a scratch
> cluster (build-hazard rule) before being recorded as real.  Repros use the
> canonical shape:
>
> ```sql
> CREATE TABLE t (id bigint NOT NULL, ts timestamptz NOT NULL,
>     PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
> CREATE TABLE t_a PARTITION OF t FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
> CREATE TABLE t_b PARTITION OF t FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
> ```

## Status summary

| # | Title | Severity | Status |
|---|-------|----------|--------|
| A | DETACH/DROP/TRUNCATE + ROLLBACK left LP_DEAD → unenforced uniqueness | corruption | **FIXED** `b0caff8cf2` |
| 1 | Colliding INSERT vs unresolvable partseq probed storage-less root → SIGSEGV | crash | **FIXED** `d6bc169300` |
| 2 | partseq reused when the highest-numbered partition is detached/dropped | wrong-result | **FIXED** `eb8b088684` (pg_spanning_seq counter) |
| 3 | Reused partseq + un-retired stale entry → wrong-heap probe | corruption | **CLOSED by #2** (no reuse → stale entry unresolvable → skipped) |
| 4 | Orphaned `pg_spanning_drainq` row + reused partseq → drain reaps live entries | corruption | **FIXED** `247bd65b13` (reap on DETACH/DROP) + closed by #2 |
| 7 | Same-txn DETACH-highest + ATTACH-new reuses partseq mid-txn | spurious-error | **CLOSED by #2** |
| 8 | TRUNCATE LP_DEAD not crash-durable + map kept → resurrected entry aliases reused TID | wrong-result | **FIXED** `893be61b00` (TRUNCATE re-maps to fresh partseq) |
| Z | amcheck `bt_index_check(idx, heapallindexed=>true)` crashes on a spanning index | crash | **FIXED** `021aa3734e` (reject heapallindexed, no crash) |
| R | REINDEX [INDEX] CONCURRENTLY on a spanning index crashed / built an empty index (silent loss of uniqueness) | crash/corruption | **FIXED** `9f30fd8d1d` (rejected; CONCURRENTLY unsupported for spanning) |
| C1 | Cross-backend INSERT racing DETACH/DROP/TRUNCATE | (probe) | **VERIFIED SAFE** — serialized by the partition-parent AccessExclusiveLock vs the INSERT's AccessShareLock on the root; probe `31a5b6a9aa` hardens the resolve to `try_table_open` |

> Update (2026-06-03): the full gate below was closed in a focused C3 pass.  Every
> corruption/crash finding is fixed (commits `eb8b088684`, `a1f7dd9789`,
> `247bd65b13`, `021aa3734e`, `9f30fd8d1d`, `31a5b6a9aa`, `893be61b00`), each fix
> live-verified and regression/TAP-tested, and an adversarial verification pass
> (4 read-only lenses) confirmed the no-reuse invariant is unbreakable and the
> concurrency path is safe.  Remaining items are coverage/feature, not
> open-correctness: a DETACH … CONCURRENTLY isolation spec (the path is verified
> correct by lock analysis but lacks an empirical spec), the broader unverified
> probes below, and FK-to-spanning (a separate feature, not a bug).

Two distinct **root causes** generate most of this:
1. **Probing the storage-less partitioned root.** A spanning index's `indrelid`
   is the partitioned root, which has `rd_tableam == NULL`
   (relcache.c:1232-1238).  Any path that calls `table_index_fetch_*` /
   `heap`-scan on the root dereferences NULL and crashes.  Reached by #1 (fixed)
   and by Z (amcheck, open).
2. **partseq reuse (#2).**  `SpanningGetOrAllocPartseq` = `MAX(indpartseq over
   surviving rows) + 1` with **no persisted high-water mark**
   (pg_index_partition.c).  Detaching/dropping the highest partseq frees the
   number; the next joiner reuses it.  This is the root enabler of #3/#4/#7 and
   contradicts the catalog's own "never reused" comment.

---

## FIXED this session

### A — abort-unsafe partition cleanup (commit `b0caff8cf2`)
DETACH/DROP/TRUNCATE retired a partition's spanning entries by marking them
`LP_DEAD` **inline**.  `LP_DEAD` is a non-transactional page hint, so it survived
`ROLLBACK` while the catalog/inheritance changes rolled back — leaving a
still-attached partition's keys invisible to `_bt_check_unique` (silent
cross-partition duplicate).  Fix: defer the marking to `XACT_EVENT_PRE_COMMIT`;
aborting (sub)transactions discard the queue.  Regress: `progresql_ddl` §6.
Isolation: `spanning-detach`.

### 1 — crash on colliding INSERT vs unresolvable partseq (commit `d6bc169300`)
`_bt_check_unique`, when an entry's partseq did not resolve, left `checkRel` =
the storage-less root and probed it → SIGSEGV.  Easily reached after fix A
deferred retirement: in one txn, `DETACH`/`DROP` deletes the map row inline but
leaves entries live, so a colliding INSERT finds a live-but-unresolvable entry.
Fix: when partseq is unresolvable (detached/dropped, or NULL), treat the entry
as not-a-conflict and **skip** the probe (do not mark LP_DEAD — an uncommitted
same-txn DETACH could still roll back).  Regress: `progresql_ddl` §7.

---

## VERIFIED, deferred (need design + review — do NOT rush)

### 2 — partseq reuse on highest-partition detach  **[live-verified]**
```sql
-- psq_a=partseq 1, psq_b=partseq 2 (highest)
ALTER TABLE psq DETACH PARTITION psq_b;     -- frees partseq 2
ALTER TABLE psq ATTACH PARTITION psq_c ...; -- OBSERVED: psq_c gets partseq 2 (reused!)
```
Observed: `psq_c` got `indpartseq = 2` (should be 3).  `spanning_max_partseq()`
scans only surviving rows; `RemoveSpanningPartitionMapForPartition` deletes the
departing row; there is no persisted counter.

**Why deferred:** a correct no-reuse fix needs a persisted high-water mark.
`pg_index_partition` has unique indexes on both `(indpartidxid, indpartseq)` and
`(indpartidxid, indpartrelid)`, so the cheap tombstone tricks don't work
(can't zero `indpartrelid`; can't leave a dangling one).  Options — (a) add a
column/per-index counter; (b) a tombstone row scheme — all change the catalog
**and** the pg_dump (`dumpSpanningIndexPartitionMap`) + pg_upgrade map-transfer
path fixed last session.  That is design-sensitive, multi-file, and must be done
with review, not overnight.

**Note:** post-fix-#1, reuse is benign in the *common* committed path (the old
partition's entries are retired before the partseq is reused, so no live stale
entry aliases).  Harm appears only combined with a retirement gap (#3/#7) or DHR
(#4).  The existing `progresql_partseq.sql` test detaches a *non-highest*
partition, so it never exercises reuse — strengthen it when #2 is fixed.

### 3 — reused partseq + surviving stale entry → wrong-heap probe  *(code-verified; gated on #2 + PREPARE/crash)*
`PREPARE TRANSACTION` discards the retire queue and a crash can lose the LP_DEAD
hint, leaving a departed partition's entries live while its map row is
committed-deleted.  If a new partition then reuses that partseq (#2), the stale
entry resolves to the **new** partition and its old TID is probed against the new
heap → spurious duplicate, or (TID overlap) a missed conflict.  **Fixing #2
closes this** (no reuse → stale entry stays unresolvable → skipped by fix #1).

### 4 — orphaned drainq row + reused partseq → drain reaps live entries  *(code-verified; gated on #2 + DHR-on)*
Confirmed: only `RemoveSpanningDrainqForIndex` exists (called at index drop);
there is **no `RemoveSpanningDrainqForPartition`**, and DETACH/DROP of a
partition does not clean its `pg_spanning_drainq` rows.  With partseq reuse (#2)
the orphaned `(idxid, partseq)` resolves to the new partition, bypassing the
vacuumlazy "partition gone" safety, and the drain retires the new partition's
live entries.  **Two fixes:** (a) add `RemoveSpanningDrainqForPartition` on
DETACH/DROP (surgical, safe, good hygiene even alone); (b) fix #2.  DHR is
off-by-default so this is currently latent.

### 7 — same-txn DETACH-highest + ATTACH-new reuses partseq mid-txn  *(code-verified; gated on #2)*
Backfill calls `SpanningGetOrAllocPartseq` after the map delete's CCI, reuses the
just-freed partseq, and `index_insert`s with `UNIQUE_CHECK_YES` while the old
entry is still live → wrong-heap probe / spurious ATTACH failure.  Closed by #2.

### 8 — TRUNCATE retirement not crash-durable + map kept  *(hypothesis; needs crash harness)*
TRUNCATE passes `drop_map=false`, so the map row persists; retirement is only the
deferred LP_DEAD hint (no WAL unless `wal_log_hints`/checksums).  A crash that
loses the hint resurrects the stale entry, whose partseq still resolves (map
kept) to the truncated-then-refilled partition; the new relfilenode restarts TIDs
at (0,1), so the stale entry can alias a reused TID → spurious unique violation
that VACUUM can't clear (or a key left un-enforced).  Fix direction: route
TRUNCATE retirement through a WAL-logged path (the `_bt_delitems_vacuum` drain
used by VACUUM) instead of a bare hint.  Not yet repro'd (needs a kill-9 TAP).

### Z — amcheck crashes on a spanning index  **[live-verified]**
`SELECT bt_index_check('t_pkey', true)` (heapallindexed) **crashed the backend**:
amcheck opens the index's heap (the partitioned root) and scans it → storage-less
root → NULL `rd_tableam` deref.  `bt_index_check('t_pkey')` (structural,
heapallindexed=false) works fine.  Minimal fix: in amcheck, skip/clean-error the
heapallindexed scan when the heap relation has no storage.  Full fix (the C3
"verifiability" item): teach amcheck the partseq→partition indirection so it
scans the partitions and validates spanning entries.  Contrib change — deferred.

---

## UNVERIFIED — follow-up probes (from the hunt's completeness critic + skipped lenses)
The lenses that did not emit structured findings (abort-safety beyond A,
ddl-vs-ddl, vacuum-drain, nulls/multicol/types, partition-key-update) and the
critic's "missing" list still warrant probing:
1. **Concurrent** INSERT racing a DETACH/DROP across two backends (catalog
   invalidation of the deleted map row may not reach a mid-INSERT backend) —
   the crash (#1) and missed-conflict windows under concurrency are unmeasured.
2. Per-statement partition cache (spanning_exec.c) vs a DETACH/ATTACH that
   changes the mapping mid-statement (BEFORE trigger / CTE DML) → stale partseq.
3. **Multi-level (sub-partitioned)** trees — backfill returns early for
   `RELKIND_PARTITIONED_TABLE`; partseq alloc/cleanup across grandchildren and
   the `get_partition_ancestors` walk untested.
4. NULL partseq (`seqnull` branch) — what writes a NULL trailing key, and does
   any legacy/dual-tracked entry reach `_bt_check_unique` with `seqnull=true`?
   (Fix #1 now skips it safely, but the *origin* of such entries is unaudited.)
5. REINDEX CONCURRENTLY / index-drop ordering of map vs drainq cleanup.
6. Multiple spanning indexes on one root (PK + a separate UNIQUE GLOBAL) —
   per-index partseq namespaces and cross-effects on MAX computation.
7. partition-key UPDATE that moves a row between partitions (cross-leaf
   delete+insert) vs spanning uniqueness; MERGE / INSERT ... ON CONFLICT.

## Effect on the experimental-flag gate
**Update (2026-06-03): the correctness gate is closed.**  Every finding it named
is resolved: #2 (+ its chain #3/#7) via the pg_spanning_seq no-reuse counter, #4
via per-partition drainq reaping, #8 via TRUNCATE partseq re-mapping, Z via the
amcheck guard, plus R (REINDEX CONCURRENTLY) found and fixed in the same pass; the
concurrency probe (1) was discharged by a rigorous lock analysis (verified safe)
and hardened defensively.  All fixes are live-verified and covered by regress +
isolation + crash/dump/upgrade TAP + amcheck suites (all green), and an
adversarial verification pass could not break the no-reuse invariant.

What still stands between here and dropping the README flag is **confidence
coverage, not known-open correctness**:
- A `DETACH … CONCURRENTLY` isolation spec (the path is verified correct by lock
  analysis; an empirical interleaving spec would lock it down).
- The broader unverified probes below (multi-level trees, partition-key UPDATE /
  MERGE / INSERT…ON CONFLICT, multiple spanning indexes per root, etc.).
- A soak/scale run on a real workload.

FK-to-spanning (below) is a **feature gap**, independent of the flag.

---

## FEATURE GAP — FK cannot reference a spanning (GLOBAL) PK  *(live-verified 2026-06-02)*
```sql
CREATE TABLE ref (rid bigint PRIMARY KEY, evt_id bigint REFERENCES evt(id));
-- ERROR: there is no unique constraint matching given keys for referenced table "evt"
```
PG's FK machinery (`transformFkeyCheckAttrs`) requires an index whose key columns
exactly match the FK columns.  A spanning index's key is `(user_cols..., partseq)`
(`indnkeyatts` includes the trailing partseq), so it is not recognized as a match
for `REFERENCES evt(id)`.  **A foreign key cannot point at a spanning PK today.**

This is a FEATURE GAP, not a bug, and is independent of #2/#4/#8/Z.  It is the
blocker for any "universal `data` table that everything FK-references" design
(e.g. a downstream consumer's perfectly-referential model).  Supporting it needs: (a)
`transformFkeyCheckAttrs` to accept a spanning index by its user-facing unique
prefix (`indnuniqatts`) rather than full `indnkeyatts`; and (b) the RI
existence-check query to resolve through the spanning index on the storage-less
root (same storage-less-root hazard as findings #1/Z).  The global-unique-id
namespace itself works (the spanning PK enforces it); only enforced FK references
to it are missing.
