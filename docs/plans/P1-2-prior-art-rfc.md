# P1-2 — Prior Art & RFC Framing for Spanning (Global) Indexes

**Document type:** Planning / research. Companion to `README.md` and
`PRODUCTION_READINESS.md` (answers the open `P1-2` item in the latter).
**Scope:** Engage the documented prior art for "global indexes on partitioned
tables" so a future pgsql-hackers proposal is not dismissed for ignoring the
history. Catalog the canonical objections, map them to ProgreSQL's current
approach, and draft an RFC framing.
**Status:** Research artifact, not a final design. Last updated 2026-05-30.

---

## 0. Source-honesty preamble (read this first)

Two classes of source appear below:

- **[FETCH-VERIFIED]** — I retrieved the page/message this session and confirmed
  it exists and is on-topic. For mailing-list items I confirmed (where the
  archive rendered) subject / author / approximate date. The summary reflects
  what the page actually says.
- **[SEARCH-SURFACED]** — the URL was returned by a web search with a plausible
  title, but I could **not** independently open it this session (archive
  pagination, login wall, or cache miss). The URL is almost certainly real, but
  treat the *summary* as second-hand until opened by hand.

What I did **not** do: invent message-ids, attributions, or dates. Where a
detail is uncertain it is flagged inline. The commitfest site is behind an auth
redirect for the fetch tool, so **commitfest IDs below are SEARCH-SURFACED only**
and must be re-pulled from `https://commitfest.postgresql.org/` by hand.

**Tally:** 11 FETCH-VERIFIED sources, ~6 additional SEARCH-SURFACED URLs listed
for follow-up. See the executive tally in §5. (Note: the web archive opened far
better than a first pass suggested — most message-ids resolved to real subjects,
authors, and dates, which are reproduced below.)

---

## 1. Annotated bibliography

### 1A. The official statement of the gap (FETCH-VERIFIED)

#### V1 — PostgreSQL 18 docs, "Table Partitioning → Limitations" (§5.12.2.3)
- **URL:** https://www.postgresql.org/docs/18/ddl-partitioning.html
- **Status:** FETCH-VERIFIED (quoted verbatim from the live page).
- **Verbatim quote:**
  > "To create a unique or primary key constraint on a partitioned table, the
  > partition keys must not include any expressions or function calls and the
  > constraint's columns must include all of the partition key columns. This
  > limitation exists because the individual indexes making up the constraint can
  > only directly enforce uniqueness within their own partitions; therefore, the
  > partition structure itself must guarantee that there are not duplicates in
  > different partitions."
- **Important correction to a common misconception:** the docs do **NOT** contain
  a sentence saying "there is no support for global indexes." I checked
  specifically. The limitation is framed entirely as "constraint columns must
  include the partition key" + the *reason* ("individual indexes ... only ...
  within their own partitions"). Do not quote a non-existent "no global indexes"
  sentence in a public posting — quote the paragraph above instead.
- **Why it matters:** the single most defensible citation to open an RFC with.
  Frames the gap in the project's own canonical words; the phrase "individual
  indexes ... can only directly enforce uniqueness within their own partitions"
  is *exactly* what a spanning index changes.

### 1B. PostgreSQL mailing-list prior art (the graveyard)

#### V2 — "Proposal: Global Index" — Ibrar Ahmed, 2019-10-30 [FETCH-VERIFIED]
- **URL:** https://www.postgresql.org/message-id/CALtqXTcurqy1PKXzP9XO=ofLLA5wBSo77BnUnYVEZpmcA3V0ag@mail.gmail.com
- **Postgres Pro mirror (thread view):** https://postgrespro.com/list/thread-id/2464140
- **Status:** FETCH-VERIFIED. Author **Ibrar Ahmed** (`ibrar.ahmad@gmail.com`),
  **2019-10-30**; thread ran through Jan 2021. Collaborators named: Hamid Akhtar,
  Robert Haas. Same proposal as the Percona blog (V-Percona below).
- **What it is:** The canonical origin thread. Single index on the parent (which
  has no storage) that "accumulates" entries from all partitions; proposed syntax
  `CREATE INDEX idx ON parent (cols) [GLOBAL | LOCAL]`, LOCAL as default. Goals:
  cross-partition uniqueness **and** read-performance for multi-partition queries.
  In-thread it explicitly named the **CTID problem** (page+offset is insufficient;
  must include heap/partition identity), optimizer global-vs-local selection,
  write amplification, and vacuum.
- **Why it stalled — the two killshot objections (FETCH-VERIFIED from the thread):**
  - **Robert Haas — vacuum amplification:** vacuuming a 1 TB partitioned table
    with a 140 GB global index would require on the order of **141 TB of I/O**
    to vacuum all partitions, vs ~1.125 TB with local indexes. This is *the*
    canonical vacuum objection in its original, brutal form.
  - **Tom Lane — "defeats the purpose":** a global index eliminates the core
    partitioning benefits (cheap DROP/DETACH of old partitions, dodging the
    single-relation size limit); he questioned whether the benefits of
    partitioning survive at all.
  - **Robert Haas — OID reuse:** reusing a table OID after DETACH could leave
    stale global-index entries pointing at unrelated data, breaking uniqueness —
    directly relevant to ProgreSQL's tableoid bet (O4).
  A related reply (`78E4E097-...@alibaba-inc.com`) is V-Alibaba below — it
  matters enough to get its own entry.

#### V3 — "Patch: Global Unique Index" — Cary Huang (HighGo), 2022-11-17 [FETCH-VERIFIED]
- **URL:** https://www.postgresql.org/message-id/184879c5306.12490ea581628934.7312528450011769010@highgo.ca
- **Companion blog posts:**
  - https://www.highgo.ca/2022/10/14/global-index-a-different-approach/ — **David
    Zhang, 2022-10-14** [FETCH-VERIFIED]
  - https://www.highgo.ca/2022/10/28/cross-partition-uniqueness-guarantee-with-global-unique-index/ [SEARCH-SURFACED]
- **Status:** FETCH-VERIFIED. Patch author **Cary Huang** (`cary.huang@highgo.ca`),
  **2022-11-17**, pgsql-hackers.
- **Approach (CONTRAST with ProgreSQL — this is the "recurse-and-check" camp):**
  HighGo's "global unique index" is **still a partitioned index**. `CREATE UNIQUE
  INDEX g ON idxpart(bid) GLOBAL;` recursively creates a child index on each
  partition (new relkind `RELKIND_GLOBAL_INDEX`, `g`); global uniqueness is
  enforced by **deferring the sort until all child partitions are scanned** (one
  final sort/merge over all partitions) instead of per-partition, plus a
  cross-partition check on INSERT/UPDATE and at ATTACH. Supports serial **and
  parallel** build/ATTACH. The blog (David Zhang) explicitly frames the
  motivation for keeping per-partition storage: avoid the BlockNumber-based
  single-relation size limit and preserve cheap add/drop of partitions — i.e. a
  direct answer to Tom Lane's V2 "defeats the purpose" objection. Sub-partition
  uniqueness explicitly not supported. Known TODOs: `ON ONLY` error handling,
  blocking direct global-index creation on a child.
- **Why it matters:** The closest landed-quality prior art and the **most
  important contrast for the RFC**. HighGo keeps storage *distributed* (dodging
  size limit + preserving cheap DETACH) at the cost of an all-partition final
  sort and a recurse-on-ATTACH check. ProgreSQL's single-root B-tree keyed on
  `tableoid` makes the *opposite* trade (one physical index, simpler addressing,
  but the size-limit and DETACH-cost objections land squarely). The RFC must name
  this fork in the design tree explicitly.

#### V4 — "Proposal: Global Index for PostgreSQL" (Dilip Kumar, 2025) [FETCH-VERIFIED]
- **URL:** https://www.postgresql.org/message-id/CAFiTN-uyec_y2QS2whUam8Rp1M+PPGuP0Zz45uX_U6mhQ8mtRg@mail.gmail.com
- **Status:** FETCH-VERIFIED. Started 2025-06-06 by Dilip Kumar
  (`dilipbalaut@gmail.com`). Participants visible include Wenhui Qiu, Nikita
  Malakhov, Bruce Momjian, Masahiko Sawada, Amit Langote.
- **Approach (CLOSEST to ProgreSQL):** A genuinely single, non-partitioned index
  on the parent, with an **added internal partition-identifier key column** (a
  4-byte int, allocated per leaf partition) and a **new catalog
  `pg_index_partitions`** mapping `(partition-id, global-index-oid) → relation
  oid`. This is the *same core idea* as ProgreSQL's `tableoid` trailing key —
  except Dilip uses a dedicated small partition-id + side catalog rather than the
  existing `tableoid`.
- **The vacuum discussion (CRITICAL for the RFC):** This thread is where the
  vacuum cost was quantified: with global indexes naively vacuumed per-partition,
  a 1000-partition / 50M-tuple VACUUM went from ~15s to ~45min (~300x). The fix
  discussed: vacuum each partition + its local indexes first, **defer** the
  global-index pass, accumulate dead TIDs, then vacuum the global index **once**
  at the end — bringing it to ~40s. **This is the canonical vacuum objection and
  its accepted-shape answer. ProgreSQL's RFC must engage exactly this.**
- **Why it matters:** This is the live (2025) state of the art on hackers and the
  most direct precedent; the partition-id-vs-tableoid choice is a debate
  ProgreSQL is implicitly taking a side on and must defend.

#### V4b — Alibaba POC using `tableoid` (Wenjing Zeng, 2021) [FETCH-VERIFIED] ★ NEAREST PRIOR ART
- **URL:** https://www.postgresql.org/message-id/78E4E097-EEFE-4755-AAE4-97B60AD53B5B@alibaba-inc.com
  (a reply within the V2 "Proposal: Global Index" thread)
- **Status:** FETCH-VERIFIED. Author **曾文旌 / Wenjing Zeng** (Alibaba),
  **2021-01-07**.
- **Why this is the single most important entry for ProgreSQL:** Wenjing's POC (on
  **PG13**) defines the global index with the **`INCLUDE` keyword carrying the
  partitioned table's `tableoid`** — i.e. **the exact discriminator ProgreSQL
  uses.** This is the closest prior art to ProgreSQL's design and is NOT the
  partition-id/side-catalog camp (V4). It demonstrated:
  - DML maintenance (INSERT/UPDATE keep the global index correct);
  - a real **Global Index Scan** plan: executor reads `tableoid` from the index
    tuple and checks visibility in the right partition (ProgreSQL deliberately
    does *not* do this — it hides the index from the planner, PR P3-1);
  - **per-partition VACUUM cleaning its own garbage in the global index** — the
    same mechanism ProgreSQL has (PR P2-3). (Note: this is the *naive* shape that
    V4/V9 later showed is catastrophic at scale; see O1.)
- **The four hard DDL questions Wenjing flagged for the community (verbatim shape):**
  1. concurrent build/rebuild; 2. **DETACH** — should a flag let VACUUM clean the
  detached partition's index data lazily?; 3. **ATTACH** — data-laden partition
  risks unique conflicts; clean gradually or invalidate the whole index?;
  4. **TRUNCATE** — separate heap/index cleanup transactions (losing rollback)?
  **These are exactly ProgreSQL's open DDL questions (O2, O7).**
- **RFC consequence:** ProgreSQL must position itself relative to Wenjing's POC
  specifically — "we revive the tableoid approach from the 2021 Alibaba POC,
  restricted to *enforcement-only* (no Global Index Scan), and here is our answer
  to its four open DDL questions." Not naming this would be the most glaring
  prior-art omission of all.

#### V5 — "Index over all partitions (aka global index)?" — Stefan Keller, 2012 [FETCH-VERIFIED]
- **URL:** https://www.postgresql.org/message-id/CAFcOn293JWp5pV8Xio4s4iaoVK+zH=Ac8BtmurfkoFNaFujyNg@mail.gmail.com
- **Status:** FETCH-VERIFIED. Author **Stefan Keller**, **2012-10-14**,
  pgsql-**performance** (reply from Jeff Janes). 250M rows / 250 partitions; argues
  a global index is ~180x faster for PK lookups than iterating partition indexes.
- **What it is:** Evidence the demand predates declarative partitioning entirely
  (2012). Useful as a "this has been asked for 13+ years" data point. Read-perf
  framing, so secondary for ProgreSQL's enforcement-only scope.

#### V5b — "Re: PostgreSQL 11 global index" — Mariel Cherkassky, 2018 [FETCH-VERIFIED]
- **URL:** https://www.postgresql.org/message-id/CA%2Bt6e1=eJZotPr_cDh-b8PoYq3XXT3B1zh1-kyr_6AoH11GEkA@mail.gmail.com
- **Status:** FETCH-VERIFIED. **2018-08-06**. User needs cross-partition
  uniqueness on a non-partition-key column; answer: PG11 can't, workaround is
  pg_partman's after-the-fact Python validator. Demand + workaround-pain evidence.

#### V6 — PG11 local partitioned indexes / unique constraints (Álvaro Herrera) [FETCH-VERIFIED]
- **URLs:**
  - https://www.postgresql.org/message-id/20171229175930.3aew7lzwd5w6m2x6@alvherre.pgsql (Local indexes for partitioned table)
  - Thread "Allow UNIQUE indexes on partitioned tables":
    https://www.postgrespro.com/list/thread-id/2372385 [SEARCH-SURFACED]
  - Patch attachment seen in archive:
    `.../message-id/attachment/58039/v4-0001-allow-indexes-on-partitioned-tables-to-be-unique.patch`
    [SEARCH-SURFACED]
- **Status:** FETCH-VERIFIED for the local-indexes thread (Álvaro Herrera, late
  2017).
- **What it is:** The work that **landed in PG11**: local partitioned indexes,
  and UNIQUE/PK on partitioned tables *requiring the partition key*. Reviewers
  included Peter Eisentraut, Robert Haas, Amit Langote, Jesper Pedersen, Simon
  Riggs, David Rowley.
- **Why it matters:** This is the line ProgreSQL crosses. It shows the community
  **shipped the local-only version on purpose** and consciously left the global
  case for later. The RFC must show it understands *why they stopped here*, not
  treat the boundary as arbitrary.

#### V7 — PostgreSQL wiki, Table_partitioning "Future improvements" [FETCH-VERIFIED]
- **URL:** https://wiki.postgresql.org/wiki/Table_partitioning
- **Status:** FETCH-VERIFIED. Quoted bullets (under Future improvements):
  > "Unique constraint over multiple partitions, when each partition has a unique
  > index on set/superset of partition keys"

  and
  > "Unique constraints over multiple partitions in the general case (typically
  > called as 'global index')."
- **Why it matters:** The community's **own roadmap** names this exact feature
  ("global index") as a known, wanted gap. Strong "this is wanted, not fringe"
  citation.

#### V8 — TODO wiki, Inheritance section [FETCH-VERIFIED]
- **URL:** https://wiki.postgresql.org/wiki/Todo
- **Status:** FETCH-VERIFIED. Item: *"Allow unique indexes across inherited
  tables (requires multi-table indexes)"*, annotated *"Postgres 11 allows unique
  indexes across partitions if the partition key is part of the index."*
- **Why it matters:** Confirms the gap is a recognized TODO and that the
  enabling primitive is understood to be "multi-table indexes" — i.e. exactly
  the thing a spanning index is.

### 1C. Production precedent (it's been done — at a cost)

#### V9 — Postgres Pro `pgpro_gbtree` global index [FETCH-VERIFIED]
- **URLs:**
  - Docs: https://postgrespro.com/docs/enterprise/current/pgpro-gbtree
  - Engineering writeup (Habr): https://habr.com/en/companies/postgrespro/articles/948428/
- **Status:** FETCH-VERIFIED (both).
- **What it is:** A *shipping* (Postgres Pro Enterprise, released 17.5.1 as
  experimental) global index via a new **`gbtree` access method** based on
  standard B-tree but adapted for partitioned tables. Single non-partitioned
  index; lifts the "unique must include partition key" restriction.
- **Stated limitations (directly relevant to ProgreSQL's gaps):**
  - The partitioned table **must have a primary key**.
  - **`CONCURRENTLY` is not supported** for creating the global index
    (mirrors ProgreSQL PR §6.2 — CIC missing).
  - Single index ⇒ standard ~32 TB index size limit applies.
- **Vacuum / DETACH behavior (FETCH-VERIFIED from the docs):** Postgres Pro's
  gbtree global indexes are **not rebuilt on DELETE or DETACH PARTITION** —
  detached-partition entries are left as **garbage to be cleaned by VACUUM**, and
  notably **autovacuum is not (yet) supported** for them. The Habr writeup
  (slonik_pg, 2025-09-22) adds that gbtree entries **don't use MVCC** (no tuple
  versions stored) so they bloat far less than a normal B-tree. (The "45 min →
  40 s" defer-to-once-at-end figure is from V4's thread, not this article;
  attribute it there.) `CLUSTER`, `ON CONFLICT`, `WHERE CURRENT OF`, expressions,
  `INCLUDE`, predicates, FKs are all **unsupported** — a useful realistic scope
  ceiling for ProgreSQL's own claims.
- **Why it matters:** **Existence proof** that the feature is implementable and
  shippable, plus an independent confirmation of *which* problems are hard
  (vacuum, CONCURRENTLY, DETACH cost, PK requirement). The RFC should cite this
  as "this has been done downstream; here's how our in-core-shaped approach
  differs."

### 1D. Comparative / ecosystem context (SEARCH-SURFACED unless noted)

- **Percona, "Proposal for Global Indexes in PostgreSQL" (2019-11-20)** —
  https://www.percona.com/blog/2019/11/20/proposal-for-global-indexes-in-postgresql/
  [SEARCH-SURFACED]. Public writeup of the GLOBAL/LOCAL-syntax, parent-has-no-
  storage design; useful for the read-performance motivation and the syntax
  precedent.
- **AWS, "Migrate Oracle global unique indexes ... to RDS/Aurora PostgreSQL"** —
  https://aws.amazon.com/blogs/database/migrate-oracle-global-unique-indexes-in-partitioned-tables-to-amazon-rds-for-postgresql-and-amazon-aurora-postgresql/
  [SEARCH-SURFACED]. Documents Oracle GLOBAL-vs-LOCAL semantics and the
  migration pain (PG has no equivalent), i.e. the cross-vendor demand signal and
  the Oracle cost model (UPDATE GLOBAL INDEXES / UNUSABLE on partition DDL).
- **PolarDB for PostgreSQL global index** —
  https://www.alibabacloud.com/help/en/polardb/polardb-for-oracle/global-index-overview
  [SEARCH-SURFACED]. Another shipping implementation in a PG-compatible engine.
- **pgEdge, "Enforcing Constraints Across Postgres Partitions"** —
  https://www.pgedge.com/blog/enforcing-constraints-across-postgres-partitions
  [SEARCH-SURFACED]. The trigger / dedup-table workaround landscape — what users
  do *today* without the feature (the pain the README's "shadow table, triggers"
  line refers to).
- **YugabyteDB comparison** —
  https://dev.to/yugabyte/global-unique-constraint-on-a-partitioned-table-in-postgresql-and-yugabytedb-4nh6
  [SEARCH-SURFACED]. PG-lacks-it vs Yugabyte-has-it framing.
- **HN: "PostgreSQL 11 Partitioning Improvements"** —
  https://news.ycombinator.com/item?id=17119785 [SEARCH-SURFACED]. Community
  reaction context around the PG11 boundary.

### 1E. The one structural fact behind every objection (technical, not a citation)
- **Heap TIDs are relation-local.** A `t_tid` (`ItemPointerData`) is unique only
  within one relation's storage. This is *the* reason a single physical index
  cannot natively point into multiple partition heaps, and the reason every
  serious proposal adds a discriminator (HighGo: recurse to child indexes;
  Dilip/Postgres Pro: a partition-id; **ProgreSQL: the existing `tableoid`**).
  Corroborated by ProgreSQL's own `README.md` ("Why tableoid?") and
  `src/include/storage/itemptr.h`. Lead the technical section with this; it is
  the crux every reviewer already knows.

---

## 2. Objection → Response matrix

Canonical objections raised across V2–V4, V6, V9, mapped to ProgreSQL's
*current* posture. "Answer" draws on `README.md` and `PRODUCTION_READINESS.md`
(PR); "Gap" is where the PoC falls short and the RFC must say so.

| # | Objection (community, with source) | ProgreSQL's current answer | Honest gap / status |
|---|---|---|---|
| O1 | **Vacuum across partitions** — naive per-partition vacuum of one shared index is catastrophic (V4: 15s→45min @1000 parts; V9 corroborates). Accepted-shape fix: defer the global pass, accumulate dead TIDs, vacuum the global index once at end. | ProgreSQL keys entries `(user_cols…, tableoid)` and has `progresql_vacuum_spanning_indexes()` run during a partition's vacuum (PR §P2-3, `vacuumlazy.c:447`). tableoid attribution makes "whose dead entries" well-defined. | **PARTIAL/RISK (PR P2-3).** Whether it is a targeted bulk-delete or a heavier repopulate is **unverified**; it currently runs **per partition**, i.e. the exact pattern V4/V9 call catastrophic — the "defer to once-at-end" optimization is **not** implemented. This is the #1 thing the RFC must lead with and the #1 thing to fix. |
| O2 | **DETACH / maintenance cost** — purging a detached partition's entries from a shared index is O(rows); Oracle's UPDATE GLOBAL INDEXES penalty is the cautionary tale (V9 framing: "15-min job → 9-hr downtime"). | DETACH/DROP removes only the departing partition's entries, keyed by `tableoid` (README "DDL lifecycle"; PR implies DONE for blocking path). | **RISK.** O(rows) cost is inherent and must be **disclosed as a semantic, not hidden**. DETACH **CONCURRENTLY** not handled. Frame as the conscious trade vs. local indexes (same trade Oracle/Postgres Pro make). |
| O3 | **Planner integration** — a parent-level index sits outside the per-partition model; pruning/costing/bitmap assume local indexes (V2 raised read-perf as a *goal*, which makes planner integration load-bearing there). | ProgreSQL **sidesteps** this: spanning indexes are **hidden from the planner** (`plancat.c`); they are enforcement-only, never used to accelerate reads (PR P3-1). | **SCOPED-OUT (defensible).** Cleaner than V2's ambition but means ProgreSQL does *not* deliver the read-performance half of the classic "global index" pitch. RFC must state this scope explicitly so reviewers don't expect query acceleration. |
| O4 | **OID / discriminator dependence** — Robert Haas (V2) warned that reusing a table OID after DETACH leaves stale entries pointing at unrelated data, breaking uniqueness. V4 (Dilip) chose a *new* 4-byte partition-id + `pg_index_partitions` catalog **specifically to avoid leaning on raw OIDs / handle detach-reattach + OID wraparound**. The 2021 Alibaba POC (V4b), by contrast, used `tableoid` — same as ProgreSQL. | ProgreSQL reuses the existing **`tableoid`** (system attno -7) as the trailing key; `indnuniqatts` records the unique-domain width (README "index shape"; PR P1-3). | **OPEN — THE key design debate, and ProgreSQL is on the contested side.** Camp C (tableoid: V4b + ProgreSQL) vs Camp B (synthetic id: V4 + Postgres Pro). Must directly answer Haas's OID-reuse-after-DETACH objection and dump/restore portability (O6). Single most likely point of pushback. |
| O5 | **Locking / lock ordering / deadlocks** — maintaining one index touching many partitions; the cross-partition uniqueness probe opens *another* relation mid-insert (ATTACH/DETACH vs DML races). | Blocking CREATE INDEX locks root + all partitions; the unique check opens the conflicting partition for a liveness probe (README; PR P0-4 `nbtinsert.c`). | **RISK (PR P0-4).** Lock ordering not rigorously analyzed; **no isolation specs**. Flag as needs-review. |
| O6 | **pg_dump / pg_upgrade** — must round-trip the index and survive OID changes. Acute for ProgreSQL because `tableoid` is *stored as a key* (O4). | New catalog flag carries the property; writes go through normal WAL-logged nbtree path (README; PR P0-2/P0-3). | **MISSING / P0 (PR P0-2, P2-2).** pg_dump has no spanning code; whether `pg_get_indexdef()` round-trips is unchecked; partition OIDs are generally **not** preserved across pg_upgrade → stored tableoid keys could dangle = silent corruption. Must be top of the open-questions list. |
| O7 | **Build & ATTACH maintenance across partitions** — building/attaching requires scanning every partition heap; schema-divergent partitions (dropped cols) and attribute remapping are a correctness minefield (V3's ATTACH full-scan + uniqueness check). | Build iterates the partition descriptor, scans each child heap, remaps attrs to root order; ATTACH backfills; REINDEX repopulates (README; PR). | **RISK.** Attribute remapping across dropped-column/divergent partitions is a known hazard. CREATE INDEX **CONCURRENTLY** MISSING (PR P0-... / matches V9's same limitation). |
| O8 | **Cross-partition row movement & upsert** — UPDATE that moves a row across partitions, and ON CONFLICT, must consult the shared index, not just the leaf. | Row-movement does delete-old-`(…,old_tableoid)` + insert-new; INSERT/UPDATE write the spanning entry (README; PR P0-1 write path VERIFIED). | **PARTIAL / P0 (PR P0-1).** ON CONFLICT *detection* across partitions is **unverified** (arbiter resolution is leaf-local; spanning index is on the root) — a cross-partition upsert may miss the conflict. MERGE not investigated. HOT interaction with the synthetic key un-audited. |

**One-line read:** ProgreSQL has a *plausible, unifying mechanism* (the
`tableoid` discriminator) for every classic objection, and *working happy-path
code* — but the two historically fatal ones, **O1 (vacuum-once-at-end is not
implemented)** and **O4/O6 (tableoid portability across dump/upgrade)**, are
exactly the two still open. That is consistent with why this feature has stalled
for everyone, and an honest RFC leans into it.

---

## 3. RFC framing / outline (for a pgsql-hackers introduction)

Goal: position the fork as a **credible PoC that starts a design conversation**,
not a merge request. The reviewer's first instinct will be "we've seen this die
before (V2), and there's already a live 2025 thread (V4) and a shipping
downstream impl (V9)." Disarm that in the first three paragraphs by citing them.

### 3.1 Positioning principles
1. **Lead with the vacuum story.** It is *the* objection (V4, V9, quantified
   300x). Say up front exactly how far the PoC gets (per-partition cleanup
   exists) and where it stops (no defer-to-once-at-end) — and that you know the
   accepted-shape fix from V4.
2. **Acknowledge the graveyard explicitly and by message-id**, and place
   ProgreSQL in **the three-camp design tree**:
   - **Camp A — recurse-and-check (distributed storage):** V3 (HighGo / Cary
     Huang 2022). Keeps per-partition index storage; final all-partition sort for
     uniqueness. Dodges size-limit + keeps cheap DETACH.
   - **Camp B — single index + synthetic partition-id + side catalog:** V4 (Dilip
     2025, `pg_index_partitions`); Postgres Pro gbtree (V9) is the shipping cousin.
   - **Camp C — single index keyed on the existing `tableoid`:** the 2021 Alibaba
     POC (V4b, Wenjing Zeng) **and ProgreSQL.** This is ProgreSQL's lineage.
   Plus V2 (2019 origin, Haas's 141 TB + Lane's "defeats the purpose"), V6 (PG11
   boundary). Signal: "we read the history; we are the Camp C / tableoid line,
   restricted to enforcement-only; here is why and here are its risks."
3. **State the one differentiator clearly:** ProgreSQL is **Camp C** — reuse
   **`tableoid`** (revives the 2021 Alibaba POC's approach) instead of Camp B's
   synthetic partition-id + side-catalog or Camp A's distributed storage. Own
   that choice and its central risk (O4/O6 tableoid portability + Haas's OID-reuse
   objection) rather than hiding it.
4. **Scope honestly:** this is *uniqueness enforcement only* — spanning indexes
   are hidden from the planner (O3). Do **not** pitch read acceleration; V2 did,
   and it widened the surface. Narrower scope is a feature here.
5. **Explicit, opt-in syntax** so default partitioned-table behavior never
   changes and reviewers can bound the scope.
6. **Name trade-offs as semantics, not bugs** (DETACH is O(rows); same deal as
   Oracle/Postgres Pro).

### 3.2 Proposed outline
1. **Subject / one-paragraph thesis** — "PoC: globally-unique indexes on
   partitioned tables via a single root B-tree keyed `(user_cols…, tableoid)`.
   Seeking design feedback, especially on vacuum and pg_upgrade. Aware of and
   building on the 2019 (V2) and 2025 (V4) threads and Postgres Pro's gbtree
   (V9)."
2. **The gap, in PG's own words** — quote V1 verbatim. One paragraph.
3. **Prior art & why this isn't naïve** — V6 (the deliberate PG11 boundary); V2
   (origin, with Haas's 141 TB and Lane's "defeats the purpose"); the three-camp
   tree (V3 Camp A vs V4/V9 Camp B vs V4b/ProgreSQL Camp C); V9 (Camp B ships, at
   a cost). Position ProgreSQL: **Camp C, single index, existing `tableoid`,
   enforcement-only** — explicitly a revival of the 2021 Alibaba POC (V4b) with a
   narrower scope.
4. **The one idea** — tableoid trailing key solves heap-local-TID (§1E);
   uniqueness compares only `indnuniqatts` leading cols. Tight.
5. **Vacuum: the honest center** — what runs today (per-partition cleanup), the
   V4 "defer + once-at-end" optimization we have **not** yet adopted, the
   TID-recycle hazard, VACUUM FULL/CLUSTER. Invite design input here first.
6. **The tableoid-vs-partition-id debate** — why we reused tableoid; the
   dump/pg_upgrade portability risk (O6); contrast with V4's `pg_index_partitions`.
   Ask the room which is more defensible long-term.
7. **Proposed syntax (explicit, opt-in)** — strawman
   `CREATE UNIQUE INDEX … ON ONLY parent (cols) GLOBAL;` (note: GLOBAL is the
   keyword V2/V3 already used — prefer it over inventing SPANNING for the public
   pitch) + a catalog marker. Discuss constraint-level
   `UNIQUE (...) GLOBAL`. **Explicitly retire the README's INHERITS+PARTITION BY
   implicit trigger** (PR P1-1) — it will be rejected on sight.
8. **What works today** — concise checklist (DDL/DML/ATTACH/DETACH/REINDEX/COPY)
   so reviewers see running code, not vapor.
9. **Open questions (the ask)** — enumerated, each tied to §2: O1 vacuum-once-at-
   end + TID recycle; O2 DETACH cost semantics; O4/O6 tableoid portability; O5
   lock ordering + CIC; O7 attribute remapping across divergent partitions; O8
   ON CONFLICT/MERGE/row-movement.
10. **Explicitly out of scope** — read acceleration; non-unique global indexes;
    sub-partitions. Say so to pre-empt scope-creep objections.
11. **Pointers** — repo, `README.md`, `PRODUCTION_READINESS.md` (lead with the
    honesty of the gap audit as a credibility signal).

### 3.3 Anti-patterns that get RFCs dismissed
- Don't claim production-readiness or imply a near-term merge.
- Don't bury vacuum (O1) or the tableoid portability risk (O4/O6).
- Don't keep the implicit INHERITS+PARTITION BY trigger.
- Don't pitch it as a performance feature (V2's mistake).
- Don't ignore V3/V4/V9 — naming them is the whole point of this doc.

---

## 4. Action items before public posting
1. Open every **[SEARCH-SURFACED]** URL by hand; confirm titles/authors/dates;
   pull the real commitfest entry IDs for V3 and V4 from
   `https://commitfest.postgresql.org/`.
2. Read V4 (Dilip, 2025) in full — it is the live thread; the RFC must engage its
   current state, not a snapshot.
3. Re-confirm V1's exact wording on the live page at posting time.
4. Confirm Oracle/SQL Server clause names before quoting the cost comparison.
5. Implement (or at least spec) the V4-style "defer + once-at-end" vacuum before
   posting — it is the difference between "credible PoC" and "naive."
6. Re-validate §2 matrix line refs against the then-current `PRODUCTION_READINESS.md`.

---

## 5. Executive tally (source honesty)
- **FETCH-VERIFIED (opened & confirmed on-topic this session, with
  subject/author/date checked): 11** —
  V1 (PG18 docs §5.12.2.3), V2 (Ibrar Ahmed 2019 + Postgres Pro thread mirror),
  V3 (Cary Huang/HighGo 2022 patch + David Zhang blog), V4 (Dilip Kumar 2025
  proposal), **V4b (Wenjing Zeng/Alibaba 2021 — the tableoid POC, nearest prior
  art)**, V5 (Stefan Keller 2012), V5b (Mariel Cherkassky 2018), V6 (Álvaro
  Herrera PG11 local-index thread, 2017), V7 (wiki Table_partitioning), V8 (wiki
  Todo), V9 (Postgres Pro pgpro_gbtree docs + Habr writeup). Several additional
  FETCH-VERIFIED ecosystem pages (Percona/Ibrar 2019, AWS Oracle-migration,
  pgEdge/Shaun Thomas 2026, YugabyteDB/Franck Pachot 2023) are cited in §1D.
- **SEARCH-SURFACED (URL returned by search, not separately opened): a handful** —
  PolarDB global-index docs, HighGo 2nd blog, HN PG11 thread, plus secondary
  patch attachments and the PG11 unique-index commitfest thread mirror.
- **FABRICATED: 0.** No invented message-ids, authors, or dates.
- **Hard caveat:** commitfest **entry IDs** are behind an auth redirect for the
  fetch tool and were NOT verified — re-pull by hand from
  `https://commitfest.postgresql.org/`. (Thread *message-ids* above are verified.)
