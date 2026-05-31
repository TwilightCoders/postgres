# P1-1 / P1-3 — Explicit Spanning-Index Syntax + `tableoid` Catalog Rationale

> **Scope:** PLANNING ONLY. No `src/` files were modified. This document plans
> (a) **P1-1** — replacing the implicit `INHERITS + PARTITION BY` opt-in with
> explicit, upstream-defensible syntax, and (b) **P1-3** — the catalog rationale
> for `tableoid`-as-key plus OID-reuse handling.
>
> **Confidence tags** (same convention as `PRODUCTION_READINESS.md`):
> - **[VERIFIED file:line]** — confirmed against the source tree (citations
>   carried forward from the 2026-05-30 audit in `PRODUCTION_READINESS.md`,
>   which read these lines in-tree).
> - **[ASSUMPTION]** — reasoned from PostgreSQL internals, not re-confirmed.
> - **[NEEDS-RESEARCH]** — must be checked against the tree before implementation.
>
> **Tooling note for this session:** tool output was intermittent. The **core
> P1-1 detection predicate WAS re-verified in-tree this session** via `grep -n`
> (citations tagged plain **[VERIFIED file:line]** below). A handful of
> downstream-blast-radius citations could only be inherited from
> `PRODUCTION_READINESS.md` / README and are tagged **[VERIFIED file:line —
> inherited; RE-CONFIRM]**. Items with no citation at all are **[NEEDS-RESEARCH]**.

---

## 1. Problem restatement

ProgreSQL adds **spanning indexes**: a real B-tree on a *partitioned root* that
stores `(user_cols…, tableoid)` for every live row in every partition and
enforces `UNIQUE` / `PRIMARY KEY` on only the leading user columns — giving
cross-partition uniqueness that vanilla PG forbids (vanilla requires the
partition key in every unique constraint). A spanning index is marked by
`pg_index.indnuniqatts > 0` (count of leading columns that form the unique
domain; the trailing key column is the row's `tableoid`, system attno **`-6`**,
`TableOidAttributeNumber`).

> **README CORRECTION [VERIFIED file:line]:** README and several comments say the
> trailing column is `tableoid` at system attno **`-7`**. The actual code appends
> `TableOidAttributeNumber`, which is **`-6`** (`src/include/access/sysattr.h:26`),
> and `indexcmds.c:1198` does exactly `ii_IndexAttrNumbers[N] =
> TableOidAttributeNumber`. The "-7" in the docs is a documentation bug; the
> RFC/SGML must say -6. (No attno -7 is defined in `sysattr.h`.)

**The P1-1 blocker:** the feature is opt-in via an *implicit* signal — a
partitioned root that **also `INHERITS` a base table** *and* declares a
`PRIMARY KEY` / `UNIQUE`. The detection predicate is effectively:

```
relkind == RELKIND_PARTITIONED_TABLE
  && (constraint is PRIMARY KEY || UNIQUE)
  && !EXCLUSION
  && !relispartition          (this is a top-level root, not itself a partition)
  && has_superclass / INHERITS list non-empty
```

This overloads two features — *legacy table inheritance* (semi-deprecated) and
*declarative partitioning* — as a hidden feature flag for a major new index
type. That is precisely the kind of implicit "magic" pgsql-hackers rejects on
sight: surprising, undocumentable as a deliberate API, and colliding with the
long-term direction of partitioning (which is steadily *removing* reliance on
the legacy inheritance machinery). **P1-1 = replace this implicit handshake
with explicit, declarative syntax.**

**P1-3 (related):** the trailing `tableoid` key column needs a written,
defensible rationale (why an OID is in the key at all), plus a concrete answer
to the OID-reuse hazard after many DROP/CREATE cycles within one partition tree.

---

## 2. Current-mechanism analysis (file:line)

### 2.1 Where the implicit opt-in is detected — `progresql_bypass` (THE predicate)

The opt-in is **not** a factored helper — it is a single local `bool
progresql_bypass` computed inline in `DefineIndex`, then consulted at six sites.
**[VERIFIED file:line]** (re-grepped in-tree this session):

- **`src/backend/commands/indexcmds.c:577`** — declaration `bool progresql_bypass;`
  (in `DefineIndex`). **[VERIFIED file:line]**
- **`src/backend/commands/indexcmds.c:754`** — the assignment:
  `progresql_bypass = partitioned && …` — this is **THE detection predicate**,
  matching the task's described conjunction
  `partitioned && (unique||primary) && !exclusion && !relispartition &&
  has_superclass`. **[VERIFIED file:line]** (exact RHS terms confirmed begin with
  `partitioned &&`; the full multi-line conjunction should be pasted verbatim by
  the implementer — line drift is minimal but re-read `:754`–~`:768`).
- **Consumption sites (all `[VERIFIED file:line]`):**
  - `indexcmds.c:981` — `if (partitioned && (stmt->unique || exclusion) &&
    !progresql_bypass)` — the spot that *skips* vanilla's "unique must include all
    partition columns" rejection when bypassing.
  - `indexcmds.c:1189` — `if (progresql_bypass)` — spanning-specific index build.
  - `indexcmds.c:1263` — `if (skip_build || concurrent || (partitioned &&
    !progresql_bypass))`.
  - `indexcmds.c:1269` — `if (partitioned && !progresql_bypass)`.
  - `indexcmds.c:1348` — `if (progresql_bypass && OidIsValid(indexRelationId))`.
  - `indexcmds.c:1351` — `if (partitioned && !progresql_bypass)` (and the comment
    at `:1361` describes spanning-index propagation).

- **`src/backend/parser/parse_utilcmd.c:1245–1246`** — the parser-side INHERITS
  allowance: comment *"For ProgreSQL spanning indexes, allow a partitioned table
  to inherit from a base table without forcing the partition key into the
  constraint."* This is the parse-analysis half that lets the INHERITS+PARTITION
  BY combination through to `DefineIndex`. **[VERIFIED file:line]** The
  diffstat shows `parse_utilcmd.c` is only +10/-? lines, so the parser change is
  small and surgical — read `:1240`–`:1290` for the exact guarded block.

**Implication for the rewrite:** the opt-in is concentrated in **two** edits —
the `:754` predicate in `indexcmds.c` and the `:1245` allowance in
`parse_utilcmd.c`. Removing the INHERITS dependency means: (1) at `:754`, replace
the `has_superclass`/INHERITS term with `stmt->isglobal` (the new explicit flag);
(2) at `parse_utilcmd.c:1245`, gate the partition-key-omission allowance on the
same explicit flag instead of "inherits a base table."

### 2.2 Catalog representation of a spanning index

- **`src/include/catalog/pg_index.h:36–42`** — new column **`indnuniqatts`**
  (`int16`, `BKI_DEFAULT(0)`), positioned right after `indnkeyatts`. Verbatim
  catalog comment: *"key columns that participate in uniqueness; 0 means same as
  indnkeyatts (all of them). ProgreSQL spanning indexes set this to indnkeyatts-1
  to exclude the appended child_relid column."* So a spanning index has
  `indnuniqatts == indnkeyatts - 1 > 0`, and the trailing (excluded) key column
  is the row's partition OID (the README's `tableoid`; the catalog comment calls
  it `child_relid`). **[VERIFIED file:line]** (read in-tree this session.)
  *Naming note:* the comment says `child_relid` while the README/AM layer says
  `tableoid` — same value (the partition's `pg_class` OID), two names. The RFC and
  code comments should unify on one term to avoid reviewer confusion.
- **`src/include/catalog/catversion.h`** — catversion bump for the new column.
  **[VERIFIED — README "What changed" table; RE-CONFIRM]**

### 2.3 Where `indnuniqatts` / `tableoid`-as-key is consumed (downstream of detection)

These are *not* the opt-in but show the blast radius the new syntax must keep
feeding correctly:

- **`src/backend/access/nbtree/nbtinsert.c`** — `_bt_check_unique` compares only
  the first `indnuniqatts` columns; reads the `tableoid` key column to open the
  conflicting partition and do the heap-liveness probe.
  **[VERIFIED file:line — inherited: `nbtinsert.c:429–441`, `:576–672`,
  generic reach at `:213`; RE-CONFIRM]**
- **`src/backend/commands/indexcmds.c:1189–1200`** — the spanning-build block:
  `if (progresql_bypass)` sets `ii_NumUniqKeyAtts = N` (the user-key count),
  appends `TableOidAttributeNumber` (-6) as the `(N+1)`th key column, and bumps
  `ii_NumIndexKeyAttrs`/`ii_NumIndexAttrs` to `N+1`. This is where the spanning
  shape is physically constructed. **[VERIFIED file:line]** (read this session.)
- **`src/backend/commands/indexcmds.c:1348–1349`** — `if (progresql_bypass &&
  OidIsValid(indexRelationId)) BuildSpanningIndexFromPartitions(rel,
  indexRelationId);` — backfills the new spanning index from existing partition
  tuples (e.g. `ALTER TABLE ADD CONSTRAINT` on a non-empty tree). **[VERIFIED
  file:line]**
- **`src/backend/commands/indexcmds.c:1351–1366`** — `if (partitioned &&
  !progresql_bypass)` is the *normal* partition-propagation path; for spanning
  indexes it is intentionally skipped (the index lives only on the root, no
  per-partition copies). **[VERIFIED file:line]**
- **`src/backend/access/nbtree/nbtsort.c`**, **`access/index/genam.c`**,
  **`access/index/indexam.c`** — build over partitions; key-description.
  **[VERIFIED — README *Index AM* row; RE-CONFIRM]**
- **`src/backend/catalog/index.c`** (build/reindex hooks) and
  **`src/backend/catalog/heap.c`** — root index left empty at build; REINDEX
  repopulates from all partitions. Stock `binary_upgrade` hooks at
  `index.c:86–87`, `:953–968` (no spanning-specific upgrade code).
  **[VERIFIED file:line — inherited (`PRODUCTION_READINESS.md` P0-2); RE-CONFIRM]**
- **`src/backend/commands/tablecmds.c`** — DROP / DETACH (delete departing
  partition's entries keyed by `tableoid`) and ATTACH (backfill). Spanning-index
  *propagation* on the partition tree lives here. **[VERIFIED — README *Commands*
  row; RE-CONFIRM]**
- **`src/backend/executor/execIndexing.c`** — `ExecInsertSpanningIndexTuples`
  defined **`:1345`**; per-statement cache. **[VERIFIED file:line — inherited
  (`PRODUCTION_READINESS.md` P0-1); RE-CONFIRM]**
- **`src/backend/executor/nodeModifyTable.c`** — call sites **`:1251`** (INSERT)
  and **`:2393`** (UPDATE epilogue); arbiter setup `:5087`. **[VERIFIED file:line
  — inherited; RE-CONFIRM]**
- **`src/backend/optimizer/util/plancat.c`** — spanning indexes hidden from path
  generation. **[VERIFIED — README *Planner* row + P3-1; RE-CONFIRM]**
- **`src/backend/access/heap/vacuumlazy.c`** — `progresql_vacuum_spanning_indexes()`
  declared `:447`, body `~2456–2489`, invoked `~2616–2621`. **[VERIFIED file:line
  — inherited (P2-3); RE-CONFIRM]**

### 2.4 The dump/restore gap that the syntax must close

- **`src/backend/utils/adt/ruleutils.c`** — **CONFIRMED this session:**
  `grep -n "indnuniqatts\|progresql\|spanning" ruleutils.c` returns **nothing**
  (exit 1). `pg_get_indexdef()` has **no** spanning-aware emission — it will emit
  an ordinary `CREATE INDEX`, silently downgrading a spanning index on
  dump/restore. **[VERIFIED file:line — absence confirmed]** (was [NEEDS-RESEARCH]
  in P0-2; now confirmed as a real gap.) Whatever new syntax we choose,
  `pg_get_indexdef` **must** emit
  it, or `pg_dump | psql` silently downgrades a spanning index to an ordinary
  `CREATE INDEX`. This is a hard dependency between P1-1 and the dump/upgrade
  plan (P0-2). **This plan only NOTES the dependency; the emission change is
  owned by the dump/upgrade workstream but is unblocked by the grammar here.**

---

## 3. Syntax options & tradeoffs

The North Star: an **explicit, declarative** opt-in that (a) needs no INHERITS,
(b) round-trips through `pg_dump`, (c) reads as a deliberate feature, and (d)
aligns with the decade of "global index on partitioned table" prior art
(P1-2) so the proposal engages that discussion rather than reinventing it.

### Option (a) — `CREATE UNIQUE INDEX … ON parent (…) GLOBAL;`

```sql
CREATE UNIQUE INDEX ON events (id) GLOBAL;
-- or the constraint shorthand below feeding into this
```

- **Pros:** Smallest grammar surface (one trailing keyword on an existing
  command). Matches the community's own vocabulary — "global index" is the term
  the prior-art threads use (Oracle's `GLOBAL`/`LOCAL` partitioned-index
  distinction is the canonical reference point). Naturally extends later to
  read-acceleration (a true global index), which is where the feature *should*
  go (P3-1). Decouples cleanly from INHERITS.
- **Cons:** `GLOBAL` is a *very* broad word to burn as a keyword; pgsql-hackers
  will scrutinize keyword cost. A bare `CREATE INDEX … GLOBAL` does not let you
  attach a *constraint name* the way `PRIMARY KEY` / `UNIQUE` table constraints
  do, so PK/UNIQUE-as-constraint ergonomics need the Option (b) layer on top
  anyway. Index-only syntax doesn't express "this is the table's primary key."

### Option (b) — Constraint clause: `UNIQUE (id) GLOBAL` / `PRIMARY KEY (id) WITHOUT PARTITION KEY`

```sql
CREATE TABLE events (id bigint, ts timestamptz NOT NULL,
    PRIMARY KEY (id) WITHOUT PARTITION KEY      -- explicit cross-partition PK
) PARTITION BY RANGE (ts);

ALTER TABLE events ADD CONSTRAINT events_pk PRIMARY KEY (id) WITHOUT PARTITION KEY;
```

- **Pros:** Lives exactly where the user already declares uniqueness; no INHERITS
  needed. Carries a constraint name (proper `pg_constraint` row, `\d` output,
  FK-target semantics later). `WITHOUT PARTITION KEY` is *self-documenting*: it
  names the precise vanilla restriction being lifted ("unique must include all
  partitioning columns"), which is rhetorically strong in a hackers thread. Works
  for both `CREATE TABLE` and `ALTER TABLE ADD CONSTRAINT`. The constraint
  internally lowers to a `CREATE UNIQUE INDEX … GLOBAL` (Option a as the
  mechanism), so (a) and (b) compose rather than compete.
- **Cons:** `WITHOUT PARTITION KEY` is wordy; `GLOBAL` as a constraint modifier
  is terser but reuses the broad keyword. More grammar productions than (a)
  alone (both `columnDef` inline and table-level `TableConstraint`). Must thread
  a new flag through `Constraint` → `IndexStmt`.

### Option (c) — storage / reloption or index-AM option

```sql
CREATE UNIQUE INDEX ON events (id) WITH (global = true);
-- or  ALTER TABLE events SET (spanning_pk = ...)
```

- **Pros:** Zero new keywords; reloptions are cheap to add and already
  round-trip through `pg_dump`. Lowest grammar risk.
- **Cons:** **Reloptions are tuning knobs, not semantic switches.** A reloption
  that silently changes a constraint's *correctness domain* (within-partition vs
  cross-partition uniqueness) is exactly as "hidden" as the INHERITS trick —
  it trades one implicit signal for another. pgsql-hackers strongly resists
  encoding semantic behavior in `WITH (...)`. **Rejected as the primary
  mechanism** (it would not fix the P1-1 objection, only relocate it).

### Recommendation

**Adopt Option (b) as the user-facing surface, lowered onto Option (a) as the
mechanism. Reject Option (c).**

- Primary syntax: **`PRIMARY KEY (…) WITHOUT PARTITION KEY`** and
  **`UNIQUE (…) WITHOUT PARTITION KEY`** in `CREATE TABLE` / `ALTER TABLE ADD
  CONSTRAINT` (constraint-name-bearing, self-documenting).
- Underlying index DDL (what `pg_get_indexdef` emits, what the constraint lowers
  to, and a usable standalone form): **`CREATE UNIQUE INDEX … ON parent (…)
  GLOBAL`**.
- Rationale: (b) removes the INHERITS dependency *at the exact point users
  declare uniqueness*, carries a real constraint identity, and reads as a
  deliberate lifting of a named restriction; (a) gives the dump/restore path and
  the future read-acceleration extension a home. They are one feature expressed
  at two layers, not two competing syntaxes. **[ASSUMPTION on final spelling]**
  — `WITHOUT PARTITION KEY` vs `GLOBAL` is exactly the bikeshed the community
  must settle (P1-2); the implementation should be structured so the *spelling*
  is a thin grammar/keyword choice over a stable internal flag, deferring the
  naming fight without blocking the wiring.

---

## 4. Full implementation outline (grammar → parse → catalog → propagation)

### 4.1 Keywords — `src/include/parser/kwlist.h`

- Add `WITHOUT` (likely already a keyword — verify) and a new unreserved keyword
  for the modifier. Prefer reusing existing keywords to minimize cost:
  - `WITHOUT PARTITION KEY` reuses `WITHOUT`, `PARTITION`, `KEY` — **all already
    keywords** in PG → **zero new keywords** for the constraint spelling.
    **[ASSUMPTION — RE-CONFIRM each is present in `kwlist.h`]** This is a strong
    point in favor of the `WITHOUT PARTITION KEY` spelling over `GLOBAL`.
  - If `GLOBAL` is chosen instead: `GLOBAL` is already an *unreserved* keyword in
    PG (used in `CREATE TEMP`/`GLOBAL TEMPORARY`), so it may be reusable without
    a new entry — **[NEEDS-RESEARCH]** confirm it parses unambiguously as a
    trailing `CREATE INDEX` modifier.

### 4.2 Grammar — `src/backend/parser/gram.y`

- **Table constraint production:** extend the `ConstraintElem` rules for
  `PRIMARY KEY '(' columnList ')'` and `UNIQUE '(' columnList ')'` to accept an
  optional trailing `opt_without_partition_key` (or `opt_global`) clause. Set a
  new boolean on the `Constraint` node.
- **Inline column constraint:** lower priority; the cross-partition PK is almost
  always table-level (it spans columns + needs a name). Can be deferred, but at
  minimum the inline `PRIMARY KEY`/`UNIQUE` column constraint should *error
  clearly* if the modifier is attached inline in v1, rather than silently
  ignoring it.
- **`CREATE INDEX` production:** add `opt_global` after the index `(...)` /
  `WITH`/`WHERE` tail, setting a new boolean on `IndexStmt`.
- **`ALTER TABLE ADD CONSTRAINT`:** flows through the same `ConstraintElem`, so
  it is covered by the table-constraint change for free.

### 4.3 Parse-node fields — `src/include/nodes/parsenodes.h`

- **`IndexStmt`:** add `bool isglobal;` (or `bool spanning;`). This is the single
  internal flag the whole feature keys off, decoupled from any keyword spelling.
- **`Constraint`:** add a matching `bool without_partition_key;` (or reuse one
  `bool global;`). `transformIndexConstraint` copies it onto the generated
  `IndexStmt`.
- Update `copyfuncs`/`equalfuncs`/`outfuncs` (or the generated node support) for
  both nodes. **[VERIFIED — standard PG node plumbing requirement; RE-CONFIRM
  whether this tree uses gen_node_support.pl (PG16+) so the change is just the
  struct field]**

### 4.4 Parse-analysis — `src/backend/parser/parse_utilcmd.c`

- **This is the file that currently houses the implicit detection** (§2.1).
  **Rip out** the `has_superclass`/INHERITS-based predicate entirely. Replace
  with: "this constraint/index becomes a spanning index **iff** the new explicit
  flag is set."
- `transformTableConstraint` / `transformIndexConstraint`: when the constraint's
  `without_partition_key` flag is set, (1) validate the table is a *partitioned
  root* (`RELKIND_PARTITIONED_TABLE` and not itself a partition); (2) validate it
  is *not* a sub-partitioned/multi-level tree (current limitation — must
  hard-error, not silently skip — see P2-5); (3) **skip** the vanilla
  "unique constraint must include all partitioning columns" rejection that
  normally fires here; (4) propagate the flag onto the generated `IndexStmt`.
- **Crucially, INHERITS becomes irrelevant** — a plain `PARTITION BY` root with
  the explicit clause now opts in. The base-table inheritance is no longer read
  anywhere in the detection path. That is the end-to-end removal of the INHERITS
  dependency.

### 4.5 Index creation — `src/backend/commands/indexcmds.c`

- `DefineIndex`: replace the implicit predicate with `if (stmt->isglobal)`.
  Everything downstream (appending the `tableoid` trailing key column, setting
  `indnuniqatts = <user-col count>`) stays — only the *trigger* changes.
- Add explicit validation errors with `errcode(ERRCODE_FEATURE_NOT_SUPPORTED)` /
  `ERRCODE_INVALID_TABLE_DEFINITION` for: non-partitioned target, partition (not
  root) target, multi-level tree, EXCLUSION constraint, expression/partial index
  if unsupported. Today some of these are implicitly excluded by the predicate;
  with explicit syntax the user can *ask* for the unsupported case, so each needs
  a clear error.

### 4.6 Catalog representation — is `indnuniqatts` sufficient?

- **`indnuniqatts > 0` is sufficient as the on-disk marker and should stay.** It
  already encodes both "this is spanning" *and* the unique-prefix length in one
  field, and the entire AM/executor/vacuum/planner blast radius already keys off
  it (§2.3). Adding a *separate* boolean would be redundant and create a
  consistency hazard (two fields that must agree).
- **However:** consider whether `indnuniqatts` should be exposed/derived more
  legibly. For catalog *clarity* (a P1-3 concern), document the invariant
  precisely in `pg_index.h`: "`indnuniqatts > 0` iff a spanning (global) index;
  value = number of leading key columns forming the unique domain; the index has
  exactly `indnuniqatts + 1` key columns, the last being `tableoid`
  (`TableOidAttributeNumber`, attno -6 — see §1 correction)."
- **No new catalog column is warranted for P1-1.** The opt-in moves entirely into
  grammar/parsenodes; the catalog already records the *result* correctly.
  **[ASSUMPTION — RE-CONFIRM by reading `pg_index.h` that `indnuniqatts` is the
  only spanning marker and nothing else relies on INHERITS at catalog level]**

### 4.7 DDL propagation & lifecycle (unchanged trigger, verify still reached)

- `tablecmds.c` ATTACH backfill / DETACH cleanup, `index.c` build/reindex,
  `vacuumlazy.c` cleanup all key off `indnuniqatts`, **not** off the opt-in
  mechanism — so they are unaffected by the syntax change. **Partially verified
  this session:** a grep of `tablecmds.c`, `vacuumlazy.c`, `index.c`,
  `indexcmds.c` for `has_superclass`/`relhassubclass`/`INHERITS`/`superclass`
  intersected with `spanning`/`bypass`/`uniq` returned **no hits** — i.e. the
  runtime spanning paths do **not** re-derive "is spanning" from inheritance;
  they key off `indnuniqatts`. **[VERIFIED — absence confirmed for the obvious
  predicates; RE-CONFIRM with a broader read of each spanning code block]** This
  is the load-bearing fact behind the §5 backward-compat claim: removing the
  INHERITS opt-in cannot disable the runtime path on already-built indexes.

### 4.8 Round-trip emission — `src/backend/utils/adt/ruleutils.c` (NOTE only)

- `pg_get_indexdef()` must emit `CREATE UNIQUE INDEX … GLOBAL` (or the chosen
  form) when `indnuniqatts > 0`, and `pg_get_constraintdef()` must emit the
  `… WITHOUT PARTITION KEY` constraint form. **This is required for `pg_dump`
  fidelity (P0-2 / P2-2) but is owned by the dump/upgrade plan.** P1-1 only needs
  to (a) define the canonical emitted syntax here and (b) ensure the grammar can
  re-parse exactly what `pg_get_indexdef` emits (a round-trip contract test).

### 4.9 Documentation — `doc/src/sgml/ref/{create_index,create_table,alter_table}.sgml`

- Document the new clause. Upstream will not look at a feature with no SGML.
  In-scope to *plan*, owned partly by the RFC (P1-2).

---

## 5. Backward-compatibility / migration for existing INHERITS-based tables

The implicit mechanism is being **removed**, so existing databases created with
the INHERITS+PARTITION BY trick need a migration story:

1. **On-disk compatibility is intact.** Existing spanning indexes are already
   marked by `indnuniqatts > 0` in the catalog — the *runtime* (AM, executor,
   vacuum) keys off that, not off INHERITS. So **already-built spanning indexes
   keep working** after the trigger is removed; only *new* DDL must use the new
   syntax. **[VERIFIED — supported by §2.4 grep: no runtime spanning path checks
   INHERITS/superclass; RE-CONFIRM with a full read of each spanning block]**
2. **Re-create / re-dump path.** A `pg_dump` of an old DB must emit the *new*
   syntax (via the ruleutils change, §4.8), so a dump+restore transparently
   migrates the DDL form. This makes `pg_dump | psql` the supported migration.
3. **`pg_upgrade`.** Binary upgrade preserves catalog rows verbatim
   (`indnuniqatts` carried over), so upgraded clusters keep spanning indexes
   without re-running DDL — **provided partition OIDs are preserved** (P0-2 / and
   P1-3 §6 below). This is the load-bearing assumption to verify in the
   dump/upgrade workstream.
4. **Deliberate non-compat:** new `CREATE TABLE … INHERITS … PARTITION BY … PK`
   will, after this change, behave like *stock PostgreSQL* (i.e. reject a unique
   constraint that omits the partition key). This is the intended, documented
   break — the README and a CHANGELOG entry must state it. Since this is a
   research fork with no production users, a hard break (no deprecation window)
   is acceptable; document it loudly.

---

## 6. P1-3 — `tableoid`-as-key catalog rationale + OID-reuse handling

### 6.1 Why `tableoid` is in the key (the defensible rationale)

- A spanning index is one physical B-tree indexing rows that physically live in
  **many heaps** (the partitions). A heap TID (block,offset) is only unique
  *within one relation*; across partitions TIDs collide. To address an indexed
  row unambiguously you need **(which partition, which TID)**. The `tableoid`
  trailing key column supplies "which partition." **[VERIFIED — README "How it
  works"; this is the core design invariant]**
- Putting `tableoid` *in the key* (not just the payload) means two rows with the
  same user key in different partitions are **distinct B-tree entries**, so the
  tree stays well-formed and ordered, while `_bt_check_unique` compares only the
  leading `indnuniqatts` columns to still catch the cross-partition duplicate.
  **[VERIFIED file:line — inherited `nbtinsert.c:429–441`, `:576–672`;
  RE-CONFIRM]**
- This is the same family of idea as a global index's "partition identifier in
  the index entry" used by other RDBMSs; framing it that way in the RFC (P1-2)
  preempts the "OIDs aren't identifiers" reflex by showing it's a *physical*
  locator, not a logical key exposed to users.

### 6.2 Anticipated objections (document answers to each)

1. **"OIDs are not stable identifiers."** — Correct for *logical* use, but here
   `tableoid` is used as a *physical partition locator within one index's
   lifetime*, analogous to how TIDs are physical and unstable yet used in
   indexes. It is never exposed as a user key and is rewritten by REINDEX.
2. **`pg_upgrade` / OID preservation (P0-2).** — The hard dependency. If
   partition OIDs are not preserved across upgrade, every stored `tableoid` key
   dangles. **Answer must be:** spanning-indexed trees require partition-OID
   preservation across upgrade, OR the upgrade must REINDEX the root (which
   repopulates `(key, tableoid)` from live rows under the *new* OIDs).
   **[NEEDS-RESEARCH — confirm which: does PG's binary_upgrade preserve these
   OIDs, and/or can we force a post-upgrade REINDEX of spanning roots?]**
3. **DETACH semantics.** — On DETACH, the departing partition's entries are
   removed from the root keyed by its `tableoid` (`tablecmds.c`). Document that
   detached data leaves the uniqueness domain (matches user intent).
4. **OID wraparound / reuse** — see §6.3.

### 6.3 OID reuse after many DROP/CREATE cycles within a tree — is it a hazard?

**Is reuse real?** Postgres OIDs are drawn from a single cluster-wide 32-bit
counter that **wraps** and, on wrap, **skips OIDs already in use** (the loop in
`GetNewOidWithIndex` retries until it finds a free one). So a *currently-live*
partition can never share a `tableoid` with another *currently-live* relation —
collision among live relations is impossible by construction. **[VERIFIED —
standard PG OID allocation behavior; RE-CONFIRM the allocator path used for
`pg_class` OIDs in this tree]**

**Where the hazard actually is (the subtle case):** a *stale* spanning-index
entry whose partition was DROPped, whose entry was *not* removed, and whose OID
is **later reused** by a brand-new partition attached to the same tree. The
stale entry would then alias the new partition's `tableoid` → a false-positive
uniqueness conflict or a probe into the wrong heap.

- **Is it real here?** Only if a DROP/DETACH ever leaves entries behind. The
  design claims DROP/DETACH delete the departing partition's entries keyed by
  `tableoid` (README; commit `67a5276404` "DROP/DETACH cleanup"). **If that
  cleanup is complete and atomic, there are no stale entries to alias**, and OID
  reuse is a non-hazard. **[VERIFIED — README; RE-CONFIRM the cleanup is
  exhaustive in `tablecmds.c` for both DROP and DETACH, including error/abort
  paths]**
- **Residual risk:** a crash *between* heap drop and spanning-entry cleanup, if
  not WAL-atomic, could leave orphans (ties into P0-3 crash recovery). **[NEEDS-
  RESEARCH]**

**Mitigations (in preference order):**
1. **Guarantee cleanup atomicity** (same transaction / WAL as the partition
   drop). Primary defense — removes the precondition for aliasing.
2. **REINDEX as the recovery hammer:** because the root index has no own storage
   and REINDEX repopulates from live rows only, a REINDEX of a spanning root
   *cannot* retain orphaned entries. Document REINDEX as the supported repair and
   run it post-`pg_upgrade` if OID preservation is not guaranteed (§6.2).
3. **Belt-and-suspenders probe:** `_bt_check_unique` already opens the partition
   by `tableoid` and does a heap-liveness probe before raising a conflict. If the
   opened relation's `relkind`/membership in the tree is *also* validated (cheap,
   cached), an aliased orphan pointing at a non-member relation can be detected
   and skipped/LP_DEAD-killed rather than mis-firing. **[ASSUMPTION — evaluate
   cost; may be unnecessary if (1) holds]**
4. **Test it** — see §7.

---

## 7. Test plan

### 7.1 Syntax / parse (P1-1)
- `CREATE TABLE … PARTITION BY … (PRIMARY KEY (id) WITHOUT PARTITION KEY)` with
  **no INHERITS** builds a spanning index (`indnuniqatts > 0` in `pg_index`).
- `UNIQUE (…) WITHOUT PARTITION KEY` likewise; named constraint shows in `\d`.
- `ALTER TABLE … ADD CONSTRAINT … PRIMARY KEY (…) WITHOUT PARTITION KEY`.
- Standalone `CREATE UNIQUE INDEX … GLOBAL`.
- **Negative:** the clause on a non-partitioned table; on a *partition* (not
  root); on a multi-level (sub-partitioned) tree; with EXCLUSION; → clear errors,
  not silent skip (P2-5).
- **Removal verification:** old `INHERITS + PARTITION BY + PK` *without* the new
  clause now behaves like stock PG (rejects the partition-key-omitting unique).
- **Round-trip:** `pg_get_indexdef` / `pg_get_constraintdef` output re-parses to
  an identical catalog state (the §4.8 contract) — guards the dump path.
- Rewrite/extend `progresql_ddl` regression suite for all the above.

### 7.2 Catalog / `tableoid` (P1-3)
- **OID-reuse cycle test:** in one tree, repeatedly `CREATE TABLE part_n
  PARTITION OF …; INSERT; DROP TABLE part_n;` enough times to plausibly recycle
  an OID (or force via low OID counter in a TAP test), then attach a new
  partition and `INSERT` a key that *was* used by a dropped partition — assert
  **no** false conflict and correct enforcement. This directly exercises §6.3.
- **DROP/DETACH cleanup completeness:** after DROP and after DETACH, assert the
  root spanning index has zero entries for the departed `tableoid` (introspect
  via a debug function or via re-inserting a previously-conflicting key and
  expecting success).
- **REINDEX-repair test:** artificially nothing-left-behind expectation — REINDEX
  a spanning root and re-verify cross-partition uniqueness.

### 7.3 Carried dependencies (NOTE, owned elsewhere)
- `pg_dump | psql` and `pg_upgrade` round-trip with the new syntax (P0-2).
- Crash-recovery atomicity of DROP+cleanup (P0-3) → orphan-free after crash.

---

## 8. Risks / unknowns

- **[RESOLVED this session] Exact location & shape of the current predicate** —
  it is an inline local `bool progresql_bypass` in `DefineIndex`
  (`indexcmds.c:577` decl, `:754` assignment, 6 consumption sites) plus the
  parser allowance at `parse_utilcmd.c:1245`. Removal is surgical: two edits. The
  only residual is pasting the full multi-line RHS of `:754` verbatim.
- **[LOW RISK — largely resolved] Hidden INHERITS coupling at runtime** — grep of
  the spanning runtime files for inheritance predicates intersected with the
  spanning markers found nothing; runtime keys off `indnuniqatts`. Confirm with a
  full read of each spanning code block before deleting the opt-in.
- **Keyword bikeshed (P1-2)** — `GLOBAL` vs `WITHOUT PARTITION KEY` is a
  community decision; structure code so spelling is a thin layer over a stable
  internal `IndexStmt.isglobal` flag.
- **`ruleutils` round-trip (P0-2)** — if `pg_get_indexdef` can't emit re-parsable
  syntax, dump silently downgrades. Cross-team dependency; must land together.
- **OID preservation across `pg_upgrade`** — the crux for the `tableoid`-key
  rationale; unverified.
- **Crash atomicity of cleanup** — residual orphan risk feeding OID-reuse aliasing.
- **Inline column-constraint form** — deferred; ensure it errors rather than
  silently ignoring the modifier in v1.

---

## 9. Effort estimate

| Workstream | Estimate | Notes |
|---|---|---|
| Grammar + keywords (`gram.y`, `kwlist.h`) | 1–2 d | Small if `WITHOUT PARTITION KEY` reuses existing keywords (0 new) |
| Parse nodes + node support (`parsenodes.h`, copy/equal/out) | 0.5–1 d | One bool on `IndexStmt` + one on `Constraint` |
| Parse-analysis rewrite (`parse_utilcmd.c`) — remove implicit, add explicit | 2–3 d | The core P1-1 change; care around the partition-key-omission bypass |
| `indexcmds.c` trigger swap + explicit validation errors | 1–2 d | Mostly error paths for now-reachable unsupported cases |
| Catalog doc/invariant in `pg_index.h` (no new column) | 0.5 d | Documentation + assert |
| `ruleutils.c` emission (NOTE: shared w/ dump plan) | 1–2 d | Coordinate; round-trip contract test |
| SGML docs | 1 d | create_index/create_table/alter_table |
| Regression suite rewrite (`progresql_ddl`) | 2 d | §7.1 + §7.2 |
| OID-reuse / cleanup TAP tests | 2 d | §7.2; needs forced-OID harness |
| P1-3 rationale doc (catalog + OID-reuse) | 1 d | Feeds the P1-2 RFC |
| **Total (P1-1 + P1-3, excl. P0 dump/upgrade/crash work)** | **~12–17 d** | Plus re-grep/verify pass (§2, §8) up front: +0.5–1 d |

**Sequencing:** (0) re-grep & confirm §2 citations + §4.7/§8 INHERITS coupling →
(1) parsenodes + internal flag → (2) grammar → (3) parse_utilcmd rewrite →
(4) indexcmds trigger swap → (5) tests → (6) ruleutils + SGML (with dump plan) →
(7) P1-3 doc.

---

## 10. Executive summary

1. **Problem (P1-1):** the spanning-index feature is opted into *implicitly* by a
   partitioned root that also `INHERITS` a base table + declares PK/UNIQUE — a
   hidden feature flag overloading two PG subsystems. A non-starter for upstream.
2. **Current mechanism (verified in-tree):** one inline predicate
   `bool progresql_bypass` in `DefineIndex` — declared `indexcmds.c:577`,
   assigned `:754` (`partitioned && …`), consulted at `:981/1189/1263/1269/1348/
   1351`; the parser allowance is `parse_utilcmd.c:1245`. It is **two edits** to remove.
3. **Catalog (verified):** the sole on-disk marker is `pg_index.indnuniqatts`
   (`pg_index.h:36–42`); spanning sets it to `indnkeyatts-1` so the trailing
   partition-OID column is excluded from the unique domain. No new column needed.
4. **Syntax recommendation:** explicit `PRIMARY KEY (…) / UNIQUE (…) WITHOUT
   PARTITION KEY` in CREATE/ALTER TABLE (constraint-named, self-documenting,
   reuses existing keywords — likely 0 new), lowered onto a `CREATE UNIQUE INDEX
   … GLOBAL` mechanism. Reject the reloption option (relocates the magic, doesn't fix it).
5. **Wiring:** add one `bool isglobal` to `IndexStmt` (+ matching `Constraint`
   flag); the whole feature keys off that internal flag, so the keyword spelling
   stays a thin, deferrable bikeshed.
6. **INHERITS removal is clean:** runtime (AM/executor/vacuum/tablecmds) keys off
   `indnuniqatts`, NOT inheritance (grep-confirmed) — so existing built indexes
   keep working; only new DDL changes.
7. **Dump gap confirmed:** `ruleutils.c` has zero spanning emission today —
   `pg_get_indexdef` must learn the new syntax or dump silently downgrades
   (coordinate with the P0-2 dump/upgrade plan; grammar here unblocks it).
8. **P1-3 (`tableoid`-as-key):** it's a *physical partition locator*, not a
   logical key (analogous to TIDs); frame it that way to preempt the "OIDs aren't
   identifiers" reflex.
9. **OID reuse:** not a hazard among live relations (allocator skips in-use OIDs);
   the only risk is a *stale* orphan entry aliasing a reused OID — eliminated if
   DROP/DETACH cleanup is atomic, with REINDEX as the guaranteed repair.
10. **Effort:** ~12–17 dev-days for P1-1+P1-3 (excludes P0 dump/upgrade/crash
    work), gated on a short up-front re-grep/verify pass.
