# P0-2 — Durability across dump/restore and pg_upgrade (+ P2-2 introspection)

**Scope:** PLANNING ONLY. No `src/` changes. Audits the dump/restore and
`pg_upgrade` durability of spanning indexes, plus the catalog-introspection
polish item P2-2 (`psql \d` / `pg_dump` verbosity).

**Fork context:** ProgreSQL = PostgreSQL `REL_18_STABLE` + "spanning indexes":
a single physical B-tree rooted on a partitioned/inheritance ROOT, keyed on
`(user_cols…, tableoid)`. `pg_index.indnuniqatts > 0` marks a spanning index
and records how many *leading* key columns form the user-visible unique key;
the trailing key column is the row's `tableoid`. The uniqueness check compares
only the first `indnuniqatts` columns so that equal user keys in *different*
partitions collide.

**Verified terminology corrections to the brief (read these first):**
- The in-memory IndexInfo field is **`ii_NumUniqKeyAtts`** (not
  `ii_NumUniqueAtts`). [VERIFIED src/backend/commands/indexcmds.c:1194
  `indexInfo->ii_NumUniqKeyAtts = N;`]
- It is persisted to `pg_index.indnuniqatts`. [VERIFIED
  src/backend/catalog/index.c:653 `values[Anum_pg_index_indnuniqatts - 1] =
  Int16GetDatum(indexInfo->ii_NumUniqKeyAtts);`]
- The trailing key column is `TableOidAttributeNumber`, whose value is **`-6`**,
  not `-7`. [VERIFIED src/include/access/sysattr.h:26
  `#define TableOidAttributeNumber (-6)`; used at indexcmds.c:1198
  `indexInfo->ii_IndexAttrNumbers[N] = TableOidAttributeNumber;`] (`-7` is
  `FirstLowInvalidHeapAttributeNumber`, sysattr.h:27 — the brief/README appear
  to have transposed them. Confirm with the README owner; this plan uses -6.)
- **Catalog field arithmetic for a spanning index** (the crux for ruleutils,
  §2b): with N user key columns, indexcmds.c sets `ii_NumIndexKeyAttrs = N+1`
  and `ii_NumIndexAttrs = N+1` (tableoid appended), and `ii_NumUniqKeyAtts = N`.
  [VERIFIED indexcmds.c:1199-1200,1194] index.c then writes
  `indnatts = N+1`, `indnkeyatts = N+1`, `indnuniqatts = N`. [VERIFIED
  index.c:651-653] So in the **stored pg_index row, `indnkeyatts` is NOT reduced
  — it is N+1 and includes the tableoid column.** (The genam.c:243 comment that
  "indnkeyatts has been reduced to indnuniqatts" refers to a *runtime* scankey
  adjustment for the uniqueness probe, NOT the persisted catalog row — see
  genam.c:243,249. This distinction is what makes the ruleutils bug real.)

---

## 1. Problem restatement

A spanning index encodes each partition's `tableoid` **as an actual datum** in
the leading-vs-trailing key structure of every index tuple. Two independent
mechanisms can break that encoding across a dump/restore or `pg_upgrade`:

1. **Logical dump/restore (`pg_dump | psql`)** — `pg_dump` has no knowledge of
   `indnuniqatts`; it reconstructs every index purely from the server-side
   `pg_get_indexdef()` string. If that string does not round-trip the spanning
   property, the restored index is a *plain* index and the cross-partition
   uniqueness domain is silently lost (a downgrade, not an error).

2. **Binary upgrade (`pg_upgrade`)** — spanning entries key on `tableoid`. If a
   partition's `pg_class` OID is not preserved across the upgrade, every stored
   `tableoid` datum becomes a dangling reference and the uniqueness domain is
   silently corrupted (false "unique" — different physical partitions now share
   an OID value, or a preserved value points at the wrong/absent relation).

Both paths currently lack spanning-specific handling. This plan determines what
each path actually does today, then recommends the cheapest correct fix.

---

## 2. Root cause — grounded in file:line

### 2a. `pg_dump` is spanning-blind and trusts `pg_get_indexdef()`
- `grep indnuniqatts | nuniqatts | spanning | NumUniqueAtts` across
  `src/bin/pg_dump/` and `src/bin/pg_upgrade/` → **0 hits**. [VERIFIED — empty
  grep over both trees]
- `pg_dump` obtains every index's DDL via the server function
  `pg_get_indexdef(i.indexrelid)`. [VERIFIED src/bin/pg_dump/pg_dump.c:6850 —
  `appendPQExpBuffer(query, "pg_get_indexdef(i.indexrelid)…")`]
  The dumped string is later emitted verbatim (`%s;`) at pg_dump.c:18136.
  [VERIFIED — `pg_get_indexdef(i.indexrelid) AS indexdef` at pg_dump.c:7762;
  `i_indexdef` captured :7879; written :18136]
- Therefore the *entire* logical fidelity of a spanning index dump depends on
  `ruleutils.c`'s `pg_get_indexdef_worker`.

### 2b. `ruleutils.c` / `pg_get_indexdef_worker` has NO spanning awareness
- `grep indnuniqatts` over `src/backend/utils/adt/ruleutils.c` → **0 hits**.
  [VERIFIED — `grep -c indnuniqatts ruleutils.c` = 0]
- `grep -i spanning` over `ruleutils.c` → **0 hits**. [VERIFIED — count 0]
  The function is stock upstream; no spanning code exists in this file.
- `pg_get_indexdef_worker` is defined at src/backend/utils/adt/ruleutils.c:1270
  (prototype at :359). [VERIFIED — grep located both]
- Its column loop is the stock upstream:
  `for (keyno = 0; keyno < idxrec->indnatts; keyno++)` (note: `indnatts`, the
  full count incl. INCLUDE cols) over `idxrec->indkey.values[keyno]`, and for a
  non-zero attnum it emits `get_attname(indrelid, attnum, false)`. [VERIFIED
  ruleutils.c:1398 (`for (keyno = 0; keyno < idxrec->indnatts; keyno++)`),
  :1400 (`AttrNumber attnum = idxrec->indkey.values[keyno];`), and :1427
  (`attname = get_attname(indrelid, attnum, false)`)]
- The INCLUDE divider fires at `keyno == idxrec->indnkeyatts` (:1411). For a
  spanning index `indnkeyatts = N+1` (incl. tableoid), so the tableoid column is
  rendered as a **key** column, NOT relegated to INCLUDE. [VERIFIED :1411 +
  catalog arithmetic above]

**Consequence (the bug):** the stored `indkey` vector of a spanning index ends
with `TableOidAttributeNumber` (-6), and `indnatts`/`indnkeyatts` both count it.
The stock loop therefore iterates over it, and `get_attname(indrelid, -6, …)`
resolves the system attribute name to the literal string `tableoid`. So
`pg_get_indexdef` emits something like:

```sql
CREATE UNIQUE INDEX measurement_pkey ON ONLY measurement
  USING btree (city_id, tableoid);
```

Two distinct failure modes follow, and **which one occurs depends on whether
re-executing that DDL is even legal in stock parse/analysis**:

- If `CREATE INDEX … (city_id, tableoid)` is *accepted* by this fork's index
  DDL path, the restored index has `indnuniqatts = 0` (a plain composite index
  on `(city_id, tableoid)`) — i.e. it enforces uniqueness of the *pair*, which
  is always satisfied (tableoid differs per partition) → **cross-partition
  uniqueness silently lost**.
- If the fork's DDL path *rejects* an explicit `tableoid` key column (likely,
  since spanning is auto-derived, not user-spelled), restore **errors out** and
  the dump is non-restorable.
- Either way the dump is wrong. The opt-in is *not* a user-written
  `(…, tableoid)` clause — it is auto-derived in DefineIndex from the
  INHERITS+PARTITION-BY shape, recording `ii_NumUniqKeyAtts` and appending the
  hidden tableoid column. [VERIFIED src/backend/commands/indexcmds.c: opt-in
  (`progresql_bypass`) computed at lines 754-757 (`partitioned &&` INHERITS
  parent `&& !relispartition`); tableoid append + `ii_NumUniqKeyAtts = N` at
  lines 1189-1215.] So `pg_get_indexdef` output cannot reproduce the
  catalog state by replay regardless. **[ASSUMPTION: exact accept/reject
  behavior of explicit `tableoid` in CREATE INDEX — must be confirmed by test;
  see Test Plan T0.]**

### 2c. Binary-upgrade hooks in `index.c` are stock (no spanning handling)
- `src/backend/catalog/index.c` includes `catalog/binary_upgrade.h` at line 36
  and declares/uses the stock `binary_upgrade_next_index_pg_class_oid` (:86) /
  `binary_upgrade_next_index_pg_class_relfilenumber` (:87) hooks at lines
  953-968. [VERIFIED] No spanning-specific catalog field is carried.
- `pg_dump --binary-upgrade` preserves the `pg_class` OID of every relation via
  `binary_upgrade_set_pg_class_oids(...)` (definition pg_dump.c:5698; call sites
  12823/16962/17064/18132/18400/18904). [VERIFIED]

### 2d. `indnuniqatts` itself IS written/read by the catalog (so the property
is durable *in-cluster* — the gap is purely transport)
- Written: src/backend/catalog/index.c:653
  `values[Anum_pg_index_indnuniqatts - 1] =
   Int16GetDatum(indexInfo->ii_NumUniqKeyAtts);`. [VERIFIED]
- Field defined: src/include/catalog/pg_index.h:36
  (`int16 indnuniqatts BKI_DEFAULT(0)`); Anum macro = 5
  (pg_index_d.h:33). [VERIFIED]
- Consulted by nbtree (`nbtinsert.c` lines 429,438,440,441,586,666,672),
  genam.c:249, execIndexing.c:1214, plancat.c:294, tablecmds.c:1287/2038/2154,
  vacuumlazy.c:2489, and the `rel.h` macro at :545-546. [VERIFIED — full grep]

---

## 3. Key durability fact: OID preservation by upgrade path

| Path | Partition `pg_class` OID preserved? | Stored `tableoid` datums valid after? |
|------|-------------------------------------|----------------------------------------|
| `pg_upgrade` (binary) | **Yes** — stock behavior; every relation's pg_class oid + relfilenode is force-set via `binary_upgrade_set_pg_class_oids()` (call sites pg_dump.c:12823 type, 16962/17064 table, 18132 index, 18400/18904) which emits `binary_upgrade_set_next_index_pg_class_oid(...)` (pg_dump.c:5759-5771) consumed by the stock hooks index.c:953-968. [VERIFIED] | Yes, *iff* the index relfilenode is also carried over OR rebuilt; see §4. |
| `pg_dump \| psql` (logical) | **No** — new OIDs assigned on restore. | N/A — index is recreated by replaying DDL, so it re-reads *new* live tableoids. Valid *iff* DDL recreates a spanning index. |

**Critical corollary:** because `tableoid` is stored as a *datum* inside each
spanning index tuple, the index's on-disk contents are only meaningful relative
to the current partition OIDs.
- Logical restore assigns fresh OIDs ⇒ any carried-over index *bits* would be
  garbage, but logical restore never carries index bits (it re-runs CREATE
  INDEX) ⇒ a freshly built spanning index is automatically correct **provided
  the DDL produces a spanning index.**
- Binary upgrade preserves OIDs ⇒ carried-over spanning index bits remain
  valid, *and* `indnuniqatts` rides along in the carried `pg_index` row (it is a
  normal catalog column, dumped as part of the relation's catalog state under
  binary upgrade). So binary upgrade is, in principle, already correct for the
  index contents — the remaining question is whether the `indnuniqatts` column
  value survives, which it does because the row is reconstructed via the same
  `pg_get_indexdef`-driven CREATE INDEX statement under binary upgrade.
  **[ASSUMPTION: under --binary-upgrade pg_dump still emits the index via
  `pg_get_indexdef` (not a raw catalog copy), so the §2b DDL defect ALSO breaks
  binary upgrade unless ruleutils is fixed. Confirm in Test Plan T1.]**

This last point is the linchpin: **fixing `pg_get_indexdef` is necessary for
BOTH paths**, because both paths ultimately re-execute a CREATE INDEX string.
OID preservation alone does not save binary upgrade if the regenerated DDL
drops the spanning property.

---

## 4. Design options & tradeoffs

### Option A — Rebuild from DDL, and make the DDL round-trippable (RECOMMENDED)
Make `pg_get_indexdef` emit a statement that recreates a *spanning* index, and
let both dump paths simply replay it. Two sub-choices for the emitted syntax:

- **A1 (preferred):** emit a statement whose plain re-parse re-triggers the
  existing auto-detection in `DefineIndex` — i.e. emit the *user-visible* key
  only (`USING btree (city_id)`), **suppressing the trailing tableoid column**,
  on a root that is `PARTITION BY …` / `INHERITS`. Re-running that statement
  goes through the same DefineIndex spanning-detection path that created it
  (indexcmds.c:754-757 opt-in; :1189-1215 tableoid append), reproducing
  `indnuniqatts` naturally. This depends on
  the opt-in mechanism (currently INHERITS+PARTITION BY) and **must coordinate
  with the P1-1 syntax plan** — when an explicit keyword exists, ruleutils must
  emit that keyword instead. *Do not design the syntax here; this plan only
  notes the dependency.*
  - Pros: zero new transport state; works identically for logical + binary;
    self-healing of `tableoid` datums; no reliance on OID preservation for
    correctness of the index *contents* (only heap data must arrive).
  - Cons: ruleutils must learn to (a) detect `indnuniqatts > 0` and (b) clip
    the trailing key column from the emitted key list; output ordering vs
    pg_upgrade's "ONLY"/ATTACH dance must be validated.

- **A2 (fallback):** emit explicit `(city_id, tableoid)` **plus** a spanning
  marker the re-parser understands. More invasive (needs grammar surface) and
  redundant with P1-1; not recommended.

### Option B — Preserve-and-trust OIDs (binary upgrade only)
Add spanning-aware binary-upgrade hooks so the carried index's `pg_index` row
(including `indnuniqatts`) and relfilenode are transplanted verbatim, never
rebuilt.
- Pros: avoids a full index rebuild on large tables during upgrade (faster).
- Cons: does nothing for the logical path; requires new binary_upgrade plumbing
  (`index.c`); fragile — any future change to spanning on-disk layout breaks
  cross-version upgrade; still needs §2b fixed for the non-binary dump anyway.
  Higher risk for marginal speed benefit.

### Option C — Post-restore REBUILD hook
Dump the index as a plain catalog stub, then emit a trailing
`REINDEX`/rebuild that reconstructs spanning state from live rows.
- Pros: conceptually simple; guaranteed fresh tableoids.
- Cons: needs a way to record "this was spanning" through the stub (same
  ruleutils problem), and REINDEX must itself preserve `indnuniqatts` (already
  handled per commit 355dfaa "fix BUG B (REINDEX)"). Largely subsumed by A1.

---

## 5. Recommendation

**Adopt Option A1.** Rationale:

1. A spanning index has **no root storage of its own beyond the B-tree**, and
   its only OID-sensitive content is the stored `tableoid` datums. Rebuilding
   from live partition rows regenerates those datums correctly by construction,
   so rebuild is the *safest* default and is already what the logical path does.
2. A single fix in `ruleutils.c` repairs **both** transport paths, because both
   funnel through `pg_get_indexdef` → CREATE INDEX replay. (§2a, §3)
3. It introduces **no new on-disk or transport state** and rides the existing,
   well-tested DefineIndex spanning-detection path (indexcmds.c:754-757,
   :1189-1215), minimizing surface area and cross-version fragility.
4. It cleanly defers the *syntax* question to P1-1 — ruleutils emits whatever
   the opt-in surface is, and only needs the structural change of clipping the
   trailing tableoid column and selecting the spanning rendering when
   `indnuniqatts > 0`.

Accept the rebuild cost on `pg_upgrade` (Option B's speed win) as a *later,
optional* optimization once A1 is correct and tested. Correctness first.

---

## 6. Step-by-step outline (implementation, for a later PR — NOT done here)

1. **ruleutils.c / `pg_get_indexdef_worker`** (src/backend/utils/adt/
   ruleutils.c:1455):
   - Read `idxrec->indnuniqatts` (or load via the index's `pg_index` tuple
     already fetched as `ht_idx`).
   - When `indnuniqatts > 0`: render only the leading user key columns
     (clip the trailing `tableoid` system column from the emitted key list),
     and emit the spanning opt-in surface (per P1-1; until then, rely on the
     INHERITS+PARTITION-BY shape being reproduced by the surrounding table DDL).
   - Guard `attrsOnly` / `keysOnly` / constraint-def callers
     (`pg_get_constraintdef`) so PK/UNIQUE constraint rendering is also clipped.
2. **Constraint path:** verify `pg_get_constraintdef_worker` (same file) routes
   through the same column rendering, so `ADD CONSTRAINT … PRIMARY KEY (city_id)`
   does not gain a `tableoid` column. [ASSUMPTION — confirm shared code path.]
3. **pg_dump:** no code change expected if ruleutils round-trips; but add a
   defensive check / comment noting the dependency. Confirm `--binary-upgrade`
   index emission still flows through `pg_get_indexdef` (pg_dump.c:6850 region)
   so the same fix covers it.
4. **psql describe (P2-2):** in `src/bin/psql/describe.c`, when describing an
   index whose `pg_index.indnuniqatts > 0`, annotate the `\d`/`\d+` output
   (e.g. "spanning, N unique key column(s)") and surface `indnuniqatts`. This
   is purely additive output. [ASSUMPTION — describe.c is the right file;
   confirm column-fetch query needs the new field added behind a server-version
   guard.]
5. **Docs:** update PRODUCTION_READINESS.md P0-2/P2-2 status when landed.

---

## 7. Test plan

### T0 — Characterize current DDL replay (pre-fix, establishes the bug)
- New regress test (or TAP) that: creates the README `measurement` spanning PK,
  captures `SELECT pg_get_indexdef('measurement_pkey'::regclass)`, and asserts
  the string does **not** contain `tableoid` and **does** reproduce a spanning
  index when replayed. Initially expected to FAIL — pins the defect.
- Assert `SELECT indnuniqatts FROM pg_index WHERE indexrelid =
  'measurement_pkey'::regclass` is `> 0` originally, and `= 0` after a naive
  dump/restore (demonstrating the silent downgrade).

### T1 — Logical round-trip (pg_dump | psql)
Location: new `src/bin/pg_dump/t/` TAP test. Existing files are
001/002/003/004/005 + 010_dump_connstr.pl [VERIFIED ls], so use a free number
such as `006_spanning_roundtrip.pl` (or 011). Alternatively extend
`src/test/regress` with a dump/restore harness if TAP is heavier.
Steps:
1. Build a 2+ partition spanning PK; insert rows with duplicate user keys
   across partitions expecting rejection, and distinct keys expecting success.
2. `pg_dump` (plain) → restore into a fresh DB via `psql`.
3. Re-assert: `indnuniqatts > 0` on the restored index; a cross-partition
   duplicate INSERT is **rejected** post-restore (the real durability check).
4. Negative: confirm a same-partition duplicate is rejected too (sanity).

### T2 — Binary upgrade (pg_upgrade)
Location: `src/bin/pg_upgrade/t/` (mirror `002_pg_upgrade.pl`). Existing files
are 001-006 [VERIFIED ls: 001_basic, 002_pg_upgrade, 003_logical_slots,
004_subscription, 005_char_signedness, 006_transfer_modes], so use
`007_spanning.pl`:
1. Old cluster: create spanning PK across partitions; insert known rows.
2. Record each partition's `oid` (`SELECT oid, relname FROM pg_class …`) and
   `SELECT tableoid, * FROM measurement`.
3. Run `pg_upgrade`.
4. New cluster: assert partition OIDs are **identical** (OID preservation);
   assert `indnuniqatts > 0`; assert stored rows' `tableoid` still resolve to
   the same partitions; assert a cross-partition duplicate INSERT is rejected.
5. Stretch: an `amcheck`/`bt_index_check` (or REINDEX + compare) on the
   spanning index to prove no dangling/garbled key datums.

### T3 — Introspection (P2-2)
- regress test on `\d measurement` / `\d measurement_pkey` output showing the
  spanning annotation; golden-file compare in `src/test/regress`.

All new tests must be wired into the relevant `Makefile`/`meson.build` and the
full suite (`make check-world` / `meson test`) run green before any release,
per project policy.

---

## 8. Risks / unknowns

- **[ASSUMPTION → must verify in T0]** Whether stock CREATE INDEX accepts an
  explicit `tableoid` key column at all; this decides "silent downgrade" vs
  "hard restore failure" today.
- **[ASSUMPTION]** Under `--binary-upgrade`, pg_dump emits the spanning index
  via `pg_get_indexdef` (not a verbatim catalog transplant). If true (expected),
  the ruleutils fix is *sufficient* for both paths; if pg_upgrade ever moved to
  raw catalog copy, Option B plumbing would be needed. Confirm in T1/T2.
- Constraint vs index rendering may diverge (`pg_get_constraintdef`); the clip
  logic must cover both, or PK restore re-adds `tableoid`.
- Partitioned-index ATTACH ordering during restore (`CREATE INDEX … ON ONLY`
  + per-partition `ATTACH PARTITION`/index attach) interacts with the spanning
  root index; must ensure the spanning root index is created with
  `indnuniqatts` intact and children attach correctly. Coordinate with P1-2
  (ATTACH/DETACH) work already partly landed (commit cc7d3..355dfaa).
- Cross-version upgrade fragility if the spanning on-disk key layout changes —
  another reason to prefer rebuild (A1) over preserve-and-trust (B).
- Dependency on **P1-1** for the eventual explicit-keyword surface; until then
  ruleutils leans on the INHERITS+PARTITION-BY shape being reproduced by the
  surrounding table DDL. Do NOT design that syntax in this item.
- **Runtime-vs-catalog `indnkeyatts` subtlety:** genam.c:243 says indnkeyatts
  "has been reduced to indnuniqatts" — but that is a *runtime* relcache/scankey
  adjustment, while the persisted pg_index row keeps `indnkeyatts = N+1`
  [VERIFIED index.c:651-652 + indexcmds.c:1199-1200]. Whoever fixes ruleutils
  must read the *catalog* `indnuniqatts` to know how many leading columns to
  emit and clip from `indnatts`, NOT assume indnkeyatts is already reduced. A
  fix that keys off indnkeyatts will misbehave depending on which value the
  syscache hands back. Pin this with T0.
- **`-6` vs `-7` discrepancy** between the brief/README and the actual code
  (TableOidAttributeNumber = -6). Any test that hard-codes the attno, and any
  doc that cites it, should be reconciled to -6. Low risk but a correctness
  trap for test authors.

---

## 9. Effort estimate

| Work item | Estimate |
|-----------|----------|
| ruleutils `pg_get_indexdef`/`pg_get_constraintdef` spanning rendering + clip | 1.0–1.5 days |
| T0 characterization regress test | 0.25 day |
| T1 pg_dump round-trip TAP | 0.5 day |
| T2 pg_upgrade TAP (+ amcheck) | 1.0 day |
| P2-2 psql `\d` introspection + T3 | 0.5 day |
| Integration, edge cases (constraints, ATTACH ordering), docs | 0.75 day |
| **Total** | **~4–4.5 days** (excludes any P1-1 syntax work it may later rebase onto) |

Option B (preserve-and-trust binary-upgrade plumbing), if pursued later as an
optimization: +1.5–2 days and a meaningfully larger risk surface.
