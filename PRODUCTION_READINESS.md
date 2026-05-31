# ProgreSQL — Production-Readiness & Upstream-Viability Tracker

> Status of this document: **honest gap audit**, not a sales sheet. The README
> describes what works; this file records what does **not** yet work, what is
> **unverified**, and what would have to change before any of this could be
> proposed to the PostgreSQL community without being dismissed.
>
> Each item carries a **confidence tag** so future readers (and future Claude
> sessions) know how much to trust the claim:
>
> - **[VERIFIED]** — read in the source tree this session; cited with `file:line`.
> - **[ASSUMPTION]** — reasoned from PostgreSQL internals knowledge, **not**
>   confirmed against this tree. Treat as a hypothesis to test.
> - **[NEEDS-RESEARCH]** — started to check but could not finish (e.g. tooling
>   failure); explicitly open.
>
> Date of audit: 2026-05-30. Branch: `progresql-18`. Base: `REL_18_STABLE`.
> Diff scale at audit: ~29 files, ~2,075 insertions vs upstream.

---

## Update 2026-05-30 (post-spike) — direction decided

After the initial audit, seven Opus planning agents produced remediation plans
(in `docs/plans/`) and a design spike. Key outcomes that supersede parts of the
text below:

- **Architecture decision: adopt "C1".** Replace the `tableoid` discriminator
  with an **index-local `partseq`** (partition sequence number) + a catalog
  mapping `(index, partseq) → partition`. The root index stays a normal btree.
  This makes **P0-2 (dump/upgrade)** and **P1-3 (OID reuse)** *cease to exist by
  construction* rather than needing patches. See
  `docs/plans/C1-discriminator-design-spike.md`.
- **partseq must be PERSISTED, not recomputed.** A focused probe confirmed the
  build walks partitions in canonical *bound* order (`partdesc.c:342-353`), which
  is **not** stable across dump/restore and renumbers on middle-partition DETACH.
  Therefore partseq is its **own catalog column**, dumped as data; correctness
  needs only intra-DB consistency (map + entries written in one txn), not
  cross-DB determinism. Do **not** mirror `pg_inherits.inhseqno`.
- **NEW confirmed P0: VACUUM TID-collision** (was mis-praised as a strength). See
  **P0-5** below. Two agents confirmed it independently.
- **Fact correction: the discriminator attno is `-6`** (`TableOidAttributeNumber`,
  `sysattr.h:26`), **not -7** as originally written here and in the README.
- **P0-1 refinement:** cross-partition `ON CONFLICT` does **not** silently
  duplicate — it *errors* ("no unique or exclusion constraint matching"), because
  `infer_arbiter_indexes` (`plancat.c:727`) requires a full-key match. Still a P0
  gap, better failure mode than first stated.
- **P0-2 refinement:** confirmed broken — `pg_get_indexdef` (ruleutils.c) emits
  the trailing discriminator column, so logical restore silently downgrades to a
  plain index. Was [NEEDS-RESEARCH]; now [VERIFIED].

---

## How to read the severity column

- **P0 — data integrity / corruption risk.** Can silently produce wrong results,
  corruption, or unrecoverable state. Blocks *any* real use.
- **P1 — viability blocker for upstream.** Will not survive pgsql-hackers review
  as-is, regardless of code quality. Blocks a merge proposal.
- **P2 — hardening / completeness.** Needed for "production" but not a design
  flaw; mostly testing and lifecycle coverage.
- **P3 — polish / nice-to-have.**

---

## P0 — Data-integrity risks

### P0-1 — `INSERT ... ON CONFLICT` conflict *detection* via spanning index is unverified
- **Confidence: [VERIFIED] (write path reached) + [NEEDS-RESEARCH] (detection path)**
- **CORRECTION to an earlier verbal claim:** I initially said ON CONFLICT was
  "unhandled / no spanning-aware path." That was **wrong on the write side.**
  `ExecInsertSpanningIndexTuples` (defined `src/backend/executor/execIndexing.c:1345`)
  has two functional call sites: `src/backend/executor/nodeModifyTable.c:1251`
  (INSERT path) and `:2393` (UPDATE epilogue). Crucially, line 1251 sits
  **after** the speculative-insertion block (1109–1223), so on a *successful*
  (non-conflicting) upsert the spanning entry **is** written.
- **The actual open risk (narrower):** conflict *detection*. Speculative
  insertion uses *arbiter indexes* (`ri_onConflictArbiterIndexes`, set at
  `nodeModifyTable.c:5087` from `node->arbiterIndexes`) resolved against the
  **leaf partition**, and `ExecCheckIndexConstraints` on that leaf. The spanning
  index lives on the **root**, not the leaf. So the question is: **does an
  `ON CONFLICT (id)` arbiter ever resolve to / consult the root's spanning index
  when the conflicting row is in a *different* partition?** If not, a
  cross-partition upsert can miss the conflict and insert a duplicate (P0) or
  fail to fire DO UPDATE/DO NOTHING.
- **Open questions / [NEEDS-RESEARCH]:**
  - Trace arbiter-index resolution for a partitioned-root spanning index. Does it
    get offered as an arbiter at all? (Likely not, since arbiter resolution is
    leaf-local — [ASSUMPTION].)
  - `_bt_check_unique` IS reached generically (`nbtinsert.c:213`) and the
    spanning reduced-`keysz` logic lives there, but whether the *speculative*
    flavor (`UNIQUE_CHECK_PARTIAL` / CEOUC, comments at `nbtinsert.c:87,98`)
    exercises the cross-partition liveness probe is unconfirmed.
  - `MERGE`: same family of concern; not investigated at all.
- **What "done" looks like:** regression coverage for upsert + MERGE against a
  spanning key, **across partitions** (the conflict in a sibling partition is the
  interesting case), both DO NOTHING and DO UPDATE, plus the speculative-abort
  path.

### P0-2 — `pg_upgrade` / binary-upgrade survival is unverified
- **Confidence: [VERIFIED] (no spanning-specific dump/upgrade code) + [ASSUMPTION] (impact)**
- **What I checked this session:**
  - `grep indnuniqatts src/bin/pg_dump` → **no hits** (exit 1). pg_dump has no
    spanning-specific code. *Caveat:* this is **not** proof of breakage — pg_dump
    reconstructs index DDL from the server via `pg_get_indexdef()`, so the real
    question is whether **ruleutils.c / `pg_get_indexdef`** emits round-trippable
    spanning syntax. **Not yet checked.** [NEEDS-RESEARCH]
  - `binary_upgrade` hooks in `src/backend/catalog/index.c` (lines 86–87, 953–968)
    are the **stock** "next index pg_class oid / relfilenumber" machinery — there
    is **no spanning-specific binary_upgrade handling.**
- **Why it matters (P0 if it fails):** the spanning index stores `tableoid` as a
  trailing key column. If **partition OIDs are not preserved** across
  `pg_upgrade`, every stored `tableoid` key becomes a dangling reference after
  upgrade — silent corruption of the uniqueness domain. (Stock PG does **not**
  preserve arbitrary table OIDs across pg_upgrade in general — [ASSUMPTION], the
  crux to confirm.)
- **Open questions / [NEEDS-RESEARCH]:**
  - Does `pg_get_indexdef()` (ruleutils.c) round-trip a spanning index? If it
    emits an ordinary `CREATE INDEX`, dump/restore silently downgrades it.
  - Are partition OIDs stable across the specific dump+restore and binary-upgrade
    paths this design relies on?
- **What "done" looks like:** a `pg_upgrade` TAP test **and** a plain
  `pg_dump | psql` round-trip test that repopulate a spanning-indexed partition
  tree and re-check cross-partition uniqueness after.

### P0-3 — Crash recovery / WAL replay of the spanning index is untested
- **Confidence: [ASSUMPTION]**
- **Reasoning:** spanning-index writes go through the normal nbtree insert path,
  which is WAL-logged — so this *may* be fine for free. But the
  vacuum/backfill/reindex code added for spanning indexes
  (`progresql_vacuum_spanning_indexes`, ATTACH backfill, REINDEX repopulate) was
  not reviewed for WAL correctness or for behavior under a mid-operation crash.
- **What "done" looks like:** TAP recovery tests (crash + restart) covering
  INSERT, UPDATE-with-key-change, DELETE, VACUUM, ATTACH, DETACH, REINDEX on a
  spanning-indexed tree; plus physical-replication apply on a standby.

### P0-4 — Concurrency / isolation behavior is untested
- **Confidence: [ASSUMPTION] → mostly resolved by `docs/plans/P0-4-concurrency.md`**
- **Reasoning:** the cross-partition uniqueness check opens the *conflicting
  partition* during `_bt_check_unique` to do the heap-liveness probe (actual
  lines `nbtinsert.c:595-810`, probe-open at `:597`/`:684`, **AccessShareLock**).
- **Plan findings:** most hazards look **safe** — AS/AS probes are self-compatible
  and the index buffer lock is released before any wait; snapshot handling matches
  stock (dirty probe + SnapshotSelf recheck); the earlier resource-leak worry was
  *refuted*. **Highest remaining risk: H3** — cross-partition UPDATE row-movement
  racing a same-key INSERT (dirty probe may transiently see the pre-delete row).
  DETACH CONCURRENTLY / ATTACH backfill / REINDEX CONCURRENTLY lock-adequacy is
  UNKNOWN.
- **What "done" looks like:** `src/test/isolation` specs — sibling-partition
  same-key races, A↔B lock-order, DETACH-vs-DML, and the H3 row-movement case.

### P0-5 — VACUUM deletes spanning entries by TID only (cross-partition collision)
- **Confidence: [VERIFIED] — confirmed independently by two agents**
- **CORRECTION:** the initial audit *praised* the vacuum path as a strength. It is
  in fact a latent data-corruption bug.
- **Where:** `progresql_vacuum_spanning_indexes` (`vacuumlazy.c:2462-2518`) hands
  the leaf's dead-TID set to `btbulkdelete` on each ancestor spanning index. The
  match runs through `vac_tid_reaped` → `TidStoreIsMember` (`vacuum.c:2698-2703`),
  which compares `(block, offset)` **only** — it never reads the trailing
  discriminator key.
- **Why it's P0:** each partition has its own TID address space, so a dead TID in
  leaf A can equal a *live* TID in sibling leaf B → VACUUM of A can silently
  delete B's live uniqueness entry → lost cross-partition enforcement.
- **Note:** this is the **same TID-collision class already fixed for DROP/DETACH**
  (commit `67a5276404`, BUG A, tableoid-keyed). The vacuum path never got the same
  treatment. Under C1 the fix becomes a partseq-filtered bulkdelete callback —
  correct by construction.
- **Bonus:** `btbulkdelete` full-scans the whole spanning index per leaf vacuum →
  ~O(N²) per autovacuum sweep on an N-leaf tree. This is *also* the historical
  pgsql-hackers killshot objection (Haas's "141 TB of I/O"); the accepted fix
  shape is a deferred single end-of-tree pass. See `docs/plans/P2-P3-remainder.md`
  and `docs/plans/P1-2-prior-art-rfc.md`.
- **What "done" looks like:** tableoid/partseq-aware delete callback (correctness)
  + deferred single-pass cleanup (perf) + recovery/isolation coverage.

---

## P1 — Upstream-viability blockers

### P1-1 — The opt-in mechanism (INHERITS + PARTITION BY) is a non-starter
- **Confidence: [VERIFIED] (mechanism) + [ASSUMPTION] (community reaction)**
- **What it is:** the feature activates for a partitioned root that *also*
  `INHERITS` a base table and declares PRIMARY KEY / UNIQUE. That combination is
  the secret handshake.
- **Why it blocks upstream:** overloading legacy table inheritance — a
  semi-deprecated feature — glued to declarative partitioning, as an *implicit*
  trigger for a major new index type, is exactly the kind of magic pgsql-hackers
  rejects on sight. It is surprising, hard to document, and collides with the
  long-term direction of partitioning.
- **Direction:** replace with **explicit syntax**. Strawman options to debate:
  - `CREATE UNIQUE INDEX ... ON parent (...) GLOBAL;`
  - a constraint clause: `UNIQUE (id) WITHOUT PARTITION KEY` / `... GLOBAL`.
  - The community has opinions here already (see P1-2); the syntax must come out
    of that discussion, not be invented unilaterally.

### P1-2 — Does not engage the global-index prior art
- **Confidence: [ASSUMPTION] (well-established context)**
- **What it is:** "global indexes on partitioned tables" is a long-standing,
  repeatedly-proposed, repeatedly-stalled feature on pgsql-hackers. There is a
  documented graveyard of design objections (vacuum across partitions, DETACH
  cost, planner integration, OID/`tableoid` dependence, locking).
- **Why it blocks upstream:** a proposal that doesn't explicitly cite and respond
  to that history reads as naive and gets dismissed regardless of code quality.
- **What "done" looks like:** a written design doc / RFC that (a) summarizes prior
  threads, (b) states where ProgreSQL's approach agrees/differs, (c) leads with
  the vacuum story (the usual sticking point) as evidence of seriousness.
  **TODO: collect the actual archive thread URLs — not yet gathered.**
  [NEEDS-RESEARCH]

### P1-3 — `tableoid` as a key column needs a defensible rationale → RESOLVED BY C1
- **Confidence: [VERIFIED]**
- **Where:** trailing key column carrying the row's `tableoid` (system attno
  **-6**, `TableOidAttributeNumber`, `sysattr.h:26` — the original "-7" here was
  wrong); `indnuniqatts` records how many leading columns are the *unique* domain.
- **Resolution:** rather than *defend* `tableoid`, **C1 replaces it** with an
  index-local `partseq`. OID wraparound/reuse and pg_upgrade dangling-reference
  objections then no longer apply — the discriminator is index-owned and stable.
  This is the direction (see top-of-file Update and
  `docs/plans/C1-discriminator-design-spike.md`). Prior art agrees: the shipping
  Postgres Pro `pgpro_gbtree` and Dilip Kumar's 2025 patch both use an
  index-local id, not `tableoid` (the 2021 Alibaba `tableoid` POC stalled).

---

## P2 — Hardening & completeness

### P2-1 — Test coverage is thin for the surface area touched
- **Confidence: [VERIFIED] (only two suites) + [NEEDS-RESEARCH] (exact contents)**
- **What exists:** `src/test/regress/sql/progresql.sql` (feature) and
  `progresql_ddl.sql` (DDL lifecycle); 233/233 pass. That proves single-backend
  happy paths + DDL. It does **not** cover: isolation/concurrency, crash/recovery
  (TAP), `pg_upgrade`, upsert/MERGE, logical replication, `pg_dump` round-trip.
- **Note:** I could not cleanly re-list the exact statements in each suite this
  session (tooling). Re-survey before claiming specific coverage.

### P2-2 — `pg_dump` faithful reproduction
- **Confidence: [NEEDS-RESEARCH]** — see P0-2; the logical-dump path (not just
  binary upgrade) must reconstruct a spanning index as a spanning index.

### P2-3 — Vacuum strategy characterization
- **Confidence: [VERIFIED] (exists) + [NEEDS-RESEARCH] (how)**
- `progresql_vacuum_spanning_indexes()` (`src/backend/access/heap/vacuumlazy.c:447`,
  body ~2456–2489, invoked ~2616–2621) cleans dead spanning entries off the root
  during a partition's vacuum. **Open:** is it a targeted bulk-delete keyed by
  the partition's dead TIDs, or a heavier repopulate? Cost model on large
  partitions is unknown. Read the body and document it; benchmark on a big tree.

### P2-4 — Logical decoding / replication of spanning writes
- **Confidence: [ASSUMPTION]** — not investigated. Spanning entries live on the
  root, which has no heap storage; how this interacts with logical decoding
  (which is row/heap oriented) is unclear but probably fine since decoding follows
  heap changes in the partitions. Verify.

### P2-5 — Sub-partition exclusion is a documented limitation, not a solution
- **Confidence: [VERIFIED] (README states it)** — multi-level partition trees are
  intentionally excluded from the spanning path. For "production" this needs to
  either work or hard-error clearly rather than silently skip enforcement.
  **Verify which it does today.** [NEEDS-RESEARCH]

---

## P3 — Polish

### P3-1 — Spanning indexes are enforcement-only (no read acceleration)
- **Confidence: [VERIFIED] (hidden from planner)**
- `src/backend/optimizer/util/plancat.c` hides spanning indexes from path
  generation, so they never accelerate queries — they exist purely to enforce
  uniqueness. That's a reasonable v1 scope, but worth stating explicitly as a
  design choice (a global index that *could* serve cross-partition lookups is a
  natural future extension, and is part of why the community wants the feature).

### P3-2 — `build.sh` is untracked
- Minor: the convenience wrapper referenced by the README is currently untracked
  in git. Decide whether to commit it.

---

## Summary scorecard (as of audit)

| Question | Answer |
|---|---|
| Does the core feature work? | **Yes** — verified, including a vacuum story (rare for prototypes). |
| Production ready? | **No** — P0-1 through P0-4 are unaddressed integrity/robustness risks. |
| Presentable to PG Core as a **merge proposal**? | **No** — P1-1 (opt-in syntax) and P1-2 (prior art) are hard blockers. |
| Presentable as a **research PoC / RFC**? | **Yes, credibly** — running code with a vacuum answer is a respectable starting point for a hackers thread. |

## Honest notes on this audit's own limits

- Several items are **[ASSUMPTION]** based on how stock PostgreSQL behaves, not on
  reading every relevant line of this fork. They are the most likely places I'm
  wrong — treat them as the first things to re-verify.
- The shell tooling failed partway through, so **pg_dump/pg_upgrade awareness
  (P0-2, P2-2), the exact vacuum strategy (P2-3), and the precise test inventory
  (P2-1) were not finished.** They are marked [NEEDS-RESEARCH] rather than guessed.
- I did **not** deep-dive: MERGE, logical replication, foreign-key references
  *to* a spanning-unique column, `REPLICA IDENTITY`, or row-level locking
  (`SELECT ... FOR UPDATE`) interactions. All are plausible additional gaps.
