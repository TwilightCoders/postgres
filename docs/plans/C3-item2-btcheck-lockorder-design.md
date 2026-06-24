# C3 — Audit item #2: `_bt_check_unique` lock-ordering restructure (design)

> Status: **DESIGN ONLY — not implemented.** The current code is documented-safe
> (deadlock-free, no correctness impact); the only residual is a narrow latency
> window. This restructure touches the hottest correctness path in the fork, so
> it is deliberately gated on a focused, reviewed session with the empirical
> repro + soak called for at the bottom — not an unattended change.
>
> Companion to the in-code "Lock-ordering note" at
> `src/backend/access/nbtree/nbtinsert.c` (candidate-side open) and the `C1` row
> in `C3-spanning-bug-hunt-findings.md` ("VERIFIED SAFE — serialized by the
> partition-parent AccessExclusiveLock vs the INSERT's AccessShareLock on the
> root").

## The finding

`_bt_check_unique()` probes a candidate duplicate's heap tuple for liveness. For
a spanning index the candidate's TID lives in a **leaf partition**, not in
`heapRel` (the storage-less partitioned root), so the code resolves the
candidate's trailing `partseq` to a child OID and opens that partition before
the liveness fetch. Two sites do this:

- **Candidate side** — `nbtinsert.c`, in the SnapshotDirty liveness branch:
  `spanChildRel = try_table_open(child_relid, AccessShareLock);`
- **Self side** — the "is the new tuple itself committed-dead?" branch:
  `selfChildRel = try_table_open(self_relid, AccessShareLock);`

Both acquire a **heavyweight lock (AccessShareLock) while the index leaf buffer
content lock is held** (`insertstate->buf`, and possibly `nbuf`). PostgreSQL's
standard lock ordering is heavyweight-locks-before-buffer-content-locks; this is
an inversion.

## Why it is currently safe (and why the residual is latency-only)

The in-code note and the `C1` finding establish the invariant:

- A spanning-maintaining INSERT holds **AccessShareLock on the root**.
- **Every path that removes a partition** (DROP, DETACH, TRUNCATE-remap) takes
  **AccessExclusiveLock on the partitioned root**, which conflicts with that
  AccessShareLock.

So the 2-party race (this INSERT vs. a concurrent partition removal) cannot
corrupt: the two serialize at the root, or the lock manager detects a cycle and
aborts one. `try_table_open` (vs `table_open`) further degrades a
genuinely-vanished relid to a clean "treat as unresolved / not-a-conflict" skip
rather than an elog on the storage-less root.

The **residual** is purely a latency window, not a correctness hole: a *third*
backend waiting on the held leaf page can be stalled for the duration of the
DDL's lock wait, because this INSERT keeps the buffer content lock while it
blocks inside `try_table_open` on the partition's AccessExclusiveLock. Stock PG's
own conflict-wait path (the `xwait` case a few lines below) avoids exactly this
by releasing buffers *before* it waits and letting `_bt_doinsert` re-descend.

> ⚠️ Invariant caveat to re-verify first: **DETACH … CONCURRENTLY** deliberately
> avoids taking the parent's AccessExclusiveLock. The fork already contemplates
> detached-but-not-yet-retired partitions (the `partseq`-unresolvable skip +
> deferred pre-commit retirement), so detach is handled for *correctness*. But
> any restructure that leans harder on "root AEL serializes removal" must first
> confirm the CONCURRENTLY path cannot let a probe name a partition whose storage
> is being torn down. This is the single most important thing to nail down before
> writing code.

## Restructure options

### Option A — open the probe partitions with `NoLock` (recommended primary)

Because the root AccessShareLock already serializes against every partition
removal, a *fresh* heavyweight lock on the child buys nothing for correctness —
it only creates the ordering inversion. Replace the two `try_table_open(...,
AccessShareLock)` calls with a `NoLock` open of the already-protected child:

- No heavyweight acquisition under the buffer content lock → inversion gone,
  latency residual gone, zero re-descent cost.
- Matches the code's *existing* safety argument exactly (we are not inventing a
  new invariant, just declining a redundant lock).

Open questions to resolve before adopting:
1. `relation_open(relid, NoLock)` assumes the caller already holds *a* lock or is
   otherwise certain the rel is stable. Our certainty comes from the root ASL +
   removal-needs-root-AEL invariant — write that justification into the comment,
   and **confirm the assert-build behavior** (`relation_open`'s NoLock assertion
   path) does not trip.
2. We lose `try_table_open`'s graceful NULL on a vanished relid. Preserve the
   defensive skip with an explicit existence guard
   (`SearchSysCacheExists1(RELOID, ...)` / `RelationIdGetRelation` returning NULL)
   so a future weakening of the invariant still degrades to "unresolved" rather
   than an elog. Keep the storage-less-root guard (`rd_tableam == NULL`) as the
   last line of defense.
3. Re-verify the DETACH CONCURRENTLY caveat above under `NoLock`.

### Option B — release buffers, open, re-descend (the note's suggestion)

Mirror the `xwait` path: on encountering a partition not already open, drop the
buffer content lock(s), `table_open(child, AccessShareLock)` in correct order,
then signal `_bt_doinsert` to re-call `_bt_check_unique` from scratch.

- Most faithful to PG conventions; keeps a real AccessShareLock.
- Cost: control-flow surgery in the hottest path; a re-descend per *distinct*
  partition probed; needs a per-call opened-relation cache (keyed by relid) to
  guarantee forward progress, else a partition that keeps (dis)appearing could
  livelock the retry. Highest complexity and regression risk.

### Option C — pre-open candidate partitions before taking the page lock

Rejected: the set of `partseq`s in the equal-key run is unknown until the page is
scanned under the buffer lock, and pre-opening every leaf of the index is
unbounded.

## Recommendation

Pursue **Option A** if (and only if) the DETACH-CONCURRENTLY invariant check and
the assert-build `NoLock` check both come back clean; it is the smallest, lowest-
overhead change and is consistent with the safety reasoning already shipped. Hold
**Option B** as the fallback if a `NoLock` open proves unsound for any removal
path. Apply the chosen change to **both** sites (candidate and self) identically.

## Verification plan (required before merge)

1. **New isolation spec** reproducing the third-backend latency window: two
   inserters contending the same leaf page for a duplicate key, a concurrent
   DROP/DETACH of the conflicting partition, and a third reader on the held page;
   assert no stall beyond the serialization point and no spurious error.
2. **cassert + amcheck soak with `--ddl-churn`** (already concurrent DROP/CREATE
   during drain) — confirm zero deadlocks, zero duplicates, zero crashes,
   amcheck OK, matching the current baseline (1.69M txns / 15 min / 0 dups).
3. Full **regression 243/243 + isolation** must stay green.
4. Bench the hot uniqueness path (pgbench unique-insert mix) before/after to
   confirm Option A is at worst neutral (expected: a hair faster — one fewer lock
   acquisition per probe).

## Why this is deferred, not dropped

The path is **correct today**; the open item is a latency edge during concurrent
partition DDL, which the in-code note already discloses. Restructuring
`_bt_check_unique` — the single hottest correctness routine the fork touches —
warrants a human-reviewed session with the empirical repro in hand, not an
unattended edit. Audit sibling **#9** (the `spanning_backfill_leaf` helper
extraction) is done (`spanning:` commit on `progresql-c1`).
