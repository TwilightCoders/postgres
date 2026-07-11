# Spanning-index duplicate-entry corruption under delete+recreate churn

**Status: ROOT-CAUSED + FIXED (2026-07-11).** The bug is entirely in the
**deferred DHR drain** (`spanning_defer_vacuum = on`), which is the GUC **default**
and what the mesh runs: the coalesced drain fails to retire dead spanning entries
under sustained churn, so duplicates accumulate (phantom violations, corruption).
The **eager path (`spanning_defer_vacuum = off`) is correct** — validated under
300 delete+recreate churns and 500 mixed cold-UPDATE+recreate across 100 ids /
2 partitions: exactly 1 index entry per live row, amcheck CLEAN, heap reaped, no
phantom violations.

**Fix (code):** flip the GUC default `spanning_defer_vacuum` to `false` (eager,
correct) — `guc_tables.c`. The broken deferred drain becomes opt-in with an
UNSAFE warning in its description. Follow-up: fix-or-remove the drain itself
(non-urgent — eager is correct).

**Fix (operational, immediate, no new binary):** `ALTER SYSTEM SET
spanning_defer_vacuum = off; SELECT pg_reload_conf();` on both mesh nodes (the
eager path is already in the running 1.0 binary) + one `REINDEX INDEX data_pkey`
per node to clear existing dupes → corruption stops accumulating permanently.

**Correction to earlier analysis below:** the "both defer modes accumulate / no
config workaround" conclusion was WRONG — `SET spanning_defer_vacuum` does not
cross psql sessions, so every early repro was secretly `defer=on`. Only a
server-level `-c spanning_defer_vacuum=off` exercises the eager path, and it is
clean. Historical analysis retained below for the record.

---

**(Historical) Status:** root-caused to a mechanism family; exact line-level
trigger not yet pinned (needs a repro). Data-integrity bug, production-observed
on both mesh nodes. Blocks the 0.2.5 conflict-classification release.

## Symptom

`bt_index_check('data_pkey')` (structure-only) raises **"item order invariant
violated"** on both mesh nodes. Downstream:

- Plain non-key `UPDATE`s on specific ids raise a spurious `data_pkey`
  `UniqueViolation` (the row is not actually duplicated in the heap).
- The logical-replication apply worker crash-loops (~5s cycle) on a `data_pkey`
  duplicate-key when it applies an `UPDATE` to a poisoned id.
- No heap duplicates: `SELECT id, count(*) FROM data GROUP BY id HAVING
  count(*)>1` is 0 across all three spanning trees.

## Evidence (claudepilot mesh specimens, 2026-07-06)

- `bt_page_items('data_pkey', 1054)` offsets 3–6 all carry the **identical
  key** (id `019ef28e-…`, **partseq 6**), heap tids `(55,15)` and `(110,2)×3`.
  The live tuple for that id is at `(110,59)` — so **multiple index entries for
  one (id, partseq) bind to dead heap versions**, three of them on a single
  (reused) tid.
- november DETAIL: two entries `(2532,4)` and `(2532,5)` both → heap `(122,1)`.
- Corrupt-page LSNs are **post-revert 1.0** (november `5/B8AFC108` >
  `5/1F86D4B0` @ Jul-3 23:08Z) — written by the 1.0 binary, **not** the rc1
  build. Shared 1.0 writer/cleanup code.
- Four victim ids; three cluster in a **few-hour window during the July-3
  rebuild** (delete N + recreate N per conversation, hundreds back-to-back).
  Correlates with a **write burst**, not a uniform background rate.
- entity/evidence/source GLOBAL indexes: structure-clean on both nodes. Only
  `data_pkey` (the hottest-churned tree) is corrupt.
- Mesh GUCs: `spanning_defer_vacuum = on`, `autovacuum = on`. `pg_spanning_drainq`
  is currently **empty** — the corruption is committed, not pending-in-queue.

## REPRODUCED (2026-07-08) — local, minimal

Scripts: `/tmp/defer_repro.sh`, `/tmp/drain_recover.sh`, `/tmp/recover_test.sh`
(cassert build `build/install-025rc1`). Minimal repro: one partitioned table
with a GLOBAL PK, `autovacuum=off`, churn a single id via delete+recreate.

Findings:
- Churn accumulates **stale duplicate index entries for the one live row**,
  proportional to churn volume (~2 per 200 cycles, ~5 per 300).
- **Config-independent:** identical with `spanning_defer_vacuum` **on and off**.
- **Permanent / non-self-healing:** 4× quiescent `VACUUM` (defer=off) or
  `VACUUM + pg_drain_spanning_index` (defer=on) does NOT reduce the count. The
  deferred drain also gets **stuck** (drainq stays 1). Only **`REINDEX`**
  recovers (→ 1).
- **Therefore `spanning_defer_vacuum=off` is NOT a mitigation** (earlier
  guidance retracted). There is no config workaround; the code fix is required.

Causal picture (matches the mesh `(110,2)×3` specimen): under churn a recreated
tuple **reuses a just-freed heap slot**; the old spanning entry for that slot is
not removed before reuse, so a duplicate same-key entry ends up bound to the
now-live reused TID. VACUUM's partseq-filtered bulkdelete (the vacuum_collision
fix) then **cannot** reclaim it — the TID is live, so the stale entry isn't in
any dead-TID list. It persists until REINDEX rebuilds from the heap.

## Census (january data_pkey, 2026-07-08, claudepilot)

Full page-walk: **8,388 distinct keys with duplicate adjacent entries / 9,551
adjacencies across 7,840 leaf pages.** This is **pervasive** — effectively every
sustained-cold-UPDATE hot row (live conversation rows: 171/176/128 entries) has
accumulated a dup chain — not a handful of victims. REINDEX-both is warranted.

### Three symptom classes (all must be in the fix's acceptance)
0. **Page-split assertion crash (cassert)** — when a poisoned page splits,
   `_bt_truncate`'s split-point choice meets non-monotonic heap TIDs among the
   duplicate entries and trips the suffix-truncation invariant
   (`TRAP` at nbtutils.c:3876). Red test must **force a page split through an
   accumulated-duplicate page** (fill a poisoned page to split point). On a
   non-cassert build this is a silent tolerated wrong-truncation, not a crash —
   so cassert is (again) the validation build that surfaces it.

1. **False unique-violation** — the operational blocker. **Entry count does NOT
   predict it**: a 176-entry key UPDATEs fine while a 2-entry victim violates on
   every touch. The discriminator is **heap-slot-reuse state**: a violation
   happens only when a stale entry's TID was reused by a **live** tuple, so the
   uniqueness scan probes it, finds a live tuple, and reports a conflict. Most
   dup entries are benign (bind to dead tuples, correctly skipped).
   - **Red-test gap:** the current repro reproduces the *accumulation* (benign
     dead dups) but NOT yet the *violating* state. The fix's red test must force
     TID reuse into a live tuple (churn + VACUUM-reclaim + recreate onto the
     reclaimed slot) so a stale entry aliases a live tuple, then show a plain
     UPDATE false-violates — and that the fix removes it.
2. **Perf degradation** — every hot-row UPDATE's uniqueness check scans the
   growing dup chain (176 entries and climbing). Slow degradation on the hottest
   rows even where nothing violates. The fix (no dup accumulation) resolves both.

## Mechanism (confirmed family; exact trigger open)

The fork defers spanning dead-entry reclaim (`spanning_defer_vacuum = on`): a
leaf VACUUM enqueues a `pg_spanning_drainq` record instead of scanning the
spanning index, and a later coalesced `pg_drain_spanning_index` retires the dead
entries. The design claims heap slots are kept "un-reusable until the drain
runs" (vacuumlazy.c:2627), which is what should make the deferral safe.

Under a **sustained delete+recreate burst** (the standard rebuild workload — and
this stays in the product; parser upgrades re-normalize from the raw floor), one
id churns through many tuple versions with heavy TID reuse. The observed end
state — multiple index entries for one (id, partseq) all bound to dead/reused
tids, never marked `LP_DEAD`, surviving into `amcheck` — means the reclaim/reuse
safety broke: **dead spanning entries were not retired before their heap slots
were reused**, so entries accumulated and alias reused slots. The `LP_DEAD`
reclaim in `_bt_check_unique` (nbtinsert.c:1047) is a non-durable dirty-hint;
the durable path is the deferred drain. A crash between VACUUM-defer and drain
(the July-3 window had crashes, incl. the rc1 crash-loop) is the leading
suspect for losing the "un-reusable until drain" guarantee.

### Ruled out
- Diamond / multi-ancestor cache double-insert — `progresql_spanning_ancestors`
  dedups (spanning_relcache.c).
- COPY double-insert — the two `copyfrom.c` call sites are mutually exclusive
  (multi-insert-flush vs single-row).
- INSERT+UPDATE double-call — guarded (cross-partition update = DELETE+INSERT,
  only the INSERT side maintains; nodeModifyTable.c:2362).
- rc1 `UNIQUE_CHECK_PARTIAL` as the *source* — corrupt-page LSNs predate/are
  independent of rc1; november carries it with minimal rc1 exposure. (rc1's
  crash-loop may have *contributed* crashes, not the duplicate entries.)

## Cadence (recurrence rate, measured on the live mesh)

- november: re-stalled ~30h after a REINDEX (with rebuild-adjacent churn).
- january: **amcheck-failing within ~10h of a REINDEX under *ordinary* steady-
  state pump churn** — no rebuild burst, pumps clean, purely structural
  accumulation.
- november: **re-corrupted within *hours*** of a REINDEX (2026-07-10). So this is
  a **continuous** bug at normal write rates, not a burst artifact; REINDEX is
  only a single-digit-hours-to-~30h stopgap. Operationally now absorbed by
  claudepilot's automated tripwire→auto-REINDEX heal on both nodes, but that
  makes the code fix the only durable lever — the mesh cannot run un-reindexed.

## Fix acceptance bar (from the workload owner)

**7 days of january steady-state churn + one full rebuild burst, `bt_index_check`
clean (structure) throughout.** If the fix carries a tunable, that's the profile
to tune against. (No config workaround exists today — both defer modes
accumulate — so the fix is a code change, and this is its gate.)

## Acceptance criterion (from the workload owner)

The delete+recreate burst is the **standard maintenance workload**, not a
synthetic stress. The fix must keep reclaim in pace under **sustained** churn
(hundreds of conversations back-to-back, recurring). This likely rules out a
tuning knob on the autovacuum-gated drain and points at making dead-entry
cleanup durable/eager enough that a rebuild pass cannot outrun it, and/or making
the "un-reusable until drain" guarantee crash-durable. P2.1's real rebuild is
the agreed soak test.

## Next steps
1. **Repro — DONE.** Reproduced on both churn shapes; **the red test must cover
   both** (the UPDATE path is the hotter real-world pattern):
   - delete+recreate (rebuild burst),
   - repeated non-key **cold** UPDATE of one row (pump metadata refresh — force
     cold via an indexed column or page-fill). Both accumulate ~identically and
     are config-independent + permanent-until-REINDEX.
2. **Pin the exact reclaim gap.** Read the spanning-index VACUUM bulkdelete
   (`vacuumlazy.c`, partseq-filtered — the vacuum_collision fix) and the
   insert-under-TID-reuse path: where does a duplicate same-key entry survive
   because its heap slot was reused (TID now live) before the old entry was
   removed? Confirm whether the fix belongs in (a) removing/superseding the old
   entry at insert time when its slot is being reused, (b) letting bulkdelete
   dedup live-TID duplicates, or (c) an eager pre-reuse retirement.
3. **Fix + validate on a cassert build** against the two-shape red test (cassert
   is now mandatory for spanning validation — the november gate that missed the
   rc1 snapshot crash had none). P2.1's real rebuild is the soak test.

### Interim posture (until the fix lands)
- **No config workaround exists** — `spanning_defer_vacuum=off` does NOT prevent
  accumulation (retracted). Do not rely on it.
- **REINDEX unblocks but does not prevent recurrence.** `REINDEX INDEX
  data_pkey` clears the corruption to unblock a stalled node (Dale-gated), but
  churn re-accumulates. So: **hold rebuild bursts (P2.1) until the fix**, or plan
  to re-REINDEX after each burst.
- **Prevention/monitoring:** nightly structure-only `bt_index_check`
  transition-alarm on both nodes (claudepilot MeshWatchdogJob) — live/building.

### Open production action (Dale-gated, pending on 2026-07-08)
november apply-stalled ~2 days on a poisoned id. Blessed unblock: plain
`REINDEX INDEX data_pkey` on november (NOT concurrently; it's apply-stalled).
No `defer` change. january untouched (specimen). Awaiting Dale's go.

## Relationship to other in-flight work
- **rc1 snapshot crash** (separate bug, fixed, unvalidated): `spanning_probe_conflict`
  passed an unregistered `GetLatestSnapshot()` to the heap fetch → cassert TRAP.
  Fixed (push active snapshot); needs a cassert re-validate. Independent of this
  corruption.
- **0.2.5 conflict-classification** feature: correct design (validated on
  november), blocked behind this corruption fix — do not ship conflict-handling
  on an index that can double-write entries under churn.
