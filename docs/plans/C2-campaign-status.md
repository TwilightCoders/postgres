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

## E5 / DHR build progress (USER #1, in progress)
Full design: `docs/plans/C2-deferred-vacuum-DHR-design.md`. 5 increments.
- **inc1 DONE** `cd857bbeb0` — `pg_spanning_drainq` catalog (OID 563, PK index
  564, syscache SPANNINGDRAINQ), `RemoveSpanningDrainqForIndex` wired into
  `index_drop`. Empty/unconsulted; clean rebuild; 238/238 green; schema
  smoke-tested on a scratch cluster. **Two deliberate deviations from the design
  appendix, both toward upstream-legibility (doc updated to match):** (a)
  `sdq_enqueue_xid` is `TransactionId`/`xid` (32-bit, as `relfrozenxid`), NOT
  `xid8` — drainq rows are transient (drained within a drain-interval by the age
  trigger) so 32-bit + `TransactionIdPrecedes` is sufficient and idiomatic, and
  avoids a `Catalog.pm` type-map addition + being the first xid8 catalog column;
  (b) ONE PK index `(sdq_idxid, sdq_partseq)`, NOT two — the PK's leading column
  already serves the drain's "all pending for this root" range scan (as
  `spanning_max_partseq` range-scans `pg_index_partition`'s PK), so a separate
  `(sdq_idxid)` index is pure redundancy.
- **inc2 NEXT** — enqueue + reap-suppression. KEY: a spanning leaf's *entire*
  dead set is spanning-relevant (root index references every partition row), so
  DHR defers ALL heap reaping to the drain. inc2-without-drain would leave
  spanning entries un-retired → false cross-partition conflicts → would BREAK
  `progresql_vacuum_collision`. Therefore **DHR must land behind a default-OFF
  GUC** (eager path preserved) so each increment is `make check` green; flip the
  GUC on for new tests in inc5, and make it the default once the full drain is
  validated. The 3 `lazy_vacuum` sites are vacuumlazy.c:2678 (nindexes==0, the
  whole heap-reap is deferred → skip `lazy_vacuum_heap_rel`, don't bump
  `num_index_scans`/M3), :2769 (bypass; already no heap reap), :2785 (normal;
  skip the paired `lazy_vacuum_heap_rel` for spanning leaves under DHR).
- **inc2 DONE** `c20f8b09bf` — GUC `spanning_defer_vacuum` (default off); enqueue
  + reap-suppression at all 3 `lazy_vacuum` sites; `SpanningDrainqEnqueue`/
  `SpanningDrainqHasPending`. 238 green (GUC off = byte-identical eager path).
- **inc3 DONE** `6d68f86e7f` — the coalesced drain: `bt_spanning_drain` +
  `BTSpanningDrainKill` (multi-partseq btree gate), `progresql_drain_spanning_index`
  (SUEL index + per-partition SUEL in OID order; ONE index scan; drain-driven
  reap; index-before-heap in one txn), SQL `pg_drain_spanning_index(regclass)
  -> bigint`, catalog readers `SpanningDrainqListDirty`/`DeleteList`. Verified:
  single + multi-partition coalesced drain retire all stale entries in one scan,
  cross-partition uniqueness airtight pre/post drain, reuse trap closed, 238
  green. **Bug caught & fixed: the deletion loop must also run when
  `spanning_kill` is set (drain passes callback==NULL) else drain deletes
  nothing.** **Audit result (LP_DEAD->LP_UNUSED sweep): inc2 reap-suppression is
  the ONLY new gate needed — on-access prune never marks LP_DEAD unused
  (pruneheap.c:263,374), PageAddItemExtended reuses only LP_UNUSED
  (bufpage.c:273); the drain is the sole reaper of spanning-leaf slots.**
- **inc3a DONE** `aabfafa292` — fixed an AB-BA deadlock the adversarial review
  caught (B1): the drain locked index-before-partition but the eager path locks
  partition-before-index. Fix: drain now locks partitions (OID order) THEN the
  index, matching the eager order; resolves partseqs via `…ByOid` (no index
  lock); + recovery guard, try_table_open for vanishing partitions, VM/lock
  comments. Review verdict otherwise: no other blockers; crash windows all
  re-runnable; on-access prune never reaps held slots; memory/temp-horizon
  concerns moot.
- **inc5 (regress+perf) DONE** `f45787f780` — regress test `progresql_drain`
  (single/multi/reuse-trap/sibling/idempotent/negative); perf characterization in
  PRODUCTION_READINESS: per-leaf cost flat at 72 (deferred) vs 173..865 growing
  (eager); sweep O(N²) 55,360 → O(N) ~5,401 at N=64 (~10×, widening). **The
  user's #1 (kill O(N²)) is proven.** 239 tests green. Note: pre-drain reinsert
  of a deleted key SUCCEEDS (correct — `_bt_check_unique` heap-liveness probe
  treats a stale-over-dead entry as non-conflict); the drain's job is to retire
  the entry before its held slot can be reused. Remaining inc5 hardening
  (isolation specs + TAP crash test) tracked separately (task #27, deferred).
- **inc4 DONE** `e89632ba7b` — autovacuum autonomy: `AVW_SpanningIndexDrain`
  work-item + `perform_work_item` handler + label; leaf vacuum nudges via
  `AutoVacuumRequestWork` (now deduped so the per-leaf re-request can't exhaust
  the work-item array). Verified: a deferred VACUUM's queue auto-drains within
  ~2s with autovacuum on. (`VACUUM <root>` end-of-command hook + launcher
  periodic sweep deferred as robustness follow-ups; the work-item nudge + leaf
  re-request give autonomy.)

## E5 / DHR — COMPLETE (all increments committed, 239/239 green)
The user's #1 (kill the O(N²) spanning-VACUUM sweep) is DONE and proven
(~10× at N=64, widening). DHR is behind GUC `spanning_defer_vacuum` (default
off), restricted to **no-local-index spanning leaves** (the common
cross-partition-PK case) — see BLK-2 below.

### Adversarial review of the drain found + fixed TWO real blockers (live repros)
- **BLK-1 (pre-existing C1 crash, not E5)** `d713843145` — duplicate-key churn
  against ANY spanning index SIGSEGV'd: the pre-split LP_DEAD cleanup
  (`_bt_delete_or_dedup_one_page`) ran heap-probing simple/bottom-up deletion
  passing the AM-less partitioned ROOT as heapRel. Fix: skip that whole
  optimization for spanning indexes (leaf just splits; VACUUM/drain retire dead
  entries). Build-time dedup already off for unique indexes, so no posting lists
  ever form for spanning indexes.
- **BLK-2 (E5 corruption)** fixed in `e89632ba7b` — the drain reaped EVERY
  LP_DEAD slot, orphaning a live LOCAL-index entry for a slot pruned on-access
  after the leaf's index vacuum (verified: index scan 426 vs seq 0). Fix:
  restrict DHR deferral to `nindexes==0` leaves; local-index leaves use the
  eager path so the drain never reaps their slots.
- MAJ-2 (ownership check on `pg_drain_spanning_index`), MAJ-1 (removed dead
  `SpanningDrainqHasPending`), MIN-6 (reap bound assert) — all fixed. Review
  verified SAFE: WAL index-before-heap ordering, crash re-runnability,
  PageAddItemExtended reuse-only-LP_UNUSED, `_bt_check_unique` heap probe.

### Remaining DHR follow-ups (non-blocking, deferred)
- inc5b (task #27): isolation specs (drain∥DML, drain∥eager-VACUUM = B1 guard) +
  TAP crash test.
- `VACUUM <root>` end-of-command drain hook; launcher periodic sweep.
- Extend deferral to local-index leaves (needs the reap to only retire slots
  whose local entries are also gone — e.g. drain vacuums local indexes too, or a
  wired per-(idxid,partseq) reap gate). Today they correctly fall back to eager.
- MAJ-3 (peak memory = Σ dirty-partition dead TIDs; bounded like a vacuum but
  could batch); MIN-2 (proactively clear drainq on partition DROP/DETACH);
  MIN-4 (drain skips pending-FSM page recycle); MIN-5 (`retired` counts TIDs).

## E2 / P0-2 — DONE `c1c7fee4ef` (the silent-data-loss path, fixed + tested)
pg_dump emitted spanning indexes/constraints with the trailing discriminator
exposed (`PRIMARY KEY (city_id, tableoid)`), so restore/upgrade rebuilt a PLAIN
index → cross-partition uniqueness silently lost. Fix (Option A1): ruleutils
(`pg_get_indexdef`/`pg_get_constraintdef`) and pg_dump (`dumpConstraint`, which
builds PK/UNIQUE itself) now clip the discriminator + emit `GLOBAL`;
`decompile_column_index_array` got a `maxcols` arg (render clipped, return full
so INCLUDE math still skips the discriminator); pg_dump learns `indnuniqatts`
(new IndxInfo field + getIndexes column, fork servers >=180000). Verified:
dumped+restored DB rejects a cross-partition dup the original rejected; regress
`progresql_dumpdef` (240 tests). Follow-ups (NOT data-loss): pg_dump|psql +
pg_upgrade TAP tests; psql `\d` spanning annotation (P2-2). Note the pg_dump
query gates indnuniqatts at remoteVersion>=180000 — fork pg_dump targets fork
servers (vanilla-18 dump would need a feature probe; documented).

## E6 / M1 — DONE `bc90284b69` (honest int4 discriminator)
The trailing spanning key column was the tableoid system slot (oid/oid_ops) but
held an int32 partseq (scankey F_INT4EQ) — a type lie `\d` exposed as "tableoid".
Fixed (option b1, no on-disk change — int4≡oid byte layout): indexcmds.c uses
int4_ops + name "partseq"; index.c `ConstructTupleDescriptor` stamps the trailing
spanning column's pg_attribute as INT4OID (gated on ii_NumUniqKeyAtts>0 + last key
col == attno -6, so normal indexes untouched); genam comment. Verified: column is
int4/int4_ops/"partseq", uniqueness + VACUUM work, 240 green. No catversion bump
(runtime-only; old oid_ops indexes still function, REINDEX relabels — optional).

## E1b — DONE (`6cacae7351` migrate tests, `f152021e4e` remove handshake)
GLOBAL is now the SOLE spanning opt-in. `progresql_bypass = stmt->isglobal`
(dropped the `has_superclass` term); restored stock PG's parse_utilcmd guard
(INHERITS+PARTITION BY → "cannot create partitioned table as inheritance child");
all `progresql*` tests migrated to inline `GLOBAL`; `progresql.sql` rewritten +
Section 13 asserts the retired handshake errors; `create_table.out` restored to
the vanilla error. README updated. 240 green.

## ALL LETTERED PRIORITIES DONE (E1, E2, E5, E6, E7, E1b). Remaining = polish.
## NEXT (priority order)
1. **E2 follow-ups (NOT data-loss):** pg_dump|psql TAP + pg_upgrade TAP (binary
   upgrade — covered by the dumpConstraint fix by construction, but unverified;
   **needs `./configure --enable-tap-tests`, NOT currently set** → can't run via
   `make check` here). **P2-2 (psql `\d`): mostly already done** — `\d <table>`
   shows `PRIMARY KEY, btree (id) GLOBAL` (free from the E2 ruleutils fix), so
   spanning is clearly indicated. Remaining minor wart: `\d <spanning_index>`
   lists the internal `partseq` column with a BLANK Definition (E2 clips its def
   but describe.c still lists it from pg_attribute). Fix = filter the trailing
   discriminator from describe.c's index-column query, but that needs the same
   server-version-gated `indnuniqatts` plumbing as pg_dump (describe.c runs vs
   many server versions). Low priority, cosmetic.
2. **Sub-partition DDL hard-error — DONE `35d1da2cbd`.** Multi-level
   partitioning now rejected at DDL in both directions: `CREATE TABLE child
   PARTITION OF <spanning-root> … PARTITION BY …` errors (tablecmds.c), and
   building a GLOBAL index on a pre-existing multi-level tree errors
   (indexcmds.c — replacing a SILENT skip that omitted grandchild rows from the
   index → had left their uniqueness unenforced; real correctness gap closed).
   progresql.sql Section 14; 240 green.
3. **E5 inc5b (task #27):** isolation specs (drain∥DML, drain∥eager-VACUUM) +
   TAP crash test — isolation needs `make -C src/test/isolation check`; TAP needs
   `--enable-tap-tests`.
4. **DHR follow-ups:** `VACUUM <root>` end-of-command drain hook; launcher
   periodic sweep; extend deferral to local-index leaves; antagonist standalone
   TODOs (B3 lock protocol, M4 memoize has-spanning-ancestor, m1 comment).

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
- **Multi-level (sub-)partitioning — now REJECTED at DDL (`35d1da2cbd`).**
  Originally fail-closed at INSERT (verified); now hard-errors at DDL in both
  directions (sub-partition under a spanning root; GLOBAL on a pre-existing
  multi-level tree). The GLOBAL-build path had a SILENT skip of sub-partitioned
  children (grandchild rows omitted → uniqueness unenforced) — that real gap is
  closed by the error. Supporting multi-level recursively remains possible future
  work, but rejection is the correct, presentable default.
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
