# Spanning-index duplicate-entry corruption under sustained churn

**Status: root-caused and fixed (2026-07-11), commit `af2296c944`.**

Under sustained UPDATE-heavy churn, the **deferred drain** path
(`spanning_defer_vacuum = on`) fails to retire dead spanning-index entries.
Duplicate entries accumulate for a single live `(id, partseq)`, and once a stale
entry's heap slot is reused by a live tuple, the cross-partition uniqueness probe
finds it and raises a **phantom `duplicate key` violation on a legitimate
UPDATE**. The heap is never wrong — the damage is index-only — but it is
permanent until `REINDEX`.

The **eager** path (`spanning_defer_vacuum = off`) is correct. The fix flips the
GUC default to `off` and marks the deferred path UNSAFE in its description.

---

## Symptom

`bt_index_check(<spanning index>)` (structure-only) raises **"item order
invariant violated"**. Downstream effects, in the order they tend to be noticed:

- Plain non-key `UPDATE`s on specific ids raise a spurious `UniqueViolation`,
  though the row is not duplicated in the heap.
- The logical-replication apply worker crash-loops when it applies an `UPDATE`
  to an affected id.
- On `--enable-cassert` builds, a page split through an affected page trips the
  suffix-truncation invariant in `_bt_truncate` (`nbtutils.c:3876`): the
  split-point choice meets non-monotonic heap TIDs among the duplicate entries.
  On a non-cassert build this is a silent wrong truncation rather than a crash —
  another reason cassert is mandatory for spanning validation.
- No heap duplicates: `SELECT id, count(*) ... GROUP BY id HAVING count(*) > 1`
  returns nothing.

## Mechanism

The fork can defer spanning dead-entry reclaim. A leaf VACUUM enqueues a
`pg_spanning_drainq` record instead of scanning the spanning index, and a later
coalesced `pg_drain_spanning_index` retires the dead entries in one pass. The
design's safety rests on heap slots being kept un-reusable until the drain runs
(`vacuumlazy.c:2627`) — so a stale entry can never alias a live row.

**That invariant does not hold under sustained churn.** A recreated tuple reuses
a just-freed heap slot before the old spanning entry for that slot has been
retired, so a duplicate same-key entry ends up bound to the now-live reused TID.
From that point the partseq-filtered bulkdelete *cannot* reclaim it: the TID is
live, so the entry never appears in any dead-TID list. It survives until
`REINDEX` rebuilds the index from the heap.

The `LP_DEAD` reclaim in `_bt_check_unique` (`nbtinsert.c:1047`) is a non-durable
dirty hint, so it cannot be relied on to close the gap; the durable path is the
drain, and the drain is what fails to keep pace.

### Three symptom classes

Any future fix to the deferred path must address all three:

1. **False unique-violation** — the operational blocker. **Entry count does not
   predict it.** A key with 176 stale entries can UPDATE fine while a 2-entry
   case violates on every touch. The discriminator is heap-slot-reuse state: a
   violation occurs only when a stale entry's TID was reused by a *live* tuple.
   Most duplicate entries are benign — they bind to dead tuples and are correctly
   skipped.
2. **Page-split assertion crash** (cassert) — as above.
3. **Progressive slowdown** — every hot-row UPDATE's uniqueness check scans the
   growing duplicate chain, so the hottest rows degrade steadily even where
   nothing violates.

### Ruled out

Recorded so these are not re-investigated:

- Diamond / multi-ancestor cache double-insert — `progresql_spanning_ancestors`
  dedups (`spanning_relcache.c`).
- COPY double-insert — the two `copyfrom.c` call sites are mutually exclusive
  (multi-insert-flush vs. single-row).
- INSERT+UPDATE double-call — guarded; a cross-partition update is DELETE+INSERT
  and only the INSERT side maintains the spanning index
  (`nodeModifyTable.c:2362`).
- The `UNIQUE_CHECK_PARTIAL` apply-worker path (0.2.5) as the *source* — affected
  pages carry LSNs predating it. That feature surfaced the bug; it did not cause
  it. This is a pre-existing 1.0-era defect.

## Reproduction

Minimal shape: one partitioned table with a `GLOBAL` PK, `autovacuum = off`,
churning a single id. Both churn shapes reproduce and both must stay covered:

- **delete + recreate** (the bulk-rebuild pattern), and
- **repeated non-key cold UPDATE** of one row — forced cold via an indexed column
  or page fill. This is the hotter real-world pattern.

Accumulation is proportional to churn volume (roughly 2 stale entries per 200
cycles, 5 per 300) and is **not self-healing**: repeated quiescent `VACUUM` plus
`pg_drain_spanning_index` does not reduce the count, and the drain itself can get
stuck with the queue non-empty. Only `REINDEX` recovers.

Pinned in-tree by `progresql_churn_vacuum`, which churns one id 40× and asserts
exactly one live index entry afterwards via `bt_page_items`. It requires
`EXTRA_INSTALL=contrib/pageinspect`.

## Validation of the eager path

Validated under 300 delete+recreate churns and 500 mixed cold-UPDATE+recreate
across 100 ids / 2 partitions: exactly one index entry per live row, `amcheck`
clean, heap slots reaped, no phantom violations. Full regress green on a cassert
build.

**Cost.** The eager path reinstates the per-leaf spanning-index scan the drain
was written to avoid — the O(N²) sweep the deferral targeted. Measured at
**~740µs/row on a rebuild burst**: bounded, and not a correctness concern.

An earlier "eager is catastrophically slow" measurement was a **confound** and
should not be cited: an 81k-row delete took 11.5 minutes, but ~91% of that was a
*missing index on the referencing side of a foreign key* causing sequential scans
during FK checks. With that index present the same delete takes 62.5s. The
lesson is worth keeping — attribute a slow bulk DML to the spanning index only
after ruling out ordinary missing-index effects.

## Recovery for an affected installation

1. `ALTER SYSTEM SET spanning_defer_vacuum = off; SELECT pg_reload_conf();` —
   this needs no new binary, since the eager path exists in any build that has
   the GUC; only the *default* changed. Accumulation stops immediately.
2. `REINDEX INDEX <spanning index>` once per affected index to clear entries
   already accumulated. Plain `REINDEX`, not `CONCURRENTLY` — the concurrent path
   is unproven for spanning indexes.

Without step 1, `REINDEX` alone is only a stopgap: at ordinary write rates
`bt_index_check` can fail again within hours.

## Follow-up (low priority)

Fix or remove the deferred drain itself. It is now opt-in and documented as
unsafe, so this is a performance-reclamation task, not a correctness one — it
buys back the ~740µs/row and only matters if large rebuild bursts become
frequent. If it is fixed rather than removed, the candidate loci are:

- superseding the old entry at insert time when its heap slot is being reused,
- letting bulkdelete dedup live-TID duplicates, or
- an eager pre-reuse retirement.

The acceptance bar should be sustained churn — several days of steady-state
write traffic plus one full rebuild burst, with `bt_index_check` clean
throughout — not a synthetic short stress, since the failure is an
accumulation that a brief run will not surface.

## Correction to earlier analysis

An intermediate conclusion recorded here — that the corruption was
*config-independent* and that `spanning_defer_vacuum = off` was therefore not a
mitigation — was **wrong**, and the retraction is the useful part of this record.
The error: `SET spanning_defer_vacuum` does not carry across `psql` sessions, so
every early repro ran with the deferred path silently still enabled. Only a
server-level setting (`-c spanning_defer_vacuum=off`, or `ALTER SYSTEM` plus
reload) actually exercises the eager path. Verify that a GUC under test is in
force at the level the test assumes before concluding that toggling it changes
nothing.
