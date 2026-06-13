# C3 / #38 — DHR-lift: deferred spanning cleanup for local-index leaves

> Design for lifting the `nindexes == 0` restriction so leaves that ALSO carry
> local indexes get the O(N²)→O(N) deferred spanning drain, not just spanning-only
> leaves. This is the XL change that gates #39 (flip `spanning_defer_vacuum` on by
> default for the common case). Builds on the DHR architecture in
> [C2-deferred-vacuum-DHR-design.md](C2-deferred-vacuum-DHR-design.md).
>
> Status: **DESIGN ONLY — not implemented.** Drafted 2026-06-13 while the
> deferred-path scale-soak (#39 gate) validated the current `nindexes==0` path on
> the NAS. Corruption-sensitive core change: implement under direct review, TDD,
> redfield (no commit until confirmed fixed).

## Where we are
DHR today defers spanning cleanup for exactly ONE of `lazy_vacuum`'s call sites:
the `nindexes == 0` branch (`vacuumlazy.c:3242`). The other two — the bypass
branch (`:3362`) and the all-indexes-succeeded branch (`:3385`) — are reached
only when the leaf has local indexes (`nindexes > 0`), and they ALWAYS retire
spanning entries **eagerly** (`progresql_vacuum_spanning_indexes`), ignoring
`spanning_defer_vacuum`. So a spanning-indexed partitioned table that also has any
local index per leaf still pays the full O(N²) spanning scan per sweep. The GUC is
a no-op for it.

That eager confinement is deliberate and documented in-code (`:3254-3260`,
`:3355-3360`, `:3379-3383`): **the coalesced drain reaps every LP_DEAD slot it
finds, and only retires SPANNING entries.** For a spanning-only leaf the spanning
index is a dead slot's sole index reference, so retire-then-reap is sound. A
local-index leaf can hold an LP_DEAD slot whose **local-index** entry is still
live — and reaping that slot orphans the live local entry (and worse, a later
insert reusing the freed slot makes the stale local entry point at the wrong
tuple). That is the reuse trap, in the local-index direction.

## The precise hazard
Drain reap today = `spanning_drain_build_lpdead_store(partRel)` (collect ALL
current LP_DEAD line pointers on the partition) → retire spanning entries for that
set → `spanning_drain_reap_partition` (free that set). Partition the drain-time
LP_DEAD set `D` of a local-index leaf into:

- **`D_old`** — slots already LP_DEAD when their leaf vacuum ran. The eager path
  would have retired their LOCAL entries in `lazy_vacuum_all_indexes` before
  enqueueing. Local entries gone; only spanning remains.
- **`D_new`** — slots that became LP_DEAD by on-access `heap_page_prune` AFTER the
  last leaf vacuum (or that the leaf vacuum never index-cleaned). **Local entries
  still LIVE**, waiting for the next local-index vacuum.

For `nindexes == 0` there is no `D_new` problem — there are no local entries, so
reaping all of `D` is safe. For a local-index leaf, the drain reaping `D_new`
orphans live local entries. **This is the single correctness barrier #38 must
remove.** (It is the mirror of the parent doc's RISKS bullet "audit ALL
LP_DEAD→LP_UNUSED transitions": the drain's reap is exactly such a transition, and
for local-index leaves it is currently unsafe.)

## Options considered

### Option 1 — eager local vacuum + durable per-TID reap-set
Keep `lazy_vacuum_all_indexes` (eager LOCAL cleanup of the leaf's own dead set
`D0`) on the leaf hot path; defer only the spanning retirement AND the reap of
`D0`; record `D0` **durably** so the drain reaps exactly `D0`, never `D_new`.
- **Cost:** the drainq must carry the dead TID list, not just `(idxid, partseq)`.
  Either one row per dead TID (volume O(dead tuples) — defeats the "tiny, O(#parts)
  rows" property the whole design rests on) or a serialized variable-length TID
  blob per `(idxid, partseq)` (awkward catalog shape, bespoke (de)serialization).
- **Plus:** the drain still retires spanning entries for ALL-LP_DEAD it scans
  (including `D_new`), so `D_new` slots end up with spanning retired but local
  live and un-reaped — a half-cleaned state the next vacuum must reconcile.
- **Verdict:** rejected. Reintroduces the O(dead-tuples) durability the DHR
  design explicitly avoids, and is the more error-prone of the sound options.

### Option 2 — defer EVERYTHING to the drain (RECOMMENDED)
For a local-index leaf under `spanning_defer_vacuum=on`, the leaf vacuum does the
same thing the `nindexes==0` path does today: **prune (mark LP_DEAD) + enqueue,
and nothing else** — no local-index vacuum, no spanning retirement, no reap. The
drain becomes the single coalesced cleanup for the whole dirty set:

For each dirty partition (under the SUEL it already holds), snapshot current
LP_DEAD set `D`, then:
1. **Vacuum each LOCAL index** against `D` (`ambulkdelete` / `_bt_delitems_vacuum`,
   WAL-logged) — retire local entries for exactly `D`.
2. **[coalesced]** retire SPANNING entries for `D` across all dirty partitions in
   the existing one-scan-per-spanning-index pass (unchanged from the #40 design).
3. **Reap** `D` (`spanning_drain_reap_partition`).

Because both local and spanning entries for exactly `D` are retired immediately
before `D` is reaped — the standard two-pass invariant — the reap is sound. `D_new`
(slots that turn LP_DEAD during the drain, after the per-partition snapshot) are
not in `D`, not reaped, and wait for the next drain. No durable TID list needed:
the drainq stays coarse `(idxid, partseq)`, and `D` is re-derived from live heap
line-pointer state at drain time exactly as today.

- **Win preserved:** the expensive SHARED spanning-index scan stays coalesced to
  one pass per sweep (O(N), the #38 headline). The per-partition LOCAL scans are
  intrinsic O(leaf) work — they happened in the leaf vacuum before and simply move
  to the drain; total local work per sweep is unchanged, only relocated/batched.
- **Tradeoff (must document):** local-index cleanup for spanning leaves is now
  *also* deferred to the drain. Until the drain runs, the leaf's LOCAL indexes
  retain dead entries and the heap's visibility-map bits stay clear, so
  index-only-scan efficiency lags by up to one drain interval. This is bounded by
  the same drain cadence (autovacuum sweep #37 + age trigger) that bounds heap
  bloat, and is tunable the same way. Mental model becomes clean: **for a spanning
  leaf, the drain IS the vacuum.**
- **Verdict:** recommended. Sound, keeps the coarse durable queue, preserves the
  coalescing win, smallest catalog/format surface.

### Option 3 — drain-time per-slot local-index liveness re-proof
Before reaping each LP_DEAD slot, have the drain prove no local index references
it. There is no cheap primitive for "does any local index point at this TID"
(would require probing every local index per slot, or an index-by-index scan to
build a live-TID set — which is just Option 2's local vacuum, done worse).
- **Verdict:** rejected; degenerates into a more expensive Option 2.

## Recommended design (Option 2) — incision points
Smallest stock-diff, logic in owned files, consistent with the spanning-enforcement
seam (one choke point, owned helpers do the work):

1. **`lazy_vacuum` decision (`vacuumlazy.c`).** Hoist the defer decision above the
   `nindexes == 0` split. When `spanning_defer_vacuum && has_spanning_index &&
   !VacuumFailsafeActive`: route to `progresql_enqueue_spanning_drain(vacrel)` and
   return, for BOTH `nindexes == 0` and `nindexes > 0`. Drop the eager
   `progresql_vacuum_spanning_indexes` calls in the bypass / all-indexes branches
   from the defer path (they stay for the eager mode). The leaf does no local
   `lazy_vacuum_all_indexes` and no reap in defer mode — `has_spanning_index`
   already forces the two-pass + off the `MARK_UNUSED_NOW` fast path, so the heap
   stays correctly LP_DEAD (reuse for `nindexes>0` is the same mechanism).
   - Audit: `consider_bypass_optimization`, `do_index_cleanup`,
     `num_index_scans` accounting, and `vacrel` stats when the leaf does zero index
     work. The `nindexes==0` defer path is the template — mirror its accounting.
2. **Drain local-index pass (`progresql_drain_spanning_index`).** After building
   each partition's LP_DEAD `entries[i].dead` store and BEFORE the reap loop
   (between step 5 spanning-scan and step 6 reap), add a per-partition local-index
   vacuum: open the partition's `RelationGetIndexList`, skip spanning indexes
   (already handled by the coalesced scan), and for each LOCAL index run
   `index_bulk_delete` with a callback that returns true iff the TID is in
   `entries[i].dead`. Keep WAL order: all index delitems (local + spanning) precede
   every heap reap in the WAL stream — they already do, since the reap loop is last.
   - Call `vacuum_delay_point()` in the new loop (parent doc PERF item 5).
   - `index_bulk_delete` needs `IndexVacuumInfo.heaprel = partRel` (a real leaf,
     not the storage-less root) — already available as `entries[i].partRel`.
3. **No catalog change.** `pg_spanning_drainq` stays `(sdq_idxid, sdq_partseq, …)`.
   No catversion bump. This is the payoff of Option 2 over Option 1.

## Correctness argument (interleaving, local-index leaf)
Spanning index `I` on root `R`; leaf `A` (partseq 3) has a LOCAL index `Lx` on a
non-key column. Row k1 at `A` TID (10,5): `I` holds `{k1,3,(10,5)}`, `Lx` holds
`{val,(10,5)}`.

- t0: DELETE k1 → tuple dead.
- t1: autovacuum vacuums `A` (defer on). Prune marks (10,5) LP_DEAD; enqueue
  `(I, partseq 3)`. **No** local vacuum, **no** spanning retire, **no** reap. `Lx`
  STILL holds `{val,(10,5)}`; `I` STILL holds `{k1,3,(10,5)}`; slot LP_DEAD.
- t2: INSERT k2 into `A`. Allocator skips LP_DEAD (10,5), lands at (10,9). New
  entries `{k2,3,(10,9)}` in `I`, `{val2,(10,9)}` in `Lx`. No collision forms.
- t2.5: on-access prune marks a DIFFERENT slot (10,7) LP_DEAD (this is `D_new`);
  its `Lx` entry is still live.
- t3: drain runs. Snapshot `A` LP_DEAD `D = {(10,5),(10,7)}`. Step 1: vacuum `Lx`
  against `D` → removes `{val,(10,5)}` AND `{val_for_(10,7),(10,7)}`. Step 2:
  coalesced spanning scan removes `I` entries for `D` → removes `{k1,3,(10,5)}` and
  the spanning entry for (10,7). Step 3: reap `D`. Both indexes had `D` retired
  immediately before the reap ⇒ no orphan. (Note `D_new=(10,7)` is handled
  correctly here precisely BECAUSE Option 2 vacuums `Lx` against the FULL current
  `D` at drain time — there is no "local entry still live" slot left un-retired.)
- t3': a slot that turns LP_DEAD AFTER the t3 snapshot is not in `D`, not reaped;
  next drain handles it. Live entries `{k2,3,(10,9)}`/`{val2,(10,9)}` untouched.

The cross-partition sibling-collision argument (partseq-first gate + per-partition
LP_DEAD re-derivation) is unchanged from the parent doc and the #40 fix.

## Crash safety
Identical shape to today: drainq rows are WAL-logged catalog tuples; the slot is
durably LP_DEAD; the drain does all index delitems (now local + spanning) then the
reap in one transaction, index-before-heap in the WAL stream. Crash before the
drain commits → rolls back, drainq rows remain, `D` re-derived next run. Crash
between enqueue and drain is the normal steady state and is safe (slot LP_DEAD +
durable queue row). The only new WAL is the local `_bt_delitems_vacuum`, which is
stock and replays on standbys unchanged.

## Concurrency / locking
No new lock classes. The drain already holds SUEL on each dirty partition and on
each spanning index. Local-index `ambulkdelete` runs under the partition's SUEL —
the same lock an ordinary VACUUM of that leaf holds — so a concurrent ordinary
VACUUM of the same partition is serialized (it can't run while the drain holds
SUEL), and DML (RowExclusiveLock) is never blocked. `_bt_start_vacuum`'s
one-active-vacuum-per-index rule is honored: the drain is the sole vacuumer of both
the spanning index and (while it holds the partition SUEL) the partition's local
indexes.

## Test plan (TDD — write before/with implementation)
- **regress:** spanning leaf WITH a local index; delete + vacuum (defer on) +
  reinsert (slot held LP_DEAD, no reuse) + `pg_drain_spanning_index` + assert (a)
  no cross-partition dup on the spanning keys, (b) the LOCAL index is consistent
  (`bt_index_check` + `bt_index_parent_check`), (c) the heap slot reaped only after
  the drain.
- **isolation:** drain vs concurrent on-access prune (force `D_new`): s1 deletes +
  vacuums (enqueue), s2 inserts/selects to drive HOT-prune of other slots, s3
  drains; assert local index never orphaned (amcheck) and dup invariant holds.
- **soak (extend `spanning_soak.sh`):** add a `--local-index` knob that adds a
  per-leaf local index on `payload`; run `--defer-vacuum --autovacuum on` and have
  `verify()` amcheck the LOCAL indexes too, not only the spanning ones. This is the
  scale oracle #39 names ("--defer-vacuum + local indexes + autovacuum on").
- **TAP recovery:** crash between enqueue and drain, and mid-drain, with a local
  index present; assert local + spanning consistent after recovery.
- **perf:** re-run the N=4..64 buffer-hit curve with a local index present; confirm
  the SPANNING scan still collapses N→1 (the local scans are the unchanged O(leaf)
  baseline).

## Risks / open questions
- **VM-bit / index-only-scan lag** from deferring local cleanup (the Option 2
  tradeoff). Bounded by drain cadence; document in `spanning_defer_vacuum`'s GUC
  help. Consider whether very-high-IOS workloads want a per-table opt-out.
- **Drain duration grows** — it now does local-index `ambulkdelete` for every
  dirty partition in one transaction, holding SUEL on each. Must `vacuum_delay_point`
  and stay interruptible. Quantify on the soak; if a single drain gets too long,
  consider per-partition drain transactions (the launcher sweep already enumerates
  indexes; a partition-scoped variant is possible but adds coordination).
- **Bypass-optimization parity:** confirm dropping the leaf's local-index bypass
  path under defer mode doesn't regress the "near-zero LP_DEAD" steady state
  (defer already skips all leaf index work, which is strictly less than bypass).
- **`do_index_cleanup` / amvacuumcleanup for local indexes:** the eager leaf path
  calls `lazy_cleanup_all_indexes`; the drain's `index_bulk_delete`-only pass skips
  amvacuumcleanup. Decide whether deferred local indexes need a periodic
  `btvacuumcleanup` (metapage bookkeeping / page recycling) — likely yes,
  mirroring how the spanning path finalizes (parent doc PERF item 8). Audit btree
  page-recycle (cf. #35) for the deferred local path.

## Sizing
XL, corruption-sensitive (mirrors the parent doc's XL sizing). Increments, each
`make check` green:
1. Drain-side local-index pass (drain still only triggered for `nindexes==0`
   leaves) — pure addition, no behavior change yet; test with a synthetic.
2. Extend the soak harness with `--local-index` + local amcheck.
3. Flip the `lazy_vacuum` decision to defer `nindexes>0` spanning leaves; make
   regress/isolation/soak green.
4. (Feeds #39) flip `spanning_defer_vacuum` default on; postgresql.conf.sample +
   docs; adjust eager-default-assuming tests.
