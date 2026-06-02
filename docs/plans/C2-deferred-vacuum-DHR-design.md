# C2 / E5 — Deferred spanning-index VACUUM (DHR): the O(N²)→O(N) perf fix

> Implementation spec for the USER's #1 priority. Output of a 5-proposal design
> panel + 2 adversarial reviews (correctness, perf). Winner: **DHR**
> (Defer-Heap-Reap), correctness 10/10, total 36/40. Distilled 2026-06-01.
> The full winning architecture is appended verbatim at the bottom.

## The problem (recap)
Each leaf-partition VACUUM calls `bt_spanning_bulkdelete`, a FULL scan of the
root's spanning index, to retire that partition's dead entries. N leaves ⇒ N
full scans of an N-sized index ⇒ **O(N²) per sweep** (measured: 198→891 buffer
hits for N=4→64). The trailing-partseq key order is REQUIRED (a leading partseq
would make every uniqueness check O(N) on the hot write path), so per-partition
entries are scattered and a targeted delete needs a full scan. The only fix is
deferral/coalescing — not re-keying.

## Why DHR is correct (the structural invariant)
A heap line pointer held at **LP_DEAD cannot be reused** — `PageAddItemExtended`/
`PageGetHeapFreeSpace` only recycle `LP_UNUSED` slots (verified in-tree). So if
the leaf VACUUM enqueues a partition's dead (partseq,TID) but leaves those heap
slots **LP_DEAD (un-reaped)**, the (partseq,block,offset) can never be reused
before its spanning entry is gone. A single coalesced **drain** per root scans
the spanning index ONCE, retires all queued partseqs' entries (WAL-logged
`_bt_delitems_vacuum`), and only THEN may the now-safe LP_DEAD slots be reaped to
LP_UNUSED. The drain re-derives the kill set from CURRENT LP_DEAD heap state, so
it never trusts a stale TID — no heap re-probe, no key-recompute. This builds
directly on the `MARK_UNUSED_NOW` suppression already in `vacuumlazy.c` (the
`has_spanning_index` two-pass forcing).

## CRITICAL: the prerequisite that was just satisfied (E7)
The design panel's correctness adversary rated DHR **BROKEN** because of one
case: a HOT key-change UPDATE created duplicate-(partseq,TID) spanning entries
WITHOUT any slot reuse (the leaf thought the spanning-key column was unindexed),
so the drain couldn't tell the stale entry from the live one. **That is now
FIXED (commit `c7d40bef36`, E7): spanning keys are HOT-blocking on leaves, so a
spanning-key UPDATE is non-HOT, the old tuple dies normally, and there are NO
duplicate-(partseq,TID) entries.** With E7 in place, DHR's LP_DEAD invariant is
sound: every dead spanning entry corresponds to a genuinely dead heap tuple.
**Do not start DHR without E7 — it is committed, so you are clear.**

## Remaining adversary concerns to ENGINEER IN (post-E7)
These were the non-HOT residual holes; the implementation MUST address each:
1. **Crash atomicity of enqueue.** The `pg_spanning_drainq` row must be durable
   with respect to the LP_DEAD marks it represents. Heap prune WAL and a
   `CatalogTupleInsert` are separate WAL records. Make the reap-gate POSITIVE:
   never reap an LP_DEAD slot to LP_UNUSED unless we can prove its spanning entry
   was drained. "Absence of a drainq row" is ambiguous (never-enqueued vs
   crashed-before-enqueue vs already-drained). Safer: the leaf vacuum that
   created the LP_DEAD items also creates/*upserts* the drainq row in the same
   transaction, and the reap step is a SEPARATE later pass that only runs after a
   committed drain. If a crash loses the drainq row but left LP_DEAD slots, a
   subsequent leaf vacuum re-discovers the LP_DEAD items and re-enqueues
   (idempotent by (idxid,partseq)) — so make re-enqueue idempotent and never
   reap without a drain having committed.
2. **Drain commit ordering (index-before-heap across commit).** The drain must
   COMMIT the `_bt_delitems_vacuum` removals BEFORE any LP_DEAD→LP_UNUSED reap of
   the corresponding slots. A concurrent `_bt_check_unique` probe must never see
   a not-yet-deleted entry whose heap slot has already been reaped/reused.
3. **Trigger durability.** Partitioned roots/indexes are NOT autovacuumed
   (`autovacuum.c` collects only r/m). Use the EXISTING autovacuum work-item
   facility (`AutoVacuumRequestWork`, add `AVW_SpanningIndexDrain` beside
   `AVW_BRINSummarizeRange`; `NUM_WORKITEMS=256`; `perform_work_item` runs it in
   a fresh-snapshot txn) as a best-effort NUDGE, but the durable
   `pg_spanning_drainq` is the source of truth. ADD a launcher-side periodic
   sweep so a never-draining queue (autovacuum off / low churn) is bounded —
   this is the real durability, not the shmem nudge. Plus manual escapes:
   `pg_drain_spanning_index(regclass)` and an end-of-command drain after the leaf
   loop in `vacuum()` for `VACUUM <root>` (outside the per-leaf transactions).
4. **Reap latency / bloat.** DEFAULT to drain-driven reap (the drain reaps the
   now-safe LP_DEAD slots in each dirty partition within the work-item txn), NOT
   "reap on next leaf vacuum" (which triples reap latency). Honestly characterize
   the held-LP_DEAD FSM/extension cost (it is NOT "zero write-path overhead":
   `PageGetHeapFreeSpace` ignores LP_DEAD slots, so held slots delay free-space
   reuse → some heap extension until drain). Make the drain interval the knob;
   default tight.
5. **Drain complexity is O(tree + dirty-dead-pages)**, amortized O(N) per sweep —
   state it honestly; the index scan is O(tree), plus a per-dirty-partition
   LP_DEAD rescan/reap. Call `vacuum_delay_point` in both loops.
6. **All THREE call sites** in `lazy_vacuum` must be converted: the
   `nindexes==0` spanning branch, the bypass branch, and the
   all-indexes-succeeded branch. Today they each either eager-scan or skip;
   under DHR they ENQUEUE (and the heap-reap-suppression must be the single
   gated reap site).
7. **Failsafe**: skip drain/enqueue under wraparound failsafe (accept the
   transient leak), mirroring the local-index failsafe (antagonist M2). Stop
   incrementing user-visible `num_index_scans` for the spanning path (M3).
8. **Leaf-only delete primitive (antagonist B1/B2).** The drain's index-entry
   removal should NOT reuse the full `btvacuumscan` page-deletion machinery on a
   storage-less-root index that never gets `amvacuumcleanup`. Use a leaf-page
   walk that issues `_bt_delitems_vacuum` and does NOT call `_bt_pagedel` /
   `BTPageIsRecyclable` (or run a proper coalesced index vacuum with the leaf
   `heaprel` already applied in `de985525d8`, and call `btvacuumcleanup` at the
   end so the metapage bookkeeping is finalized).

## AS-BUILT deviations from the appendix below (read this)
The appendix is the original design-panel output. The implementation deviates in
two places, both toward upstream-legibility (see C2-campaign-status.md):
- **`sdq_enqueue_xid` is `TransactionId`/`xid` (32-bit), not `xid8`.** Drainq
  rows are transient — drained within a drain-interval by the age trigger — so a
  32-bit witness compared with the standard `TransactionIdPrecedes` is sufficient
  and idiomatic (cf. `pg_class.relfrozenxid`). Avoids a `Catalog.pm` type-map
  addition and being the first-ever `xid8` catalog column.
- **One PK index `(sdq_idxid, sdq_partseq)`, not two.** Its leading column serves
  the drain's "all pending for this root" range scan (as `spanning_max_partseq`
  range-scans `pg_index_partition`'s PK), so a dedicated `(sdq_idxid)` index is
  redundant.
- **DHR lands behind a default-OFF GUC** (the appendix's `defer_spanning_reap`
  becomes a user-facing GUC defaulting off). A spanning leaf's entire dead set is
  spanning-relevant, so DHR defers ALL heap reaping to the drain; enqueue-without-
  drain would un-retire spanning entries and break cross-partition uniqueness.
  The GUC keeps each increment `make check` green (eager path preserved) until the
  full drain exists; flip default on once validated.

## Suggested implementation increments (each compiles + `make check` green)
1. **Catalog `pg_spanning_drainq`** (genbki, fixed oids, PK (sdq_idxid,
   sdq_partseq) + index on sdq_idxid, syscache). Empty; dumps. Mirror
   `pg_index_partition` (C1-B pattern). Remove rows in `index_drop` (like
   `RemoveSpanningPartitionMapForIndex`). +catversion bump + CLEAN REBUILD.
2. **Enqueue + reap-suppression**: convert the 3 `lazy_vacuum` call sites to
   enqueue; route the single heap-reap site through a gate that refuses to reap
   LP_DEAD slots with a pending drainq row for their (idxid,partseq).
3. **Drain**: `progresql_drain_spanning_index(idxRel)` — one coalesced
   leaf-only delete pass (partseq-gated, `_bt_delitems_vacuum`) for all pending
   partseqs, then reap the now-safe LP_DEAD slots across the dirty partitions,
   then delete the drainq rows — index-before-heap across the commit. SUEL on
   the index. Manual `pg_drain_spanning_index(regclass)` + `VACUUM <root>` hook.
4. **Trigger**: `AVW_SpanningIndexDrain` work-item + launcher periodic sweep.
5. **Tests**: positive deletion (re-insert into same partition succeeds after
   drain), cross-partition isolation under coalesced drain, crash/restart
   (drainq durable; no lost obligation), and a perf characterization showing the
   N→1 collapse (reuse the N=4..64 buffer-hit harness from PRODUCTION_READINESS).
   Flip `progresql_vacuum_collision` etc. to the drained path.

## Prior-art anchor (for the RFC + defensibility)
Robert Haas "decoupling table and index vacuum" conveyor-belt (dead-TID fork,
never-reused 64-bit ids, recycle-after-all-indexes-pass); Dilip Kumar 2025
"defer the global pass, vacuum once" (45min→40s). DHR is a third point: "pin the
heap line pointer as the conveyor, drain the shared index out-of-band." Frame it
honestly vs both; its distinguishing risk is bloat-on-drain-starvation (mitigated
by the durable queue + launcher sweep).

---

## APPENDIX: full winning DHR architecture (verbatim from the design panel)
```

#### NAME
Defer-Heap-Reap with Autovacuum-Driven Coalesced Spanning Drain (DHR)

#### ONE_LINE
Leaf vacuum enqueues spanning (partseq,TID) dead-tuples to a durable per-root queue but leaves the matching heap slots LP_DEAD (un-reaped), so a dead slot can never be reused before its spanning entry is gone; a single coalesced drain per root (one full spanning scan) retires all queued partseqs' entries and then a later cross-partition reap turns the now-safe LP_DEAD slots into LP_UNUSED — N independent O(tree) scans collapse to one, O(N^2)->O(N).

#### ARCHITECTURE
DATA STRUCTURES

1. Durable queue catalog `pg_spanning_drainq` (one row per (spanning index, partseq) that has un-drained dead heap TIDs):
   - `sdq_idxid oid`        -- spanning index oid (FK-ish to pg_class; cleaned with the index)
   - `sdq_partseq int4`     -- index-local partseq of the partition whose dead entries are pending
   - `sdq_enqueue_xid xid8` -- FullTransactionId of the leaf vacuum that enqueued (horizon witness; see crash_safety)
   - `sdq_ndead int8`       -- approximate count of dead TIDs enqueued since last drain (drain-cost / threshold accounting)
   PK btree (sdq_idxid, sdq_partseq). A second btree on (sdq_idxid) for the drain's "all pending partseqs for this root" scan.
   This is a SYSTEM CATALOG (genbki, fixed oid like pg_index_partition's 560/561/562), giving WAL-logging, crash safety, MVCC, and syscache for free — exactly the existing pg_index_partition pattern, so it reuses that whole toolchain.

   NOTE on what the queue does NOT store: it deliberately does NOT store the dead TID list. The dead TIDs stay materialized where they already are — as LP_DEAD line pointers in the leaf heap pages. The queue only records "partseq P of index I has >=1 pending dead entry"; the authoritative set of TIDs to delete is recovered at drain time by re-scanning the leaf heap for LP_DEAD items (see drain path). This is what makes the queue tiny (one row per dirty partition, not per dead tuple) and keeps it O(N) rows worst case, not O(dead-tuples).

2. LVRelState additions (vacuumlazy.c): a new flag `defer_spanning_reap` (set when has_spanning_index && the drain-deferral GUC/reloption is on) and a small per-vacuum list of (idxRel oid, partseq, ndead) that this leaf vacuum touched, used to write queue rows once at end of vacuum.

STORAGE CHOICE — catalog vs relation fork: I choose a catalog table, not a relation fork. A fork would need bespoke WAL records, bespoke recovery, and bespoke FSM/cleanup; the queue is small (bounded by number of partitions, not dead tuples), and the project already has the pg_index_partition catalog idiom (writer/reader/cleanup in catalog/pg_index_partition.c, syscaches, genbki oids). Reusing that pattern is the lowest-risk, most upstream-legible option and gets crash safety + MVCC visibility for free.

ENQUEUE PATH (in lazy_vacuum, vacuumlazy.c)

The enqueue replaces today's eager per-leaf bt_spanning_bulkdelete call. It hooks the exact three sites that call progresql_vacuum_spanning_indexes today (lazy_vacuum: nindexes==0 branch ~2675, bypass branch ~2764, normal branch ~2780). New helper progresql_enqueue_spanning_drain(vacrel):
   - For each ancestor spanning index I, resolve this leaf's partseq P (SpanningLookupPartseqByRelid — already exists). If P<0, skip.
   - UPSERT a pg_spanning_drainq row (I, P): if present, bump sdq_ndead += this round's lpdead count and leave sdq_enqueue_xid at the OLDER of existing/current (we want the oldest witness); if absent, insert with sdq_enqueue_xid = GetTopFullTransactionId of the vacuum.
   - This takes only RowExclusiveLock on the queue catalog and a brief AccessShareLock on the index to read partseq — it does NOT open or scan the spanning btree at all. Enqueue is O(#ancestor spanning indexes), i.e. O(1) per leaf vacuum. This is the headline cost win at enqueue time.

CRITICAL: the matching heap reap is SUPPRESSED. Today lazy_vacuum runs lazy_vacuum_heap_rel (the LP_DEAD->LP_UNUSED second pass) right after the spanning cleanup. In DHR, when defer_spanning_reap is on, we do NOT call lazy_vacuum_heap_rel for the dead TIDs that have a pending spanning entry. The heap line pointers stay LP_DEAD. (pruneheap.c already keeps LP_DEAD durable and re-discovers it: heap_prune_record_unchanged_lp_dead, deadoffsets "Includes existing LP_DEAD items", pruneheap.c:125,1508.) We must also force the leaf OFF the MARK_UNUSED_NOW fast path — has_spanning_index already does this today (vacuumlazy.c:1997-1998), so no new work; it now serves double duty.

This is the entire reuse-safety mechanism: a heap slot that still has a live spanning entry is held at LP_DEAD, and LP_DEAD slots are NEVER reused by new tuples (only LP_UNUSED slots are allocatable). So no new insert can ever land on a (partseq,block,offset) that the queue still owns.

DRAIN PATH (one coalesced pass per root)

New function progresql_drain_spanning_index(Oid spanningIndexOid), runnable in its own transaction:
   1. Open spanning index ShareUpdateExclusiveLock (serializes vs leaf vacuums & other drains, respects _bt_start_vacuum's one-active-vacuum rule). Open the root table AccessShareLock (heaprel arg, storage-less, as today).
   2. Scan pg_spanning_drainq for all rows with sdq_idxid == this index -> set of (partseq, enqueue_xid) pairs = "dirty partitions". Snapshot the set.
   3. For each dirty partseq P: resolve P->partition heap (SpanningResolvePartseqRelid — exists), open that partition AccessShareLock, build a fresh TidStore of its CURRENT LP_DEAD items by a heap scan that reads line-pointer state only (no MVCC tuple fetch needed — LP_DEAD is authoritative). Tag each TID with P. Union all dirty partitions' (P, TidStore) into one combined kill structure keyed first by partseq.
   4. ONE btvacuumscan of the spanning index (reuse bt_spanning_bulkdelete's machinery, generalized to a multi-partseq gate): for each index entry, read trailing partseq; if it is in the dirty set AND its TID is in that partseq's LP_DEAD TidStore, delete via WAL-logged _bt_delitems_vacuum. This is exactly one full tree scan covering ALL dirty partitions at once -> the N->1 collapse.
   5. After the scan succeeds, for each (P, TidStore) the now-orphaned heap LP_DEAD slots are SAFE to reap: open partition, run the standard LP_DEAD->LP_UNUSED reap (factor lazy_vacuum_heap_page out, or simply leave them and let the partition's NEXT ordinary vacuum reap them — see autovacuum_story for both options). Then delete the drained pg_spanning_drainq rows (only those whose enqueue_xid <= the set we just processed, to not lose concurrently-enqueued newer work).
   6. Commit.

LOCKING SUMMARY: drain holds SUEL on the spanning index (self-conflicting; serializes drains and leaf-vacuum enqueue-time index access, but does NOT block DML's RowExclusiveLock); AccessShareLock on each partition heap and the root. Concurrent INSERT/UPDATE/DELETE (RowExclusiveLock) never blocked.

#### REUSE_HANDLING
MECHANISM: defer the heap reap. A dead heap slot whose spanning entry is still in the index is held at LP_DEAD, never advanced to LP_UNUSED, until the drain has removed that spanning entry. Heap free-space allocation for new tuples only ever reuses LP_UNUSED line pointers (and never an LP_DEAD one), so the (partseq, block, offset) address cannot be handed to a new live row while the stale spanning entry still exists. There is therefore NO window in which a new live entry and a stale dead entry share the same (partseq, TID). The drain keys deletion on (partseq AND TID-is-currently-LP_DEAD); since the slot is provably still dead (it is still LP_DEAD at drain time, re-read from the heap, not from a stale snapshot), the entry it deletes is provably the stale one.

This is strictly stronger than a "membership of (partseq,TID)" drain (the trap the prompt warns about): we do not trust a recorded TID list at all. We re-derive the kill set from the heap's LIVE line-pointer state at drain time. An LP_DEAD line pointer is, by heap-AM invariant, a slot with no live tuple and no possibility of one until vacuum reaps it. So "TID is LP_DEAD now" is the runtime proof of deadness.

WALKED INTERLEAVING (the classic reuse trap, and how DHR defeats it):

Setup: spanning index I on root R; partition A has partseq=3; user key k1 currently at A heap TID (blk=10, off=5). Spanning index holds entry E_old = {key=k1, partseq=3, tid=(10,5)}.

t0: DELETE the row with k1 in A. Its heap tuple becomes dead.
t1: autovacuum vacuums A. lazy_scan_prune marks (10,5) LP_DEAD and records it in dead_items. DHR enqueue: UPSERT pg_spanning_drainq (I, partseq=3, enqueue_xid=X1, ndead+=1). DHR SUPPRESSES the heap reap for (10,5): the line pointer STAYS LP_DEAD. E_old is still in I. Commit. << key moment: (10,5) is LP_DEAD, not LP_UNUSED. >>
t2: INSERT a new row with a DIFFERENT user key k2 into partition A. The heap allocator looks for a free slot. (10,5) is LP_DEAD -> NOT free -> the allocator skips it and uses some other slot, say (10,9) (or extends). The new spanning entry is E_new = {key=k2, partseq=3, tid=(10,9)}. << E_new's TID is NOT (10,5); the collision the trap needs never forms. >>
   - Contrast with today/eager-reap: had vacuum reaped (10,5) to LP_UNUSED at t1, the t2 INSERT could have reused (10,5), producing E_new'={k2,3,(10,5)} colliding with the still-present E_old={k1,3,(10,5)} -> a (partseq,TID)-membership drain would silently delete the LIVE E_new'. DHR makes this impossible by withholding LP_UNUSED.
t3: drain of I runs. It sees dirty (I, partseq=3). It opens A, re-scans for LP_DEAD: finds (10,5) (still LP_DEAD — nothing reused it). Kill set for partseq 3 = {(10,5)}. It does NOT contain (10,9) because (10,9) is a live tuple, not LP_DEAD. btvacuumscan deletes only entries with partseq==3 AND tid in {(10,5)} -> deletes E_old, leaves E_new. Correct.
t4: post-drain reap: (10,5)'s spanning entry is gone; the slot is now safe to advance LP_DEAD->LP_UNUSED. Either the drain reaps it immediately, or A's next vacuum re-discovers it LP_DEAD (pruneheap re-records existing LP_DEAD), finds it no longer in the drain queue, and reaps it normally. Slot returns to circulation only AFTER its old entry is provably gone.

CROSS-PARTITION SIBLING COLLISION (the original P0-5): partition B has partseq=7, a LIVE row with B-heap-TID (10,5) (same numeric TID as A's dead slot — legal, partition-local address spaces). The drain's gate is partseq-first: B's live entry has partseq=7, which is either not in the dirty set or, if B is also dirty, its kill set is B's OWN LP_DEAD scan which does not contain B's live (10,5). So B's live entry is never touched. The partseq gate + per-partition LP_DEAD re-derivation closes P0-5 by construction, same as C1-D2, but now in a single coalesced scan.

#### CRASH_SAFETY
DURABILITY OF THE QUEUE: pg_spanning_drainq is an ordinary system catalog. Its inserts/updates/deletes are CatalogTupleInsert/Update/Delete -> fully WAL-logged and crash-safe, identical to pg_index_partition. The enqueue happens in the leaf-vacuum's own transaction; if that transaction commits, the queue row is durable; if it aborts/crashes before commit, the queue row never existed AND the heap reap also never happened (the leaf vacuum's prune/reap are in the same xact framing), so state is consistent: either (slot LP_DEAD + queue row present) or (neither). There is no torn state where the slot was reaped but the queue row is missing.

WAL-LOGGED REMOVAL: the actual spanning-entry deletion in the drain stays on the existing _bt_delitems_vacuum path (the drain reuses btvacuumscan exactly as bt_spanning_bulkdelete does today). So index-entry removal is crash-safe and replicates to standbys unchanged.

CRASH BETWEEN ENQUEUE AND DRAIN: this is the normal, expected steady state, and it is safe. The dead slot is LP_DEAD (durable on the heap page, WAL-logged by the prune), the queue row is durable. On restart, nothing is lost: the slot is still un-reusable (LP_DEAD), the queue still says "drain me." The drain is idempotent and re-derives its kill set from live heap state, so re-running after a crash is harmless. No correctness dependency on the crash timing.

CRASH DURING DRAIN: the drain does its index deletes (WAL-logged) and only then deletes queue rows and reaps heap slots, all in one transaction (or the index-delete and queue-delete in one xact, heap reap deferred to next vacuum — see autovacuum_story). If it crashes mid-scan before commit, the whole drain xact rolls back: queue rows remain, any not-yet-committed index deletes are undone, no heap slot was reaped. Re-run is clean. Because the kill set is re-derived from LP_DEAD at each run, a partially-completed-then-rolled-back drain leaves no stale state.

UNBOUNDED-LEAK BOUND: the queue cannot grow without bound. It has at most one row per (spanning index, partseq) = O(#partitions) rows, regardless of how many dead tuples accumulate (ndead just increments in place). Even if the drain never ran, the queue stays O(N) rows. The heap-side cost (LP_DEAD slots not yet reaped) is bounded by the dead-tuple production between drains; the drain cadence (threshold on sum(ndead) or oldest enqueue_xid age) caps that — see autovacuum_story and perf_analysis for the bloat bound.

REPLICATION/RECOVERY TESTS NEEDED: crash + restart with pending queue; drain crash mid-scan; physical standby apply of the deferred-reap WAL (the LP_DEAD-held pages and the later reap must replay correctly — they use stock heap/btree WAL records, so this should be free, but P0-3 says verify).

#### AUTOVACUUM_STORY
THE PROBLEM RESTATED: autovacuum vacuums each leaf in its own worker/xact at its own cadence (do_autovacuum -> per-leaf vacuum_rel -> Start/CommitTransactionCommand). No agent ever holds the whole tree. So the drain cannot piggyback on a single vacuum; it must be an independent, separately-triggered pass.

TRIGGER MECHANISM (concrete, reuses existing core machinery): the autovacuum work-item queue. Core already has AutoVacuumRequestWork(type, relOid, blkno) writing to shared-memory av_workItems, drained by perform_work_item() in the autovacuum worker — currently used for AVW_BRINSummarizeRange. perform_work_item runs each item in its own transaction with a user/security context switch (autovacuum.c:2605-2700). DHR adds a new work-item type AVW_SpanningIndexDrain whose avw_relation = the spanning index oid (or the root oid).

ENQUEUE-TO-TRIGGER: at the end of a leaf vacuum that enqueued queue rows, after computing the new sum(ndead) for the root's spanning index, the leaf vacuum decides whether to REQUEST a drain:
   - Threshold model (mirrors autovacuum_vacuum_threshold): request a drain when sum(sdq_ndead) for the index crosses spanning_drain_threshold + spanning_drain_scale_factor * reltuples(index), OR when the oldest sdq_enqueue_xid is older than a bound (age-based safety, so a low-traffic root still drains eventually and slots don't sit LP_DEAD forever). Both are new reloptions on the spanning index / GUCs, defaulting to sensible values.
   - If the threshold is crossed, call AutoVacuumRequestWork(AVW_SpanningIndexDrain, spanningIndexOid, 0). This is best-effort and idempotent: if a request slot is full or a request is already pending, that is fine — the queue rows are durable and the next leaf vacuum will request again. AutoVacuumRequestWork can be lossy (perform_work_item notes the list "can be lossy"); DHR tolerates this precisely because the durable catalog queue, not the shmem request, is the source of truth. The shmem request is only a nudge.

WHY THIS FITS AUTOVACUUM INDEPENDENCE: each leaf vacuum independently bumps the shared durable counter (queue) and independently may nudge the launcher. The DRAIN itself is a single agent (the autovacuum worker running the work item) that, for the first time, DOES hold the whole tree's pending set — because it reads it from the durable catalog, not from any one vacuum's in-memory dead_items. This is the structural unlock the C1-D2 UPDATE said was missing ("no point at which one agent holds the whole tree's dead sets"): DHR creates that point, durably, off the vacuum hot path.

FALLBACKS / MANUAL CONTROL:
   - A new SQL function pg_drain_spanning_index(regclass) (and a VACUUM (DRAIN_SPANNING) option on the root) lets an operator force a drain; same progresql_drain_spanning_index() body. Good for tests and emergencies.
   - VACUUM <root> (manual, expands to all leaves) runs the per-leaf enqueue across the loop and then, at end-of-command (after the leaf loop, in vacuum() not vacuum_rel(), so it is outside the per-leaf transactions), invokes one drain. This gives manual VACUUM the coalesced single pass directly, without waiting for the launcher — and it is safe because the drain runs in its own transaction after all leaf xacts committed.

HEAP REAP COORDINATION ACROSS PARTITIONS (the angle's specific ask): two viable reap strategies, DHR supports both, default is (b):
   (a) Drain-driven reap: after the single spanning scan, the drain itself opens each dirty partition and reaps its now-safe LP_DEAD slots (LP_DEAD->LP_UNUSED) in the same work-item transaction. Pro: slots freed promptly. Con: drain now touches every dirty partition heap (still O(dead pages), not O(tree)) and needs cleanup-lock-grade access.
   (b) Next-vacuum reap (default, simplest, fully correct): the drain only deletes spanning entries and queue rows. The LP_DEAD slots are left for the partition's NEXT ordinary autovacuum, which re-discovers them (pruneheap re-records existing LP_DEAD), sees no pending queue row for that partseq, takes the normal (non-deferred) path, and reaps them LP_DEAD->LP_UNUSED. This needs a check at reap time: "is there still a pending drain-queue row for (this index, this partseq)?" If yes, keep deferring; if no, reap. Pro: drain stays a single index scan, zero per-partition heap work; reap cost is absorbed into vacuums that were going to run anyway. Con: slots stay LP_DEAD slightly longer (until next leaf vacuum). The age-based drain trigger bounds this.

BLOAT BOUND: a heap slot is held LP_DEAD from the leaf vacuum that found it until (drain removes its spanning entry) + (next reap). The maximum LP_DEAD backlog per partition is bounded by dead tuples produced in one drain interval; the drain interval is bounded by spanning_drain_threshold (count) and the enqueue_xid age bound (time). So worst-case extra heap bloat = one drain-interval's worth of dead tuples per partition, which the operator tunes exactly like autovacuum_vacuum_threshold. This is the same class of bounded, tunable deferral autovacuum already embodies — defensible and familiar.

#### CONCURRENCY
DML (RowExclusiveLock on partitions / inserts into the spanning index): never blocked. The drain takes ShareUpdateExclusiveLock on the spanning INDEX and AccessShareLock on heaps; none of these conflict with RowExclusiveLock. Inserts continue to write new spanning entries during a drain; the drain only deletes entries whose TID is currently LP_DEAD, and a freshly inserted entry's TID is live, so concurrent inserts are invisible to the kill set.

LEAF VACUUM vs DRAIN: both want the spanning index. Today's eager path opens it SUEL; the drain opens it SUEL. SUEL is self-conflicting, so a drain and a concurrent leaf vacuum's enqueue serialize — BUT in DHR the leaf vacuum no longer scans the index at all (enqueue only reads partseq under a brief AccessShareLock on the index and writes the queue catalog under RowExclusiveLock on the queue). So the leaf vacuum's index footprint shrinks to a metadata read; contention with the drain drops to near zero. Only drain-vs-drain on the same index serialize (correct and desired — one active vacuum per index, satisfying _bt_start_vacuum).

_bt_start_vacuum "one active vacuum per index": honored. The drain is the sole caller of btvacuumscan on the spanning index (leaf vacuums stopped calling it). Two concurrent drains of one index can't happen: SUEL serializes them, and even if requested twice, the second blocks on the lock then finds the queue already drained (re-derives empty kill set) — idempotent no-op.

QUEUE CATALOG concurrency: enqueue UPSERTs under RowExclusiveLock on pg_spanning_drainq with the PK (idxid,partseq) as the serialization backstop (like SpanningGetOrAllocPartseq relies on its unique index). Two leaf vacuums of DIFFERENT partitions of the same root touch different (idxid,partseq) rows -> no row conflict. Two vacuums of the SAME partition can't run concurrently (the partition heap's vacuum lock prevents it). The drain reads the queue under its snapshot and deletes only rows with enqueue_xid <= what it processed, so rows inserted concurrently (newer xid) survive to the next drain — no lost-wakeup.

DEADLOCK: lock acquisition order in the drain is fixed (spanning index SUEL -> queue catalog -> partition heaps AS in partseq order). Leaf vacuum order is (partition heap cleanup-lock, already held) -> index AS -> queue RowExclusive. No cycle: the drain never takes a partition heap lock that conflicts with the leaf vacuum's, and never waits on the queue while holding a heap lock in a way that inverts the leaf's order (queue access is short and released). The cross-partition heap-liveness probe in _bt_check_unique (AccessShareLock on sibling partitions) is AS-vs-AS with the drain's AS — compatible.

STANDBY: drain WAL is stock btree delitems + heap reap records; replays without special handling. No new lock classes on the standby.

#### PERF_ANALYSIS
AMORTIZED COMPLEXITY: O(N) per full sweep, down from O(N^2).
   - Today (C1-D2): each of N leaf vacuums runs a full btvacuumscan of the whole spanning index = O(size_of_tree) each; N of them per sweep = O(N * tree) = O(N^2) in leaves (the measured curve: buffer hits 198/244/336/520/891 for N=4/8/16/32/64, ~150 fixed + total_index_pages, i.e. each single-leaf vacuum reads essentially the entire index).
   - DHR: each leaf vacuum does O(1) index work (read partseq + one catalog UPSERT) and ZERO spanning-tree scan. The single coalesced drain per sweep does ONE full btvacuumscan of the spanning index = O(size_of_tree). Total per sweep = N * O(1) enqueue + 1 * O(tree) drain = O(tree) = O(N) (tree size is linear in N for fixed rows/leaf). For N=64 that is ~one 891-buffer-hit scan per sweep instead of 64 of them — roughly a 64x reduction in spanning-index I/O at N=64, and the gap widens linearly with N.

DOMINANT COSTS after DHR:
   1. The one drain scan: O(tree) buffer reads, once per drain interval, on the autovacuum worker (off the per-leaf-vacuum critical path entirely). This is the irreducible cost of the trailing-partseq key order (entries scattered), and it is now paid ONCE, which is the best achievable without re-keying (which is forbidden).
   2. Per-partition LP_DEAD re-scan at drain time to build the kill set: O(dead pages) across dirty partitions — but only the pages that HAVE LP_DEAD items (vacuum already knows these are few in steady state), and it is line-pointer-only (no tuple fetch).
   3. Reap: O(dead pages), absorbed into next ordinary vacuum (strategy b) — net-new cost ~0 because those vacuums run anyway.

NEW WRITE-PATH / ENQUEUE OVERHEAD:
   - Hot DML write path: ZERO change. Enqueue is in VACUUM, not in INSERT/UPDATE/DELETE. The reuse-safety mechanism (hold LP_DEAD) costs nothing at write time; it only affects which slots the allocator picks, and skipping LP_DEAD slots is already what the allocator does.
   - Per-leaf-vacuum overhead: one syscache partseq lookup + one catalog UPSERT per ancestor spanning index (typically 1). Negligible vs the full-tree scan it REPLACES. Net: leaf vacuums get dramatically CHEAPER (they stop scanning the tree).
   - Extra heap bloat: bounded by one drain-interval of dead tuples per partition (tunable), the only real new cost, traded for the N^2->N win.

WIN QUANTIFICATION: at N leaves, spanning-index I/O per sweep goes from ~N * (150 + P) to ~(150 + P) where P = tree pages, i.e. ~N-fold reduction in the exact metric the readiness doc benchmarks. This directly answers Haas's "141 TB of I/O" objection: a single end-of-cycle pass is the accepted shape.

#### IMPLEMENTABILITY
SIZE: XL (multi-week), consistent with the readiness doc's option-B sizing — but DHR is the strongest member of the defer family because it needs NO heap-deadness re-proof at drain time (the LP_DEAD invariant IS the proof), removing the hardest correctness sub-problem of a TID-list-based queue.

FILES / FUNCTIONS TO TOUCH:

NEW catalog (S, pattern already exists):
- src/include/catalog/pg_spanning_drainq.h — CATALOG def (mirror pg_index_partition.h: fixed oids, two btree indexes, MAKE_SYSCACHE), function prototypes.
- src/backend/catalog/pg_spanning_drainq.c — SpanningDrainqEnqueue(idxRel, partseq, ndead, xid), SpanningDrainqListDirty(idxOid)->List of (partseq,xid), SpanningDrainqDeleteUpTo(idxOid, xidBound), RemoveSpanningDrainqForIndex(idxOid), RemoveSpanningDrainqForPartition (mirror pg_index_partition.c's writer/reader/cleanup exactly).
- src/include/catalog/catversion.h bump; Makefile/meson CATALOG_HEADERS lists; src/backend/catalog/Makefile + catalog/meson.build.

ENQUEUE + reap suppression (M):
- src/backend/access/heap/vacuumlazy.c — replace the three progresql_vacuum_spanning_indexes call sites in lazy_vacuum (~2675, ~2764, ~2780) with progresql_enqueue_spanning_drain(vacrel) when defer mode on; gate lazy_vacuum_heap_rel so dead TIDs with a pending spanning entry are NOT reaped (new helper progresql_partseq_has_pending_drain checked in the reap, or simpler: when defer mode on, skip the reap entirely for spanning leaves and let the post-drain path/next vacuum reap). Add LVRelState.defer_spanning_reap, the per-vacuum touched-partseq list, and the end-of-vacuum AutoVacuumRequestWork(AVW_SpanningIndexDrain,...) threshold check. has_spanning_index already forces two-pass + off MARK_UNUSED_NOW — reuse as-is (vacuumlazy.c:1997, 2407).
- Keep progresql_vacuum_spanning_indexes/bt_spanning_bulkdelete for the non-defer (eager) mode and as the building block the drain reuses.

DRAIN (L):
- src/backend/access/heap/vacuumlazy.c (or a new src/backend/access/heap/spanningdrain.c) — progresql_drain_spanning_index(Oid): list dirty partseqs, build per-partseq LP_DEAD TidStores by heap line-pointer scan, call generalized multi-partseq spanning scan, delete queue rows, optional reap.
- src/backend/access/nbtree/nbtree.c — generalize bt_spanning_bulkdelete to accept a SET of partseqs (a small partseq->TidStore map) instead of one partseq; the btvacuumpage gate (nbtree.c:1584-1599) changes from "partseq == one value" to "partseq in dirty set, then look up that partseq's TidStore." Modest change to the existing gate.

TRIGGER (S):
- src/include/postmaster/autovacuum.h — add AVW_SpanningIndexDrain to AutoVacuumWorkItemType.
- src/backend/postmaster/autovacuum.c — perform_work_item() switch case calls progresql_drain_spanning_index(workitem->avw_relation); autovac_report_workitem label.

SQL surface + reloptions/GUCs (S):
- New pg_proc entry pg_drain_spanning_index(regclass); optional VACUUM (DRAIN_SPANNING) option (gram.y/vacuum.c) and VACUUM <root> end-of-command drain hook in src/backend/commands/vacuum.c (after expand_vacuum_rel leaf loop).
- reloptions for spanning_drain_threshold/scale_factor (src/backend/access/common/reloptions.c) or GUCs.

CLEANUP wiring (S):
- index_drop -> RemoveSpanningDrainqForIndex (next to the existing RemoveSpanningPartitionMapForIndex call).
- partition drop/detach -> RemoveSpanningDrainqForPartition.

TESTS (M): regress (reuse interleaving: delete+vacuum, insert reusing slot, drain, assert old gone/new survives), the P0-5 sibling-collision still green, isolation (drain vs concurrent DML, drain vs leaf vacuum), TAP recovery (crash between enqueue and drain; drain crash mid-scan), benchmark re-run of the N=4..64 curve to show flat per-sweep cost.

#### RISKS

  - Heap bloat from held LP_DEAD slots: if the drain falls behind (launcher starvation, request slot exhaustion, threshold mis-tuned), dead slots accumulate as LP_DEAD across all partitions and are not reclaimable until drain. Mitigation: age-based drain trigger (oldest enqueue_xid bound) + manual pg_drain_spanning_index + the per-drain-interval bloat bound; but mis-tuning is a real operational footgun reviewers will probe.
  - AutoVacuumRequestWork is lossy and NUM_WORKITEMS-bounded (shared array). If many roots want drains at once, requests can be dropped. DHR tolerates this (durable queue is source of truth, next leaf vacuum re-requests), but a pathological case could delay drains; may want a launcher-side periodic sweep of pg_spanning_drainq instead of relying solely on requests.
  - Drain transaction can be long (one full tree scan + per-partition LP_DEAD scans) and holds SUEL on the spanning index for its duration; while it does not block DML, it serializes other drains and any operation needing >SUEL on that index (e.g. REINDEX). Need to ensure drain yields to vacuum_delay_point and is interruptible.
  - Reap coordination (strategy b) requires the next ordinary vacuum to CHECK the drain queue before reaping an LP_DEAD slot, adding a syscache lookup per dirty partseq in every spanning-leaf vacuum's reap path; must verify this doesn't reintroduce per-leaf catalog churn that erodes the win.
  - Re-deriving the kill set from LP_DEAD at drain time assumes LP_DEAD line pointers are stable between enqueue and drain. A concurrent process that converts LP_DEAD->LP_UNUSED out from under the drain (e.g. a non-deferred vacuum path, HOT pruning edge cases, or a future optimization) would shrink the kill set and orphan a spanning entry. Must audit ALL LP_DEAD->LP_UNUSED transitions to confirm the drain queue gates every one of them for spanning partitions; this is the single most important correctness audit.
  - New system catalog = catversion bump + pg_upgrade consideration (the queue itself need not survive upgrade — it can be drained-to-empty pre-upgrade — but this must be explicit, tying into the open P0-2 work).
  - Manual VACUUM <root> end-of-command drain runs outside the per-leaf transactions; must confirm vacuum()'s control flow (expand_vacuum_rel loop) has a clean post-loop hook and that errors there don't leave the command half-done.
  - Posting-list tuples in the spanning index share one partseq but multiple TIDs; the multi-partseq gate must correctly intersect each posting TID against the right partseq's TidStore (the existing btreevacuumposting path handles per-TID, but the gate generalization must be tested for posting lists spanning the dirty set).
```
