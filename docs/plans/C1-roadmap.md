# C1 Implementation Roadmap

> **Document type:** IMPLEMENTATION ROADMAP. Sequences the C1 re-platform (replace
> the `tableoid` discriminator with an index-local, **persisted** `partseq` + a
> `(index, partseq) → partition` catalog) into independently-compilable,
> independently-testable increments, each with a green-test gate.
>
> **Reads on top of:** `C1-discriminator-design-spike.md` (the design),
> `PRODUCTION_READINESS.md` (the audit), and the per-item plans in this dir.
>
> Branch: **`progresql-c1`** (cut from `progresql-18` @ `f2f0aafe3a`).
> Base: `REL_18_STABLE`. Last updated 2026-05-31.

---

## 0. Correction that supersedes part of the spike (READ FIRST)

The spike's **§3.4, §6 (P0-2 row), and §11** assume partseq can be **re-derived
deterministically on rebuild**, so the catalog map would not need to be dumped.
**A focused probe falsified this.** Findings (all [VERIFIED file:line]):

- The spanning build walks partitions in **canonical partition-bound order**
  (`partdesc.c:342-353`), built from `partition_bounds_create`, **not** catalog
  or attach order.
- The pre-canonical list is **OID-sorted** (`pg_inherits.c:201`,
  `qsort(..., oid_cmp)`) — and OIDs are **not** preserved across logical
  dump/restore.
- Canonical bound order **renumbers survivors** when a middle partition is
  DETACHed, and HASH ordering is `(modulus, remainder)`.
- `pg_inherits.inhseqno` is attach-order, **not dumped**, reassigned on restore,
  and table-scoped — it fails every property partseq needs. **Do not mirror it.**

**Therefore C1 adopts the "persist-and-preserve" model, not "recompute":**

> **partseq is allocated once (max+1) when a partition joins the index, stored in
> the `pg_index_partition` catalog, and that catalog is dumped as data. Index
> entries are rebuilt to AGREE with the restored catalog. partseq is NEVER
> recomputed from walk order.**

Correctness then needs only **intra-database consistency** (the catalog map and
the index entries are written in the same transaction → they agree with each
other), **not** cross-database determinism. This makes build-walk order
irrelevant to correctness and keeps the §6 bug-evaporation claims valid by
construction. Everywhere the spike says "re-derive deterministically," read
"resolve `relid → partseq` from the catalog and emit that."

---

## 1. Guiding principles for the re-platform

1. **Every increment compiles and passes `make check` (233/233 baseline).** No
   increment may leave the tree half-converted and red. Where an increment must
   temporarily support *both* discriminators, it does.
2. **Separate "tidy" from "re-platform."** Increment A is a pure,
   behavior-preserving refactor (the SOLID seams). The discriminator value does
   not change until Increment C. This gives a clean bisect point between "we
   consolidated the checks" and "we changed the on-disk meaning."
3. **Characterization tests first, as red proofs.** Before changing the
   discriminator, land tests that demonstrate the bugs C1 eliminates (VACUUM
   TID-collision, OID-reuse aliasing) as *currently failing/corrupting*. C1 turns
   them green. This is the unambiguous definition of done.
4. **The catalog is the spine.** It is the root of the dependency graph (resolver
   reads it; writer allocates into it; dump preserves it). It lands early, before
   anything depends on a partseq existing.
5. **C1 is independent of the P1-1 opt-in *spelling*.** It keys off the built
   index being spanning (`indnuniqatts > 0` → `RelationIsSpanning`), not off how
   the user requested it. P1-1 can land before or after; do not couple them.
6. **Re-confirm every `file:line` before editing it.** The spike flagged that
   tool truncation blocked a fresh re-read of some citations. Grep first.

---

## 2. The increments

Each increment lists: goal, scope, the test gate that must be green to call it
done, and rough effort. Effort totals match the spike's ~25–27 dev-days.

### Increment A — SOLID seams (pure refactor, ZERO behavior change)
- **Goal:** introduce the descriptor/predicate without changing what's stored.
- **Scope:**
  - Add `RelationIsSpanning(rel)` and `IndexInfoIsSpanning(ii)` to `rel.h`, beside
    `IndexRelationGetNumberOfUniqueAttributes` (`rel.h:541-545`).
  - Add `ii_SpanningKeyAttno` to `IndexInfo`, derived in `BuildIndexInfo` from
    `indnuniqatts`; default `0` for ordinary indexes.
  - **Mechanically replace** every scattered runtime `indnuniqatts ==0/>0` check
    (spike §5.2 enumeration: `execIndexing.c:1214`, `index.c:3851`, `genam.c:249`,
    `vacuumlazy.c:2489`, `tablecmds.c:1287/2038/2154`, `plancat.c:294`,
    `nbtinsert.c:438`) with the predicate. **Leave the creation-time
    `progresql_bypass` opt-in (`indexcmds.c:577/754`) DISTINCT** — it is "user
    asked for spanning," not "this index is spanning."
- **Test gate:** `make check` 233/233 unchanged. No new tests (behavior identical);
  a diff review confirms every replaced check is logically equivalent.
- **Effort:** ~2 d. **Checkpoint commit:** "C1-A: consolidate spanning predicate."

### Increment B — Catalog `pg_index_partition` (no readers/writers yet)
- **Goal:** the spine exists and dumps, before anything uses it.
- **Scope:**
  - New catalog `pg_index_partition` (schema per spike §3.2):
    `indpartidxid`, `indpartseq`, `indpartrelid`, `indpartstate`.
  - PK index `(indpartidxid, indpartseq)`; secondary unique `(indpartidxid,
    indpartrelid)`; syscache entries for both lookups; catversion bump.
  - Genbki/BKI plumbing; empty at this point (no feature writes to it yet).
  - **pg_dump:** emit the catalog rows as data (this is the persist-and-preserve
    contract from §0). Wire it now so later increments inherit dump-correctness.
- **Test gate:** `make check` green; a trivial test that the catalog exists, is
  empty, and survives `pg_dump | psql` round-trip (vacuously).
- **Effort:** ~2 d. **Checkpoint commit:** "C1-B: add pg_index_partition catalog."

### Increment C — Resolver + partseq allocation (dual-write, still tableoid-keyed)
- **Goal:** partseqs get allocated and resolvable, WITHOUT yet changing what the
  index stores. This is the bridge increment.
- **Scope:**
  - `SpanningResolvePartition(index, partseq) → Relation` and the reverse
    `relid → partseq`, over the catalog (spike §5.3).
  - Wire into the per-statement partition cache (`es_progresql_partition_cache`)
    and add the relcache-attached backstop cache + ATTACH/DETACH/REINDEX
    invalidation.
  - **Allocate partseq** (max+1, under the existing build/ATTACH root lock) at
    every site that adds a partition to the index domain: build
    (`indexcmds.c:1189-1200/1348`), ATTACH backfill, CREATE. Populate the catalog.
  - **Do NOT yet change the stored discriminator** — the index still stores
    `tableoid`. The catalog is now populated *alongside* it. (Dual state: catalog
    has partseq; index tuples still have tableoid. Both derivable from the
    partition relid, so they agree.)
- **Test gate:** `make check` green; new test asserts catalog is populated 1..N in
  allocation order on ATTACH, a DETACHed middle partseq is never re-handed out,
  and the resolver round-trips `partseq↔relid`. OID-reuse *infrastructure* test
  scaffolding lands here (red until Increment D).
- **Effort:** ~3 d (resolver+cache) + ~2 d (allocation). **Commit:** "C1-C:
  partseq allocation + resolver (dual-write)."

### Increment D — Flip the discriminator (the actual re-platform)
- **Goal:** the index stores and uses partseq, not tableoid. The payoff increment.
- **Scope (the three chokepoints, spike §4):**
  - **WRITE:** `execIndexing.c:1376-1378` emits `partseq` (from the cache entry)
    instead of `ObjectIdGetDatum(partOid)`; build sites emit partseq in the
    trailing key column. Rename `tableoidKeyPos → discriminatorKeyPos`. Set the
    trailing column's opclass to int4/oid (confirm sort order).
  - **DEREF:** `_bt_check_unique` (`nbtinsert.c:595-597`, `:665-666`) reads the
    trailing datum as partseq and calls the resolver instead of
    `table_open(child_relid)`.
  - **MATCH:** spanning bulkdelete in `vacuumlazy.c` filters on the tuple's partseq
    vs the vacuuming partition's partseq — **this fixes P0-5 (VACUUM TID-collision)
    as a side effect.**
  - DETACH/DROP key the entry-delete on partseq; implement `indpartstate`
    lifecycle (eager delete now; lazy as a documented option).
  - Key-description (`genam.c`/`indexam.c`) renders partseq→partition name in
    violation messages.
- **Test gate (the green flip):**
  - Core cross-partition uniqueness regression **unchanged/green**.
  - **P0-5 VACUUM TID-collision characterization test → now GREEN** (was red).
  - **OID-reuse aliasing test → now GREEN** (impossible to fail by construction).
  - `make check` 233/233 + new spanning tests.
- **Effort:** WRITE ~3 d, DEREF ~1 d, MATCH ~2 d, DETACH/lifecycle ~2 d,
  key-desc ~1 d. **Commit(s):** one per chokepoint, ending "C1-D: store and
  resolve partseq; remove tableoid discriminator."

### Increment E — Migration + dump/upgrade hardening
- **Goal:** existing indexes convert; dump/upgrade is proven, not assumed.
- **Scope:**
  - **REINDEX-only migration** (spike §7): REINDEX of a spanning root populates
    the catalog + emits partseq. Largely free from Increment D. Upgrade note:
    "REINDEX all spanning roots after applying C1."
  - **pg_dump logical round-trip** against the persist-and-preserve model (§0):
    catalog dumped as data; entries rebuilt to agree. This closes **P0-2**.
  - **pg_upgrade TAP** that deliberately does NOT rely on partition OID
    preservation and asserts uniqueness holds — the test that would have caught
    P0-2.
- **Test gate:** `pg_dump|psql` and `pg_upgrade` TAP both green with
  cross-partition uniqueness re-checked post-restore.
- **Effort:** ~1 d (migration) + ~2 d (dump/upgrade). **Commit:** "C1-E:
  migration + dump/upgrade."

### Increment F — amcheck (deferrable)
- **Goal:** structural verification understands partseq + the catalog map.
- **Scope:** amcheck learns the trailing column is a partseq; optional cross-check
  of distinct partseqs against `pg_index_partition`.
- **Test gate:** corrupt a `pg_index_partition` row → amcheck flags divergence.
- **Effort:** ~2 d. **Can ship C1 without F**; without it a corrupt map is silent.

---

## 3. Dependency order (why this sequence)

```
A (seams, no-op) ─┐
                  ├─> C (resolver+alloc) ─> D (flip) ─> E (migration/dump) ─> F (amcheck)
B (catalog) ──────┘
```

- A and B are independent and could be done in either order / parallel.
- C needs B (catalog to allocate into) and benefits from A (predicate to gate on).
- D needs C (a partseq must exist and be resolvable before the index can store it).
- E needs D (there must be partseq-keyed entries to dump/migrate).
- F needs D (verifying the partseq representation).

**Characterization tests** (P0-5 collision, OID-reuse) are written as **red** in
Increment C's scaffolding and flip **green** in Increment D — they are the
acceptance proof for the whole effort.

---

## 4. Out of scope for C1 (tracked elsewhere)

- **P0-1** ON CONFLICT/MERGE arbiter — orthogonal to the discriminator; unchanged
  by C1. Separate workstream.
- **P0-3** crash/WAL review of bulk DDL — C1 slightly improves it (no OID-aliasing
  orphans) but the full review is its own plan.
- **P0-4** concurrency/lock-ordering — C1 changes *how* the probe finds the
  partition, not the lock ordering; isolation specs still owed.
- **P1-1** explicit opt-in syntax — independent; C1 keys off the built index.
- **CONCURRENTLY** (CIC / DETACH CONCURRENTLY) — partseq allocation race unsolved;
  matches gbtree and ProgreSQL today. Documented precondition, not built.
- **Vacuum *perf*** (deferred single end-of-tree pass) — C1 fixes the vacuum
  *correctness* (partseq filter); the O(N²) perf shape is a follow-on (P2-3 /
  the RFC's headline). Note it; don't block C1 on it.
- **Planner read-acceleration** (P3-1) — C1's resolver is the seam a future
  Global Index Scan would reuse, but the scan itself is "B"-adjacent and out of
  scope.

---

## 5. First action

**Increment A, step 1:** re-confirm the spike §5.2 citation list against the tree
(grep each `indnuniqatts` site), then add `RelationIsSpanning` to `rel.h` and
begin the mechanical predicate consolidation. No behavior change; `make check`
must stay 233/233.
