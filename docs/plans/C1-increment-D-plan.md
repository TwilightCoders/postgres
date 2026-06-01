# C1 Increment D — The Discriminator Flip (behavior-changing)

> **Status:** PLAN for review (no code yet). Increment D of the C1 roadmap.
> Branch `progresql-c1` @ C1-C (`4fa263ce08`). Base `REL_18_STABLE`.
>
> **The payoff increment.** The spanning index stops storing each row's
> partition `tableoid` in its trailing key column and stores the index-local
> **partseq** instead; the uniqueness probe and the vacuum cleanup resolve
> partseq → partition via the C1-C catalog/resolver. This is the change that
> makes P0-2 (dump/upgrade) and P1-3 (OID reuse) cease to exist, and fixes the
> P0-5 VACUUM TID-collision. It is **behavior-changing and on-disk-affecting**,
> so it gets its own careful increment and begins by forcing a REINDEX of any
> existing spanning index so the partseq map is complete before the flip.

---

## 0. What flips, in one paragraph

Today every spanning index entry is `(user_cols…, tableoid)` and the trailing
column holds the partition's `pg_class` OID. C1-C already records, for every
partition in a spanning index's domain, a stable index-local `partseq` in
`pg_index_partition`, and stamps each per-statement cache entry with it
(`se->partseq`), **but the write path still stores the tableoid**. D changes the
*value* written into the trailing column from tableoid to partseq at every WRITE
site, and changes every READ of that column (uniqueness probe, vacuum match,
detach cleanup) from "interpret as tableoid, open it" to "interpret as partseq,
resolve it via `SpanningResolvePartseqRelid`". After D the tableoid is no longer
stored anywhere in the index.

---

## 1. The key representation decision (NEEDS YOUR CALL — Q1)

The trailing key column is currently declared over the **`tableoid` system
attribute** (`TableOidAttributeNumber = -6`, type `oid`), at the build-shape site
`indexcmds.c:1198` (`ii_IndexAttrNumbers[N] = TableOidAttributeNumber`). At every
WRITE site the code calls `FormIndexDatum` (which extracts the real tableoid) and
then **overrides** that slot with an explicit value. So the column's *declared
identity* and its *stored value* are already decoupled.

Two ways to store partseq:

### Option D-min — keep the column declared as `tableoid`(oid), store partseq in it
- **Change:** only the overridden *value* (tableoid → partseq) and the *reads*.
  Index shape, build column, opclass (`oid_ops`), catalog, `\d`, amcheck all
  unchanged. partseq is a small positive int; stored as a Datum it is bit-identical
  whether widened by `Int32GetDatum` or `ObjectIdGetDatum`, and `oid_ops`
  (unsigned) sorts small positive ints correctly, so uniqueness/sort are correct.
- **Pro:** smallest possible diff; no catalog/opclass/build-shape change; lowest
  risk; fastest to land and verify.
- **Con:** the column *claims* to be `tableoid` (system attr, oid) but holds a
  partseq — a semantic fib. `\d` and amcheck would label it `tableoid`. For an
  upstream RFC this is a wart; for the research fork it is invisible to users
  (the column is hidden from the planner already).

### Option D-clean — make the trailing column a genuine `int4` partseq column
- **Change:** the build-shape site declares an `int4` key column (an expression
  or a dedicated attr) instead of the `tableoid` system attr; opclass becomes
  `int4_ops`; `FormIndexDatum` no longer needs to extract tableoid.
- **Pro:** honest representation; `\d`/amcheck show an int partseq; matches the
  spike's stated C1 ("replace the trailing tableoid key column with a partseq
  key column").
- **Con:** larger diff into the index-build shape and FormIndexDatum path;
  touches opclass selection; more surface to get wrong; the trailing column is
  synthetic (not a table column) so it must be an expression-index-style entry,
  which the current build doesn't use.

**Recommendation:** **D-min now**, with a `[follow-up]` note to consider D-clean
before any upstream submission. Rationale: D-min captures 100% of the *behavioral*
win (P0-2/P1-3/P0-5 all resolved by storing+resolving partseq) at a fraction of
the risk, and the column-honesty wart is cosmetic and reversible. D-clean is a
representation refactor that can ride later without changing semantics.

> Everything below assumes D-min unless noted. If you pick D-clean, §2's build
> sites grow to include the column declaration + opclass.

---

## 2. The chokepoints (verified file:line)

### (a) WRITE — store partseq instead of tableoid
1. **Executor hot path** — `execIndexing.c:1377-1378`
   `values[se->tableoidKeyPos] = ObjectIdGetDatum(partOid);` →
   `values[se->tableoidKeyPos] = Int32GetDatum(se->partseq);`
   `se->partseq` is already populated in C1-C. If `se->partseq < 0` (unmapped —
   should never happen post-backfill) raise an error rather than store garbage.
2. **Build / REINDEX** — `indexcmds.c:2950-2952`
   `values[k] = ObjectIdGetDatum(RelationGetRelid(partRel));` →
   capture `int32 partseq = SpanningGetOrAllocPartseq(idxRel, partOid);` (the
   call is already made once per partition in C1-C — hoist its return into a local
   used here) and `values[k] = Int32GetDatum(partseq);`
3. **ATTACH backfill** — `tablecmds.c:2176-2177`
   `values[k] = ObjectIdGetDatum(attachOid);` → use the partseq from the
   `SpanningGetOrAllocPartseq(idxRel, attachOid)` call already added in C1-C.

### (b) DEREF — resolve partseq → partition for the liveness probe
- **`_bt_check_unique`** (`nbtinsert.c`): three sites read the trailing column and
  `table_open` it — `child_relid = DatumGetObjectId(datum)` then
  `table_open(child_relid, AccessShareLock)` at lines **597, 620, 669** (datum read
  at **594** via `index_getattr(itup, itup_key->keysz + 1, itupdesc, &isnull)`).
  - **Change at each:** `int32 partseq = DatumGetInt32(datum);` then
    `child_relid = SpanningResolvePartseqRelid(rel, partseq);` then the existing
    `table_open(child_relid, …)`. If the resolver returns `InvalidOid` (entry for
    a detached/dropped partition that vacuum hasn't reaped), treat as "not a live
    conflict" — skip the probe, mirroring an LP_DEAD/stale entry. **This is a
    behavior subtlety to test:** today a stale tableoid entry would `table_open`
    a possibly-recycled OID; under D a stale partseq simply resolves to nothing.
  - The reduced-keysz clamp (`nbtinsert.c:434-437`,
    `IndexRelationGetNumberOfUniqueAttributes`) is **unchanged** — partseq vs
    tableoid doesn't affect how many leading columns are compared.

### (c) MATCH — partseq-filter the cleanup deletes
1. **VACUUM** — `progresql_vacuum_spanning_indexes` (`vacuumlazy.c` ~2462-2518):
   currently `vac_bulkdel_one_index(..., NULL, dead_items, ...)` with a TID-only
   reaper → the **P0-5 collision bug**. Under D the spanning bulkdelete must keep
   an entry unless **both** its TID is in the vacuumed partition's dead set **and**
   its trailing partseq equals the vacuumed partition's partseq. Shape: a
   spanning-specific `IndexBulkDeleteCallback` whose `state` carries the vacuumed
   partition's partseq; it reads the index tuple's trailing partseq and returns
   "remove" only on (partseq match AND TID-reaped). Resolve the partition's partseq
   once (via `SpanningLookupPartseqByRelid`) before the scan.
2. **DROP / DETACH cleanup** — `progresql_clean_spanning_indexes_for_partition`
   (`tablecmds.c`): the scan key at **2052-2057** matches the trailing column
   `= ObjectIdGetDatum(partOid)` with `F_OIDEQ` on attno `indnkeyatts`.
   - **Change:** look up the partition's partseq first
     (`SpanningLookupPartseqByRelid(idxRel, partOid)`); if `>= 0`, scan-match the
     trailing column `= Int32GetDatum(partseq)`. (Under D-min the column is still
     oid-typed, so `F_OIDEQ` + `ObjectIdGetDatum(partseq)` also works and is
     bit-identical; prefer that to avoid an opclass/strategy mismatch. Document the
     choice.) The `RemoveSpanningPartitionMapForPartition` call C1-C added stays —
     but note ordering: it must run **after** the entry scan (the scan needs the
     map row to find the partseq), or capture partseq before deleting the map row.
     **This is a real ordering bug to avoid** — see §4.

### 2.1 Signature-change ledger
| Chokepoint | file:line | Change | New param? |
|---|---|---|---|
| WRITE exec | execIndexing.c:1377 | `Int32GetDatum(se->partseq)` | none |
| WRITE build | indexcmds.c:2951 | hoist partseq local, `Int32GetDatum` | none |
| WRITE attach | tablecmds.c:2176 | partseq from existing alloc call | none |
| DEREF | nbtinsert.c:594/597/620/669 | `DatumGetInt32` + resolver | none |
| MATCH vacuum | vacuumlazy.c ~2504 | partseq-filtered callback | +partseq in cb state |
| MATCH detach | tablecmds.c:2052 | scan-match partseq, fix ordering | none |

---

## 3. Pre-flip migration (D begins here)

Existing spanning indexes built before D store tableoid in the trailing column;
their `pg_index_partition` map exists (C1-C populated it) but the *index entries*
hold tableoid. The moment D's reader interprets the trailing column as partseq,
those old entries are misread. So **D must rebuild every spanning index before its
readers go live**. Options:
- **Fresh cluster (our test path):** no pre-existing data; CREATE INDEX under D
  writes partseq from the start. Nothing to migrate.
- **Existing cluster:** documented hard requirement — `REINDEX` all spanning
  indexes after upgrading to D. REINDEX rebuilds from live rows writing partseq
  (the build WRITE site). The `pg_index_partition` map is preserved (get-or-alloc
  reuses existing partseq). This is the same "REINDEX-only migration" the spike's
  §7 specified.

Because this is a research fork with no production data, an upgrade note suffices;
no online-migration machinery.

---

## 4. Ordering hazard (must get right)

The detach/drop cleanup both **scans index entries by partseq** and **deletes the
map row**. If `RemoveSpanningPartitionMapForPartition` (deletes the map row) runs
*before* the entry scan resolves the partseq, the scan can't find the partseq and
leaves entries behind. C1-C placed `RemoveSpanningPartitionMapForPartition` at the
*top* of `progresql_clean_spanning_indexes_for_partition` — which was fine in C
(the scan still matched on tableoid, independent of the map). **In D it is a bug.**
Fix: resolve partseq (or capture it) *before* deleting the map row, or move the
map-row delete to the end of the function. Plan: compute partseq at the top into a
local, do the entry scan using the local, then delete the map row last.

---

## 5. Tests — flip the dormant red tests green

C1-C wrote two tests left out of `parallel_schedule`:
- **`progresql_oid_reuse`** — RED today (stale tableoid aliases a recycled OID);
  must go GREEN under D (partseq never reused → no false conflict).
- **`progresql_vacuum_collision`** — RED today (TID-only delete kills a sibling's
  live entry); must go GREEN under D (partseq-filtered delete).

D registers both in `parallel_schedule` and they must pass on arrival. Regenerate
their `expected/*.out` from `results/` after the flip (they're skeleton now).
Also re-run `progresql` + `progresql_ddl` + `progresql_partseq` — all must stay
green (the flip changes the stored value but not observable uniqueness behavior).
Target: **236 tests** (234 + the two newly-scheduled).

Add to `progresql_partseq` (or a new case): after the flip, the trailing stored
value resolves to the right partition under the uniqueness probe across a
cross-partition conflict in **every** partition (not just the first), to exercise
all three nbtinsert DEREF sites.

---

## 6. Step order (each compiles + green)

1. **Pre-flip migration guard:** make D's first action a REINDEX of spanning
   indexes (or document fresh-cluster assumption for tests). ~0.5 d
2. **DEREF first (read side), tolerant of both:** teach `_bt_check_unique` to
   resolve via partseq — but since entries still hold tableoid until step 3, do
   DEREF + WRITE in the *same* build/test cycle (they must flip together; a half
   state misreads). Practically: change WRITE (all 3 sites) and DEREF (3 sites)
   together, rebuild, fresh initdb, verify cross-partition uniqueness still works.
   ~2 d
3. **Detach cleanup:** flip the scan-match to partseq + fix the ordering hazard
   (§4). Verify DETACH/DROP still cleans entries (no dangling, no false conflict
   after reattach). ~1 d
4. **VACUUM match:** partseq-filtered bulkdelete callback (fixes P0-5). ~2 d
5. **Flip dormant tests green:** register both, regenerate expected, gate 236.
   ~1 d
6. **Retire tableoid references:** remove now-dead comments/asserts that mention
   storing tableoid; confirm no code still reads the column as an OID-to-open.
   ~0.5 d

**Est: ~7 dev-days.**

---

## 7. Risks / open items
- **[Q1] column representation** (D-min vs D-clean) — §1, needs your call.
- **Stale-entry semantics** (§2b): resolver returns InvalidOid for a
  detached/vacuumed partseq; DEREF must treat as "no live conflict." Test it.
- **Ordering hazard** (§4): map-row delete vs partseq scan in detach cleanup.
- **oid_ops vs int4_ops** under D-min: storing int via oid column relies on
  small-positive partseq + unsigned compare being order-correct. True for partseq
  (monotonic from 1), but document the assumption; D-clean removes it.
- **Resolver on the hot path:** `_bt_check_unique` calls the resolver per
  candidate conflict. It's a syscache hit (cheap) but only on the conflict path,
  not per-row; acceptable. The relcache-attached backstop cache the spike
  mentioned can be deferred unless profiling shows syscache pressure.
- **amcheck** still unaware of the trailing column's meaning (partseq); out of
  scope, note for a later increment.

---

## 8. Decision needed before coding
- **Q1:** D-min (store partseq into the existing oid-typed trailing column;
  smallest diff, cosmetic wart) **[recommended]** vs D-clean (genuine int4
  partseq column; honest, bigger diff). Pick before step 2.
