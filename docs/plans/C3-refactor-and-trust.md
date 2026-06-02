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

## Recommended next-session order
1. (B) testing #1 concurrency, #2 recovery, #3 amcheck — each verified.
2. (A) BLOCK 5 vacuumlazy extraction, with the new tests as the safety net.
3. Then perf fix → soak → relax the flag.
A Workflow fits (B) well (parallel authoring of isolation specs / recovery tests
/ amcheck), but keep it surgical — these are tests, not new core subsystems.
