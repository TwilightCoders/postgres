# P0-1 — `INSERT ... ON CONFLICT` and `MERGE` conflict detection through the spanning index

Status: **PLAN ONLY.** No `src/` files were modified to produce this document.

Every claim below is tagged `[VERIFIED file:line]` (read directly in this tree
during the audit) or `[ASSUMPTION]` (reasoned from PostgreSQL internals, not
confirmed against this tree — re-verify before coding). Branch: `progresql-18`,
base `REL_18_STABLE`.

---

## 1. Problem restatement

A **spanning index** is a unique B-tree on the partitioned **ROOT** storing
`(user_cols…, tableoid)` and enforcing uniqueness on the leading `indnuniqatts`
columns only; the trailing `tableoid` (system attno -7) disambiguates which
partition owns each entry `[VERIFIED README.md:96-104]`. `pg_index.indnuniqatts > 0`
is the marker; `IndexRelationGetNumberOfUniqueAttributes()` returns `indnuniqatts`
for spanning indexes and `indnkeyatts` otherwise `[VERIFIED rel.h:544-547]`.

**Write-time enforcement works.** `ExecInsertSpanningIndexTuples`
`[VERIFIED execIndexing.c:1345]` forms the index datums with `FormIndexDatum`,
overrides the trailing column with the leaf's `tableoid`
`[VERIFIED execIndexing.c:1374-1378]`, and `index_insert`s into the root with
`UNIQUE_CHECK_YES` `[VERIFIED execIndexing.c:1380-1387]`. It is called on the INSERT
path after the speculative block `[VERIFIED nodeModifyTable.c:1251]` and in the
UPDATE epilogue `[VERIFIED nodeModifyTable.c:2393]`. nbtree's `_bt_check_unique`
reduces the duplicate-scan keysize to `indnuniqatts` for spanning indexes and, when
a candidate duplicate is found, opens the partition named by the trailing tableoid
column for the heap-liveness probe `[VERIFIED nbtinsert.c:429-441, 570-600, 665-685]`.

**The gap is conflict DETECTION for upserts/MERGE.** The ON CONFLICT pre-check and
speculative insertion resolve **arbiter indexes** and call `ExecCheckIndexConstraints`
against the **leaf** partition, whose index array never contains the root spanning
index. So when the conflicting row lives in a **sibling partition**:

- **DO NOTHING** fails to suppress the insert; the row reaches the write-time root
  enforcement and raises a hard `unique_violation` instead of silently no-op'ing.
- **DO UPDATE** never finds the conflict to update; it likewise hits the write-time
  violation.
- Detection failing turns every cross-partition upsert into an error rather than the
  requested upsert semantics. (The genuine cross-partition duplicate is still blocked
  by the write-time check, so this is "wrong error / wrong behavior", not silent
  corruption — but it violates the contract of ON CONFLICT.)

**MERGE** matches via the planner join, not an index probe, and `WHEN NOT MATCHED
→ INSERT` reaches `ExecInsert` with no spanning consultation
`[VERIFIED nodeModifyTable.c:2982 ExecMerge, 3108 ExecMergeMatched, 3653
ExecMergeNotMatched]`.

**Acceptance (from the tracker):** upsert + MERGE against a spanning key must
detect/resolve a conflict whose row is in a sibling partition, for both DO NOTHING
and DO UPDATE, including the speculative-abort path
`[VERIFIED PRODUCTION_READINESS.md:63-66]`.

---

## 2. Root-cause analysis (grounded in code)

### 2.1 Arbiter inference cannot select the spanning index
`infer_arbiter_indexes` `[VERIFIED plancat.c:727]` opens the result relation
(`relation = table_open(rte->relid, …)` where `rte` is the `resultRelation`)
`[VERIFIED plancat.c:761-763]`, builds a bitmap `inferAttrs` of the user's inference
attrs, then walks `RelationGetIndexList(relation)` `[VERIFIED plancat.c:814]`
matching each unique index. The match is **purely single-relation, exact over the
full key** — there is **no** partition-intersection / `find_all_inheritors` / root→child
OID-translation logic in this function (the loop simply does
`lappend_oid(results, idxForm->indexrelid)` on match and returns; on no match it
`ereport`s "there is no unique or exclusion constraint matching the ON CONFLICT
specification") `[VERIFIED plancat.c:814-980, terminal ereport at :973-978]`.

Why the spanning index is rejected: the matcher builds `indexedAttrs` over
`idxForm->indnkeyatts` (skipping `attno == 0` expression slots) and requires
`bms_equal(indexedAttrs, inferAttrs)` `[VERIFIED plancat.c "Non-expression
attributes must match"]`. For a spanning index `indnkeyatts` includes the trailing
`tableoid` column, so `indexedAttrs` is a **superset** of the user key — `bms_equal`
fails and the index is skipped. There is **no `indnuniqatts` branch** here. Net
effect: `ON CONFLICT (id)` against a spanning-indexed root raises *"there is no
unique or exclusion constraint matching the ON CONFLICT specification"*
`[VERIFIED plancat.c:973-978]` — the upsert cannot even bind.
`[ASSUMPTION — that the negative tableoid system attno survives into indexedAttrs;
the `attno != 0` guard admits it. Either way the `bms_equal` mismatch is the
operative failure.]`

Whatever `infer_arbiter_indexes` returns becomes `node->arbiterIndexes`, copied
verbatim to `ri_onConflictArbiterIndexes` `[VERIFIED nodeModifyTable.c:5087]`.

### 2.2 The conflict pre-check only iterates leaf indexes
`ExecCheckIndexConstraints` `[VERIFIED execIndexing.c:546]` loops over
`resultRelInfo->ri_NumIndices` / `ri_IndexRelationDescs` / `ri_IndexRelationInfo`,
skips non-unique/non-exclusion indexes, and (when arbiters are specified) skips any
index not in `arbiterIndexes` via `list_member_oid`, finally `elog`ing "unexpected
failure to find arbiter index" if no arbiter was examined
`[VERIFIED execIndexing.c:546-662]`. The root spanning index is never in the leaf's
`ri_IndexRelationDescs`, so it is unreachable here. There is **no spanning analogue**
of this function — the only spanning machinery in `execIndexing.c` is the
per-statement cache (`progresql_build_partition_cache_entry`),
`ExecInsertSpanningIndexTuples`, `spanning_unique_unchanged`, and
`ProgresqlReleasePartitionCache` `[VERIFIED execIndexing.c — those are the only
spanning symbols]`.

### 2.3 The speculative dance is leaf-scoped
The ON CONFLICT block reads `arbiterIndexes = ri_onConflictArbiterIndexes`
`[VERIFIED nodeModifyTable.c:1119]`, pre-checks with `ExecCheckIndexConstraints`
`[VERIFIED nodeModifyTable.c:1137-1139]`, and on no committed conflict acquires a
spec token, `table_tuple_insert_speculative` into the **leaf**, then
`ExecInsertIndexTuples(..., &specConflict, arbiterIndexes, …)` against **leaf**
indexes, `table_tuple_complete_speculative`, and re-loops to `vlock:` if
`specConflict` `[VERIFIED nodeModifyTable.c:1119-1226]`. nbtree supports
`UNIQUE_CHECK_PARTIAL`/CEOUC with the reduced spanning keysz **if a probe reaches the
root index** `[VERIFIED nbtinsert.c:87,98,397,616 + reduced keysz]`, but no executor
path issues that speculative probe against the root, and the actual spanning write
happens **outside** the spec window at `:1251` with plain `UNIQUE_CHECK_YES`
`[VERIFIED nodeModifyTable.c:1251; execIndexing.c:1385]`. A concurrent
sibling-partition inserter is therefore not detected through the spec/CEOUC
mechanism; the loser only finds out at the post-window `UNIQUE_CHECK_YES`, which
`ereport`s rather than retrying.

### 2.4 DO UPDATE locks the conflict TID against the leaf relation
`ExecOnConflictUpdate` `[VERIFIED nodeModifyTable.c:2764]` does
`table_tuple_lock(relation, conflictTid, …)` where `relation` is the leaf's
`ri_RelationDesc` `[VERIFIED nodeModifyTable.c:2800]`, then `ExecUpdate(context,
resultRelInfo, conflictTid, …)` `[VERIFIED nodeModifyTable.c:2949-2958]`. A
cross-partition conflict's tuple lives in a **sibling** partition, so a `conflictTid`
that pointed there could not be locked/updated through the leaf's `ResultRelInfo`.
There is **no synthetic/encoded TID scheme** — `ExecInsertSpanningIndexTuples` passes
the real leaf `tupleid` to `index_insert` `[VERIFIED execIndexing.c:1380-1387]`. So a
spanning probe that returned a conflict would yield a leaf-TID plus a `tableoid` key
value; the executor would need that tableoid to know which partition `ResultRelInfo`
to target. This cross-partition retarget is the deepest part of the problem.

### 2.5 MERGE never consults the spanning constraint
`ExecMerge` / `ExecMergeMatched` / `ExecMergeNotMatched`
`[VERIFIED nodeModifyTable.c:2982, 3108, 3653]` are driven by the source/target
join; no path probes the spanning index. `WHEN NOT MATCHED → INSERT` flows to
`ExecInsert` and therefore the write-time root enforcement at `:1251`, so a
cross-partition duplicate raises a raw `unique_violation` rather than being matched
or cleanly rejected `[ASSUMPTION — MERGE INSERT reuses the ExecInsert spanning call;
consistent with the shared insert path, confirm the action wiring]`.

---

## 3. Design options

### Option A — Full fix: spanning-aware arbiter + detection + cross-partition DO UPDATE
Teach `infer_arbiter_indexes` to offer the spanning index (match leading
`indnuniqatts` cols); add a spanning conflict pre-check; make `ExecOnConflictUpdate`
retarget the owning partition (lock/EvalPlanQual/SET/route there).
- Pros: meets the acceptance criteria fully — both same- and sibling-partition
  conflicts work for DO NOTHING and DO UPDATE.
- Cons: largest surface; cross-partition tuple-lock + EvalPlanQual is genuinely hard
  (retarget `ResultRelInfo`, project SET against a different partition's descriptor,
  possibly re-route the updated tuple to yet another partition).

### Option B — Detection now; DO NOTHING fully; cross-partition DO UPDATE = clean ERROR
Same arbiter + pre-check changes as A. For **cross-partition** DO UPDATE, raise a
documented `FEATURE_NOT_SUPPORTED` error instead of attempting the retarget;
same-partition DO UPDATE and all DO NOTHING work.
- Pros: closes the contract hole (no spurious unique_violation for DO NOTHING; no
  silent wrong behavior); far smaller/safer; the dangerous cross-partition lock/EPQ
  is deferred; the arbiter+detection plumbing is exactly what A needs later.
- Cons: cross-partition DO UPDATE is an explicit unsupported-error, not a feature —
  partial against the stated acceptance.

### Option C — Speculative-only (minimal)
Route the spanning write into the speculative window with `UNIQUE_CHECK_PARTIAL` so a
conflict surfaces as `specConflict` and the existing `vlock:` retry re-checks.
- Pros: smallest change; reuses nbtree's spanning-aware CEOUC.
- Cons: a spec conflict says *that* there is a conflict, not *which* committed tuple
  to update, so DO UPDATE still can't fire; and `ON CONFLICT (id)` still won't even
  bind (arbiter inference unchanged). Insufficient alone.

---

## 4. Recommended approach

**Option B now, structured so Option A is a follow-up, not a rewrite.** The P0
charter is data integrity / correctness; today a cross-partition upsert that should
no-op or upsert instead throws a hard error, and `ON CONFLICT (id)` won't even plan.
Option B fixes binding + DO NOTHING + same-partition DO UPDATE, and converts the one
genuinely hard case (cross-partition DO UPDATE) from "wrong-by-accident" to an
"explicit, documented FEATURE_NOT_SUPPORTED" — an honest P0 resolution. Track the
cross-partition DO UPDATE retarget and spanning-aware MERGE MATCHED as **P0-1b**.

---

## 5. Implementation outline (functions to touch, in order)

1. **`optimizer/util/plancat.c` — `infer_arbiter_indexes`** `[VERIFIED :727]`
   - Add a spanning branch: when a candidate root index has `indnuniqatts > 0`,
     build `indexedAttrs` over only the leading
     `IndexRelationGetNumberOfUniqueAttributes(idxRel)` `[VERIFIED rel.h:544]` attrs
     (exclude the trailing tableoid) before the `bms_equal(indexedAttrs, inferAttrs)`
     test; on match, `lappend_oid` its OID like any other arbiter.
   - Guard to the partitioned-root insert (resultRelation is the root).

2. **`executor/execIndexing.c` — new `ExecCheckSpanningIndexConstraints`**
   - Reuse the per-statement spanning cache via
     `progresql_build_partition_cache_entry` to obtain
     `(indexRel, indexInfo, parentRel, tableoidKeyPos)`.
   - Form the candidate's index datums with `FormIndexDatum` + tableoid override
     (mirroring `ExecInsertSpanningIndexTuples` `[VERIFIED :1374-1378]`), then run a
     `check_exclusion_or_unique_constraint`-style probe `[VERIFIED :708]` against the
     root index with the reduced keysz (nbtree already reduces it
     `[VERIFIED nbtinsert.c:429-441]`).
   - On conflict, return `false`, set `*conflictTid` to the conflicting leaf TID, and
     set an out-param `*conflictTableOid` (from the matched entry's tableoid key
     column) so the caller can distinguish same- vs sibling-partition.
   - Declare it (and any helper) in `executor/executor.h` next to
     `ExecCheckIndexConstraints`.

3. **`executor/nodeModifyTable.c` — ON CONFLICT block** `[VERIFIED :1109-1251]`
   - When the chosen arbiter is the spanning index, after/instead of the leaf
     `ExecCheckIndexConstraints` `[VERIFIED :1137]`, call
     `ExecCheckSpanningIndexConstraints`.
   - DO NOTHING + conflict (any partition) ⇒ no-op (preserve existing
     `ExecCheckTIDVisible` semantics for same-partition; for sibling-partition do the
     visibility recheck against the owning partition or accept a conservative no-op).
   - DO UPDATE + same-partition ⇒ existing `ExecOnConflictUpdate` `[VERIFIED :2764]`.
   - DO UPDATE + sibling-partition ⇒ `ereport(ERROR, errcode(FEATURE_NOT_SUPPORTED))`.

4. **Speculative path** `[VERIFIED :1189-1226]`
   - When the arbiter is the spanning index, make the spanning write participate in
     the speculative window: insert into the root with `UNIQUE_CHECK_PARTIAL`, feed
     `specConflict` from that probe, and ensure `table_tuple_complete_speculative`
     and the root entry are committed/rolled back together so a concurrent
     sibling-partition inserter triggers the `vlock:` retry. **Highest-care change.**

5. **MERGE** `[VERIFIED :3653 ExecMergeNotMatched]`
   - In the NOT-MATCHED INSERT action, run the spanning check before the write; raise
     a clear error on cross-partition conflict instead of the raw `unique_violation`.
   - Leave a TODO for spanning-aware `WHEN MATCHED` (P0-1b).

6. **P0-1b (NOT this item)** — cross-partition DO UPDATE retarget: use the decoded
   tableoid to find the sibling partition's `ResultRelInfo` (partition routing
   infra), `table_tuple_lock` + EvalPlanQual there, project SET, re-route; plus
   spanning-aware MERGE MATCHED.

---

## 6. Test plan

### 6.1 Regression SQL (`src/test/regress/sql/`)
Model on `insert_conflict.sql` and `merge.sql` `[VERIFIED regress/sql: both present]`
and the project's `progresql*` suites. Add `progresql_onconflict.sql` (+ expected),
against a 2+ partition root with a spanning unique on `(id)`:
- DO NOTHING, conflict in **same** partition ⇒ no-op, no error.
- DO NOTHING, conflict in **sibling** partition ⇒ no-op, no error (**headline fix**).
- DO NOTHING, no conflict ⇒ inserts.
- DO UPDATE, conflict same partition ⇒ updates the row.
- DO UPDATE, conflict sibling partition ⇒ clean `FEATURE_NOT_SUPPORTED` (assert exact
  errcode/message) — Option B.
- `ON CONFLICT (id)` binds to the spanning index (no "there is no unique or exclusion
  constraint matching the ON CONFLICT specification" planner error).
- MERGE `WHEN NOT MATCHED THEN INSERT` with a cross-partition existing key ⇒ clean
  error, not raw `unique_violation`.
- Register in `src/test/regress/parallel_schedule` (and serial schedule).

### 6.2 Isolation specs (`src/test/isolation/specs/`)
Model on `insert-conflict-do-nothing.spec`, `insert-conflict-do-update.spec`,
`insert-conflict-specconflict.spec`, `merge-insert-update.spec`
`[VERIFIED isolation/specs: all present]`. Add `progresql-spanning-specconflict.spec`
(**critical sibling-partition concurrency case**):
- s1 inserts key 42 into partition A (uncommitted, holds spec/xact lock).
- s2 `INSERT ... ON CONFLICT DO NOTHING` key 42 routed to partition B.
- Expect s2 to **wait** on the sibling's spec/xact lock; on s1 commit ⇒ s2 no-op; on
  s1 abort ⇒ s2 inserts. Exercises §2.3 / step 4.
- Second permutation: DO UPDATE same-partition to confirm standard lock-and-update.
- Register in `src/test/isolation/isolation_schedule`.

### 6.3 Regression guard
- Run upstream `insert-conflict-*` specs and `insert_conflict.sql` unchanged to
  confirm single-partition behavior is byte-identical.

---

## 7. Risks / unknowns

- **[ASSUMPTION] exact attr-match outcome for the tableoid system column** in
  `infer_arbiter_indexes` (§2.1). The *absence* of any spanning branch is VERIFIED;
  the precise `bms_equal` result deserves a quick experimental confirmation.
- **No synthetic-TID scheme exists** `[VERIFIED execIndexing.c:1380-1387 uses the
  real leaf tupleid]`. A spanning conflict yields a leaf TID + a tableoid key value;
  the pre-check must surface the tableoid so the caller can map to a partition.
- **Speculative window + root index** (§2.3 / step 4): routing the spanning insert
  into the spec window risks double-insert or orphaned root entries on retry; the
  root entry and `table_tuple_complete_speculative(!specConflict)` must roll back
  together. Highest-risk area.
- **Cross-partition lock semantics** deferred to P0-1b — but its absence must be a
  hard ERROR, never a silent wrong update.
- **MERGE WHEN MATCHED** spanning support intentionally out of scope; ensure we do
  not *appear* to support it.
- Interacts with P0-4 (concurrency/isolation untested) — the isolation spec is the
  main guard for both.

---

## 8. Effort estimate (rough)

- Step 1 (arbiter inference, leading-`indnuniqatts` match): 1–1.5 days.
- Step 2 (`ExecCheckSpanningIndexConstraints` + header): 1.5–2 days.
- Step 3 (ON CONFLICT wiring: DO NOTHING + same-part DO UPDATE + cross-part ERROR):
  1 day.
- Step 4 (speculative-window correctness + isolation behavior): 2–3 days (riskiest).
- Step 5 (MERGE NOT-MATCHED guard): 0.5 day.
- Tests (§6 regress + isolation): 1.5–2 days.
- **Total Option B: ~8–10 engineer-days.**
- **Option A follow-up (P0-1b: cross-partition DO UPDATE retarget + spanning MERGE
  MATCHED): +5–8 days**, separately scheduled.
