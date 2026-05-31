# C1 — Partition-Sequence-Number Discriminator: Design Spike

> **Document type:** DESIGN SPIKE — PLANNING ONLY. No `src/` files were modified
> in producing this document. It plans the replacement of the spanning index's
> `tableoid` discriminator with an **index-local partition sequence number
> (`partseq`)** plus a catalog mapping `(index, partseq) → partition relation`.
>
> **Decision already made (input to this spike):** we commit to **option C1** —
> keep a single ordinary nbtree on the partitioned root, but replace the trailing
> `tableoid` key column with a small, stable, index-owned `partseq`. We do **not**
> adopt option **B** (a true "extended TID" / new on-disk index-pointer format);
> the boundary where C1 stops and B begins is characterized in §9.
>
> **Provenance / confidence tags** (same convention as the sibling plan docs):
> - **[VERIFIED file:line]** — confirmed against the source tree, with the
>   citation read in-tree. Where the citation was established by a prior in-tree
>   audit (the `file:line` references carried in `P1-1-syntax-catalog.md`,
>   `P0-2-dump-upgrade.md`, `P2-P3-remainder.md`, `P0-1-upsert-merge.md`) rather
>   than re-read this session, it is tagged **[VERIFIED file:line — per
>   plan-doc audit; RE-CONFIRM]**. Tool-result truncation in this session
>   prevented a fresh line-by-line re-read of every cited site; the implementer
>   MUST re-confirm each `file:line` before editing.
> - **[PRIOR-ART <source>]** — drawn from `P1-2-prior-art-rfc.md`, which carries
>   FETCH-VERIFIED / SEARCH-SURFACED tags per source. Anything not independently
>   re-fetched this session inherits that doc's tag; unverifiable claims are
>   flagged inline. **Nothing here is fabricated.**
> - **[ASSUMPTION]** — reasoned from PostgreSQL internals, not confirmed in-tree.
>
> Last updated 2026-05-30. Branch `progresql-18`. Base `REL_18_STABLE`.

---

## 0. The one-paragraph thesis

Today the spanning index keys every entry `(user_cols…, tableoid)`, where the
trailing key column is the row's partition `pg_class` OID
(`TableOidAttributeNumber` = **-6**, not -7 — the README/comments are wrong;
[VERIFIED file:line — `sysattr.h:26`, `indexcmds.c:1198`, per plan-doc audit
P1-1 §1; RE-CONFIRM]). `tableoid` is **globally scoped, unstable across
`pg_upgrade`, and reusable after DROP/DETACH**. C1 replaces it with a **per-index
small integer `partseq`** that the index assigns when a partition first joins the
index's domain, recorded in a catalog mapping owned by the index. Because
`partseq` is **index-local and stable for the life of the partition's membership**,
two whole bug classes (P0-2 dump/upgrade OID-dangling, P1-3 OID-reuse aliasing)
**cease to exist by construction**. The index stays an ordinary nbtree: `partseq`
is still a key/sort column, not a new on-disk pointer format. The SOLID surface is
**one descriptor field + one predicate + one resolver**, and the discriminator is
touched at exactly **three chokepoints** (WRITE, MATCH, DEREFERENCE).

---

## 1. Prior-art extraction (how others represent the discriminator + map + lifecycle)

All sourcing inherits the per-source tags in `P1-2-prior-art-rfc.md`. The three
camps that doc identifies are the design tree C1 sits in.

### 1.1 Dilip Kumar, "Global Index for PostgreSQL" (2025) — Camp B, the direct precedent for C1's *mechanism*
- **Source:** pgsql-hackers thread, message-id
  `CAFiTN-uyec_y2QS2whUam8Rp1M+PPGuP0Zz45uX_U6mhQ8mtRg@mail.gmail.com`, started
  2025-06-06. [PRIOR-ART Dilip 2025 — FETCH-VERIFIED per P1-2 §1B/V4].
- **Discriminator representation:** a single non-partitioned index on the parent
  with **an added internal partition-identifier key column — a 4-byte int,
  allocated per leaf partition.** This is *exactly* C1's `partseq` idea: a small,
  dedicated, synthetic id rather than the existing `tableoid`. [PRIOR-ART Dilip
  2025].
- **Catalog map:** a **new catalog `pg_index_partitions`** mapping
  `(partition-id, global-index-oid) → relation oid`. [PRIOR-ART Dilip 2025]. This
  is the closest published precedent for C1's `(index, partseq) → partition`
  catalog and should be cited as such in any RFC. **The key= ordering Dilip uses
  is `(partition-id, index-oid)`; C1's natural primary key is the inverse,
  `(index-oid, partseq)`** — see §3.
- **partseq assignment/lifecycle:** the thread describes allocation "per leaf
  partition." The *exact* allocation algorithm (counter on the index? max+1?
  gap-reuse on DETACH?) is **not pinned down in the summary available to this
  spike** — flag as **[PRIOR-ART Dilip 2025 — assignment detail unverifiable from
  the summary; re-read the thread before claiming Dilip's specific algorithm]**.
- **Vacuum (relevant because the discriminator scopes "whose dead entries"):**
  the thread quantified the canonical objection — naive per-partition vacuum of a
  global index went **~15s → ~45min @ 1000 partitions (~300×)**; the accepted-shape
  fix is *defer the global pass, accumulate dead TIDs, vacuum the global index
  once at the end* (~40s). [PRIOR-ART Dilip 2025 — FETCH-VERIFIED per P1-2 §1B].
  C1 does not change this cost shape, but `partseq` makes "whose dead entries"
  exactly as well-defined as `tableoid` did (§6).

### 1.2 Alibaba POC, Wenjing Zeng (2021) — Camp C, ProgreSQL's *current* lineage (the thing C1 moves away from)
- **Source:** reply in the 2019 "Proposal: Global Index" thread, message-id
  `78E4E097-EEFE-4755-AAE4-97B60AD53B5B@alibaba-inc.com`, 2021-01-07, on PG13.
  [PRIOR-ART Alibaba 2021 — FETCH-VERIFIED per P1-2 §1B/V4b].
- **Discriminator representation:** the global index carries the partition's
  **`tableoid`** via the `INCLUDE` keyword — i.e. **the exact discriminator
  ProgreSQL uses today.** [PRIOR-ART Alibaba 2021]. This is what C1 replaces.
- **Lifecycle questions Wenjing flagged (verbatim shape):** concurrent
  build/rebuild; **DETACH** (lazy VACUUM cleanup of detached data?); **ATTACH**
  (data-laden partition risks unique conflicts — clean gradually or invalidate?);
  **TRUNCATE** (separate heap/index cleanup transactions). [PRIOR-ART Alibaba
  2021]. These are precisely the lifecycle races C1's catalog must answer (§3.3).
- **Robert Haas's OID-reuse objection** (in the same 2019 thread, V2): reusing a
  table OID after DETACH can leave stale global-index entries pointing at
  unrelated data, breaking uniqueness. [PRIOR-ART Haas 2019/2021 — FETCH-VERIFIED
  per P1-2 §1B/V2]. **This is the objection C1 exists to neutralize:** `partseq`
  is never an OID, so a reused OID can never alias a stale entry.

### 1.3 Postgres Pro `pgpro_gbtree` (shipping) — Camp B cousin
- **Source:** docs `postgrespro.com/docs/enterprise/current/pgpro-gbtree` + Habr
  writeup `habr.com/en/companies/postgrespro/articles/948428/`. [PRIOR-ART
  Postgres Pro gbtree — FETCH-VERIFIED per P1-2 §1C/V9].
- **Discriminator representation:** a **new `gbtree` access method** (a distinct
  AM, not stock btree) "based on standard B-tree but adapted for partitioned
  tables." [PRIOR-ART gbtree]. **This is the seam decision C1 must consciously
  reject:** Postgres Pro made the discriminator an *AM-level* concern (new AM);
  C1 keeps stock nbtree and makes the discriminator a *descriptor field* (§4, §7).
- **Catalog/lifecycle:** gbtree global indexes are **not rebuilt on DELETE or
  DETACH** — detached entries are left as **garbage cleaned by VACUUM**, and
  **autovacuum is not (yet) supported** for them. Entries **don't store MVCC tuple
  versions**, so they bloat less. PK required; `CONCURRENTLY` unsupported. [PRIOR-ART
  gbtree — FETCH-VERIFIED per P1-2 §1C]. The "garbage cleaned by VACUUM" choice is
  a valid C1 option for DETACH (§3.3) and de-risks the DETACH-is-O(rows) objection
  by deferring it.
- **Existence proof:** the feature is implementable and shippable; the hard
  problems are independently confirmed to be vacuum, CONCURRENTLY, DETACH cost,
  PK requirement. [PRIOR-ART gbtree].

### 1.4 HighGo / Cary Huang (2022) — Camp A, the contrast (NOT a discriminator design)
- **Source:** patch message-id
  `184879c5306.12490ea581628934.7312528450011769010@highgo.ca`, 2022-11-17.
  [PRIOR-ART HighGo 2022 — FETCH-VERIFIED per P1-2 §1B/V3].
- **Approach:** keeps a *partitioned* index (storage distributed per partition,
  new relkind `RELKIND_GLOBAL_INDEX`); global uniqueness via a deferred
  all-partition final sort. **No single-index discriminator at all** — included
  here only to mark the boundary: C1 is squarely a single-root-index design and
  therefore does not inherit HighGo's "no size limit / cheap DETACH" properties.
  [PRIOR-ART HighGo 2022].

### 1.5 The one structural fact behind every discriminator
- **Heap TIDs are relation-local** (`storage/itemptr.h`): a `t_tid` is unique only
  within one relation's storage, so a single physical index pointing into many
  partition heaps MUST carry a discriminator. Every serious design adds one;
  C1 chooses a synthetic index-local id (Dilip's camp) over the existing OID
  (Alibaba/ProgreSQL's camp). [PRIOR-ART P1-2 §1E].

### 1.6 Extraction summary table

| Design | Discriminator | Catalog map | Stable across pg_upgrade? | OID-reuse safe? | Seam |
|---|---|---|---|---|---|
| ProgreSQL today | `tableoid` (-6), key col | none (OID *is* the map) | **No** (OIDs not preserved on logical dump; baked-in) | **No** | btree-internal via `indnuniqatts` |
| **C1 (this doc)** | **`partseq` (int), key col** | **`(index, partseq) → relid` catalog** | **Yes** (partseq is index-local) | **Yes** (partseq ≠ OID) | btree-internal flag + descriptor field |
| Dilip 2025 (B) | 4-byte partition-id, key col | `pg_index_partitions` | Yes | Yes | (in-core global index) |
| Postgres Pro gbtree | (synthetic, AM-internal) | (AM-internal) | n/a (downstream) | Yes | **new AM** |
| Alibaba 2021 (C) | `tableoid` via INCLUDE | none | No | No | btree + executor scan |
| HighGo 2022 (A) | none (recurse to child idx) | n/a | Yes | Yes | new relkind |

---

## 2. The decision restated, with the boundary to "B" made explicit

**C1 = keep stock nbtree; replace the trailing `tableoid` key column with a
trailing `partseq` key column; add a catalog mapping `partseq → partition`.**

- The discriminator **stays a key/sort column** of an ordinary nbtree tuple. Tuple
  format, page format, WAL records, amcheck's structural invariants, and the
  insert/split/dedup machinery are **unchanged** — they just sort on a 4-byte int
  instead of an OID (both are pass-by-value `int4`-shaped; OID *is* `unsigned int`).
  [ASSUMPTION — comparator/opclass for the trailing column: today it sorts by the
  `oid` opclass; C1 sorts by the `int4`/`oid` opclass for `partseq`. Confirm the
  opclass wiring at the spanning-build site, `indexcmds.c:1189–1200` — see §5.]
- **Where B begins (out of scope):** "B" is a *true extended TID* — changing the
  index entry's **`t_tid`/pointer representation** to natively carry
  `(partition, blk, off)` instead of stuffing the partition id into a key column.
  B touches `ItemPointerData`, the nbtree tuple/posting-list layout, every AM that
  reads `t_tid`, amcheck, and on-disk compatibility cluster-wide. **C1
  deliberately does none of that.** The C1↔B boundary is: *C1 never alters the
  meaning of `t_tid` or the index tuple's pointer; it only changes which value
  occupies the existing trailing key column and adds a side catalog.* (§9.)

---

## 3. Catalog design — `(index, partseq) → partition`

### 3.1 New catalog vs. reuse

Three candidates were evaluated:

| Option | Verdict |
|---|---|
| **(a) New catalog `pg_index_partition`** (one row per `(index, partseq)`) | **RECOMMENDED.** Mirrors Dilip's `pg_index_partitions` [PRIOR-ART Dilip 2025] — the published precedent. Clean ownership, dumps as catalog data, indexable for the resolver, room for lifecycle columns (state, attach-xid). |
| (b) Reuse `pg_inherits` | **Rejected.** `pg_inherits` maps child→parent for the *table* tree; it has no per-index column and no slot for a per-index sequence number. Overloading it couples the index discriminator to table inheritance — the exact "magic" P1-1 is trying to kill. |
| (c) Store the map in the index **metapage** | **Rejected for the primary store.** The root spanning index has **no storage of its own at the heap level but it does have a real nbtree relfilenode** (Model A, [VERIFIED — `index.c` leaves root empty at build, REINDEX repopulates; per plan-doc audit P0-2 §3]). A metapage map would be invisible to SQL/`pg_dump`, hard to MVCC, and would have to be re-emitted by binary-upgrade by hand. A catalog is strictly better for dump/restore (§6). *Could* be a future cache, not the source of truth. |
| (d) Extra columns on `pg_index` | **Rejected.** `pg_index` is one row per index; the map is one row per `(index, partition)` — wrong cardinality. |

**Recommendation: new catalog `pg_index_partition`** ([ASSUMPTION on final name];
singular to match PG convention, e.g. `pg_index`, `pg_inherits`).

### 3.2 Proposed schema

```
CATALOG(pg_index_partition,<OID>,<rowtype-oid>)
{
    Oid     indpartidxid   BKI_LOOKUP(pg_class);  /* the spanning index   */
    int32   indpartseq;                           /* the partseq (>= 1)   */
    Oid     indpartrelid   BKI_LOOKUP(pg_class);  /* the partition relid  */
    char    indpartstate;                         /* 'a' active, 'd' detaching/garbage */
}
```
- **Primary key:** `(indpartidxid, indpartseq)` — the **resolver** lookup
  (`partseq → relid`, scoped to one index). [ASSUMPTION].
- **Secondary unique index:** `(indpartidxid, indpartrelid)` — the **writer**
  lookup (given a partition relid, find its partseq before inserting). [ASSUMPTION].
- `indpartseq` is a small monotonically-increasing per-index integer (§3.3).
- `indpartstate` supports the gbtree-style "leave as garbage, clean lazily"
  DETACH option (§3.3) without deleting the row out from under in-flight scans.

### 3.3 partseq allocation + ATTACH/DETACH lifecycle (incl. races)

**Allocation (ATTACH / build / CREATE):**
- partseq = `max(indpartseq) for this index + 1`, allocated **inside the same
  transaction and under the same lock** that adds the partition to the index's
  domain. Because CREATE INDEX (spanning) and ATTACH already take a strong lock on
  the root + the partition ([VERIFIED — blocking build locks root+all partitions,
  per plan-doc audit / README "DDL lifecycle"; RE-CONFIRM], `tablecmds.c` ATTACH),
  the `max+1` read-then-insert is **serialized by that lock** — no separate
  sequence object, no allocation race for the blocking path. [ASSUMPTION — holds
  only as long as the spanning ATTACH/build path holds `ShareLock`+ on the root;
  CONCURRENTLY is out of scope here and is where a real allocation race would
  appear, see §8].
- **Never reuse a partseq.** On DETACH/DROP, mark the row `indpartstate='d'` (or
  delete it after cleanup) but **do not hand its number to a future partition.**
  This is the property that makes OID-reuse irrelevant: even partseq-reuse is
  forbidden, so a stale entry can never alias a new partition. (Monotonic int32
  gives 2^31 attaches per index lifetime before REINDEX resets it — REINDEX
  renumbers from 1; see §8 catalog-bloat note.)

**DETACH (two supported policies, pick per the vacuum decision in §6):**
1. **Eager (today's behavior, keyed by discriminator):** delete the departing
   partition's entries from the root keyed by its `partseq`, then delete the
   catalog row. O(rows in partition). [VERIFIED — DROP/DETACH delete departing
   entries keyed by `tableoid` today; per plan-doc audit, commit `67a5276404`;
   RE-CONFIRM]. Under C1 the *only* change is the key column matched on
   (`partseq` instead of `tableoid`).
2. **Lazy (gbtree-style):** set `indpartstate='d'`, leave entries as garbage,
   let VACUUM reap them. [PRIOR-ART gbtree §1.3]. The MATCH/DEREFERENCE
   chokepoints (§4) must treat a `'d'` partseq as "not a live conflict."

**ATTACH backfill:** scan the attaching partition's heap, insert
`(user_cols…, partseq)` for every live row, **after** allocating its partseq.
[VERIFIED — ATTACH backfills today; per plan-doc audit / README; RE-CONFIRM]. The
backfill can surface a cross-partition unique violation (Wenjing's open question
#3, [PRIOR-ART Alibaba 2021]) — unchanged by C1; the discriminator swap does not
affect *whether* a duplicate is found, only how the conflicting partition is
resolved (§4 DEREFERENCE).

**Race summary:**
- Blocking ATTACH vs concurrent INSERT: the INSERT's writer-side resolver
  (relid→partseq) and the ATTACH's allocator both serialize on the root lock →
  safe. [ASSUMPTION].
- DETACH vs in-flight uniqueness probe: the `indpartstate` column lets the probe
  see "this partseq is detaching" rather than racing a hard catalog-row delete.
- CONCURRENTLY (CIC / DETACH CONCURRENTLY): **out of scope** (matches gbtree and
  ProgreSQL today). The catalog design does not *preclude* it but does not solve
  the weaker-lock allocation race. [ASSUMPTION].

### 3.4 Dump / restore of the mapping

- The catalog rows are ordinary catalog data; **logical dump** must emit the
  partseq map so the restored index's stored partseqs line up with the catalog.
  But there is a cleaner option: because the root index is **rebuilt from live
  rows** on logical restore (Model A, §3.1), the cleanest contract is **"on
  rebuild, re-derive the map deterministically"** — assign partseq in **partition
  bound order** (or pg_inherits seqno order) during build, so a fresh build and a
  dumped catalog agree without dumping the catalog at all. [ASSUMPTION — requires
  the build to iterate partitions in a stable, dump-reproducible order; confirm
  the partition-descriptor iteration order at the build site].
- **binary upgrade:** preserve the catalog rows verbatim (partseqs unchanged) AND
  the index relfilenode — and **partseq removes the OID-preservation dependency
  entirely**: even if a partition got a different OID, the stored partseqs still
  resolve via the carried-over catalog rows. This is the single biggest win (§6).

---

## 4. The chokepoint map (WRITE / MATCH / DEREFERENCE)

The discriminator is touched in exactly three places. Every other spanning site
(propagation skips, planner hiding, error paths) is discriminator-agnostic and
needs **no change** beyond switching its predicate to `RelationIsSpanning` (§5).

> All `file:line` below are **[VERIFIED file:line — per plan-doc audit; RE-CONFIRM]**
> unless tagged otherwise; tool truncation this session blocked a fresh re-read.

### (a) WRITTEN — where the discriminator value is put into the index tuple
- **Executor insert (hot path):** `ExecInsertSpanningIndexTuples`
  (**[VERIFIED `execIndexing.c:1345`** read this session]), called from
  `nodeModifyTable.c:1251` (INSERT) and `:2393` (UPDATE epilogue). Today it forms
  the index datums and **overrides the trailing column with the partition OID**:
  **[VERIFIED `execIndexing.c:1376–1378`** — `values[se->tableoidKeyPos] =
  ObjectIdGetDatum(partOid); isnull[se->tableoidKeyPos] = false;`], then
  `index_insert` on the root.
  - **C1 change:** at `:1376–1378`, replace `ObjectIdGetDatum(partOid)` with the
    partition's **partseq** (resolved `relid → partseq`, §5). One line, plus the
    cache field rename below.
  - **Signature impact:** none to `index_insert`. The per-statement cache is
    already the home: **[VERIFIED `execnodes.h:783`** — `HTAB
    *es_progresql_partition_cache;`] populated by
    `progresql_build_partition_cache_entry` with a `ProgresqlSpanningEntry`
    carrying `parentRel`, `indexRel`, `indexInfo`, and `tableoidKeyPos`
    (**[VERIFIED `execIndexing.c:1144–1152, 1224`** — `se->tableoidKeyPos =
    se->indexInfo->ii_NumIndexKeyAttrs - 1;`]). C1 adds an `int partseq` field to
    `ProgresqlSpanningEntry` (resolved once when the entry is built) and renames
    `tableoidKeyPos → discriminatorKeyPos`, so the hot path stays O(1) with **zero
    per-row catalog lookups**.
- **Build / backfill / REINDEX:** the spanning-build site sets the index shape and
  appends the trailing column at `indexcmds.c:1189–1200` (today appends
  `TableOidAttributeNumber`), and `BuildSpanningIndexFromPartitions`
  (`indexcmds.c:1348–1349`) / the REINDEX repopulate path scan partitions and
  insert entries.
  - **C1 change:** (1) at `:1189–1200`, the trailing key attno is **no longer**
    `TableOidAttributeNumber`; it becomes a **virtual/expression key column whose
    value is the row's partseq** — see §5 "how the value is produced" and the
    [ASSUMPTION] on opclass. (2) at backfill/REINDEX, allocate/look-up partseq per
    partition (catalog) and write it.

### (b) MATCHED — where an existing entry's discriminator is compared during cleanup
- **VACUUM:** `progresql_vacuum_spanning_indexes()` (**[VERIFIED `vacuumlazy.c:447`
  decl, `:2463` body, `:2489` the `indnuniqatts == 0` skip, `:2620–2621` invoke**
  read this session]). This is the **TID-collision bug site** ([per plan-doc audit
  P2-P3 §1 / P0-3 R2 — confirmed there]): the body calls
  `vac_bulkdel_one_index(&ivinfo, NULL, vacrel->dead_items, …)` with a **NULL
  callback** ([VERIFIED P0-3 §Path C / P2-P3 §P2-3 at `vacuumlazy.c:2504`]), so the
  stock TID-only reaper (`vac_tid_reaped`, pure `(block,offset)` membership) sees
  **only the heap TID**, not the trailing discriminator key — it cannot tell two
  partitions' colliding TIDs apart. Note the **DETACH path already does this
  correctly**: `progresql_clean_spanning_indexes_for_partition`
  (**[VERIFIED `tablecmds.c:2006`, comment `:1991–2003`** read this session]) scans
  for entries whose trailing tableoid key equals `partOid` and sets
  `kill_prior_tuple` — precisely the discriminator-aware match the VACUUM path
  lacks. C1 should mirror that, keyed on `partseq`.
  - **C1 change (this is where C1 *helps* the existing bug):** the cleanup must
    match on **(partseq, t_tid)**, not `t_tid` alone. C1 does **not** by itself
    fix the callback-signature limitation — but it makes the fix cleaner: the
    bulkdelete pass over the spanning index can read the trailing `partseq` key
    column directly from each index tuple and compare it to *the partition being
    vacuumed's* partseq (one catalog lookup, hoisted out of the loop), short-
    circuiting before the TID test. The recommended shape is a **spanning-specific
    bulkdelete** that filters `index_tuple.partseq == this_partition.partseq`
    first, then applies the normal `vac_tid_reaped` TID test. Same shape as the
    P2-P3 plan's "tableoid-aware callback," with `partseq` substituted.
  - **Signature impact:** the spanning bulkdelete needs the *vacuuming
    partition's partseq* in its callback `state` — a single int added to the
    state struct, resolved once via the resolver/catalog. No change to the generic
    `IndexBulkDeleteCallback` ABI.

### (c) DEREFERENCED — where the discriminator is turned back into a partition + heap probe
- **Uniqueness check:** `_bt_check_unique` (`nbtinsert.c`), which limits the
  scan-key comparison to the first `IndexRelationGetNumberOfUniqueAttributes(rel)`
  columns (**[VERIFIED `nbtinsert.c:438–441`** read this session — `nuniqs =
  IndexRelationGetNumberOfUniqueAttributes(rel); ... itup_key->keysz = nuniqs;`]),
  and on a candidate duplicate reads the trailing discriminator key column as an
  OID and opens that partition: **[VERIFIED `nbtinsert.c:595–597`** read this
  session — `Oid child_relid = DatumGetObjectId(oidval); spanChildRel =
  table_open(child_relid, AccessShareLock);`]. The "use the partition that owns the
  new tuple (last key column of itup carries its tableoid)" branch is at
  **[VERIFIED `nbtinsert.c:665–666`]**.
  - **C1 change:** at `:595–597`, instead of `DatumGetObjectId(oidval)` +
    `table_open(child_relid, …)`, read the trailing datum as a `partseq` and call
    the **resolver** (`partseq → Relation`, §5) to get the partition; probe it the
    same way. Everything else (`kill_prior_tuple`/`LP_DEAD` retirement) is
    unchanged. The reduced-keysz logic is **unchanged** because
    `IndexRelationGetNumberOfUniqueAttributes` is OID/partseq-agnostic.
  - **Signature impact:** `_bt_check_unique` already has the index `Relation`
    (hence its `pg_class` OID, the resolver's first key) and reads the trailing
    datum (now `partseq`). It needs **no new parameter** — only the descriptor
    field (§5) to know it is spanning and the resolver to map partseq→Relation.
- **Future Global Index Scan (NOT in scope, noted for the boundary):** if the
  index ever serves reads (P3-1 is currently "hidden from planner"), the scan
  would dereference partseq the same way. C1's resolver is the single seam that a
  future scan would reuse. [ASSUMPTION].

### 4.1 Chokepoint signature-change ledger

| Chokepoint | File:line | Today | C1 minimal change | New param? |
|---|---|---|---|---|
| WRITE (hot) | `execIndexing.c:1345` (via `nodeModifyTable.c:1251/2393`) | put `tableoid` in trailing datum | resolver `relid→partseq`, put `partseq` | No — local + cache field |
| WRITE (build) | `indexcmds.c:1189–1200`, `:1348` | append `TableOidAttributeNumber` | append partseq virtual/expr key col; allocate partseq | No — descriptor + catalog |
| MATCH (vacuum) | `vacuumlazy.c:447/2456–2489/2616` | TID-only callback (buggy) | spanning bulkdelete filters on `partseq` then TID | +1 int in callback `state` |
| DEREF (unique) | `nbtinsert.c:429–441/576–672/213` | `table_open(tableoid)` | resolver `partseq→Relation`, probe | No — descriptor field only |

---

## 5. The SOLID surface — one descriptor, one predicate, one resolver

### 5.1 One descriptor field
**Recommendation: carry the discriminator metadata on `IndexInfo`**, the
descriptor already built per index by `BuildIndexInfo` and already threaded
through build, insert, and uniqueness checking — i.e. through every chokepoint.

- **Field:** `int ii_SpanningKeyAttno;` (the attribute number of the trailing
  discriminator key column within the index), **defaulting to `0` / `InvalidAttrNumber`
  for ordinary indexes.** A non-spanning index has `ii_SpanningKeyAttno == 0`, so
  it flows through the **identical** code path — the discriminator logic is
  guarded by `if (indexInfo->ii_SpanningKeyAttno != 0)`. This is the Liskov-style
  zero-default: ordinary indexes are unaffected because the invalid value *is* the
  ordinary default. [ASSUMPTION — `IndexInfo` reaches all three chokepoints;
  strongly supported by the fact that `ii_NumUniqKeyAtts` (the existing
  spanning field, per plan-doc audit P1-1 §2.3) already does].
- **Why `IndexInfo` over the alternatives:**
  - `BTScanInsert` — too narrow (insert-key descriptor only; not present at vacuum
    or build). Reject as the *carrier* (it can read from IndexInfo-derived state).
  - `IndexScanDesc` — scan-time only; spanning is enforcement-time. Reject.
  - `Relation->rd_index` (the `pg_index` tuple) — this is where the **persistent**
    marker lives (`indnuniqatts`, [VERIFIED — `pg_index.h:36–42`; per plan-doc
    audit P1-1 §2.2]). Keep it as the on-disk truth, but **derive** the in-memory
    `ii_SpanningKeyAttno` from it in `BuildIndexInfo`, so runtime code reads one
    in-memory field, not the catalog tuple, on the hot path.
- **Net:** `indnuniqatts` (catalog, persistent) is the source of truth;
  `IndexInfo.ii_SpanningKeyAttno` (in-memory, derived) is the single descriptor
  field the chokepoints read. ProgreSQL today already has `ii_NumUniqKeyAtts`
  on IndexInfo [per plan-doc audit P1-1 §2.3] — C1 keeps it and adds the attno (or
  reuses the existing one if it already encodes the position).

### 5.2 One predicate — `RelationIsSpanning(rel)`
- **Definition:** `#define RelationIsSpanning(rel) \
  ((rel)->rd_index != NULL && (rel)->rd_index->indnuniqatts > 0)` —
  in `src/include/utils/rel.h`, where the related accessor
  `IndexRelationGetNumberOfUniqueAttributes` **already lives**
  (**[VERIFIED `rel.h:541–545`** read this session — it returns `indnuniqatts`
  when `> 0`, else `indnkeyatts`]). That macro is the existing "spanning-aware"
  seam; `RelationIsSpanning` is its boolean sibling and belongs right beside it.
  There is also a natural `IndexInfoIsSpanning(ii)` =
  `((ii)->ii_SpanningKeyAttno != 0)` for sites that hold an IndexInfo, not a Relation.
- **It replaces the scattered `indnuniqatts > 0` / `progresql_bypass` checks.** The
  full enumeration of scattered runtime `indnuniqatts == 0`/`> 0` tests to
  consolidate (all **[VERIFIED — read this session]** unless noted):
  - `execIndexing.c:1214` — `if (indexRel->rd_index->indnuniqatts == 0) continue;`
    (cache-build skip of non-spanning indexes).
  - `index.c:3851` — `if (iRel->rd_index->indnuniqatts > 0 && …)`
    (REINDEX-repopulate guard).
  - `genam.c:249` — `if (idxrec->indnuniqatts > 0)` (key-description / error
    rendering branch).
  - `vacuumlazy.c:2489` — `if (idxRel->rd_index->indnuniqatts == 0) continue;`
    (VACUUM "only spanning indexes" skip).
  - `tablecmds.c:1287` (`> 0`, skip spanning in some index walk), `:2038`
    (`== 0` continue in DETACH cleanup), `:2154` (`== 0` continue in ATTACH
    backfill).
  - `plancat.c` — `if (index->indnuniqatts > 0) continue;` (hide from planner;
    **[VERIFIED — per plan-doc audit P2-P3 §P3-1 at `plancat.c:294`]**).
  - `nbtinsert.c:438` — via `IndexRelationGetNumberOfUniqueAttributes(rel)`
    (already the macro form — leave as-is or route through the predicate).
  - **Creation-time opt-in (KEEP DISTINCT, do NOT fold into `RelationIsSpanning`):**
    `indexcmds.c:577` decl `bool progresql_bypass`, `:754` the detection predicate
    (`partitioned && …`), consumed at `:981`, `:1189`, `:1263`, `:1269`, `:1348`,
    `:1351` (**[VERIFIED — read this session]**). P1-1 plans to replace the
    INHERITS trigger with an explicit `IndexStmt.isglobal` flag; C1 lands on top of
    that. Keep the two predicates distinct: `stmt->isglobal` = "user asked for
    spanning"; `RelationIsSpanning(rel)` = "this built index *is* spanning."
  - **Action:** every *runtime* test above switches to `RelationIsSpanning(rel)`
    or `IndexInfoIsSpanning(ii)`; only the creation predicate stays an opt-in check.

### 5.3 One resolver — `partseq → Relation`
- **Signature (recommendation):**
  `Relation SpanningResolvePartition(Relation spanningIndex, int partseq);`
  living in `execIndexing.c` (next to the spanning insert) or a new
  `access/index/spanning.c` if the surface grows. Returns the open partition
  Relation (caller does not close — see cache ownership) or raises/None for a
  detaching/unknown partseq.
- **Implementation:** one `(indpartidxid, indpartseq)` syscache/catalog lookup →
  `indpartrelid` → open. **Reuse the executor's existing partition-open cache
  where possible:** the per-statement cache already memoizes
  `get_partition_ancestors → table_open → index_open → BuildIndexInfo` per
  partition [VERIFIED — README "per-statement cache"; RE-CONFIRM]. C1 keys that
  same cache additionally by partseq, so the WRITE path resolves
  `relid→partseq` and the DEREF path resolves `partseq→Relation` against one
  shared structure. A backstop `RelationGetPartitionSeqCache` (a small per-index
  hash on the relcache entry, invalidated on relcache inval) serves the VACUUM and
  uniqueness-probe paths that don't have an EState. [ASSUMPTION — relcache-attached
  cache invalidation is the right lifetime; confirm against ATTACH/DETACH inval].
- **Non-spanning no-op:** the resolver is never called for non-spanning indexes
  because every call site is already behind `RelationIsSpanning`. The default
  descriptor value (`ii_SpanningKeyAttno == 0`) short-circuits before any resolver
  call. (Open/Closed: ordinary indexes never enter spanning code.)

### 5.4 Seam recommendation: btree-internal flag, NOT the AM vtable
- **Evaluated:** should the discriminator be a new `IndexAmRoutine`
  (`amroutine`) capability/method (the Postgres-Pro-gbtree path — a whole new AM),
  or a btree-internal flag (ProgreSQL's current path)?
- **Recommendation: btree-internal flag (status quo seam), expressed as the
  `IndexInfo` descriptor field + `RelationIsSpanning` predicate.** Rationale:
  1. C1 keeps stock nbtree tuple/page/WAL formats (§2); there is no new *access
     method behavior*, only a different value in an existing key column plus a
     side catalog. An AM is the wrong granularity for "one key column means
     something."
  2. A new AM (gbtree) forces duplication of the entire btree surface and breaks
     `USING btree` ergonomics, `amcheck`, opclass defaults, and tooling. Upstream
     reviewers distrust a fork of nbtree more than a guarded field on a descriptor.
  3. The three chokepoints are already btree-internal or executor-internal; none
     of them is a clean AM-vtable boundary (e.g. `_bt_check_unique` is *inside*
     nbtree, not at the `aminsert` seam). Putting the discriminator on
     `IndexAmRoutine` would require new vtable entries that only nbtree implements
     — a vtable with one implementor is a code smell upstream rejects.
- **The one AM-adjacent touch we DO keep:** the existing key-description hook in
  `genam.c`/`indexam.c` (BuildIndexValueDescription, per README) must render
  `partseq` (or resolve it to a partition name) in error messages instead of a raw
  OID — a presentation change, not a vtable change. [VERIFIED — README "Index AM"
  row mentions key-description; RE-CONFIRM].

---

## 6. Bug-evaporation ledger

| Audited problem | Under C1 | Why |
|---|---|---|
| **P0-2 dump/upgrade — stored `tableoid` keys dangle when partition OIDs change** | **EVAPORATES** | partseq is index-local and carried in the catalog map; it does not depend on partition OID stability. Binary upgrade preserves the catalog rows; logical restore re-derives partseq deterministically on rebuild (§3.4). The single most likely silent-corruption vector is removed *by construction*. [ASSUMPTION — contingent on §3.4 deterministic re-derivation; this is the load-bearing claim to test]. |
| **P1-3 / O4 OID-reuse aliasing (Haas's objection)** | **EVAPORATES** | a reused *OID* can never alias a spanning entry because entries are keyed by *partseq*, and partseq is **never reused** (§3.3). The whole "stale entry aliases a recycled OID" failure mode has no precondition. [PRIOR-ART Haas 2019]. |
| **VACUUM TID-collision (P2-P3 §1, latent P0)** | **IMPROVED, not auto-fixed** | C1 does not change the `IndexBulkDeleteCallback` ABI, so the TID-only stock callback still can't disambiguate. BUT the recommended spanning bulkdelete reads `partseq` from the index tuple and filters on it (§4b) — exactly the disambiguation the bug needs, now keyed on a stable int instead of an OID. The fix must still be *written*; C1 makes it correct-by-key. |
| **P0-1 ON CONFLICT arbiter mismatch** | **UNCHANGED** | The arbiter resolves leaf-local (`nodeModifyTable.c:5087`) and never sees the root spanning index [per plan-doc audit P0-1 §2]. That is orthogonal to the discriminator — C1 changes what's *in* the index, not *whether the arbiter consults it*. P0-1 needs its own fix regardless. |
| **P1-1 implicit INHERITS opt-in** | **UNCHANGED / independent** | Opt-in syntax is a separate workstream; C1 lands on top of `IndexStmt.isglobal`. |
| **P0-3 crash/WAL of multi-step DDL** | **SLIGHTLY IMPROVED** | partseq allocation is a catalog insert in the same xact as the partition-join — ordinary catalog WAL, atomic with the DDL. Removes the "crash between heap-drop and entry-cleanup leaves an OID-aliasable orphan" sub-hazard (orphan can't alias since partseq isn't reused). The bulk DDL operations still need the P0-3 review. |
| **P0-4 concurrency / lock ordering of the cross-partition probe** | **UNCHANGED** | The probe still opens another relation mid-insert; C1 changes how it *finds* that relation (resolver vs raw OID), not the lock ordering. P0-4 isolation specs still needed. |
| **DETACH cost is O(rows)** | **UNCHANGED (semantic)** | Same trade as today/gbtree; C1 just keys the delete on partseq. The lazy `indpartstate='d'` option (§3.3) is *newly available* under the catalog design. |

### 6.1 NEW risks introduced by C1

- **partseq allocation race under CONCURRENTLY** — blocking path is safe (root
  lock serializes max+1); CIC/DETACH CONCURRENTLY would need a real allocator
  (sequence or advisory). Out of scope but must be documented as a precondition.
  [ASSUMPTION].
- **Catalog bloat** — one `pg_index_partition` row per `(index, partition)`. For
  a tree with P partitions and K spanning indexes, P×K rows — small (thousands),
  but DETACH-heavy workloads that never REINDEX accumulate `indpartstate='d'`
  tombstones until a REINDEX renumbers from 1. Mitigation: VACUUM/REINDEX reclaims;
  document the int32 partseq ceiling (2^31 attaches/index before REINDEX). [ASSUMPTION].
- **amcheck** — amcheck must learn that the trailing key column is a partseq and
  (optionally) cross-check each distinct partseq against `pg_index_partition`. New
  verification surface; without it, a corrupt map is undetectable by amcheck.
  [ASSUMPTION].
- **On-disk migration** — every existing tableoid-keyed entry must become a
  partseq-keyed entry (§7). This is the chief one-time cost.
- **Resolver cache invalidation** — the new relcache-attached partseq cache must
  invalidate correctly on ATTACH/DETACH/REINDEX; a stale cache could probe the
  wrong partition. Mirrors existing relcache inval discipline but is new code.
  [ASSUMPTION].

---

## 7. Migration: tableoid → partseq on existing on-disk indexes

- **The root index has no storage of its own beyond its nbtree relfilenode, and
  that nbtree is populated by scanning partitions** (Model A, [VERIFIED — `index.c`
  build leaves root empty, REINDEX repopulates from all partitions; per plan-doc
  audit P0-2 §3]). The discriminator value lives in *every leaf index tuple* as
  the trailing key column. Changing it from OID to partseq therefore requires
  rewriting every spanning index tuple.
- **Recommendation: REINDEX-only migration.** Because the trailing key value is
  derived during build (not copied from a user column), a `REINDEX` of the
  spanning root: (1) allocates partseqs (populating `pg_index_partition` in
  partition-bound order), (2) rebuilds the nbtree with `partseq` in the trailing
  column. No in-place tuple surgery, no bespoke upgrade code. This reuses the
  existing REINDEX-repopulate path — the *only* new logic is "fill the catalog +
  emit partseq instead of tableoid," which is the same change as the build
  chokepoint (§4a). [ASSUMPTION — REINDEX repopulate path is the build path; per
  plan-doc audit it is].
- **In-place upgrade is NOT recommended:** scanning the nbtree to overwrite each
  trailing datum in place would need a new WAL-logged rewrite op and is strictly
  more code than REINDEX for no benefit (REINDEX already exists and is the
  documented spanning-repair hammer, [per plan-doc audit P1-1 §6.3]).
- **Cost characterization:** REINDEX of a spanning root is O(total live rows
  across all partitions) heap scan + sort + nbtree build — i.e. the same cost as
  the original spanning build. For a research fork with no production data this is
  a non-issue; for any real cluster it is a one-time, planned, offline-ish
  operation (REINDEX takes a strong lock; REINDEX CONCURRENTLY for spanning is out
  of scope, §8). Since this is a research fork, **a hard "REINDEX all spanning
  roots after applying C1" upgrade note is acceptable** — no online-migration
  machinery required.

---

## 8. Step-by-step implementation outline

> Sequence assumes P1-1's explicit `IndexStmt.isglobal` opt-in is already in place
> or lands first; C1 is independent of the opt-in *spelling*.

0. **Re-confirm every `file:line` in §4/§5** against the tree (truncation blocked a
   fresh read this session). ~0.5 d.
1. **Catalog:** add `pg_index_partition` (`.h` + `.dat`/genbki), its two indexes,
   syscache entries, catversion bump. ~2 d.
2. **Predicate + descriptor:** add `RelationIsSpanning`/`IndexInfoIsSpanning`;
   add `ii_SpanningKeyAttno` to `IndexInfo`, derive it in `BuildIndexInfo` from
   `indnuniqatts`; **mechanically replace** the scattered runtime `indnuniqatts > 0`
   checks (§5.2 list) with the predicate. Pure refactor, no behavior change yet. ~2 d.
3. **Resolver:** implement `SpanningResolvePartition` (+ `relid→partseq` reverse)
   over the catalog; wire it into the existing per-statement partition cache and
   add the relcache-attached backstop cache + invalidation. ~3 d.
4. **WRITE chokepoint:** allocate partseq at build/ATTACH (catalog insert);
   emit partseq in the trailing key column at build (`indexcmds.c:1189–1200`,
   `:1348`) and in the executor hot path (`execIndexing.c:1345`). Sort opclass for
   the trailing column = int4/oid. ~3 d.
5. **DEREF chokepoint:** `_bt_check_unique` (`nbtinsert.c:576–672`) calls the
   resolver instead of `table_open(tableoid)`. ~1 d.
6. **MATCH chokepoint:** spanning bulkdelete in `vacuumlazy.c` filters on the
   tuple's partseq vs the vacuuming partition's partseq (fixes the TID-collision
   bug as a side effect). ~2 d.
7. **DETACH/DROP:** key the entry-delete on partseq; implement `indpartstate`
   lifecycle (eager delete now; lazy as an option). ~2 d.
8. **Key-description / errors:** `genam.c`/`indexam.c` render partseq→partition
   name in violation messages. ~1 d.
9. **Migration:** make REINDEX populate the catalog + emit partseq (largely free
   from step 4); write the upgrade note. ~1 d.
10. **dump/restore:** binary-upgrade preserves catalog rows; logical dump relies on
    deterministic re-derivation (§3.4) — coordinate with the P0-2 workstream. ~2 d.
11. **amcheck:** teach it the trailing column is a partseq + optional map
    cross-check. ~2 d (can defer).

---

## 9. Where C1 stops and "B" begins (the boundary, explicitly)

- **C1 (this doc):** discriminator is a **key column value** (partseq) of an
  ordinary nbtree tuple; the index entry's `t_tid` still means "heap TID within
  the partition identified by the trailing key column." No tuple/page/WAL/`t_tid`
  format change. Resolution is `partseq → catalog → partition`, then a normal TID
  probe. amcheck's structural checks are unchanged (it just sorts on an int).
- **B (out of scope):** discriminator moves **into the pointer** — an *extended
  TID* `(partition-id, block, offset)` replacing the 6-byte `ItemPointerData` in
  index tuples, so the index natively addresses cross-partition heaps without a
  trailing key column. B changes `ItemPointerData`/nbtree tuple layout, every AM
  and tool that reads `t_tid`, posting-list/dedup encoding, amcheck, and on-disk
  compatibility cluster-wide. B *might* later enable a true Global Index Scan more
  cheaply, but it is a far larger, on-disk-breaking change.
- **The crisp line:** *C1 never changes the meaning or width of `t_tid`; it only
  changes which value sits in the existing trailing key column and adds a side
  catalog. The moment a design alters `ItemPointerData` or the index tuple's
  pointer field, it is B, not C1.*

---

## 10. Test plan

- **Unit / regression (`progresql`, `progresql_ddl`):**
  - partseq allocation: ATTACH N partitions → partseqs 1..N in bound order;
    DETACH middle one → its partseq is never re-handed to a later ATTACH.
  - cross-partition uniqueness still enforced after the discriminator swap
    (the core feature must be unchanged).
  - **OID-reuse test (the evaporation proof):** force OID recycling (DROP/CREATE
    cycles or low OID counter in a TAP test), attach a new partition that gets a
    *recycled* OID, insert a key formerly used by a dropped partition → assert **no
    false conflict** (this test should be *impossible to fail* under C1; under the
    old tableoid design it could). [PRIOR-ART Haas 2019 objection, now testable].
  - **VACUUM TID-collision test:** two partitions with live rows at the same heap
    TID and the same/different user keys; vacuum one → assert the other's spanning
    entry survives (the P2-P3 §1 bug, now keyed by partseq).
- **Dump/upgrade (coordinate with P0-2):**
  - `pg_dump | psql` round-trip: rebuilt index's partseqs match the catalog;
    cross-partition uniqueness re-checks pass.
  - `pg_upgrade` TAP: **deliberately do NOT preserve a partition's OID** (or verify
    behavior is OID-independent) and assert uniqueness still holds — the test that
    would have caught P0-2 and now must pass.
- **Migration:** REINDEX an old tableoid-keyed spanning root (if any exist in a
  fixture) → verify catalog populated + partseq-keyed entries + uniqueness intact.
- **Concurrency (isolation specs, partial — full is P0-4):** concurrent ATTACH vs
  INSERT resolving the same partition; DETACH vs in-flight uniqueness probe
  (`indpartstate='d'` visibility).
- **amcheck:** corrupt a `pg_index_partition` row → amcheck flags map/index
  divergence (once amcheck support lands).

---

## 11. Risks / unknowns

- **[ASSUMPTION, load-bearing] Deterministic partseq re-derivation on rebuild**
  (§3.4) — the whole logical-dump story rests on build assigning partseq in a
  dump-reproducible order. Must confirm the partition-descriptor iteration order
  at the build site is stable and equals dump order.
- **[VERIFIED — per plan-doc audit; RE-CONFIRM] every §4/§5 `file:line`** — not
  re-read this session (tool truncation). Re-grep before editing.
- **[ASSUMPTION] opclass for the trailing key column** — switching the trailing
  column's type/opclass from oid to int4 (or keeping oid-shaped) must be wired at
  the build site; trivial but must be correct for sort order.
- **[ASSUMPTION] resolver cache invalidation lifetime** — relcache-attached cache
  vs syscache; ATTACH/DETACH/REINDEX inval must be exhaustive.
- **CONCURRENTLY** — allocation race unsolved (out of scope, matches gbtree).
- **Catalog bloat / partseq int32 ceiling** under DETACH-heavy churn without
  REINDEX (§6.1).
- **amcheck coverage** — without it, a corrupt map is silent.

---

## 12. Rough effort estimate

| Workstream | Estimate |
|---|---|
| Re-confirm citations | 0.5 d |
| Catalog `pg_index_partition` + caches | 2 d |
| Predicate + descriptor refactor (consolidate scattered checks) | 2 d |
| Resolver + cache wiring/inval | 3 d |
| WRITE chokepoint (build + ATTACH + executor) | 3 d |
| DEREF chokepoint (`_bt_check_unique`) | 1 d |
| MATCH chokepoint (spanning bulkdelete; fixes TID-collision) | 2 d |
| DETACH/DROP + `indpartstate` lifecycle | 2 d |
| Key-description / error rendering | 1 d |
| Migration (REINDEX-only) + upgrade note | 1 d |
| dump/restore (with P0-2) | 2 d |
| amcheck (deferrable) | 2 d |
| Tests (regression + TAP OID-reuse + vacuum + dump) | 4 d |
| **Total** | **~25–27 dev-days** (≈ 23 d excluding deferrable amcheck) |

Sequencing: §8 order. C1 is independent of the P1-1 opt-in *spelling* but should
land after the explicit `IndexStmt.isglobal` flag exists, and should be
coordinated with the P0-2 dump/upgrade workstream (shared resolver + catalog).
