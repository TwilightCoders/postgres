# P0-3 — WAL-Logging Correctness & Crash-Recovery / Physical-Replication of Spanning Indexes

> Scope: every code path that **writes to or deletes from** a ProgreSQL spanning
> index. Goal: prove (or close gaps in) WAL-logging correctness, crash-recovery
> self-healing, and physical-replication (standby apply) safety.
>
> Method: source audit of `progresql-18` against `REL_18_STABLE`. Tags:
> **[VERIFIED file:line]** = read this session; **[ASSUMPTION]** = reasoned from
> PG internals, not line-confirmed here.
>
> Status of P0-3 going in (per `PRODUCTION_READINESS.md:94`): **[ASSUMPTION]** —
> "writes go through normal nbtree insert path … may be fine for free, but the
> vacuum/backfill/reindex code was not reviewed for WAL correctness." This plan
> upgrades that to a per-path verdict.

---

## 1. Problem restatement

A spanning index is a **real, fully-storaged nbtree** that physically lives on
the partitioned **root** relation (the root has no *heap* storage, but the index
relfilenode is real and on disk). Its leaf tuples are `(user_cols…, tableoid)`
and point — via `IndexTuple.t_tid` — at **heap TIDs in the leaf partitions**, not
at the root. Every mutation of that nbtree must be WAL-logged exactly like any
other nbtree, or:

- a crash loses spanning entries that the committed heap rows still need →
  **false-negative uniqueness** (duplicates accepted) — P0 corruption;
- a crash keeps spanning entries whose heap rows never committed →
  **false-positive uniqueness** (spurious conflicts) — P0 availability/integrity;
- a standby that doesn't replay the spanning nbtree changes diverges from the
  primary → cross-partition uniqueness **not enforced on the replica**, and a
  promoted standby silently drops the invariant — P0.

The central question for **each** path: does it ride entirely on stock,
already-WAL-logged AM entry points (`index_insert`, `index_bulk_delete`,
`index_build`/`ambuild`), or does it hand-roll buffer manipulation that could
skip WAL or be replication-unsafe?

The secondary question: for the multi-step DDL paths (ATTACH backfill, REINDEX
repopulate, DROP/DETACH removal, VACUUM cleanup), is a **mid-operation crash**
self-healing — i.e. does the surrounding transaction's atomicity (or a
re-runnable recovery action) leave the index in a correct or trivially
repairable state?

---

## 2. Per-path WAL-correctness analysis

### Path A — INSERT / COPY write
- **Entry:** `ExecInsertSpanningIndexTuples`
  **[VERIFIED execIndexing.c:1344-1389]**, called from the INSERT path with
  `resultRelInfo == NULL` **[VERIFIED nodeModifyTable.c:1246-1248]** (one INSERT
  call site).
- **Mechanism:** for each cached spanning index, `FormIndexDatum`, override the
  trailing key with the partition OID **[VERIFIED execIndexing.c:1377]**, then
  stock `index_insert(... UNIQUE_CHECK_YES ...)` **[VERIFIED :1380]**.
- **Verdict: WAL-safe for free.** `index_insert` → `btinsert` →
  `_bt_doinsert`/`_bt_insertonpg` emits the standard `XLOG_BTREE_INSERT_LEAF`
  (and page-split) records inside the **same transaction** as the heap insert.
  Heap-row WAL and spanning-entry WAL are ordered within one xact; commit makes
  both durable atomically. **[VERIFIED :1380]** + **[ASSUMPTION]** (stock
  `btinsert` WAL behavior, unchanged by this fork).
- **Per-statement cache (`es_progresql_partition_cache`)**: pure in-memory
  resolution cache (relcache handles + `IndexInfo`), built lazily
  **[VERIFIED execIndexing.c:1162-1259]**, freed in `FreeExecutorState` via
  `ProgresqlReleasePartitionCache` **[VERIFIED execIndexing.c:1262-1285;
  call from execUtils.c FreeExecutorState]**. **No on-disk or WAL state** — it
  cannot affect recovery. Not a WAL concern.

### Path B — UPDATE write (key change vs. unchanged)
- **Entry:** same function, called from the UPDATE epilogue with a non-NULL
  `resultRelInfo` — **three** call sites
  **[VERIFIED nodeModifyTable.c:2362, 2383, 2393]** (cross-partition-move vs.
  in-place vs. HOT re-root variants; each passes `resultRelInfo` so the
  unchanged-key skip applies). Confirm each variant in the fix phase; the WAL
  argument is identical for all (stock `index_insert` or skip).
- **Unchanged-key skip:** `spanning_unique_unchanged`
  **[VERIFIED execIndexing.c:1298-1326]** returns true when none of the leading
  `ii_NumIndexKeyAttrs - 1` (i.e. excluding tableoid) columns are in
  updated/extraUpdated cols; the new entry is **skipped**
  **[VERIFIED :1370-1372]**. Rationale: HOT chain keeps the old entry's `t_tid`
  valid.
- **WAL concern — none new on the index side:** when the key *changes*, a fresh
  `index_insert` is emitted (WAL-safe as Path A). When it *doesn't* change, no
  index write is emitted at all, which is exactly correct — durability of the
  (re-rooted) heap row is the stock heap/HOT WAL machinery's job.
- **One thing to confirm under recovery (not a WAL-logging defect, a
  *correctness* defect to test):** the skip relies on the old spanning entry's
  `t_tid` still resolving to the live heap tuple after replay. For a **non-HOT**
  UPDATE the new heap tuple gets a new TID; stock index maintenance inserts a new
  entry into *ordinary* indexes and the old entry is later vacuumed. The spanning
  path **also** inserts (key changed) or skips (key unchanged). The unchanged +
  non-HOT case is the subtle one: does the old spanning `t_tid` follow the HOT
  chain, or is it stale? This is a **logic** question, but a crash-recovery test
  is the cheapest way to surface it because replay reconstructs the same chain.
  **[ASSUMPTION]** — flag for the TAP matrix (test B2 below).

### Path C — VACUUM cleanup of dead spanning entries
- **Entry:** `progresql_vacuum_spanning_indexes`
  **[VERIFIED vacuumlazy.c:2462-2518]**, invoked from `lazy_vacuum` only after a
  successful `lazy_vacuum_all_indexes` + `lazy_vacuum_heap_rel`, gated on
  `relispartition` **[VERIFIED vacuumlazy.c:2606-2621]**.
- **Mechanism:** walks `get_partition_ancestors`, opens each ancestor's spanning
  indexes, and calls stock `vac_bulkdel_one_index(&ivinfo, NULL,
  vacrel->dead_items, vacrel->dead_items_info)` **[VERIFIED :2504-2506]** — the
  *same* leaf-partition dead-TID set. `vac_bulkdel_one_index` →
  `index_bulk_delete` → `btbulkdelete` → `btvacuumscan`/`_bt_delitems_vacuum`,
  which emits `XLOG_BTREE_VACUUM`.
- **Verdict: WAL-safe for free.** It is the *normal* nbtree bulk-delete machinery,
  not a hand-rolled page edit. **[VERIFIED :2504]**.
- **Crash-mid-VACUUM:** stock VACUUM semantics. `index_bulk_delete` is
  page-at-a-time WAL-logged and idempotent under replay; a crash leaves the index
  with *some* dead entries still present, which is **always safe** (a stale
  spanning entry only risks a false-positive conflict, and the
  `_bt_check_unique` liveness probe — Path E — kills it on next touch, and the
  next VACUUM removes it). Self-healing. **[VERIFIED :2504]** + **[ASSUMPTION]**
  (stock bulkdelete idempotency).
- **CONFIRMED correctness bug (not WAL) — R2:** the VACUUM path matches dead
  entries by **heap TID only**. It passes `vacrel->dead_items` straight to
  `vac_bulkdel_one_index` with **no callback / no tableoid filter**
  **[VERIFIED vacuumlazy.c:2504-2506]**. Spanning `t_tid`s are leaf-partition
  TIDs and **collide across partitions** (every partition starts at block 0).
  So a VACUUM of partition X can delete a still-live spanning entry belonging to
  partition Y that shares one of X's dead TIDs → **false-negative uniqueness**
  (a duplicate later accepted). **This is the exact TID-collision class the
  project already fixed for DROP/DETACH** — and the fix is *right there in the
  tree*: `progresql_clean_spanning_indexes_for_partition` uses a **tableoid-aware
  callback** `progresql_spanning_killtuple_cb` precisely because "TIDs collide
  across partitions" **[VERIFIED tablecmds.c:1992-1995, 2060-2062]**. The VACUUM
  path was *not* given the same treatment — a real, line-confirmed asymmetry.
  Not a WAL-logging defect (it IS WAL-logged), but a P0 correctness bug reachable
  *through* the WAL-logged path. Recovery test C2 is its regression guard.

### Path D — ATTACH backfill
- **Entry:** `progresql_backfill_spanning_indexes_for_attached_partition`
  **[VERIFIED tablecmds.c:2099-2167]**, called from `ATExecAttachPartition`
  **[VERIFIED tablecmds.c:20688]**, *after* the pg_inherits catalog change so
  `get_partition_ancestors` sees the new edge **[VERIFIED comment :2095-2096]**.
- **Mechanism:** for each spanning index on each ancestor root,
  `table_beginscan(attachrel, GetActiveSnapshot())`, `FormIndexDatum`, override
  the trailing key with `attachOid` **[VERIFIED :2146-2148]**, stock
  `index_insert(... ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO ...)` per live
  row **[VERIFIED :2150-2153]**.
- **Verdict: WAL-safe for free** (per-row `index_insert`, Path A reasoning).
- **Crash-mid-ATTACH:** ATTACH runs in one transaction. If the session crashes
  mid-scan **before commit**, the whole `ALTER TABLE … ATTACH PARTITION` aborts →
  the pg_inherits row and *all* partial spanning inserts roll back together →
  partition not attached, no orphan entries. **Self-healing via xact atomicity.**
  **[VERIFIED :20688 call lands in normal ATExec path]** + **[ASSUMPTION]** (no
  internal commit boundary between the catalog change and the backfill; confirm —
  Risk R3).
- **WAL-volume note (not a defect):** backfill of a large partition emits one WAL
  record family per row, same as building an ordinary index via INSERTs; large
  but correct.

### Path E — DROP / DETACH removal
- **Entry:** `progresql_clean_spanning_indexes_for_partition`
  **[VERIFIED tablecmds.c:2006-2074]**, called from the DROP-partition path
  **[VERIFIED tablecmds.c:2476]**, the DETACH path
  **[VERIFIED tablecmds.c:21202]**, and the concurrent-DETACH path
  **[VERIFIED tablecmds.c:21360]** — all *before* the pg_inherits row is removed.
- **Deletion mechanism — NOT a bulk-delete; LP_DEAD hints (important):** it does
  a `tableoid = partOid` index scan under **`SnapshotAny`** and sets
  `scan->kill_prior_tuple = true` on every match **[VERIFIED tablecmds.c:2044-2072]**.
  Entries are marked **LP_DEAD via the standard `kill_prior_tuple` mechanism**
  (same as Path G's hint), **not** physically deleted and **not** WAL-logged at
  clean time. The header comment is explicit: "set kill_prior_tuple … so the
  btree marks the index entry LP_DEAD … Physical removal happens lazily via
  btree's simple/bottom-up deletion passes when pages are next modified, or via
  VACUUM" **[VERIFIED comment :1991-1997]**. It keys on the trailing tableoid
  column precisely because "TIDs collide across partitions ((0,1) is the first
  row of every partition)" — the BUG-A-safe replacement for the prior
  bulk_delete approach **[VERIFIED comment :1999-2003]**.
- **Verdict: correct AND a WAL/recovery question — R7 (new).** LP_DEAD marking is
  a *hint*, deliberately not WAL-logged (Path G reasoning: a lost hint just means
  re-probe later). So losing the hints on crash is harmless **for uniqueness
  correctness** (a stale entry only risks a false-positive conflict, killed again
  on next touch). **BUT** for DROP/DETACH the entries now point at a heap
  relation that **no longer exists** (the partition's storage is dropped /
  detached in the *same* transaction). The *catalog* change (pg_inherits/pg_class)
  IS WAL-logged and durable, so after recovery the partition is gone but the
  spanning entries (hint lost) may remain until the next page modification. On the
  next insert with a colliding user key, `_bt_check_unique` does
  `table_open(child_relid)` on that now-missing/reused OID
  **[VERIFIED nbtinsert.c:597]**. Whether the probe tolerates a dropped/reused
  tableoid after recovery is the open question (R7).
- **Crash-mid-DETACH/DROP:** single transaction; the catalog change rolls back
  with the (un-WAL-logged) hint attempts → either fully detached/dropped or not.
  The durable part is the catalog; the hints are best-effort. Self-healing for
  the catalog; R7 covers the dangling-tableoid-after-recovery edge.
  **[ASSUMPTION]** (single-xact for non-concurrent; concurrent DETACH is
  multi-xact — see Risk R4).

### Path F — REINDEX / build over partitions
- **Empty build:** for a spanning root the spool stays empty — the heap scan is
  skipped when `relkind == RELKIND_PARTITIONED_TABLE`
  **[VERIFIED nbtsort.c:485-488]**, so `_bt_leafbuild`/`_bt_load` writes only the
  empty-index metapage structure via the stock bulk-smgr path
  `smgr_bulk_start_rel` / `smgr_bulk_finish` **[VERIFIED nbtsort.c:1161,1387]**,
  WAL-logged iff the relation needs WAL — **stock behavior, unchanged.**
- **Repopulate:** `BuildSpanningIndexFromPartitions`
  **[VERIFIED indexcmds.c:2892-2973]**, called from **two** sites:
  (1) CREATE / ALTER TABLE ADD PK path **[VERIFIED indexcmds.c:1348-1349]**, and
  (2) the **tail of `reindex_index`** **[VERIFIED index.c:3851-3853]**, after the
  rebuilt index is reopenable. It scans every leaf partition
  (`RelationGetPartitionDesc`, skipping `RELKIND_PARTITIONED_TABLE`
  **[VERIFIED :2930-2934]**) and does per-row stock `index_insert(... rel /*root,
  rd_tableam==NULL*/ ..., UNIQUE_CHECK_YES)` **[VERIFIED :2961-2962]**.
- **Verdict: WAL-safe for free** (empty build = stock; repopulate = per-row
  `index_insert`).
- **CRITICAL crash/ordering concern (Risk R1 — needs confirmation):** the empty
  build for a normal (non-concurrent) `CREATE INDEX`/`REINDEX` on a
  `wal_level=minimal`, newly-created-in-this-xact relfilenode can take the
  **skip-WAL + sync-at-commit** optimization (`smgr_bulk_finish` issues the
  immediate sync; the build is not WAL-logged because the relfilenode is new).
  **But `BuildSpanningIndexFromPartitions` runs `index_insert` *after* the
  build** (at `reindex_index` tail, index.c:3853, and at the CREATE site,
  indexcmds.c:1349). If `RelationNeedsWAL(idxRel)` is **false** at that point
  (new in-xact relfilenode + `wal_level=minimal`), the repopulate inserts go
  through `btinsert` with WAL skipped — and unlike the bulk build, the per-row
  insert path does **not** itself arrange an end-of-xact `smgrimmedsync` of the
  pages it dirtied via the normal buffer manager. The danger: **dirty index
  pages from the repopulate may reach disk only via bgwriter/checkpointer or not
  at all before a crash, with no WAL to replay them** → lost spanning entries →
  false-negative uniqueness on recovery; and on a **standby** the entries are
  simply absent (a skip-WAL build is *invisible* over replication). This is the
  **single most likely place a WAL gap exists** in the whole feature. Settle by:
  (a) checking `RelationNeedsWAL(idxRel)` / `pendingSyncs` state at the
  `index_insert` (indexcmds.c:2961) for the `wal_level=minimal` new-relfilenode
  case, and (b) tests F2 (minimal+crash) and std-F (standby).
  **[ASSUMPTION]** — this is the key unknown; verify before trusting Path F.
- **Crash-mid-REINDEX:** `reindex_index` rebuilds the index in the REINDEX
  transaction and `BuildSpanningIndexFromPartitions` runs at its tail
  **[VERIFIED index.c:3851-3853]**, *before* commit. A crash before commit
  discards the rebuilt relfilenode; partial repopulate rolls back with it.
  Standard REINDEX atomicity. Self-healing **[VERIFIED :3853 inside
  reindex_index]** + **[ASSUMPTION]** (relfilenode-swap atomicity unchanged),
  **except** for the R1 minimal-WAL hazard above, which can defeat durability
  even on a clean commit.

### Path G — cross-partition uniqueness liveness probe + LP_DEAD hint
- **Entry:** inside `_bt_check_unique`, spanning branch
  **[VERIFIED nbtinsert.c:560-634]**; helpers `_bt_spanning_tableoid` and
  `_bt_spanning_heap_liveness` **[VERIFIED nbtinsert.c:1240-1268]**.
- **Read side:** `_bt_spanning_heap_liveness` opens the owning partition
  (`table_open` AccessShareLock) and does a **read-only**
  `table_tuple_fetch_row_version` under `SnapshotDirty`
  **[VERIFIED nbtinsert.c:1251-1267]**. No writes, no WAL.
- **Hint side (the one explicitly flagged in the prompt):** when the conflicting
  tuple is dead, the branch does `ItemIdMarkDead(curitemid)` +
  `opaque->btpo_flags |= BTP_HAS_GARBAGE` + **`MarkBufferDirtyHint(buf, true)`**
  **[VERIFIED nbtinsert.c:617-631]**. This is **exactly** the stock
  `kill_prior_tuple` / LP_DEAD mechanism (cf. stock `_bt_check_unique` and
  `_bt_killitems`): a *hint*, not a logged change.
- **Verdict: correct and replication-safe, with the standard hint caveats:**
  - LP_DEAD hints are deliberately **not WAL-logged**; they are reconstructable
    (a hint that's lost just means a future scan re-probes and re-kills). Safe to
    lose on crash. **[VERIFIED :630 uses MarkBufferDirtyHint, not
    MarkBufferDirty]**.
  - The `buffer_std=true` argument is correct **iff** the buffer is a standard
    page-layout page (it is — an nbtree leaf), so `MarkBufferDirtyHint` will set
    the page LSN / compute the checksum correctly. With **data checksums or
    `wal_log_hints=on`**, the *first* hint-dirty after a checkpoint triggers a
    full-page-image WAL record automatically inside `MarkBufferDirtyHint` — this
    is stock behavior and is the reason the hint is checksum-safe. **No fork
    change needed.** **[ASSUMPTION]** (relies on stock `MarkBufferDirtyHint`
    semantics, unchanged).
  - **Standby:** a hot-standby backend must **never** set this hint on a replica
    (read-only); stock `MarkBufferDirtyHint` already no-ops/avoids dirtying
    during recovery. Since the spanning hint goes through the *same* function, it
    inherits that protection. **[ASSUMPTION]** — confirm no spanning code calls
    the kill outside the stock guard (it doesn't appear to — it's inline in
    `_bt_check_unique`, only reached on the primary's insert path).

---

## 3. Identified risks (ranked)

- **R1 [P0, most likely real gap] — REINDEX/CREATE-INDEX repopulate vs. skip-WAL
  build optimization.** Path F. If the empty build takes the
  `wal_level=minimal` / new-relfilenode skip-WAL path and the subsequent
  `BuildSpanningIndexFromPartitions` `index_insert` repopulate (indexcmds.c:2961)
  does not consistently match that decision, spanning
  entries created during repopulate may be neither WAL-logged nor synced →
  lost on crash → false-negative uniqueness on recovery, and **definitely**
  absent on a standby if FPW/WAL is the only channel. **Must prove or fix.**
- **R2 [P0 correctness, CONFIRMED] — VACUUM dead-TID collision across
  partitions.** Path C. `progresql_vacuum_spanning_indexes` passes the leaf's
  dead-TID set to `vac_bulkdel_one_index` **with no callback**
  **[VERIFIED vacuumlazy.c:2504]**; leaf TIDs collide across partitions; the
  `tableoid` key is not consulted. A VACUUM on partition X can delete a live
  spanning entry belonging to partition Y. The fix already exists in-tree for
  DROP/DETACH (tableoid-aware `progresql_spanning_killtuple_cb`,
  tablecmds.c:2060) and must be mirrored into the VACUUM path. This is a
  line-confirmed asymmetry, not a hypothesis.
- **R3 [P1/P0] — ATTACH backfill / catalog-change transaction boundary.** Path D.
  If anything splits the backfill from the pg_inherits catalog change across a
  commit boundary, a crash in between yields an attached-but-unbackfilled
  partition → silent false-negatives. Confirm single-xact atomicity.
- **R4 [P1] — DETACH CONCURRENTLY and CREATE INDEX CONCURRENTLY / REINDEX
  CONCURRENTLY.** None of the spanning paths were audited for the *concurrent*
  variants, which run in **multiple transactions** and therefore break the
  "one-xact atomicity" argument that makes R3/Paths D-F self-healing. Determine
  whether these are rejected for spanning trees or silently mis-handled.
- **R5 [P2] — WAL volume / vacuum cost on large trees.** Backfill and repopulate
  are per-row `index_insert`; correct but heavy. Out of P0-3 scope (tracked in
  P2-3) — note only.
- **R6 [low] — hint under `wal_log_hints`/checksums** is believed correct via
  stock `MarkBufferDirtyHint`; cheap to confirm with the checksum standby test.
- **R7 [P1, new — from reading Path E] — DROP/DETACH uses LP_DEAD hints, not a
  durable delete, leaving dangling-tableoid entries after a crash.** Path E.
  `progresql_clean_spanning_indexes_for_partition` only sets `kill_prior_tuple`
  (un-WAL-logged LP_DEAD hints) — physical removal is deferred to later page
  modification/VACUUM **[VERIFIED tablecmds.c:2071, comment 1991-1997]**. The
  *catalog* drop/detach is durable, but the spanning entries can survive a crash
  with their hints lost, now pointing at a **dropped/detached partition OID**. On
  the next insert with a colliding user key, `_bt_check_unique` does
  `table_open(child_relid)` on that OID **[VERIFIED nbtinsert.c:597]**. If the OID
  is gone (DROP) this errors; if reused by an unrelated relation, it probes the
  wrong heap → wrong liveness answer. Must confirm `_bt_check_unique`/the cleanup
  tolerate a missing or reused tableoid after recovery (skip vs. error vs.
  mis-probe). Tests E1/E2 below must run the crash *and* then re-insert a
  colliding key to exercise the dangling probe.

---

## 4. Plan to close gaps / prove correctness

1. **Settle R1 (build vs. repopulate WAL):**
   - Read the `RelationNeedsWAL(idxRel)` / `pendingSyncs` state at the
     `BuildSpanningIndexFromPartitions` `index_insert` (indexcmds.c:2961) for the
     `wal_level=minimal` new-relfilenode case. Decide: does the repopulate
     `index_insert` WAL-log, and if not, are its dirtied pages in `pendingSyncs`
     for the end-of-xact `smgrimmedsync`?
   - If neither, force durability: either make `RelationNeedsWAL` true at insert
     time for the spanning root (the empty build produced no rows, so the
     skip-WAL build optimization buys nothing here), or explicitly register the
     index relfilenode for end-of-xact sync after the repopulate.
   - **Prove with tests F2 (crash, `wal_level=minimal`) and std-F (standby).**
2. **Fix R2 (VACUUM TID collision):** make the spanning VACUUM cleanup
   **tableoid-aware** — only remove an entry whose trailing `tableoid` key equals
   the partition being vacuumed. Today `progresql_vacuum_spanning_indexes` passes
   the leaf dead-TID set to `vac_bulkdel_one_index` with no filter
   (vacuumlazy.c:2504); mirror the DROP/DETACH approach
   (`progresql_clean_spanning_indexes_for_partition`, tableoid-keyed scan). This
   is a correctness fix independent of WAL but is exercised by test C2.
3. **Confirm R3/R4 (transaction boundaries):** audit the ATTACH and DETACH call
   chains for commit boundaries; explicitly reject (clear `ereport`) the
   CONCURRENTLY variants for spanning trees if they cannot be made atomic, or
   document/handle them. Cover with tests D1 and DETACH-concurrent (negative).
4. **Settle R7 (DROP/DETACH dangling tableoid):** confirm `_bt_check_unique`'s
   `table_open(child_relid)` (nbtinsert.c:597) tolerates a dropped/reused
   partition OID after a crash that lost the LP_DEAD hints. If it can error or
   mis-probe, either (a) make the cleanup a durable tableoid-keyed
   `index_bulk_delete` (like the VACUUM fix in step 2) so removal is WAL-logged
   and not hint-dependent, or (b) make the probe skip a missing/foreign OID.
   Cover with tests E1/E2 (crash then re-insert colliding key).
5. **Otherwise: prove the "free" paths.** Paths A, B, C(non-colliding), D, F
   (build), G are believed WAL-safe via stock AM entry points; the deliverable
   for them is **passing recovery + standby TAP tests**, not code changes. Path E
   is WAL-safe for the *catalog* change but hint-dependent for cleanup — see R7.

---

## 5. TAP test plan — `src/test/recovery/t/`

New file: **`t/045_progresql_spanning_recovery.pl`** (crash+restart matrix) and
**`t/046_progresql_spanning_standby.pl`** (physical-replication apply). Register
both in `src/test/recovery/meson.build` and `src/test/recovery/Makefile`.

**Shared fixture (`setup_tree($node)`):** the README demo tree — base table +
`INHERITS`+`PARTITION BY RANGE` root with `PRIMARY KEY (id)`, ≥3 leaf partitions.
Helper `assert_unique_enforced($node)` = attempt a known cross-partition
duplicate INSERT and expect the duplicate-key error; `count_spanning($node)` via
a probe (e.g. forcing a conflicting insert and observing detection, since the
index is hidden from the planner — cannot `SELECT` it directly).

**Crash idiom** (from existing tests, e.g. `008_fsm_truncation.pl:11`):
`$node->stop('immediate'); $node->start;` then re-assert invariants.

### 045 — crash + restart matrix
For each case: do the operation, `stop('immediate')` at the chosen point,
`start`, then assert (a) no duplicate accepted across partitions, (b) no spurious
conflict for a non-duplicate, (c) row counts consistent.

- **A1 INSERT durability:** commit N cross-partition inserts; crash; restart;
  re-attempt one as a duplicate → must conflict (entry survived). Re-insert a
  *new* key → must succeed.
- **A2 INSERT not-yet-committed:** `BEGIN; INSERT; <crash before COMMIT>`;
  restart → the key must be **free** (no orphan spanning entry → no false
  conflict).
- **B1 UPDATE key-change durability:** update a row's PK to a new value across
  the same partition; crash after commit; restart → old key free, new key
  conflicts.
- **B2 UPDATE key-unchanged (HOT and non-HOT):** update a non-key column (force
  non-HOT with a filler index); crash after commit; restart → key still
  conflicts and points at the live row (validates the skip-write + re-root
  assumption under replay; R-B logic check).
- **C1 DELETE + VACUUM durability:** delete rows, `VACUUM` the partition, crash
  after commit; restart → deleted keys are reusable (entries gone), surviving
  keys still conflict.
- **C2 VACUUM TID-collision (R2):** construct two partitions with a live row at
  the **same heap TID** (block/offset) carrying *different* PK values; delete +
  VACUUM one partition; restart → the *other* partition's key must **still**
  conflict (proves the surviving entry was not wrongly deleted). **Expected to
  FAIL until R2 is fixed — this test is the regression guard for the fix.**
- **D1 ATTACH backfill durability:** create a standalone table with rows, ATTACH
  it; crash after commit; restart → backfilled keys conflict across partitions.
- **D2 ATTACH crash-before-commit:** ATTACH inside an aborted/crashed xact;
  restart → partition not attached, its keys do **not** participate (no orphan
  entries).
- **E1 DETACH durability + dangling probe (R7):** DETACH a partition; crash
  after commit; restart → detached partition's keys must be **free** to reinsert
  into a sibling. Critically, *re-insert one of the detached keys into a sibling*
  after restart — this forces `_bt_check_unique` to probe any surviving
  (hint-lost) spanning entry whose tableoid now names the detached relation. Must
  succeed (no error, no false conflict). **Likely-fail until R7 is settled.**
- **E2 DROP durability + dangling probe (R7):** DROP a partition; crash after
  commit; restart → its keys free, sibling keys intact. Then re-insert a dropped
  key into a sibling → must succeed; the probe must tolerate the now-nonexistent
  tableoid (the partition OID is gone). **Likely-fail until R7 is settled.**
- **F1 REINDEX durability (default wal_level):** REINDEX the spanning index;
  crash after commit; restart → all live keys across all partitions conflict
  (repopulate survived).
- **F2 REINDEX under `wal_level=minimal` (R1):** same as F1 with
  `wal_level=minimal`; crash after commit; restart → entries must survive.
  **This is the R1 acid test for the skip-WAL gap.**

### 046 — standby apply (physical replication)
- Set up primary + a streaming standby (`enable_streaming`, `init_from_backup`,
  per `001_stream_rep.pl`). Run with **data checksums on** (covers R6) and assert
  on the standby after `wait_for_catchup`:
  - **std-A:** after primary INSERTs, the standby has the rows (heap) — and after
    **promotion**, the promoted node enforces cross-partition uniqueness
    (spanning nbtree was replayed). The pre-promotion check is indirect (standby
    is read-only, index hidden from planner), so the decisive assertion is the
    post-promotion duplicate-insert conflict.
  - **std-C:** primary DELETE+VACUUM, catch up, promote → deleted keys reusable
    on promoted node (vacuum WAL replayed).
  - **std-D/E:** primary ATTACH and DETACH, catch up, promote → invariant matches
    primary.
  - **std-F:** primary REINDEX (incl. a run with `wal_level=replica`), catch up,
    promote → all keys enforced (validates R1 on the replication channel, which
    is the strictest: a skip-WAL build is *invisible* to a standby).
  - **std-G:** drive LP_DEAD-hint creation on the primary (insert-then-delete
    then re-insert to trigger the dead-entry kill), confirm no standby
    inconsistency and (with checksums) no checksum failure in the standby log.

### Pass/fail definition
- **Done** = 045 (all cases incl. C2 and F2) and 046 (all cases incl. std-F)
  pass; C2 and F2/std-F specifically demonstrate the R2 and R1 fixes. Until the
  fixes land, C2 and F2/std-F are **expected-fail** guards that pin the bug.

---

## 6. Risks / unknowns to the plan itself

- **U1 (drives R1):** The repopulate call sites are now confirmed
  (`BuildSpanningIndexFromPartitions` at indexcmds.c:2947 via index.c:3852 and
  indexcmds.c:1349), but the **`RelationNeedsWAL` / `pendingSyncs` state at the
  `index_insert`** for the `wal_level=minimal` new-relfilenode case has **not**
  been line-confirmed. The R1 *verdict* (gap vs. no-gap) remains
  **[ASSUMPTION]** until that is read. Highest-value next read.
- **U2:** Observability — spanning entries are hidden from the planner
  (`plancat.c`), so tests cannot `SELECT`/`amcheck` the index directly to count
  entries. Tests must infer state via conflict/no-conflict behavior. Consider
  adding a debug-only SQL function (e.g. `progresql_spanning_count(regclass)`) to
  make assertions direct; otherwise tests are behavioral only. **[ASSUMPTION]**
  that `amcheck`/`bt_index_check` works on a spanning root — likely needs its own
  handling; do not assume.
- **U3:** CONCURRENTLY variants (R4) were **not** audited at all this session.
- **U4:** Same-TID-across-partitions fixture (C2) is fiddly to construct
  deterministically; may need `pageinspect` or careful insert/delete sequencing
  to align block/offset. Budget time for fixture engineering.
- **U5:** `wal_level=minimal` + streaming standby are mutually exclusive; F2
  (minimal, crash) and std-F (replica, standby) are **separate** tests by
  necessity.

---

## 7. Effort estimate

| Work item | Est. |
|---|---|
| U1 deep-read (RelationNeedsWAL at repopulate index_insert) — settle R1 | 0.5 day |
| R1 fix (force WAL for spanning repopulate or hook commit-sync) | 0.5–1.5 day |
| R2 fix (tableoid-aware VACUUM cleanup) | 0.5–1 day |
| R7 audit + (if needed) durable DROP/DETACH cleanup or probe-skip | 0.5–1 day |
| R3/R4 audit + reject/handle CONCURRENTLY for spanning | 0.5–1 day |
| Optional debug `progresql_spanning_count` helper (U2) | 0.5 day |
| `045_…_recovery.pl` (12 cases) + harness | 1.5–2 days |
| `046_…_standby.pl` (6 cases, checksums + promotion) | 1–1.5 days |
| meson/Makefile wiring, flake-hardening, review | 0.5 day |
| **Total** | **~6.5–10 days** |

Lower bound if R1 turns out to already-WAL-log (tests only); upper bound if both
R1 and R2 need real fixes plus the CONCURRENTLY rejection.
