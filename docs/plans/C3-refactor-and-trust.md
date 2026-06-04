# C3 — Incision refactor + trust-testing (READ FIRST after compaction)

> Forward plan written 2026-06-02 as a pre-compaction handoff. Branch
> `progresql-c1`. The single source of correctness/state remains
> `docs/plans/C2-campaign-status.md`; this file is the *next-steps* tracker for
> two things: (A) finish the incision refactor, (B) test to the degree we can
> drop the README "experimental / don't trust your data" flag.

## Standing guardrail (do not violate)
Surgical, complementary, backwards-compatible — see memory
`scope-surgical-discipline`. The CORRECTNESS feature is done; the live work is
making the fork (1) cleanly incised into core, and (2) trustworthy. Default to
NOT adding core code; tests + verifiers are in-scope and don't bloat the fork.

## Env / build notes (easy to lose)
- TAP is enabled. **TAP runs need `export PERL5LIB=$HOME/perl5/lib/perl5`** (IPC::Run
  was installed non-sudo there). `pageinspect` is built into `build/install`.
- Build: `make -j8 && make install`; full suite `make -C src/test/regress check`
  (240 tests). Scratch clusters: `build/install/bin/{initdb,pg_ctl,psql}`,
  initdb with `-U volte -A trust` (pg_upgrade connects as the OS user).
- Clean diff vs vanilla: branch **`pg18-base`** (= fork point `ac3b97db38`) is on
  origin. Compare: `https://github.com/TwilightCoders/progresql/compare/pg18-base...progresql-c1`

## (A) Refactor — 4 of 5 blocks DONE; one remains
Module lives at `src/backend/access/spanning/` (spanning_exec.c, spanning_ddl.c,
spanning_relcache.c) + header `src/include/access/spanning.h`. Pattern: thin
guarded call at the incision in core; body in the module. Done (each a verified
no-op commit, 240 green):
- `53a124a518` execIndexing → spanning_exec.c (execIndexing.c + executor.h now PRISTINE)
- `3dbf4e34f4` tablecmds attach/detach/truncate → spanning_ddl.c (tablecmds.h PRISTINE)
- `51c9b929cf` BuildSpanningIndexFromPartitions → spanning_ddl.c (defrem.h PRISTINE)
- `b4f5db5a47` relcache HOT helpers → spanning_relcache.c
Cumulative: ~1,029 → 209 fork lines across those core files; 4 files byte-identical
to vanilla. ~820 lines relocated into the module.

**BLOCK 5 (remaining): `vacuumlazy.c` (+762, the DHR subsystem).** This is the
HARD one — do it as a FOCUSED pass, AFTER (B) testing exists as the net:
- The clean movers (take Oid/Relation): `progresql_drain_spanning_index(Oid)`,
  `spanning_drain_reap_partition`, `pg_drain_spanning_index` (fmgr),
  `progresql_rel_has_spanning_ancestor(Relation)` → move to `spanning_vacuum.c`.
- The coupled two take `LVRelState *` (vacuumlazy's private struct):
  `progresql_vacuum_spanning_indexes`, `progresql_enqueue_spanning_drain`.
  **Signature-shim** (user-approved): change them to take plain args (Relation,
  the dead-items TidStore, ndead, relname) instead of `LVRelState`; the 3
  lazy_vacuum call sites pass `vacrel->rel`, `vacrel->dead_items`, etc. Leaves a
  thin branch in vacuumlazy.c, body in the module.
- Also extractable: `bt_spanning_drain` / `bt_spanning_bulkdelete` in nbtree.c are
  largely inline btree internals — likely STAY (irreducible). nbtinsert.c BLK-1
  guard, index.c E6 stamping = inline residue, STAY.
- Verify: build + 240 + the new isolation/recovery tests + progresql_drain regress.

## (B) Trust-testing — what drops the README flag

### PROGRESS 2026-06-04 (#34 FIXED — concurrency gating blocker CLOSED)
The cross-partition uniqueness race (#34), the last data-safety blocker, is fixed
and verified. README flag softened "Highly experimental / don't trust your data"
→ "Beta".

**Root cause (two defects, both at the same seam).** Stock btree serializes
same-key inserters by the leaf-page write lock and descends with the scantid
omitted so the rightward `_bt_check_unique` scan covers the whole equal-key run.
A spanning index breaks BOTH invariants because entries sort by
`(user_cols…, partseq)`:
  1. **Concurrency:** two inserts of the same user key into different partitions
     form different full keys `(uk, partseq_a)`/`(uk, partseq_b)` that can sit on
     different pages → no shared buffer lock → check-and-insert not atomic →
     silent duplicate (soak: INSERT+cross-partition-MOVE ~73–116 dups).
  2. **Serial page-boundary:** the descent used the FULL key, so inserting
     `(uk, partseq_hi)` could land PAST a live `(uk, partseq_lo)` on a left
     sibling, which the rightward-only scan never revisits → a duplicate with NO
     concurrency at all. (Independent of #1; both had to be fixed.)

**Fix — one choke point, the btree AM (`_bt_doinsert`).** An adversarial design
panel confirmed `_bt_doinsert` is the single point ALL concurrent spanning-entry
inserts funnel through (ExecInsert / cold UPDATE / cross-partition UPDATE / COPY /
logical-repl apply → `index_insert` → `btinsert` → `_bt_doinsert`; the only
bypass, bulk `_bt_load`, only runs under a strong lock). So the fix lives there,
no per-executor-path interception:
  - **Value lock** (`LOCKTAG_SPANNING_KEY`, new heavyweight lock type): a
    short-duration exclusive lock keyed on `(db, index, hash(user_cols))`, taken
    before the descent and released after the physical insert — the
    cross-partition analogue of the leaf-page write lock. Logic in the owned
    module `src/backend/access/spanning/spanning_lock.c`; only a thin guarded call
    in `_bt_doinsert`. Released across `XactLockTableWait` (re-taken on retry),
    exactly like the buffer lock, so a waiter can't deadlock the txn it waits on.
  - **Page-boundary fix:** descend the check with the user-key prefix only (lands
    on the leftmost page of the run); after uniqueness is established, re-descend
    with the full key and insert as an ordinary non-unique insert.

**Verification (all green):** pgbench soak `-c 12..24` over all op mixes
(insert-only, HOT-churn, insert+delete, insert+cross-partition-move, mixed):
**0 duplicates, 0 deadlocks, 0 crashes**; `bt_index_check` + `bt_index_parent_check`
clean post-soak. 241 regress (new deterministic `progresql_concurrency`), 121
isolation (`spanning-unique`, `spanning-detach`), recovery/subscription/pg_upgrade
spanning TAP. ~11% TPS overhead on the pathological all-MOVE workload, far less on
normal mixes (lock only touches spanning inserts).

**Soak harness — now committed: `src/test/spanning/spanning_soak.sh`** (+ README).
Stress + dual-oracle verification (the PostgreSQL way): hammer a spanning-indexed
partitioned table (int PK + text UNIQUE, both GLOBAL) with concurrent
INSERT/MOVE/DELETE/batch/HOT via pgbench, then check (1) the app invariant — no
cross-partition dup on either key — and (2) amcheck on every spanning index;
`--crash` adds kill-9-mid-load + recovery. Two findings worth keeping:
  - **amcheck alone is insufficient** for spanning uniqueness: it validates btree
    order on `(user_cols…, partseq)`, and a cross-partition duplicate is
    well-ordered at that level, so amcheck reports OK *with duplicates present*.
    The app invariant is the load-bearing oracle; amcheck catches the ordering
    corruption a bad insert-positioning fix would cause. Both are needed.
  - The harness runs **autovacuum off**: the race needs a hot key's dead entries
    to accumulate so its run spans btree pages; aggressive autovacuum masks it.
  Validated as a real oracle: pre-fix `9d92004062` → 24–43 dups (FAIL); fix
  `1d94d3320e` at the same brutal settings (idspace 100–300, 16–24 clients) and
  under `--crash` → 0 dups, 0 deadlocks, amcheck clean.

### PROGRESS 2026-06-02 (night)
Trust-testing started AND turned up real bugs. Done this pass:
- Isolation specs: `spanning-unique` (cross-partition uniqueness under
  concurrency — the core "correct by reuse of `_bt_check_unique`" proof) and
  `spanning-detach` (DETACH vs concurrent DML). Both green, in `isolation_schedule`.
- Crash-recovery TAP: `src/test/recovery/t/049_spanning_crash.pl` (11 assertions:
  committed rows survive WAL replay, uncommitted rolled back with no phantom
  entry, uniqueness preserved, map intact). Green.
- **Bugs found + fixed** (commit-verified): (A) abort-unsafe DETACH/DROP/TRUNCATE
  cleanup left LP_DEAD across ROLLBACK → silent corruption — `b0caff8cf2`;
  (1) colliding INSERT vs unresolvable partseq probed the storage-less root →
  SIGSEGV — `d6bc169300`.
- **Bugs found, VERIFIED, DEFERRED** (design/review needed — do NOT rush
  overnight): partseq reuse on highest-partition detach (#2, needs a persisted
  high-water mark + pg_dump/pg_upgrade work), its reuse-chains (#3/#4/#7),
  TRUNCATE hint crash-durability (#8), amcheck heapallindexed crash on the
  storage-less root (Z).  **See `docs/plans/C3-spanning-bug-hunt-findings.md`**
  for repros, mechanisms, recommended fixes, and the unverified follow-up probes
  (concurrent DETACH races, sub-partition trees, multiple spanning indexes, etc).
- amcheck `bt_index_check(idx)` (structural) WORKS on a spanning index;
  `bt_index_check(idx, true)` (heapallindexed) CRASHES (item Z).

Remaining for the flag: fix #2 + chain, #4, #8, Z; the concurrency-race probe;
then the perf fix + soak. The two fixes above remove the most-reachable bugs but
are NOT by themselves sufficient to drop the flag.

### PROGRESS 2026-06-03 (single-session + write-path fixed; CONCURRENCY race OPEN)
> CORRECTION: an earlier draft of this section said "correctness gate CLOSED."
> That was premature.  An empirical probe + write-path audit + concurrency soak
> (run after that draft) fixed a whole class of write-path bypasses AND found an
> OPEN architectural concurrency race.  Accurate status below; full detail in
> `docs/plans/C3-spanning-bug-hunt-findings.md` (read its top banner first).

**Write-path bypasses — FIXED & shipped** (the audit found spanning enforcement
was bolted onto ExecInsert/ExecUpdate and bypassed elsewhere): non-key UPDATE
HOT/cold `6cba71e23b`, COPY `4711f4c7ac`, ATTACH-of-partitioned + logical
replication apply `cd064cc2cd`, CLUSTER/VACUUM FULL/ALTER leaf rewrite
`7e9e45739e`.  Each live-verified, regress/TAP/isolation green.

**[RESOLVED 2026-06-04 — see the PROGRESS 2026-06-04 section above.]** The
gating blocker: cross-partition uniqueness is racy under concurrency.  A 12-client
soak admits silent duplicates when concurrent INSERT races a DELETE or
cross-partition UPDATE of the same key (INSERT+MOVE ~73–116 dups; INSERT-only and
INSERT+churn = 0).  Mechanism: same-user-key inserts into different partitions
sort to different btree keys (different partseq) → no shared page → no page-level
value lock → check-and-insert not atomic.  (The earlier note "delete/move side
also needs to lock" was a mis-diagnosis: the real fix is the value lock at the
`_bt_doinsert` choke point, which all insert paths funnel through, PLUS the
separate serial page-boundary descent fix.  Fixed and soak-verified to zero dups.)

**Earlier deferred findings — also fixed, live-verified, tested:**
- **#2 partseq no-reuse** — new `pg_spanning_seq` counter catalog (OID 565/566);
  monotonic, never lowered by DETACH/DROP; binary-upgrade preserves it. `eb8b088684`,
  pg_upgrade TAP `a1f7dd9789`. This also **closes #3 and #7** (no reuse → a stale
  entry stays unresolvable and is skipped, never re-resolves to a later joiner).
- **#4** — `RemoveSpanningDrainqForPartseq`, reap the drain-queue row on
  DETACH/DROP. `247bd65b13`.
- **Z amcheck** — reject `heapallindexed` on a spanning index (clean error, no
  crash); structural check still works. `021aa3734e`.
- **R (new, found this pass)** — `REINDEX [INDEX] CONCURRENTLY` on a spanning index
  crashed / built an empty index (silent loss of uniqueness); now rejected.
  `9f30fd8d1d`.
- **#8 TRUNCATE crash-durability** — TRUNCATE re-maps the partition to a fresh
  partseq (a WAL-logged catalog change), so the truncated heap's stale entries are
  unresolvable and a lost LP_DEAD hint can't resurrect a resolving entry. Crash TAP
  extended. `893be61b00`.
- **#1 concurrency** — an adversarial verification pass proved cross-backend INSERT
  vs DETACH/DROP/TRUNCATE is serialized by the partition-parent AccessExclusiveLock
  (vs the INSERT's AccessShareLock on the root); the resolve was hardened to
  `try_table_open` (`31a5b6a9aa`). The reuse-breaker lens could not defeat no-reuse.

The gate is now **coverage-bound, not correctness-bound.** Before dropping the
flag: a `DETACH … CONCURRENTLY` isolation spec (path verified by lock analysis,
spec still TODO), the broader unverified probes, the DHR-default perf work, and a
soak run. FK-to-spanning (#33) is a separate feature, not a flag blocker.

### Original audit / plan
Audit (2026-06-02): we have ZERO concurrency + ZERO crash-recovery tests; amcheck
can't validate a spanning index. Reassurance: we wrote NO custom WAL / resource
managers (all standard btree+heap WAL) and uniqueness uses btree's own
page-locked `_bt_check_unique` — so concurrency+recovery are most likely correct
BY REUSE, but unproven. Closing that gap = the flag.

Priority order:
1. **Concurrency isolation specs** (load-bearing). `src/test/isolation/specs/` +
   `make -C src/test/isolation check`. Cases: two sessions INSERT the same key
   into different partitions → exactly one commits; INSERT vs concurrent
   VACUUM/drain; ATTACH/DETACH vs concurrent DML. This validates antagonist B3
   (lock protocol).
2. **Crash-recovery TAP** (`src/test/recovery/t/` or a bespoke kill-9 test): load
   under a spanning constraint, `kill -9` mid-load, restart, assert the spanning
   index + pg_index_partition are consistent and cross-partition uniqueness still
   holds. Validates the "correct by reuse" WAL assumption.
3. **amcheck / verifiability**: teach amcheck (or ship a small checker) the
   partseq→partition indirection so spanning-index corruption is DETECTABLE.

After 1–3: honestly soften the flag to "Beta — single-node correctness,
concurrency, and crash recovery tested; not yet battle-tested at scale; keep
backups." To DROP it entirely also need: the perf fix (O(N²)→O(N) vacuum by
DEFAULT + cover local-index partitions — currently DHR is off-by-default AND only
covers `nindexes==0` leaves; see C2-campaign-status.md), a soak run (concurrent
load for hours + periodic amcheck, zero corruption), and ideally outside review.

## Recommended next-session order (revised 2026-06-02 night)
1. **Fix the deferred bug-hunt findings** (`C3-spanning-bug-hunt-findings.md`),
   highest-leverage first: #2 partseq-no-reuse (a reviewed catalog/pg_dump change —
   defuses #3/#7 and most of #4), then #4 `RemoveSpanningDrainqForPartition`
   (surgical), then Z amcheck guard (and ideally full partition-aware support =
   the verifiability item), then #8 TRUNCATE crash-durable retirement (with a
   kill-9 TAP). Each: live repro → fix → regression test → full suite → commit.
2. Run the unverified follow-up probes (concurrency races, sub-partition trees,
   multiple spanning indexes, partition-key UPDATE) — extend the hunt.
3. (A) BLOCK 5 vacuumlazy extraction, with the now-substantial test net.
4. Then perf fix → soak → relax the flag.
NOTE: do NOT rush #2/#8 — they touch the catalog / pg_dump / pg_upgrade and
crash-durability; my abort-safety fix (A) itself introduced crash #1, the lesson
being that complex spanning fixes need careful verification + review, not 2am
surgery. A Workflow fits the read-only HUNT well (it found these); keep fixes solo
+ live-verified.
