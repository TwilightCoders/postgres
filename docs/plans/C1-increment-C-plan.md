# C1 Increment C — Resolver + partseq Allocation (the bridge)

> **Status:** PLAN for review (no code yet). Increment C of the C1 roadmap.
> Branch `progresql-c1` @ C1-B (`25a8e6776d`). Base `REL_18_STABLE`.
>
> **The bridge increment.** After C, partseqs are *allocated and resolvable*,
> but the index still *stores* tableoid. Catalog rows and index entries are
> **dual-tracked** — both derivable from the partition relid, so they agree.
> The discriminator flip itself is Increment D. C must stay zero-behavior-change
> on the read path: 233/233.

---

## 0. Goal & non-goals

**Goal:** stand up the writer (partseq allocation → `pg_index_partition`) and the
reader (resolver: `partseq ↔ partition`), wired to caches, populated at every
site where a partition joins a spanning index's domain — **without** changing
what the index stores.

**Non-goals (deferred to D and beyond):**
- Changing the trailing key column value (stays tableoid in C).
- VACUUM/`_bt_check_unique` consuming partseq (D).
- `indpartstate` lifecycle / lazy DETACH (later).
- CONCURRENTLY allocation races (out of scope, documented).

---

## 1. The model (locked by the partseq probe)

**Persist-and-preserve, never recompute.** partseq is allocated `max+1` once when
a partition first joins a spanning index, written to `pg_index_partition`, and
that row is authoritative thereafter. Correctness needs only **intra-DB
consistency** (catalog row + future index entry written in the same txn). Build
walk order is irrelevant. (See `C1-roadmap.md` §0; probe findings.)

---

## 2. The partition-join sites (where allocation must happen)

A partition enters a spanning index's domain at exactly these sites. Each must,
in the same transaction that adds it, allocate a partseq and insert a catalog row
(idempotently — see §4).

| Site | File:line (RE-CONFIRM) | When | Lock context |
|------|------------------------|------|--------------|
| **CREATE INDEX (spanning)** | `indexcmds.c` `BuildSpanningIndexFromPartitions` (1308), write at 1341 | build over all existing partitions | idx `RowExclusiveLock`, parts `AccessShareLock` |
| **REINDEX** | same `BuildSpanningIndexFromPartitions`, via `index.c:3851` | rebuild | idx `AccessExclusiveLock` |
| **ATTACH** | `tablecmds.c` `progresql_backfill_spanning_indexes_for_attached_partition` (~2152) | one partition | strong ATTACH locks |
| **CREATE TABLE … PARTITION OF** | ATTACH path (same as above) | one partition | strong locks |

> **REINDEX subtlety (important):** REINDEX must **reuse** a partition's existing
> partseq from the catalog, not allocate a new one (allocating new would
> renumber and, in D, would orphan stored entries). So allocation is really
> **"get-or-allocate"**: look up `(idxid, relid)`; if found, reuse; else `max+1`.
> This is why the writer is `SpanningGetOrAllocPartseq`, not a blind insert.

---

## 3. The SOLID surface added in C

### 3.1 Writer — `SpanningGetOrAllocPartseq`
```c
/* Returns the partseq for (spanningIndex, partition), allocating + inserting a
 * pg_index_partition row on first sight. Caller must hold a lock on the index
 * that serializes concurrent joiners (build/ATTACH already do). */
int32 SpanningGetOrAllocPartseq(Relation spanningIndex, Oid partitionOid);
```
- Lookup `(indpartidxid, indpartrelid)` via the `INDEXPARTITIONREL` syscache.
  - **found** → return existing `indpartseq` (REINDEX/idempotent re-run).
  - **not found** → `partseq = SpanningMaxPartseq(idxid) + 1`; `CatalogTupleInsert`
    a row `(idxid, partseq, relid)`; return partseq.
- `SpanningMaxPartseq` = scan `pg_index_partition` for this idxid, max(indpartseq),
  0 if none. (Small N; an index-scan on the PK suffices. Never reuses a gap →
  monotonic per index.)
- **No reuse on DETACH** (rows deleted, but max keeps climbing — see §6 ceiling).

### 3.2 Reader — `SpanningResolvePartition`
```c
/* Map a stored partseq back to its partition relid (and open it if requested).
 * NULL/InvalidOid for an unknown partseq (detached/garbage). */
Oid SpanningResolvePartseqRelid(Relation spanningIndex, int32 partseq);
```
- Lookup `(indpartidxid, indpartseq)` via the `INDEXPARTITIONSEQ` syscache →
  `indpartrelid`.
- A Relation-returning convenience wrapper (`table_open`) lives at the call site
  in D (it owns the lock/lifetime); C provides the relid mapping only, since C
  has no consumer of the opened relation yet.

### 3.3 Header / placement
- Declarations in a new `src/include/catalog/pg_index_partition.h` companion or
  in `catalog/index.h` (TBD — lean: put the get-or-alloc + resolver protos in
  `pg_index_partition.h` next to the catalog, with the impl in a new
  `src/backend/catalog/pg_index_partition.c`). **Decision needed (Q1).**

### 3.4 Cache wiring (the per-statement cache)
- Add `int32 partseq` to `ProgresqlSpanningEntry` (`execIndexing.c:1095`),
  resolved **once** when the cache entry is built (`:1224`), so the future hot
  path (D) reads it O(1) with zero per-row catalog hits.
- In C the field is *populated but unused* by the write path (still writes
  tableoid). This is the one deliberate "carry before consume" — justified
  because the cache entry is exactly where D will need it, and populating it in C
  lets us **test the resolver against the cache** now.
- **Relcache backstop:** VACUUM and `_bt_check_unique` (D consumers) have no
  EState. Plan a small relcache-attached `(partseq→relid)` cache invalidated on
  ATTACH/DETACH/REINDEX. **Defer the backstop to D** (no C consumer); C uses the
  syscache directly. Noted here so D doesn't forget.

---

## 4. Idempotency & races

- **Get-or-allocate is idempotent:** re-running build/REINDEX over an
  already-mapped partition returns the existing partseq, inserts nothing.
- **Blocking-path race safety:** build holds `RowExclusiveLock`+ on the index;
  ATTACH holds strong locks. Two concurrent joiners of the *same* index serialize
  on that lock, so `max+1` read-then-insert cannot interleave. The
  `(idxid, relid)` unique index (OID 562) is the **backstop**: a duplicate insert
  raises, converting any missed serialization into a hard error rather than a
  duplicate partseq. [ASSUMPTION — confirm the lock is held across the whole
  build loop, not dropped per-partition.]
- **CONCURRENTLY:** explicitly unsupported (matches gbtree/today). Documented.

---

## 5. Characterization tests (land RED in C, flip GREEN in D)

Per the roadmap, C lands the failing proofs of the bugs D eliminates:

1. **`progresql_oid_reuse`** — force OID recycling, attach a partition that gets a
   recycled OID matching a previously-dropped partition, insert a key the dropped
   partition used → assert no false conflict. RED today (tableoid aliases); GREEN
   after D (partseq never reused). In C, also assert the catalog assigns a *fresh*
   partseq (not the recycled OID).
2. **`progresql_vacuum_collision`** (isolation or regress) — two partitions, live
   rows at the same heap TID; vacuum one; assert the other's spanning entry
   survives. RED today (TID-only delete); GREEN after D (partseq-filtered).
3. **`progresql_partseq` (new, GREEN in C — C's own gate)** — the C-level
   contract: ATTACH N → partseqs 1..N; DETACH middle → its partseq not re-handed;
   resolver round-trips `partseq↔relid`; REINDEX preserves partseqs.

> **Carrying mechanism (LOCKED, Q2):** tests 1–2 are written to
> `src/test/regress/{sql,expected}/` but **NOT added to `parallel_schedule`** in
> C — they exist in the tree, dormant, so `make check` neither runs nor is broken
> by them. D adds them to the schedule; they must pass on arrival (that passage is
> the proof). Only `progresql_partseq` is scheduled in C.

---

## 6. Risks / open items

- **partseq int32 ceiling** — 2^31 attaches per index before REINDEX resets.
  Non-issue in practice; document.
- **Catalog rows for pre-existing indexes** — an index built *before* C has no
  catalog rows. C's get-or-allocate populates lazily on next build/REINDEX/ATTACH;
  a fresh cluster (our test path) populates at CREATE INDEX. For D's correctness a
  spanning index MUST be fully mapped before it stores partseq → **D begins with a
  forced REINDEX/backfill of the map.** Noted for D.
- **`allow_system_table_mods`** — inserting into `pg_index_partition` from C code
  uses `CatalogTupleInsert` (normal for catalog maintenance, e.g. how
  `StoreSingleInheritance` writes pg_inherits) — not a user INSERT, so no GUC
  needed. [VERIFIED pattern — pg_inherits.c StoreSingleInheritance].
- **`tableoidKeyPos` rename** — the spike wants `discriminatorKeyPos`; do the
  rename in D (where semantics change), not C, to keep C's diff about *adding*.

---

## 7. Step order (each step compiles + `make check` green)

1. `pg_index_partition.c` + protos: `SpanningMaxPartseq`, `SpanningGetOrAllocPartseq`,
   `SpanningResolvePartseqRelid`. Build (no callers yet). ~1 d
2. Call get-or-alloc at the build site (`BuildSpanningIndexFromPartitions`) and
   ATTACH backfill — populate the catalog. Still stores tableoid. ~1.5 d
3. Add `partseq` to `ProgresqlSpanningEntry`; populate at cache-build. Unused on
   write path. ~0.5 d
4. New green test `progresql_partseq` (alloc/no-reuse/reindex-preserve/resolver). ~1 d
5. Land red-carrying characterization tests (oid_reuse, vacuum_collision) in the
   chosen xfail mechanism. ~1 d
6. Full `make check` 233/233 + new green test; commit C1-C.

**Est: ~5 dev-days.**

---

## 8. Decisions (LOCKED)

- **Q1 — code placement:** **new `src/backend/catalog/pg_index_partition.c`** for
  the writer/reader. Cleanest SOLID separation; index.c is already ~4000 lines.
- **Q2 — known-RED characterization tests:** **written but kept OUT of
  `parallel_schedule` until D** (present, dormant, not run). Avoids committing a
  `.out` that asserts today's wrong behavior. C's only *scheduled* new test is the
  GREEN `progresql_partseq`. D will add oid_reuse + vacuum_collision to the
  schedule and they must pass on arrival.
- **Q3 — map population:** **lazy now, D forces it.** C populates only at
  build/REINDEX/ATTACH. D begins by forcing a REINDEX/backfill of all spanning
  indexes before the flip, guaranteeing a complete map.
