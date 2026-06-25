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

### PROGRESS 2026-06-24 (perf baseline measured; full validation matrix green on HEAD+#9)
Autonomous overnight pass on november. The refactor-audit tail was closed and the
**"scale-test / perf" flag-drop criterion now has data**.

**Audit #9 (DRY: shared `spanning_backfill_leaf()` helper) — DONE + validated to
release bar.** Commit `09bb6b4f60`. Folded the two near-identical per-leaf backfill
loops (ATTACH backfill + CREATE-index-on-populated build) into one helper, −37
lines, behavior-preserving. Independently re-reviewed for behavior-equivalence
(EQUIVALENT: snapshot discipline, push/pop balance, heapRel/rootOid threading,
relkind filter, resource pairing all verified). **Audit #2** (the `_bt_check_unique`
heavyweight-lock-under-buffer-content-lock at the two spanning probe sites) is
**designed, not coded** — see `C3-item2-btcheck-lockorder-design.md`; deliberately
gated on a reviewed session (hottest correctness path; current state is
documented-safe, latency-only residual).

**Full validation matrix (cassert + amcheck, HEAD `b033a35545` + #9 overlay):**
- regression **243/243**, isolation **122/122** (incl. progresql_inherit — both
  backfill paths — and spanning-{unique,detach,relcache}).
- soak `--defer-vacuum --ddl-churn --secs 900`: **1.74M txns, 569 DDL drop/recreate
  cycles, duplicate ids=0 / tags=0, amcheck st_pkey+st_tag_key OK, 0 deadlocks/0
  crashes → PASS**.
- spanning TAP **69/69 across 4 integration suites**: pg_dump roundtrip (14),
  pg_upgrade (14), recovery crash incl. INHERITS (27), logical replication incl.
  INHERITS (14). The integration surfaces that exercise backfill via
  dump/restore/upgrade/inherit are green.

**Perf baseline (`spanning_bench.sh`, optimized fork vs vanilla 18.3) — see
`C3-perf-baseline-2026-06-24.md`.** Do-no-harm: TPC-B and partitioned-local-PK
insert are within measurement noise of vanilla (−0.3%..+1.7%, identical at c=32) —
**the fork is free when the spanning feature is unused**, which is the core
PG-Core-credibility number and a named flag-drop criterion. Feature cost (GLOBAL vs
local PK, uncontended): −10.7%/−48.5%/−35.5% at c=1/8/32 (value-lock + descend-twice
overhead, paid only when used).

Remaining for the flag (unchanged, all user-gated or design-gated): outside review;
the C2 perf-default flip decision; the **contended-key-space** perf number (needs
`spanning_bench.sh` to expose `--idspace` + careful workload design — a naive small
idspace just measures conflict-abort throughput); and audit #2 implementation.

### PROGRESS 2026-06-13 (deferred-path scale-soak PASS; HEAD re-verified green)
The perf-default staircase advanced and the committed tree was re-verified after
the perf-flag struct change.

**#39 deferred-path scale-soak — PASS (the gate for flipping the default).** 6h NAS
soak (container `progresql-soak-defer`, cassert build of HEAD `70c95a3646`),
`spanning_soak.sh --defer-vacuum --autovacuum on --clients 16 --churn 8
--idspace 800 --partitions 6 --secs 21600 --round-secs 600`. This exercises the
DHR coalesced-drain path (not the eager path) with autovacuum-driven drains, on a
table with TWO spanning indexes (int PK GLOBAL + text UNIQUE GLOBAL) on
no-local-index leaves — the exact multi-index, `nindexes==0` geometry the #40
deferred-drain corruption fix addressed. Result: **36/36 rounds, 72/72 amcheck OK
(bt_index_check + bt_index_parent_check on both spanning indexes), 0 dups, 0
unexpected crashes, 0 deadlocks**, ~8M txns/round (~13k tps persistent). Bloat
plateaued at 9776 kB and held — the bounded held-LP_DEAD behaviour the DHR design
predicts, NOT a leak. The deferred drain path is now scale-validated; the #40 fix
holds under load. (The flip of the default itself remains blocked by #38 — see
below — and #39 also wants a re-soak WITH local indexes once #38 lands.)

**Higher-partition deferred soak — PASS** (NAS, container `progresql-soak-hi`,
reused the soak3 cassert build). Same DHR config at 4× the partition count to
stress the #40 root-coordination (drain unions many dirty partitions across both
spanning indexes, per-partition kill-maps + reaps): `--clients 24 --churn 12
--idspace 2000 --partitions 24 --secs 10800`. Result: **18/18 rounds, 36/36
amcheck OK, 0 dups, 0 crashes, 0 deadlocks**, size bounded at 12 MB. Confirms the
multi-partition drain coordination holds at higher partition fan-out.

**Crash-torture of the deferred path — 25/25 PASS** (local cassert HEAD,
`/tmp/crash-defer-loop.sh`). 25 cycles, each: fresh fsync-on cluster, load with
autovacuum-driven deferred drains in flight, `kill -9` the postmaster mid-load,
crash-recover, then verify. Result: 25/25 dup-clean, 50/50 amcheck OK, 0
unexpected crashes. This validates the DHR crash_safety design (drainq catalog
durability, held-LP_DEAD survival across crash, in-flight-drain WAL replay) and
substitutes for the spanning crash TAP (`049_spanning_crash`) that can't run on
this host (no IPC::Run).

**HEAD re-verified green on a clean local rebuild.** The local `build/install`
predated the perf-flag commit (which added two bools to `RelationData` in rel.h —
a struct change requiring a full rebuild). Clean-rebuilt HEAD and ran: **regress
241/241, isolation 122/122** (incl. progresql_drain/concurrency/global/hot,
spanning-unique/detach/relcache). The perf flag (cache "has spanning ancestor" on
the leaf relcache) is sound end-to-end.

**#38 (DHR-lift) design drafted + committed** (`a75f80c4bc`,
`docs/plans/C3-DHR-lift-local-index-leaves-design.md`). Recommends Option 2 (defer
all leaf index work to the coalesced drain; drain gains a per-partition
local-index ambulkdelete pass before the reap), keeping the coarse drainq and the
O(N) spanning-scan coalescing. This is the XL change gating #39's default flip.

**#38 IMPLEMENTED + #39 DEFAULT FLIPPED (committed `b79e384ddc`..`2ad374e329`).**
The DHR-lift landed per the Option-2 design: `lazy_vacuum`'s defer decision is
hoisted to cover BOTH no-local-index and local-index spanning leaves, and
`progresql_drain_spanning_index` gained `spanning_drain_vacuum_local_indexes` — a
per-partition local-index `ambulkdelete` against the same LP_DEAD set, run before
the reap so no live local entry is orphaned (the coarse `pg_spanning_drainq` is
unchanged; the O(N) spanning coalescing is preserved).  With deferral now safe for
every spanning leaf, `spanning_defer_vacuum`'s default was flipped to **on**
(guc_tables.c, postgresql.conf.sample, README).  Validation: regress 241/241 +
isolation 122/122 with the default on; a new `progresql_drain` local-index case;
and the structural oracle — a `--local-index` cassert soak (new soak knob,
heapallindexed amcheck of every leaf index) — PASS at 5×120s with churn (30/30
local amchecks clean, 0 dups/crashes).  A 3h x86 local-index soak runs on the NAS
(soak4) as post-push assurance.

**Env caveat (machine-specific):** the PERL5LIB/IPC::Run note above is true on the
WORK machine (Ash Forge) only. On the HOME machine (Otto Loom / volte) IPC::Run is
NOT installed, so the spanning TAP suite (`049_spanning_crash`, `031_spanning`)
can't run here without `sudo cpan IPC::Run` (or brew perl + cpanm). regress +
isolation cover the perf flag's actual surface; the soak covers deferred-drain
correctness; the perf flag touches no WAL/crash path, so TAP was not a gating gap
this session.

**#33 IMPLEMENTED — FK references to a spanning (GLOBAL) PK.** A foreign key can
now target a spanning PRIMARY KEY/UNIQUE. Two gaps closed in tablecmds.c:
`transformFkeyCheckAttrs` matches the referenced columns against the spanning
index's leading `indnuniqatts` user key (not the full `(user,partseq)`), and a new
`fkReferencedPartitionIndex` helper feeds the root spanning index OID to the
referenced-side recursion at every partition level (a spanning index has no
per-leaf child) so per-leaf action triggers are still created — both
`addFkRecurseReferenced` and ATTACH-time `CloneFkReferenced`. No RI-trigger or
enforcement-query change needed. Validated by `progresql_fk` (CHECK + RESTRICT/
CASCADE/SET NULL/ON UPDATE through the root AND direct-leaf, ATTACH, multi-column
key, zero-orphans). regress 242/242, isolation 122/122. Design + as-built:
`docs/plans/C3-FK-to-spanning-PK-design.md`.

**#41 SOAKED — concurrent DROP/CREATE of a spanning index during drains.** Added a
`--ddl-churn` knob to `spanning_soak.sh` that drops and re-creates the tag spanning
index (`st_tag_key`) in a loop while the load and the deferred coalesced drains
run.  This exercises the #40 concurrent-DROP hardening (a drain that snapshotted
the root's index list before the drop hits the `try_index_open`/`try_table_open`
NULL paths), plus `RemoveSpanningDrainqForIndex` on drop and the backfill on
re-create.  Safe under load because the PK on id makes tag (= 't'||id) unique by
construction, so the UNIQUE backfill never finds a dup.  Result: ~39 cycles/round,
0 dups, 0 crashes, amcheck clean on both spanning indexes, only tolerated
DDL-vs-DML deadlocks.  #41 closed.

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

**#35 (found by the soak, fixed `c58f3fb064`) — btree page-recycle crash under
autovacuum.** The first long soak with `--autovacuum on` crashed in ~44s: a
spanning btree page split that recycles a deleted page (`_bt_allocbuf`), and the
btree vacuum recyclability checks, fed the index's "heap" (the storage-less
partitioned root, relkind PARTITIONED_TABLE) to the GlobalVis horizon machinery,
which asserts on relkind (procarray.c). Autovacuum-OFF detector soaks never
created recyclable pages, so this was invisible until the autovacuum-ON run.
Fixed by passing NULL (the documented conservative horizon) for spanning at
`_bt_allocbuf` / `_bt_pendingfsm_finalize` (nbtpage.c) and `btvacuumpage`
(nbtree.c), and relaxing `BTPageIsRecyclable`'s `Assert(heaprel != NULL)`.
Re-verified: autovacuum-ON soak ~3M txns → 0 dups/deadlocks/crashes, amcheck
clean, bounded size; 241 regress + 121 isolation green. (Lesson: a header-only
edit + `objfiles.txt`-based incremental make shipped a STALE binary for ~4 verify
cycles; `touch` the consuming `.c` and `rm src/backend/postgres` after header
changes — see memory `progresql-build-and-agent-hazards`.)

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
