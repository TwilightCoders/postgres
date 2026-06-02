# C2 Campaign Status — "PG-Core-presentable" hardening (READ FIRST after compaction)

> Master tracker for the spanning-index hardening campaign on branch
> `progresql-c1`. Written 2026-06-01 as a pre-compaction checkpoint. If you are a
> fresh context: read this, then `C2-deferred-vacuum-DHR-design.md` (the perf
> fix, the #1 user priority), then resume the "NEXT" list.

## Standing user directives (this sprint)
- **Max effort / "weapons free"**: use sub-agents/workflows freely; token cost is
  not a constraint; be exhaustive and surgical.
- **#1 priority = PERF.** The O(N²) spanning-VACUUM sweep is unacceptable to the
  user ("cannot allow for it") and is NOT stock-PG behavior — it is specific to
  the single cross-partition index. Must be fixed → that is the **DHR** build.
- Commit only working, tested increments (each compiles + `make check` green).
  Commit messages: no Claude attribution.

## DONE this session (all committed, 238/238 regression tests green)
- `ebd1a0e9cb` **P1-1a: explicit `GLOBAL` syntax (additive)** — `CREATE UNIQUE
  INDEX … GLOBAL`, `{PRIMARY KEY|UNIQUE} (…) GLOBAL`; internal flag
  `IndexStmt.isglobal`/`Constraint.isglobal` (appended at struct END — see
  BUILD HAZARD). Legacy implicit handshake still works (OR'd in). Test:
  `progresql_global`.
- `c7d40bef36` **E7 / P0: HOT key-change corruption fix** — VERIFIED silent
  uniqueness corruption in shipped code. A spanning-key UPDATE on a leaf was
  HOT-eligible (leaf has no local index on the column) → stale spanning entry →
  permanent false cross-partition conflict. Fixed in
  `RelationGetIndexAttrBitmap` (relcache.c): a leaf with a spanning ancestor now
  adds that index's user-key cols to its HOT-blocking set (catalog scans only;
  both "no local index" fast-path exits gated on
  `progresql_leaf_has_spanning_ancestor`). Test: `progresql_hot`. **This also
  removed the duplicate-(partseq,TID) entries that had made the DHR perf design
  "broken" → DHR is now unblocked.**
- `de985525d8` **B1: latent VACUUM assert-abort fix** — `bt_spanning_bulkdelete`
  passed the partitioned ROOT as `ivinfo.heaprel`; `BTPageIsRecyclable →
  GlobalVisHorizonKindForRel` Asserts relkind ∈ {r,m,t}, so the root aborted
  assert builds once the spanning index had recyclable pages. Now passes the
  leaf (`vacrel->rel`).
- Earlier in session (pre-ultracode): C1-D2 crash-safe partseq VACUUM
  (`bc49f971c8`, `66c9c0a3d9`), perf characterization (`a4cd26e2b1`).

## NEXT (priority order)
1. **E5 / DHR — deferred spanning VACUUM, O(N²)→O(N). USER #1. XL.** Full design:
   `docs/plans/C2-deferred-vacuum-DHR-design.md`. Now unblocked by E7. Subsumes
   antagonist B2 (leaf-only delete primitive) and the per-leaf full-scan.
2. **E2 / P0-2 — dump + `pg_upgrade`.** The ONE real silent-data-loss path:
   `pg_get_indexdef` (ruleutils.c ~1398 loop over `indnatts`) emits the trailing
   discriminator → logical restore silently downgrades to a plain index. Fix:
   ruleutils clip trailing col + emit `GLOBAL` (now that P1-1 exists);
   `binary_upgrade_set_index_partition_map(...)` in pg_upgrade_support.c (mirror
   `binary_upgrade_add_sub_rel_state`), gate `SpanningGetOrAllocPartseq` off under
   `IsBinaryUpgrade`; pg_dump `getIndexes` SELECT `indnuniqatts` + emit the calls;
   TAP for pg_dump round-trip + pg_upgrade re-checking cross-partition uniqueness.
   Plan: `docs/plans/P0-2-dump-upgrade.md`.
3. **E6 / M1 — real `int4` discriminator column** (retire `tableoid`-carrier).
   Recipe: `docs/plans/M1-int4-discriminator-recipe.md` (option b1, M-effort).
   Upstream type-honesty gate; aligns with Dilip-2025.
4. **E1b — retire implicit INHERITS handshake**, migrate all `progresql*.sql`
   tests + README to `GLOBAL` (drop INHERITS/base, inline cols, add `GLOBAL`).
   Removes `has_superclass` term at `indexcmds.c:755` + `parse_utilcmd` allowance.

## VERIFIED findings (live repros, not assumptions)
- **HOT key-change = P0** (fixed, E7). Repro is `progresql_hot`.
- **Cross-partition uniqueness, DELETE-then-reinsert, UPDATE row-movement, NULL
  distinctness, ON CONFLICT** = all behave correctly OR fail-closed. Recon
  agents OVERSTATED 3 "bugs" that live repros disproved (DELETE cleanup, UPDATE
  movement, NULL). **Lesson: always verify agent-claimed bugs with a live repro
  before fixing.** The antagonist + design-panel adversary were grounded/correct.
- **ON CONFLICT / MERGE**: hard-errors ("no unique or exclusion constraint
  matching") — unsupported, NOT silent corruption. Lower urgency. Arbiter
  inference (plancat.c `infer_arbiter_indexes`) never sees the root spanning
  index from a leaf. Feature gap, not a P0.
- **partseq allocation race** (concurrent ATTACH): PK backstop turns it into a
  spurious duplicate-key error (retriable), NOT silent corruption.
- **Multi-level (sub-)partitioning is BROKEN**: partseq allocated for direct
  children only; INSERT into a grandchild errors "has no partseq for partition".
  Either hard-error at DDL or support recursively. (Untracked task — add it.)
- **Strategic**: C1 (partseq + `pg_index_partition`) ≈ **Dilip Kumar/Google's
  live June-2025 in-core "Global Index" proposal** (msg
  CAFiTN-uyec_y2QS2whUam8Rp1M+PPGuP0Zz45uX_U6mhQ8mtRg), which is Haas-blessed.
  `GLOBAL` is the syntax Oracle + both PG proposals use and is already an
  unreserved keyword. README/RFC still say "Camp C / tableoid" — STALE; update to
  reflect Camp-B/Dilip alignment when touching docs.

## BUILD HAZARD (cost me time twice — DO THIS)
The incremental `make` does NOT reliably rebuild all consumers of a changed core
header (e.g. `parsenodes.h`, `nbtree.h`). After ANY struct/header change, either
(a) append new struct fields at the END (so existing field offsets never shift),
AND (b) **`make clean && make -j8 && make install`** (full rebuild) before
trusting a run. Symptoms of stale-.o: bogus asserts (`stmt->isconstraint`),
`initdb` SIGABRT, fields reading as garbage. Confirmed: `USE_ASSERT_CHECKING 1`
in this build, so asserts fire.

## Build / test / repro quickfacts
- Build: `make -C src/backend && make -C src/backend install` (or full `make`).
- Run suite: `cd src/test/regress && make check` (target: 238 passed).
- Test `.sql` files are gitignored (`~/.gitignore_global: *.sql`) → `git add -f`.
- Scratch cluster: `build/install/bin/{initdb,pg_ctl,psql}` on port 54399.
- Spanning predicates: `RelationIsSpanning(rel)` / `IndexFormIsSpanning(pgindex)`
  (`indnuniqatts>0`). Trailing key col is physically `tableoid` (attno -6) but
  stores an int32 partseq (M1 fixes that). `pg_index_partition` maps
  (idx oid, partseq) → partition oid (oids 560/561/562).
- Resolver: `SpanningResolvePartseqRelid`, `SpanningLookupPartseqByRelid`,
  `SpanningGetOrAllocPartseq` (catalog/pg_index_partition.c).

## Background-agent outputs (EPHEMERAL /tmp — distilled into docs already)
- design panel (DHR) → `C2-deferred-vacuum-DHR-design.md`
- antagonist review → `C2-antagonist-review-C1D2.md`
- m1-scout → `M1-int4-discriminator-recipe.md`
