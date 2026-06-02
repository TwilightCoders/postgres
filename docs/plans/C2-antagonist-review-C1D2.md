# C2 — Antagonist review of C1-D2 (vacuum/partseq), with disposition

> Hostile pgsql-hackers-style review of commits `bc49f971c8` + `66c9c0a3d9`.
> Each finding tagged with current status. Grounded and trustworthy (B1 and the
> HOT issue were independently verified by live repro). 2026-06-01.

## Blockers
- **B1** — `BTPageIsRecyclable` Asserts on the partitioned-root `heaprel`
  (`GlobalVisHorizonKindForRel` relkind assert). **FIXED** `de985525d8` (pass the
  leaf as heaprel). Verified: churn repro (delete 59k + double-VACUUM) no longer
  aborts.
- **B2** — `bt_spanning_bulkdelete` runs full `btvacuumscan` page-deletion
  machinery on an index whose root never gets `amvacuumcleanup` → metapage
  bookkeeping never finalized (page recycling still happens per-call via
  `_bt_pendingfsm_finalize`, so severity is bloat/stat lag, not a hard leak).
  **FOLD INTO DHR** (E5): the drain uses a leaf-only delete primitive OR a proper
  coalesced index vacuum that calls `btvacuumcleanup`. See DHR design §8.
- **B3** — lock protocol: vacuum takes SUEL on the spanning index; the DDL
  cleanup path (`progresql_clean_spanning_indexes_for_partition`) takes
  RowExclusiveLock + `kill_prior_tuple`; SUEL ⊥ RowExclusive don't conflict, so
  vacuum-vs-DDL on the same index aren't serialized, and `_bt_start_vacuum`'s
  "multiple active vacuums" elog only catches vacuum-vs-vacuum. Page-level buffer
  locks make this not-corrupting today, but the protocol is unclean.
  **STANDALONE TODO** — unify on one self-conflicting lock + shared helper, and
  audit DHR's drain vs DDL cleanup. (Re-verify whether any real corruption window
  exists with a concurrent isolation spec before investing.)

## Majors
- **M1** — discriminator overloads the `tableoid` system column (int4 value in an
  oid/oid_ops column). **PLANNED E6** → `M1-int4-discriminator-recipe.md`.
- **M2** — `nindexes==0` spanning branch has no wraparound-failsafe escape (full
  index scan under failsafe). **FOLD INTO DHR** (enqueue is cheap; skip drain
  under failsafe). Until DHR, a small standalone fix could gate the branch on
  `lazy_check_wraparound_failsafe`.
- **M3** — `vacrel->num_index_scans++` for the nindexes==0 spanning leaf is a
  user-visible stat lie (VERBOSE prints "index scan not needed" while the counter
  shows 1). **FOLD INTO DHR** (use an internal flag, not the public counter).
- **M4** — `progresql_rel_has_spanning_ancestor` (and the E7 relcache helper) walk
  ancestors+catalog on every cold vacuum/relcache build of every partition, incl.
  non-spanning. Cold-path + memoized, but on a 1000-partition non-spanning table
  it is real overhead. **STANDALONE TODO** — memoize "has spanning ancestor" on
  the relcache entry / short-circuit. (Applies to both vacuumlazy and the E7
  relcache helper.)
- **M5** — the headline crash-safe deletion is under-tested (only sibling-survival
  is asserted; a delete-nothing impl would pass). **FOLD INTO DHR tests** (E5 §5):
  positive deletion + posting-list + crash/restart coverage.

## Minors
- **m1** — the "fields at end of struct for ABI stability" comment on `BTVacState`
  is misleading (private in-process struct; it's a stale-.o hazard, not ABI).
  STANDALONE doc fix.
- **m2/m3/m6** — `bt_spanning_bulkdelete` clone of `btbulkdelete` + per-item
  `spanning` branch in the hot `btvacuumpage` loop. **FOLD INTO DHR** (leaf-only
  primitive removes the clone + the hot-loop branch).
- **m4** — `drop_map` bare-bool flag arg on
  `progresql_clean_spanning_indexes_for_partition`. STANDALONE nicety (enum or two
  wrappers). All 4 call sites are correct.
- **m5** — add `Assert(RelationIsSpanning(info->index))` to the spanning delete
  routine. STANDALONE.

## Credit (correct as-is, per the antagonist)
Core diagnosis (partition-local TIDs need a discriminator gate); index-before-heap
ordering in C1-D2; WAL-logged `_bt_delitems_vacuum` over kill-hints; posting-list
gate; NULL/unmapped-partseq handled fail-safe; `drop_map` semantics + ordering;
`ivinfo` fully initialized; balanced lock cleanup in `_bt_check_unique`.

## Standalone TODOs not yet folded anywhere (track these)
B3 (lock protocol), M4 (memoize has-spanning-ancestor), m1 (comment), m4 (enum),
m5 (assert). None are correctness-blocking today; do as a cleanup pass.
