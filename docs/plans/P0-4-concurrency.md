# P0-4 — Concurrency, Locking, Deadlock & Isolation Correctness

Status: ANALYSIS / PLAN. No `src/` files were modified.
Scope: the spanning-index cross-partition uniqueness machinery and its concurrency
behavior. Fork base: PostgreSQL `REL_18_STABLE`.

Verification tags: `[VERIFIED file:line]` = read directly in source this session;
`[ASSUMPTION]` = inferred from stock-PG semantics or commit history, not byte-verified.

> Note on the parent doc: PRODUCTION_READINESS.md P0-4 cites
> `nbtinsert.c ~576-672` and the brief references separate
> `_bt_check_unique_spanning` / `_bt_spanning_heap_live` helpers and an
> `es_progresql_partition_cache` release at the tail of `ExecInsertIndexTuples`.
> **All three are stale.** As actually implemented: the spanning logic is *inline*
> in `_bt_check_unique` (no helper functions); the relevant lines are 433-810; the
> probe opens partitions at `nbtinsert.c:597` and `:684`; and the executor cache is
> released from `FreeExecutorState`, not from `ExecInsertIndexTuples`. This plan
> uses the verified line numbers.

---

## 1. Problem restatement

A spanning index is a B-tree on the **partitioned root** whose key is
`(user_cols…, tableoid)` with `tableoid` as a hidden trailing attribute;
`indnuniqatts > 0` records how many *leading* attributes form the user uniqueness
key. The check limits the scankey to those leading columns
`[VERIFIED nbtinsert.c:438-441]`.

Because one physical B-tree references heap rows in *different* physical heaps (one
per partition), the heap-liveness probe cannot run against a single heap. When
`_bt_check_unique` finds a candidate duplicate index tuple, it decodes the trailing
`tableoid` from the candidate (`index_getattr(curitup, saved_keysz, …)`
`[VERIFIED :591-592]`) and, if non-null, `table_open(child_relid, AccessShareLock)`
`[VERIFIED :597]`, then probes liveness in *that* heap. There is a *second*,
symmetric open for the **self** tuple's partition during the definite-conflict
recheck: `table_open(self_relid, AccessShareLock)` `[VERIFIED :684]`.

This mid-insert open of one (often two) sibling relations is the entire source of the
P0-4 hazard surface: a second/third relation lock acquired in the middle of an insert,
in an order determined by which key collides with which partition (data-dependent lock
ordering — the classic deadlock precondition). Stock `_bt_check_unique` never opens a
second relation.

Secondary surface: the executor opens partition ancestors + spanning indexes during
DML through a per-statement `EState` cache (`es_progresql_partition_cache`), built
lazily per partition `[VERIFIED execIndexing.c:1162-1235]` and released by
`FreeExecutorState` → `ProgresqlReleasePartitionCache`
`[VERIFIED execUtils.c:228-231, execIndexing.c:1262-1285]`.

---

## 2. Mechanism analysis (with file:line)

### 2.1 `_bt_check_unique` — the spanning probe (inline)
`[VERIFIED nbtinsert.c:410-810]`. Setup: `nuniqs =
IndexRelationGetNumberOfUniqueAttributes(rel)`; `saved_keysz = itup_key->keysz`; if
`nuniqs < keysz`, temporarily reduce `keysz` to `nuniqs` `[VERIFIED :438-441]`.
`SnapshotData SnapshotDirty` is declared once `[VERIFIED :419]` and
`InitDirtySnapshot` once `[VERIFIED :446]`. Per candidate heap-TID in the equal-key
scan `[VERIFIED loop :467+]`:

- LP_DEAD items are skipped `[VERIFIED :521]` (this is how DROP/DETACH-retired entries
  are ignored — see commit `67a5276404`).
- Conflict probe: if `nuniqs < saved_keysz`, decode the candidate's trailing tableoid
  `[VERIFIED :586-600]`, `spanChildRel = table_open(child_relid, AccessShareLock)`
  `[VERIFIED :597]`, `checkRel = spanChildRel`. Then
  `table_index_fetch_tuple_check(checkRel, &htid, &SnapshotDirty, &all_dead)`
  `[VERIFIED :602-604]` — the authoritative dirty probe.
- `UNIQUE_CHECK_PARTIAL` early-out: close `spanChildRel`, release nbuf, return Invalid
  `[VERIFIED :616-625]`.
- `xwait = TransactionIdIsValid(SnapshotDirty.xmin) ? xmin : xmax` `[VERIFIED :631-632]`.
  If valid: **close `spanChildRel` first** `[VERIFIED :637]`, release nbuf,
  `*speculativeToken = SnapshotDirty.speculativeToken` `[VERIFIED :641]`, return xwait
  `[VERIFIED :645]`. Caller (`_bt_doinsert`) then waits and retries
  `[VERIFIED :216-235]`.
- Definite conflict path: re-open the *self* partition `table_open(self_relid,
  AccessShareLock)` `[VERIFIED :684]`, probe self with `SnapshotSelf`
  `[VERIFIED :690-691]`; if self is dead, close self + span rels and break
  `[VERIFIED :701-705]`; else close self rel `[VERIFIED :708-709]`,
  `CheckForSerializableConflictIn` `[VERIFIED :719]`, close span rel `[VERIFIED :722]`,
  `ereport(ERROR, UNIQUE_VIOLATION)` `[VERIFIED :749]`.
- Not-live / all-dead continue paths close `spanChildRel` at `[VERIFIED :772]` and
  `[VERIFIED :790]`.

**Cleanup audit result:** every exit of the per-candidate block that opened
`spanChildRel` closes it (619, 637, 702+704, 722, 772, 790) and every `selfChildRel`
open (684) is closed (702, 709). I found **no leaked `table_open`** on the paths I
read. The variable is block-scoped to each iteration, so there is no stale carry-over
across iterations. This *contradicts* my first-draft H6 worry — see revised H6 below.

### 2.2 Caller wait/retry — `_bt_doinsert`
`[VERIFIED :208-236]`: on valid xwait, release buffer, then
`SpeculativeInsertionWait(xwait, speculativeToken)` if token set, else
`XactLockTableWait(xwait, rel, &itup->t_tid, XLTW_InsertIndex)`, free stack, `goto
search` (full restart). Standard stock protocol; the only spanning change is that
`xwait`/token came from a sibling-heap dirty probe.

### 2.3 Executor cache
`progresql_build_partition_cache_entry` `[VERIFIED execIndexing.c:1162-1235]`: lazily
creates an HTAB in (per docstring) `es_query_cxt`; on miss walks
`get_partition_ancestors`, `table_open(parentOid, AccessShareLock)` `[VERIFIED :1204
and :1230]`, opens each ancestor index, keeps only spanning ones
(`indnuniqatts != 0` filter `[VERIFIED :1214]`) at the lock `index_open` took.
Released by `ProgresqlReleasePartitionCache` `[VERIFIED :1262-1285]`, which
`hash_seq` the table and `table_close(se->parentRel, AccessShareLock)` `[VERIFIED
:1280]` then `hash_destroy` `[VERIFIED :1284]`. Wired into `FreeExecutorState`
`[VERIFIED execUtils.c:228-231]` and the field is zero-initialized at EState creation
`[VERIFIED execUtils.c:168]`. Because `FreeExecutorState` runs on **both** normal and
error/abort teardown of the executor, the cache locks are released on the error path
too. (Even if it did not, ResourceOwner would release the locks at xact abort.)

### 2.4 Snapshot/visibility
Both the conflict probe `[:602]` and the executor entry-point use the standard dirty
snapshot; the self-recheck uses `SnapshotSelf` `[:691]` exactly as stock does. Dirty
snapshot is the correct and required choice for uniqueness: it must see in-progress
(uncommitted) inserts to wait on them; an MVCC snapshot would miss a concurrent
uncommitted duplicate and let two committers both succeed.

### 2.5 DDL lock levels (verified)
- CREATE/ALTER...ADD spanning index build is gated by `progresql_bypass`
  `[VERIFIED indexcmds.c:754, 1348-1349]`; `BuildSpanningIndexFromPartitions` opens
  each partition `table_open(partOid, AccessShareLock)` `[VERIFIED indexcmds.c:2924]`.
- REINDEX (non-concurrent) takes **AccessExclusiveLock** on the index
  `[VERIFIED indexcmds.c:3099-3100]`; REINDEX CONCURRENTLY uses
  ShareUpdateExclusiveLock `[VERIFIED :3099-3100, :3145-3146]` and the
  multi-phase `WaitForLockersMultiple(... ShareLock / AccessExclusiveLock ...)`
  `[VERIFIED :4247, :4306, :4472, :4514]`. Spanning repopulate after rebuild is via
  `BuildSpanningIndexFromPartitions` (commit `355dfaa9fc`, BUG B).
- ATTACH backfill: `progresql_backfill_spanning_indexes_for_attached_partition`
  `[VERIFIED tablecmds.c:2112, called :20688]`, inserts existing rows with
  `UNIQUE_CHECK_YES` (per commit `355dfaa9fc`) — runs inside ATExecAttachPartition,
  which holds the attach-time locks `[ASSUMPTION: ShareUpdateExclusiveLock on parent
  for the ATTACH machinery; partition lock level not byte-verified]`.
- DROP / DETACH cleanup: `progresql_clean_spanning_indexes_for_partition`
  `[VERIFIED tablecmds.c:2006, called :2476, :21202, :21360; heap.c]`, retires entries
  via kill_prior_tuple (commit `67a5276404`, BUG A). DROP/DETACH take
  AccessExclusiveLock on the partition `[ASSUMPTION: stock]`; DETACH CONCURRENTLY uses
  a lighter lock and a two-step finalize (`DetachPartitionFinalize`) `[VERIFIED
  cleanup is called from both; lock level ASSUMPTION]`.

---

## 3. Enumerated hazard scenarios + verdicts

### H1 — Two inserts into different partitions, opposite probe order (deadlock?)
Backend 1 inserts key K into partition A; its probe opens B (`:597`). Backend 2
inserts K' into B; its probe opens A. All probe opens use **AccessShareLock**
`[VERIFIED :597, :684]`; the executor cache opens are AccessShareLock (parent) +
index lock `[VERIFIED :1204/:1230]`. AS is self-compatible, so two backends crossing
A↔B cannot deadlock on heap relation locks. The insert itself only holds an
nbtree **buffer** lock on the root index leaf page, which is released before waiting
`[VERIFIED :219, :637-639]`.
**Verdict: SAFE against relation-lock deadlock** (AS/AS compatible). The row-level
(xact) waits are H2; the DDL-interleaved AS-vs-AX case is H4.

### H2 — Two concurrent inserts of the SAME user key into DIFFERENT partitions
Core spanning guarantee: exactly one must win. Walk: each backend heap-inserts (INSERT
is speculative only under ON CONFLICT; plain INSERT is non-speculative), then
index-inserts into the root spanning B-tree. The second to reach the leaf finds the
first's index entry, opens the sibling partition (`:597`), dirty-probes it (`:602`),
reads `xwait` = the first's xid (`:631`), and waits via `XactLockTableWait`
(non-speculative) or `SpeculativeInsertionWait` (ON CONFLICT) (`:227-230`), then
restarts. On restart it sees the committed row → `ereport(UNIQUE_VIOLATION)` (`:749`),
or sees it aborted → proceeds. Structurally identical to stock with `probeRel`
swapped to the sibling.
Open concerns:
  (a) **Symmetric wait / mutual deadlock.** If both backends insert into the *same
      root index leaf page* (same user key ⇒ same leaf), the nbtree buffer-lock
      discipline serializes them: only one holds the leaf write-lock at a time, and a
      waiter drops the buffer before `XactLockTableWait` (`:219`). So the *non*-spec
      path serializes through the page lock and one inserter strictly precedes the
      other — no symmetric xact-wait cycle. **Likely SAFE**, but must be proven by
      test (the page-lock argument is stock; the spanning twist is the extra heap open,
      which is released before the wait at `:637`).
  (b) The dirty snapshot is reused across loop iterations within one call (`:446`
      once). Stock relies on `table_index_fetch_tuple_check` refilling it each call;
      confirm the spanning multi-iteration scan does not act on a stale
      `xmin/xmax/speculativeToken` from a previous offset. `[ASSUMPTION: refilled per
      call]`.
**Verdict: LIKELY-SAFE, UNVERIFIED. Gate on isolation test S-samekey before any "safe"
claim.** This is the headline scenario.

### H3 — Cross-partition UPDATE (row movement) racing a concurrent insert of same key
An UPDATE changing the key moves the row A→B (delete leg in A + insert leg in B). The
spanning write side: `ExecInsertSpanningIndexTuples` is called from the UPDATE
epilogue, and `spanning_unique_unchanged` `[VERIFIED execIndexing.c:1298-1303]` skips
the spanning write when no unique column changed — but a *key change* does not skip, so
a new `(key, newTableoid)` entry is written and the old `(key, oldTableoid)` entry is
retired by the row-movement delete. During the window the mover has an uncommitted
insert in B and the old row in A still dirty-visible. A concurrent INSERT of the same
key: if its scan hits the B entry first, it dirty-probes B, sees the mover's xid,
waits — correct. If it hits the A entry first, it dirty-probes A, finds the old row
still live (delete uncommitted), and could `ereport` a violation against a row about
to vanish, *or* wait on the mover and re-resolve on commit. Whether the candidate scan
order + the dirty probe always yield "wait, then exactly-one" has **not** been traced
end-to-end. Row movement is `ExecDelete`+`ExecInsert` with its own update-chain locks.
**Verdict: UNKNOWN — highest-risk scenario. Needs S-rowmove.**

### H4 — DETACH / DROP / ATTACH / REINDEX racing with DML
- **REINDEX (non-concurrent):** AccessExclusiveLock on the index `[VERIFIED
  indexcmds.c:3099-3100]` serializes with all DML on the spanning index → SAFE.
- **REINDEX CONCURRENTLY:** ShareUpdateExclusiveLock + multi-phase WaitForLockers
  `[VERIFIED :3145-3146, :4247+]`. The spanning repopulate
  (`BuildSpanningIndexFromPartitions`) must run in a phase where it cannot miss
  concurrently-inserted rows or admit duplicates. Not analyzed for the spanning case.
  **UNKNOWN.**
- **DROP / DETACH (non-concurrent):** AccessExclusiveLock on the partition
  `[ASSUMPTION]`. An in-flight insert holding AS on its target partition + wanting AS
  on the partition being dropped, vs. the DDL holding AX-intent — AS-vs-AX is
  *incompatible*, so a **genuine deadlock window** exists. PG's deadlock detector
  resolves it by aborting one side (safe for integrity), but user DML can be aborted
  by a concurrent DROP/DETACH. Cleanup ordering (retire entries *before* pg_inherits
  update, commit `67a5276404`) is correct for the serialized case. **SAFE for
  integrity; deadlock/abort behavior must be characterized.**
- **DETACH CONCURRENTLY:** lighter lock + two-step finalize `[VERIFIED cleanup called
  from DetachPartitionFinalize]`. Risk: a probe opens a partition mid-detach (catalog
  says detaching but heap still present), reading a row no longer in the index's
  logical domain, yielding a spurious violation — or the reverse, a missed conflict.
  Needs a guard that the decoded `tableoid` is still a current partition of the root
  before trusting a live hit. **UNKNOWN/UNSAFE.**
- **ATTACH backfill:** inserts pre-existing rows with UNIQUE_CHECK_YES `[VERIFIED
  commit 355dfaa9fc]`; must hold a lock that blocks concurrent same-key DML *or*
  re-validate, else a duplicate inserted concurrently during backfill can slip
  through. Lock level `[ASSUMPTION]`. **UNKNOWN.**
**Verdict: REINDEX(non-conc) SAFE; DROP/DETACH SAFE-for-integrity (deadlock TBD);
DETACH CONCURRENTLY / ATTACH backfill / REINDEX CONCURRENTLY UNKNOWN.**

### H5 — Snapshot correctness of the liveness probe
Dirty snapshot is the right choice and is used `[VERIFIED :446, :602]`; self-recheck
uses SnapshotSelf `[VERIFIED :691]`. Remaining subtlety: token/xmax freshness when the
single `SnapshotDirty` is reused across multiple equal-key candidate iterations
(H2b). `[ASSUMPTION-clean; verify against tableam contract.]`
**Verdict: SAFE (snapshot type) / UNKNOWN (struct reuse across iterations).**

### H6 — Resource discipline (revised after source audit)
My first-draft worry (leaked `spanChildRel` on the xwait/retry path, stale variable
across iterations, cache not freed on error) is **largely refuted by the source**:
`spanChildRel` is closed before the xwait return `[VERIFIED :637]`; the variable is
re-declared per iteration; the executor cache is freed by `FreeExecutorState`
`[VERIFIED execUtils.c:228-231]` which runs on error too. Residual items worth a
second look (lower priority): (i) the conflict probe at `:597` and self probe at `:684`
do **two** `table_open`s per definite-conflict candidate — correctness-neutral but a
hot-path cost; (ii) confirm there is no path where `ereport` at `:749` fires while
`selfChildRel` is still open (it is closed at `:709` before the ereport block) — looks
fine.
**Verdict: SAFE (no leak found). Optional perf cleanup only.**

---

## 4. Plan — fix unsafe, prove the unknowns

### P-A. Characterize / harden DML-vs-DDL deadlock (H4)
Decide policy for the AS-vs-AX DROP/DETACH window: (1) document + accept (rely on the
deadlock detector; add an isolation test asserting `deadlock detected` rather than a
hang), and/or (2) order partition opens by OID in the probe (`:597`/`:684`) and the
executor cache so DML-vs-DML is canonical (does not help DML-vs-DDL). Recommend (1)
now; (2) is a stretch.

### P-B. Guard the probe against detaching/dropped partitions (H4 DETACH CONCURRENTLY)
Before trusting a live hit from a sibling partition, verify the decoded `tableoid` is
still a current partition of the root (e.g. it still inherits, or hold the open under
a lock that conflicts with the detach finalize). This closes the
read-a-row-no-longer-in-domain window.

### P-C. Prove ATTACH backfill + REINDEX CONCURRENTLY uniqueness under concurrency
Read the exact lock levels around `ATExecAttachPartition` /
`progresql_backfill_spanning_indexes_for_attached_partition` and the REINDEX
CONCURRENTLY phases; prove backfill/rebuild either blocks concurrent same-key DML or
re-checks. If not, add the missing lock or recheck.

### P-D. Settle H2 / H3 by isolation tests (no code change unless tests fail)
The page-lock serialization argument for H2 is strong but must be demonstrated; H3
(row movement) is genuinely uncertain. Build S-samekey and S-rowmove first; only then
decide whether code changes are needed.

### P-E. (Optional) consolidate the double heap open + verify SnapshotDirty reuse
Confirm `SnapshotDirty` is refilled per `table_index_fetch_tuple_check` (H5/H2b). If
desired, avoid the second `table_open` at `:684` by reusing the already-open
`spanChildRel` when self and conflict are the same partition. Perf/clarity only.

---

## 5. Isolation-spec test plan (`src/test/isolation/specs`)

Each spec builds a spanning-UNIQUE root (the `INHERITS` + `PARTITION BY` opt-in) with
2-3 partitions; sessions s1/s2 (+s3 for DDL). Wire into
`src/test/isolation/isolation_schedule`; capture expected output.

- **`spanning_samekey_siblings`** (H2 — headline). Root + parts p_a, p_b on disjoint
  ranges. s1 `BEGIN; INSERT (K into p_a-range);` s2 `BEGIN; INSERT (K into
  p_b-range);`. Permutations: s1ins, s2ins(blocks), s1commit → s2 must get
  unique_violation; and s1ins, s2ins, s1rollback → s2 succeeds. Assert **exactly one**
  surviving row with key K in every permutation. Include the "both reach the insert
  before either commits" permutation to surface any symmetric deadlock.

- **`spanning_lockorder_ab_ba`** (H1). s1 inserts K1 into A (probe touches B); s2
  inserts K2 into B (probe touches A); crossing order, distinct keys. Assert both
  succeed — regression guard that the probe lock was not escalated.

- **`spanning_rowmove_vs_insert`** (H3). s1 `BEGIN; UPDATE ... SET key=K` causing A→B
  movement; s2 `BEGIN; INSERT (K)`. Permute s2's probe before/after s1's delete and
  insert legs. Assert exactly one row with key K after both commit; assert s2
  waits on s1 and resolves correctly on s1 commit vs rollback.

- **`spanning_detach_vs_dml`** (H4). s1 long-running insert; s3 `DETACH PARTITION b`.
  Assert serialization and a consistent outcome (no orphan index entries, no lost
  uniqueness); a variant asserting the deadlock detector fires rather than hanging.
  Second variant with `DETACH ... CONCURRENTLY` asserting the probe never reports a
  violation against a detaching partition's rows.

- **`spanning_attach_backfill_vs_insert`** (H4). s3 `ATTACH PARTITION` (rows containing
  K); s2 concurrently inserts K elsewhere. Assert the duplicate is caught regardless of
  interleaving — concurrency counterpart of the BUG-C regression.

- **`spanning_reindex_vs_insert`** (H4). s3 `REINDEX` and (separate) `REINDEX
  CONCURRENTLY` the spanning index while s1/s2 insert distinct + duplicate keys. Assert
  uniqueness holds and the rebuilt index has no missing/duplicate entries.

Emphasis per brief: `spanning_samekey_siblings` (sibling same-key race) and
`spanning_lockorder_ab_ba` + `spanning_detach_vs_dml` (lock ordering) are the must-pass
gates.

---

## 6. Risks / unknowns
- **H3 row movement** is the deepest open correctness question (transient
  false-positive or missed conflict during the move window).
- **DETACH CONCURRENTLY / ATTACH backfill / REINDEX CONCURRENTLY** bypass the strong
  serialization the safe verdicts lean on; their spanning-specific lock adequacy is
  `[ASSUMPTION]` until tablecmds.c/indexcmds.c lock levels are byte-verified.
- **SnapshotDirty reuse** across candidate iterations is `[ASSUMPTION]`-clean.
- DML can be aborted by concurrent DROP/DETACH via the deadlock detector (AS-vs-AX);
  acceptable for integrity, decide operational policy.
- Double `table_open` per definite-conflict candidate (`:597` + `:684`) is a hot-path
  cost, not a bug.

## 7. Effort estimate
- Verification/tracing (P-B/P-C lock levels, H3 row-move trace, SnapshotDirty reuse):
  ~1.5-2 days.
- Code fixes *iff* tests fail (P-B detach guard; P-C backfill lock/recheck; optional
  P-E): ~1-2 days, contingent.
- Isolation specs (6 specs + schedule + expected output), dominated by S-samekey /
  S-rowmove / DETACH permutation design and flake shake-out: ~2-3 days.
- **Total ~4-7 days**, front-loaded on `spanning_samekey_siblings`,
  `spanning_rowmove_vs_insert`, and `spanning_detach_vs_dml`.
