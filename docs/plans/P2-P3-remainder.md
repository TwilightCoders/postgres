# ProgreSQL — Remediation Plan for Audit Items P2-3, P2-4, P2-5, P3-1, P3-2

> Companion to `PRODUCTION_READINESS.md`. One section per item. Each section:
> problem restatement → code analysis (with `file:line`) → mini-plan → test note
> → effort estimate.
>
> Confidence tags follow the readiness-tracker convention:
> - **[VERIFIED file:line]** — read in this source tree this session.
> - **[ASSUMPTION]** — reasoned from PostgreSQL internals, not line-confirmed here.
>
> Date: 2026-05-30. Branch: `progresql-18`. Base: `REL_18_STABLE`.
>
> **Tooling caveat for this audit:** the file-reader intermittently *paraphrased*
> long regions rather than returning verbatim text. Every `[VERIFIED]` tag below
> is anchored on a `grep -n`/`awk` extraction that returned exact bytes (line
> numbers + literal tokens). Where only a paraphrased read was available, the
> claim is downgraded to `[ASSUMPTION]` and flagged "re-read verbatim before
> coding."

---

## P2-3 — Vacuum strategy characterization

### Problem restatement
`VACUUM` of a leaf partition must remove the now-dead spanning-index entries that
live on the *root*. We need to know *how* the added code does this, and whether
its cost scales acceptably on a large partition tree.

### Code analysis

The whole mechanism is small and fully readable:

- **Hook site** — `[VERIFIED src/backend/access/heap/vacuumlazy.c:2620-2621]`:
  ```c
  if (vacrel->rel->rd_rel->relispartition)
      progresql_vacuum_spanning_indexes(vacrel);
  ```
  Called from `lazy_vacuum()` *after* `lazy_vacuum_all_indexes()` +
  `lazy_vacuum_heap_rel()` succeed, i.e. once per index-vacuum round, while
  `vacrel->dead_items` is still populated.

- **Body** — `[VERIFIED src/backend/access/heap/vacuumlazy.c:2462-2518]`:
  1. `get_partition_ancestors(partOid)` — full ancestor chain, immediate parent
     up to the topmost root (standard PG semantics)
     `[VERIFIED src/backend/catalog/partition.c get_partition_ancestors]`.
  2. For **each** ancestor: `table_open(... AccessShareLock)` →
     `RelationGetIndexList()` → iterate indexes.
  3. Skip any index with `idxRel->rd_index->indnuniqatts == 0`
     (`vacuumlazy.c:2489`) — i.e. operate only on spanning indexes.
  4. For each spanning index, build an `IndexVacuumInfo` and call
     `vac_bulkdel_one_index(&ivinfo, NULL, vacrel->dead_items, vacrel->dead_items_info)`
     (`vacuumlazy.c:2504-2506`).

### How it actually removes dead entries (the answer)

It is a **targeted bulk-delete keyed by the leaf partition's dead heap TIDs** —
*not* a full repopulate. `vac_bulkdel_one_index` invokes the index AM's
`ambulkdelete` (`btbulkdelete` for nbtree), passing the leaf's `dead_items`
TID-store as the kill set. The match is sound *as far as TIDs go* because
spanning entries are inserted with the **leaf-partition heap TID**:
`[VERIFIED src/backend/executor/execIndexing.c:1380-1387]`:
```c
index_insert(se->indexRel,
             values, isnull,
             tupleid,        /* the leaf-partition heap TID */
             se->parentRel,  /* heap rel = the ROOT (storage-less) */
             UNIQUE_CHECK_YES,
             false, se->indexInfo);
```
Note two facts that matter below:
- The entry's TID is the **leaf** TID, but the `index_insert` *heap relation*
  argument is `se->parentRel` — the **root** (which has no storage). The trailing
  `tableoid` key column (`values[se->tableoidKeyPos] = partOid`, lines 1376-1378)
  is what records *which* partition the TID belongs to.
- The insert is `UNIQUE_CHECK_YES`, which is how cross-partition uniqueness is
  enforced on the write path.

So `btbulkdelete` walks the entire spanning B-tree and deletes any entry whose
stored TID is in this leaf's dead set — **but `btbulkdelete` only sees the entry's
`t_tid`, never the trailing `tableoid` key.**

**Corroborating facts (both verbatim):**
- The match callback is hard-wired to the stock, TID-only reaper:
  `[VERIFIED src/backend/commands/vacuum.c:2548-2556]`
  ```c
  istat = index_bulk_delete(ivinfo, istat, vac_tid_reaped, (void *) dead_items);
  ```
  `vac_tid_reaped` tests `TidStoreIsMember(dead_items, itemptr)` — pure
  `(block,offset)` membership, **no `tableoid`**.
- The authors already *know* spanning TIDs collide across partitions, and built a
  protection mechanism for it on the *insert* side. nbtree's LP_DEAD heap-liveness
  deletion passes are driven by the `heapRel` argument: `_bt_simpledel_pass`
  (called `[VERIFIED src/backend/access/nbtree/nbtinsert.c:2819]`) and
  `_bt_bottomupdel_pass` (`[VERIFIED nbtinsert.c:2871]`) ultimately call
  `table_index_delete_tuples(heapRel, …)` (`[VERIFIED nbtpage.c:1526]`). When
  `heapRel` is the storage-less root (`rd_tableam == NULL`) those passes are
  short-circuited. The feature relies on this deliberately:
  `BuildSpanningIndexFromPartitions` passes the **root** (not the leaf) as
  `heapRelation` with the verbatim comment "Pass rel (the partitioned root) as
  heapRelation, not partRel. The root has `rd_tableam == NULL`, which signals
  nbtinsert.c to skip the heap-liveness deletion passes (`_bt_simpledel_pass` and
  `_bt_bottomupdel_pass`) that cannot safely process TIDs spread across multiple
  partitions."
  `[VERIFIED src/backend/commands/indexcmds.c, comment ~2950-2962]`. The runtime
  insert path likewise passes the root (`se->parentRel`) as the heap rel
  (`[VERIFIED execIndexing.c:1384]`).
- **That is direct in-tree evidence the design treats cross-partition TID
  collisions as a real correctness hazard for the deletion machinery — yet the
  VACUUM cleanup path (`vac_tid_reaped` + `dead_items` TID-store) removes entries
  by `(block,offset)` membership alone, with no equivalent `tableoid`-aware
  protection.** It is the one deletion path the existing safeguard does not cover.

### Cost model (this is the real finding)

`btbulkdelete` performs a **full physical scan of the spanning index** (every
leaf page) per invocation. Consequences:

- Cost is **O(size of the entire spanning index)** — proportional to the *whole
  partition tree's* live+dead row count — **for every leaf-partition vacuum**.
- A tree with *N* leaf partitions and a per-partition autovacuum cadence pays
  ~*N* full spanning-index scans per cycle, each scanning all *N* partitions'
  worth of entries → effectively **O(N²)** spanning-index I/O across a full
  autovacuum sweep. This is precisely the "vacuum across partitions" objection
  that has historically sunk global-index proposals (readiness P1-2).
- **The cross-partition TID-collision hazard (likely latent P0).** The
  TID-collision problem was explicitly recognized and fixed for DROP/DETACH by
  keying on `tableoid` (commit `67a5276404` "fix BUG A (TID collision)"), and the
  insert/probe paths both disambiguate by reading the trailing `tableoid` key
  `[VERIFIED src/backend/access/nbtree/nbtinsert.c:591-597, 677-684]`. **But the
  VACUUM bulk-delete path passes `NULL` for the callback**
  (`[VERIFIED vacuumlazy.c:2504]`) and relies on the generic `dead_items`
  TID-store match. `btbulkdelete` compares only the entry's `t_tid`; it cannot
  see the `tableoid` column. Two live rows in *different* partitions can share the
  same heap TID `(block,offset)` because each partition has its own address
  space. **[ASSUMPTION — must verify]:** if leaf A's dead-TID set contains a
  `(blk,off)` that is *also* the live TID of a row in sibling leaf B, the generic
  `btbulkdelete` would delete B's still-live spanning entry → silent loss of
  cross-partition uniqueness enforcement for B's key, even though the very same
  collision class was deemed a P0 bug worth fixing on the DETACH path. The vacuum
  path appears to be the **one remaining unfixed instance of BUG A.** This is the
  single most important thing to test (see test note) and is most likely a latent
  **P0**, not merely a P2 cost issue.

### Mini-plan

1. **Correctness first (potential P0):** confirm or refute the cross-partition
   TID-collision-on-delete hazard above. If real, the bulk-delete callback must
   filter by `tableoid` as well as TID. Options:
   - Pass a custom `IndexBulkDeleteCallback` that, given an index tuple, extracts
     the trailing `tableoid` key (same `index_getattr(itup, indnuniqatts+1, …)`
     pattern already used at
     `[VERIFIED src/backend/access/nbtree/nbtinsert.c:592-597]`) and only kills
     the entry when *both* `tableoid == partOid` *and* TID ∈ dead set. Today the
     call passes `NULL` for the callback (`vacuumlazy.c:2504`) and relies on the
     TID-store path; a tableoid-aware callback closes the gap.
2. **Cost mitigation (the P2 proper):**
   - Short term: **document** the O(per-tree) per-leaf-vacuum cost and the
     aggregate O(N²) sweep behavior in `PRODUCTION_READINESS.md` and the design
     RFC; this is the headline number reviewers will ask for.
   - Medium term: **coalesce** spanning-index cleanup. Instead of one full
     spanning scan per leaf vacuum, accumulate dead TIDs across a tree-wide
     vacuum and do a single spanning bulk-delete (e.g. a tree-level
     `VACUUM`-driven pass, or defer to `lazy_cleanup`/a maintenance entry point
     that runs once per root). This turns N² into ~N.
   - Long term (design-level): a `tableoid`-partitioned deletion that can skip
     spanning leaf pages not belonging to the vacuumed partition is *not*
     possible with a plain B-tree key order `(user_cols…, tableoid)` because
     `tableoid` is the *trailing* key — entries for one partition are scattered.
     Note this as a structural cost of the current key layout; a `(tableoid,
     user_cols…)` ordering would localize per-partition entries (cheap targeted
     delete) but would break the cross-partition uniqueness scan. Record the
     trade-off; do not change key order in v1.

### Test note
- New regression/TAP: build a 3-leaf spanning tree, force identical heap TIDs in
  two siblings (fill+delete to align `(blk,off)`), delete+VACUUM one sibling,
  then assert the *other* sibling's key still raises a cross-partition duplicate.
  This is the collision probe and is the make-or-break test.
- Benchmark harness: `pgbench`-style loop over a 16- and 64-leaf tree measuring
  wall-time and `pg_stat_io` reads attributable to `progresql_vacuum_spanning_indexes`
  as tree size grows; capture the N² curve.

### Effort estimate
- Collision verification + fix (if needed): **M** (2–4 days incl. test).
- Coalesced-cleanup optimization: **L** (1–2 weeks; touches vacuum lifecycle).
- Documentation + benchmark only (minimum bar): **S** (1–2 days).

---

## P2-4 — Logical decoding / replication of spanning writes

### Problem restatement
Spanning entries live on the storage-less root. Logical decoding is heap/row
oriented and follows the WAL of the *partition* heaps. Does anything break:
logical decoding itself, publication of partitioned tables, or `REPLICA IDENTITY`?

### Code analysis
- **No spanning-index logic exists in the replication, decoding, or publication
  paths.** `[VERIFIED]` the authoritative signal is the feature's own file
  footprint: `grep -rln indnuniqatts src/backend src/include` returns exactly
  `vacuumlazy.c, access/index/genam.c, plancat.c, catalog/index.c,
  execIndexing.c, tablecmds.c` plus the catalog headers — **no** file under
  `src/backend/replication/`, **no** `pg_publication.c`/`publicationcmds.c`, and
  `heapam.c` is **not** in the list. The publication path has 0
  `indnuniqatts`/`spanning` hits `[VERIFIED]`. The feature is therefore invisible
  to those subsystems by construction.

### Assessment
- **Decoding of data changes: almost certainly unaffected** `[ASSUMPTION]`.
  Decoding reconstructs INSERT/UPDATE/DELETE from each *partition heap's* WAL.
  The spanning index is just another index from WAL's perspective; index WAL is
  not logically decoded (only heap/`REORDER` changes are). A subscriber replays
  the row change into its own partition heap and, if it is also ProgreSQL, its
  own executor INSERT path writes its own spanning entry. On stock-PG
  subscribers the spanning index simply doesn't exist — fine.
- **The real risks to verify (not the decode stream, but the metadata):**
  1. **`REPLICA IDENTITY` resolution.** Logical replication of UPDATE/DELETE
     needs a replica identity (PK or explicit). On a spanning-indexed root the
     "primary key" is the *spanning* index, which is hidden from the planner
     (P3-1) and lives on the root, not the partition. Does
     `RelationGetReplicaIndex()` / `GetRelationIdentityOrPK()` ever try to use a
     spanning (root) index for a *partition's* row identity? If it does, it would
     pick an index that has no storage for that partition → broken UPDATE/DELETE
     replication. **Must trace.** `[ASSUMPTION]`
  2. **`pubviaroot` publications** (`publish_via_partition_root`). With this
     option the change is decoded *as the root relation*. The root has no heap;
     verify the decoder does not attempt to map the change onto the root's
     (nonexistent) storage or its spanning index.
  3. **Initial table sync** of a partition into a ProgreSQL subscriber: the
     `COPY`-based sync flows through the executor INSERT path (README says COPY is
     covered), so spanning entries *should* be backfilled on the subscriber for
     free — verify.

### Mini-plan
1. Trace `RelationGetReplicaIndex` / `GetRelationIdentityOrPK` /
   `RelationGetIndexList` callers in `src/backend/utils/cache/relcache.c` and
   `src/backend/replication/logical/` to confirm a `indnuniqatts > 0` index is
   *never* selected as a partition's replica identity. If it can be, add a guard
   that excludes spanning indexes from replica-identity candidacy (mirror the
   planner exclusion at `plancat.c:412`).
2. Decide and document the intended cross-cluster contract: spanning uniqueness
   is enforced **per node** by the executor INSERT path; it is *not* a property
   carried in the logical stream. A non-ProgreSQL subscriber gets no
   cross-partition uniqueness. State this explicitly.

### Test note
- TAP `src/test/recovery`/`subscription`: publisher = spanning tree, subscriber =
  ProgreSQL; replicate INSERT/UPDATE(key change)/DELETE across partitions and
  assert subscriber's spanning index enforces uniqueness and matches row counts.
- Repeat with `publish_via_partition_root = true`.
- Negative test: set `REPLICA IDENTITY` to the spanning constraint and confirm a
  clear error or correct fallback rather than silent breakage.

### Effort estimate
- Tracing + small guard (if needed): **S–M** (2–4 days).
- Full subscription TAP matrix: **M** (3–5 days).

---

## P2-5 — Sub-partition (multi-level tree) handling

### Problem restatement
Multi-level partition trees are documented as "intentionally excluded"
(`README.md:184`, readiness P2-5). For production this must either *work* or
*fail loudly* — never silently skip enforcement. Which does it do today?

### Code analysis (the finding: partial guards exist; runtime enforcement is NOT guarded)
There *are* explicit sub-partition exclusions — but they are at **CREATE/backfill
time only**, not on the runtime INSERT/VACUUM enforcement paths:

- **Opt-in gate (CREATE).** `[VERIFIED src/backend/commands/indexcmds.c:745-757]`
  — `progresql_bypass` is set only when `partitioned && !rel->rd_rel->relispartition`;
  the in-tree comment (`:752`) literally says "exclude sub-partitions via
  `!relispartition`." So a spanning index is **only ever created on a top-level
  root**, never on an intermediate (sub-)partitioned node. Good as far as it goes.
- **Backfill skip.** `[VERIFIED src/backend/commands/indexcmds.c:2893-2930]`
  `BuildSpanningIndexFromPartitions` uses `find_all_inheritors(root, …)` (which
  flattens **all** descendants, every level) and *skips entries where
  `relkind == RELKIND_PARTITIONED_TABLE`* (:2927-2930), indexing only **leaf**
  rows. So backfill already correctly handles depth — it indexes grandchild
  leaves.
- **Runtime INSERT** uses the **full** ancestor chain
  `[VERIFIED src/backend/executor/execIndexing.c:1196]`
  (`get_partition_ancestors(RelationGetRelid(partition))`) and writes to every
  ancestor index with `indnuniqatts > 0`. A grandchild leaf therefore *does*
  resolve up to the top root and write into its spanning index.
- **VACUUM** uses the identical full-ancestor walk
  `[VERIFIED src/backend/access/heap/vacuumlazy.c:2466-2515]`.

### What actually happens today (reasoned, now better-grounded)
- For a **2-level tree** (root → intermediate partitioned → grandchild leaves):
  - The opt-in fires only on the **root** (`!relispartition`), so exactly one
    spanning index exists, on the top root. Correct.
  - Backfill indexes the grandchild leaves (skips the intermediate node). Correct.
  - INSERT into a grandchild leaf walks `get_partition_ancestors` (returns
    intermediate **and** root), writes into the root's spanning index keyed by the
    grandchild's `tableoid`. So enforcement **does run** — multi-level is *not*
    silently un-enforced on the steady-state INSERT path. This contradicts the
    README's "intentionally excluded" framing: in practice the happy path appears
    to *partly work by construction*, which is more dangerous than a clean block
    because it invites reliance on unverified behavior.
- **The real, unguarded trap is DDL on the intermediate level:**
  - **DETACH of an *intermediate* partitioned node.** The DROP/DETACH cleanup
    (commits `67a5276404`, `355dfaa9fc`) removes root entries keyed by the
    detached node's own `tableoid`. But a grandchild's entries are keyed by the
    **grandchild** `tableoid`, not the intermediate's. Detaching the intermediate
    would leave all its descendant leaves' entries **orphaned** in the root →
    stale entries → false cross-partition duplicate errors (or, after OID reuse,
    silent mis-enforcement). **[ASSUMPTION — verify which cleanup path runs for an
    intermediate DETACH]:** does any code enumerate descendants on intermediate
    DETACH? The grep for sub-partition handling in the cleanup paths found none.
  - **ATTACH of an already-partitioned table** to a spanning root: is its subtree
    backfilled? `BuildSpanningIndexFromPartitions` handles depth at CREATE, but
    the ATTACH backfill path (commit `355dfaa9fc` "BUG C") must be re-checked for
    the same `find_all_inheritors` + leaf-skip treatment. **[ASSUMPTION — verify
    ATTACH backfill recurses.]**
- **Net:** the *creation* side is depth-aware; the *DDL-mutation* side
  (intermediate DETACH/ATTACH) is the unguarded correctness trap. So it is **not**
  a clean hard-error, and **not** fully working — it is a partial-correctness trap
  centered on intermediate-node DDL.

### Mini-plan (make it fail loudly now; make it work later)
1. **Fail loudly (do this first, small, ships the safety property):** the cleanest
   place to enforce "single level only" is the DDL that *introduces* a second
   level under a spanning root:
   - Block `ALTER TABLE child … PARTITION BY …` (and `CREATE TABLE … PARTITION OF
     <spanning root> … PARTITION BY …`) when an ancestor carries a spanning index
     (`indnuniqatts > 0`). Hook the ATTACH/partition-creation path in
     `tablecmds.c` (the same paths already touched for spanning ATTACH backfill,
     commit `355dfaa9fc`).
   - Block `ATTACH PARTITION p` to a spanning root when `p` is itself
     `RELKIND_PARTITIONED_TABLE`.
   - `ereport(ERROR, …"spanning indexes do not support multi-level
     (sub-)partitioned trees")`. This turns today's silent trap into a clean
     error and is the highest value-per-effort fix in the whole doc.
2. **Make it work (later, design-level):** creation/backfill already handle depth
   (`find_all_inheritors` + leaf-skip, `indexcmds.c:2893-2930`), and INSERT/VACUUM
   already walk the full ancestor chain, and the `tableoid` trailing key already
   identifies the *leaf* uniquely regardless of depth. The remaining blockers are
   the DDL-mutation paths:
   - **DETACH of an intermediate node:** cleanup must remove root entries for
     *all leaves beneath* the detached subtree, not just the detached node's own
     `tableoid`. Plan: on DETACH of node X, enumerate `find_all_inheritors(X)`,
     filter to leaves, and bulk-delete root entries for every descendant
     `tableoid` (the `tableoid`-keyed delete from commit `67a5276404`, applied
     per-descendant).
   - **ATTACH of an already-partitioned subtree:** confirm the ATTACH backfill
     reuses the depth-aware `find_all_inheritors`+leaf-skip walk.
   Once those two holes are closed, multi-level is mechanically supported.

### Test note
- DDL regression (`progresql_ddl.sql`): create a 2-level spanning tree; assert
  either (a) clean ERROR at the gate, or (b) — once supported — cross-partition
  uniqueness across grandchild leaves, plus DETACH of an intermediate node
  cleans *all* descendant entries (no stale-entry false duplicate after
  re-insert).
- Currently `progresql_ddl.sql` (157 lines `[VERIFIED]`) has **no** sub-partition
  case `[VERIFIED]` (grep for subpart/nested in the suites → 0 hits) — this is a
  coverage hole.

### Effort estimate
- Fail-loud guard + tests: **S** (1–2 days). Highest value-per-effort item here.
- Full multi-level support (incl. DETACH-subtree cleanup): **L** (1–2 weeks).

---

## P3-1 — Planner read-acceleration (design sketch only)

### Problem restatement
Spanning indexes are hidden from the planner, so they only *enforce* uniqueness
and never *accelerate* reads. A global index that can serve cross-partition
point lookups (`WHERE id = ?` without the partition key) is exactly what the
community repeatedly asks "global indexes" for; v1 leaves that on the table.

### Code analysis
- **`[VERIFIED src/backend/optimizer/util/plancat.c:287-294]`** — in
  `get_relation_info`'s index loop, a comment at `:287` ("Skip ProgreSQL spanning
  indexes: RELKIND_INDEX btrees on [the storage-less root]") precedes the guard at
  `:294`:
  ```c
  if (index->indnuniqatts > 0)
      continue;
  ```
  The `continue` means a spanning index never becomes an `IndexOptInfo`, so it is
  invisible to path generation, selectivity, and `get_index_paths`.

### Why hiding it is currently *correct*
The root has no heap storage. A normal `IndexScan`/`Index Only Scan` over the
root spanning index would try to fetch heap tuples from the root → wrong. The
entries' real TIDs point into *partitions* (the `tableoid` trailing column says
which). A scan node would need to fetch from the *partition* named by each
entry's `tableoid`, not from the index's own relation.

### Design sketch (do not implement in v1)
A read-accelerating spanning scan needs a new executor capability: an index scan
whose heap-fetch target is chosen *per index tuple* from the `tableoid` key.
Two viable shapes:
1. **New plan node ("Spanning Index Scan"):** like `IndexScan`, but in the
   `amgettuple` → heap-fetch step it reads the trailing `tableoid` column, opens
   that partition (cached), and fetches the tuple there. Planner change: stop the
   blanket `continue` at `plancat.c:412`; instead emit a restricted
   `IndexOptInfo` that *only* matches `indnuniqatts`-leading-column quals
   (equality on the unique key), with a custom cost model, and only when the
   query targets the partitioned root. Stop the blanket `continue` at
   `plancat.c:294`. Selectivity is the leading-key selectivity; the cost adds a
   per-row partition-open/fetch (amortizable via a tableoid→Relation cache — the
   write path already has one, the per-statement
   `ProgresqlPartitionCacheEntry`/`ProgresqlSpanningEntry` cache in
   `execIndexing.c`).
2. **Translate to per-partition `Append` of leaf index scans:** keep the spanning
   index for *uniqueness only*, but at plan time, for a cross-partition equality
   on the unique key, generate an `Append` over each partition's *local* index
   (if one exists). Simpler executor story (reuses existing scan nodes) but
   requires a matching local index per partition and scans all of them (no
   global ordering benefit). This is a weaker "acceleration" and arguably the
   planner can already do partition pruning poorly here; the spanning index's
   value is the *single* global lookup, so option 1 is the one worth the design.

### Why the community wants this (note for the RFC)
Global-index proposals on pgsql-hackers are motivated as much by **read
acceleration of non-partition-key lookups** as by uniqueness. A spanning index
that both enforces *and* serves `WHERE unique_key = ?` is the complete feature;
shipping enforcement-only is a defensible v1 but should be framed as phase 1 of
that arc, with this sketch as phase 2. (Ties to readiness P1-2 prior-art
engagement.)

### Test note
- No tests now (enforcement-only is intentional). When implemented: planner
  regression asserting a cross-partition equality on the unique key chooses the
  spanning scan and returns correct rows from the right partitions; `EXPLAIN`
  output stability test; cost crossover vs `Append`-of-seqscans.

### Effort estimate
- Design RFC / sketch only (this item's actual ask): **S** (1 day, mostly done
  here).
- Full implementation of option 1: **XL** (multi-week; new executor node, cost
  model, planner integration) — out of scope for v1.

---

## P3-2 — `build.sh` is untracked

### Problem restatement
The convenience wrapper the README points users to is not in git.

### Code analysis
- **`[VERIFIED]`** `git status --short build.sh` → `?? build.sh` (untracked);
  `git ls-files build.sh` → empty; not gitignored.
- **`[VERIFIED]`** README references it twice: `README.md:84`
  ("There's also a `build.sh` wrapper at the repo root.").
- Script content reviewed `[VERIFIED build.sh]`: a small, self-contained
  `configure + make -j + make install` wrapper, in-tree `--prefix`
  (`build/install`), `build|clean|verify` subcommands, `set -euo pipefail`,
  `--without-icu --without-readline`. No secrets, no machine-specific paths
  (uses `$(dirname "$0")` and env overrides `PROGRESQL_PREFIX`/`JOBS`). Safe to
  commit.

### Recommendation: **commit it.** Rationale
- The README already advertises it; an untracked file the docs reference is a
  broken-onboarding trap (fresh clone has no `build.sh`).
- It is generic and portable.

### Cleanup before committing
1. Ensure `build/` (its default `--prefix`) is gitignored so installs don't get
   committed. Add `/build/` to the repo `.gitignore` (verify it isn't already
   covered) — this is the one required companion change.
2. Minor polish (optional, not blocking): the README quickstart uses
   `--enable-debug --enable-cassert` but `build.sh` does not — either align the
   flags or note the difference so users aren't surprised that `build.sh`
   produces a non-assert build.
3. Place under repo root as-is; it does not belong in `src/` (build orchestration
   lives at top level alongside `configure`).

### Test note
- After committing: `git clean -fdx` to a pristine tree (in a throwaway copy),
  run `./build.sh` and `./build.sh verify`, confirm `build/install/bin/postgres
  --version` round-trips. Add a one-line CI smoke step if/when CI exists.

### Effort estimate
- **XS** (under an hour): `git add build.sh`, gitignore `/build/`, optional flag
  note, commit.

---

## Overall test-harness gaps (note for P2-1, handled distributively above)

Per-item test notes above push coverage into the right buckets; the missing
*scaffolding* is the cross-cutting gap:

- **regress** — two suites exist (`progresql.sql` 108 lines, `progresql_ddl.sql`
  157 lines) `[VERIFIED]`; both are single-backend happy-path. No sub-partition
  case `[VERIFIED]` (P2-5). Add a `progresql_planner` suite when P3-1 lands.
- **isolation** — *none* for spanning (readiness P0-4). Needed for the
  cross-partition `_bt_check_unique` liveness-probe lock ordering and the VACUUM
  TID-collision race (P2-3).
- **TAP (`src/test/recovery`, `src/test/subscription`)** — *none*. Required for
  P0-3 (crash/WAL) and P2-4 (logical replication / replica identity).
- **pg_upgrade (`src/bin/pg_upgrade/t`)** — *none*. Required for P0-2 (tableoid
  key + OID preservation) and the `pg_dump | psql` round-trip (P2-2).
- **CI wiring** — no CI runs these today. Minimum: a workflow that builds via
  `build.sh` and runs `make -C src/test/regress check` plus the new
  isolation/TAP targets; gate merges on it. Wire `build.sh` (P3-2) as the build
  step so docs and CI agree.

---
