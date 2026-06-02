# ProgreSQL

**A PostgreSQL fork that adds native _spanning indexes_ — true cross-partition
`PRIMARY KEY` / `UNIQUE` enforcement on partitioned tables, without forcing the
partition key into the constraint.**

> ⚠️ **Highly experimental.** This is a research fork. Don't run it anywhere you
> care about your data.

Built on [PostgreSQL](https://github.com/postgres/postgres) 18
(`REL_18_STABLE`). Everything stock Postgres does, ProgreSQL does — plus one
feature the upstream planner / executor / access-method layers were extended to
support.

---

## The problem

In vanilla PostgreSQL, a `UNIQUE` or `PRIMARY KEY` on a partitioned table **must
include every partition-key column**:

```sql
-- Vanilla PG: rejected unless (id) includes the partition key (ts)
CREATE TABLE events (id bigint PRIMARY KEY, ts timestamptz)
  PARTITION BY RANGE (ts);
-- ERROR: unique constraint on partitioned table must include all
--        partitioning columns
```

That means you **cannot** have a globally-unique `id` across partitions when you
partition by `ts`. The usual workarounds — a shadow table, triggers, or an
application-level uniqueness check — are slow, racy, or both.

## What ProgreSQL adds

A **spanning index**: a real B-tree built on the partitioned *root* that stores
`(user_columns…, partseq)` for every live row in every partition, and enforces
uniqueness on just the user columns. The trailing `partseq` is a small,
index-local partition sequence number — a stable discriminator that survives OID
reuse and `pg_upgrade`. You opt in with the **`GLOBAL`** keyword. Cross-partition
`PRIMARY KEY` / `UNIQUE` simply works:

```sql
CREATE TABLE events (
    id   bigint NOT NULL,
    kind text,
    ts   timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL       -- unique across ALL partitions, not just within one
) PARTITION BY RANGE (ts);

CREATE TABLE events_2024 PARTITION OF events
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE events_2025 PARTITION OF events
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

INSERT INTO events VALUES (1, 'a', '2024-06-01');   -- ok
INSERT INTO events VALUES (1, 'b', '2025-06-01');   -- ERROR: duplicate key (id)=(1)
                                                    --        across partitions ✔
```

`GLOBAL` is the explicit opt-in, available three ways — a table constraint
(`PRIMARY KEY (…) GLOBAL` / `UNIQUE (…) GLOBAL`), a standalone index
(`CREATE UNIQUE INDEX … ON root (…) GLOBAL`), or `ALTER TABLE … ADD CONSTRAINT …
GLOBAL`. Ordinary partitioned tables behave exactly like stock PostgreSQL. (The
syntax aligns with Oracle's `GLOBAL` partitioned indexes and the in-core
"global index" proposal under discussion on pgsql-hackers.)

---

## Quickstart

Build it like any PostgreSQL source tree:

```sh
git clone -b progresql-18 https://github.com/TwilightCoders/progresql.git
cd progresql

./configure --prefix="$PWD/install" --enable-debug --enable-cassert
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" && make install

install/bin/initdb -D data
install/bin/pg_ctl -D data -l server.log start
install/bin/psql -d postgres   # then paste the demo above
```

(`--enable-cassert` is for development; drop it for a release build. There's also
a `build.sh` wrapper at the repo root.)

Run the feature's regression suites:

```sh
make -C src/test/regress check    # 240/240, includes the `progresql*` suites
```

---

## How it works (the nerd section)

### The index shape
- A new `pg_index` column, **`indnuniqatts`**, marks a spanning index: a value
  `> 0` says "this index stores N+1 key columns, but uniqueness is enforced on
  only the first `indnuniqatts`." The trailing column is an honest **`int4`
  `partseq`** — an index-local partition sequence number recorded in the
  **`pg_index_partition`** catalog (`(index, partseq) → partition`). partseq is
  allocated once when a partition joins the index's domain and never reused, so
  spanning entries survive partition OID reuse and `pg_upgrade`.
- So two rows that share a user key but live in *different* partitions are
  **distinct entries** in the B-tree (different `partseq`), yet a uniqueness
  check that compares only the leading `indnuniqatts` columns still catches the
  collision.

### Uniqueness checks across partitions
- `_bt_check_unique` (`nbtinsert.c`) compares only the first `indnuniqatts`
  columns. When a candidate duplicate is found, the conflicting tuple lives in a
  *partition*, not the root — so the check reads the `partseq` key column,
  resolves it to the partition via `pg_index_partition`, opens that partition,
  and performs the heap-liveness probe there.
- Stale entries are retired with the standard `kill_prior_tuple` / `LP_DEAD`
  mechanism so aborted / deleted rows don't raise false conflicts. A spanning
  index's leaf pages skip the pre-split heap-probing deletion passes (their
  "heap" is the storage-less root); VACUUM and the drain retire dead entries.

### Writing the index (executor)
- After a row lands in a leaf partition, `ExecInsertSpanningIndexTuples`
  (`execIndexing.c`) writes `(user_cols…, partseq)` into the root's spanning
  index. INSERT and COPY both route through this path.
- A **per-statement cache** on `EState` hoists the
  `get_partition_ancestors` → `table_open` → `index_open` → `BuildIndexInfo`
  resolution out of the per-row hot path (built lazily per partition, released
  by `FreeExecutorState`). On a 3-partition bench this cut bulk-INSERT spanning
  overhead ~7.5× versus the naive per-row reopen.
- `UPDATE` skips the spanning write when the unique-key columns are unchanged —
  the existing entry stays valid through the heap HOT chain. A leaf has no local
  index on the spanning-key columns, so those columns are made **HOT-blocking**
  on the leaf (`RelationGetIndexAttrBitmap`): a spanning-key UPDATE is therefore
  non-HOT, the old tuple dies normally, and the stale spanning entry is retired —
  without this, an in-place HOT update would leave a permanent false conflict.

### DDL lifecycle (the hard part)
The spanning index has to stay correct across every operation that mutates the
partition tree (`tablecmds.c`, `index.c`, `heap.c`):
- **DROP / DETACH** — remove only the departing partition's entries, keyed by
  `partseq` (not heap TID, which collides across partitions).
- **ATTACH** — allocate the attaching partition's `partseq` and backfill its
  existing rows into the spanning index.
- **REINDEX** — `index_build` leaves the root index empty (the root has no
  storage), so the rebuild repopulates `(key, partseq)` from every live row in
  every partition.
- **COPY** — flows through the executor INSERT path, so it's covered for free.
- **dump / restore / `pg_upgrade`** — `pg_get_indexdef` / `pg_get_constraintdef`
  and `pg_dump` clip the trailing discriminator and emit `GLOBAL`, so a dump
  replays as a *spanning* index. Without this the restore would silently
  downgrade to a plain composite index and lose cross-partition uniqueness.

### Planner
- Spanning indexes are hidden from the planner's index list (`plancat.c`): rows
  live in partitions, never in the root's (nonexistent) storage, so the planner
  must never consider scanning them.

### VACUUM — the deferred coalesced drain
- A spanning index's entries for one partition are scattered across the whole
  B-tree (the discriminator is the *trailing* key, which keeps the uniqueness
  probe O(log n) on the hot write path). So retiring one leaf's dead entries
  needs a full index scan — and N leaf VACUUMs would each scan the whole index:
  **O(N²) per sweep**.
- The fix (GUC `spanning_defer_vacuum`, default off): a leaf VACUUM *enqueues*
  its dead entries to the durable **`pg_spanning_drainq`** catalog and leaves the
  heap slots `LP_DEAD` (un-reaped, so they can't be reused). A single coalesced
  **drain** then retires every queued partition's entries in *one* index scan and
  reaps the now-safe slots — **O(N)**. Measured: per-leaf VACUUM cost goes from
  growing-with-N to flat; the sweep drops ~10× at 64 partitions and the gap
  widens. The drain runs automatically (an autovacuum work item) or on demand via
  `pg_drain_spanning_index(regclass)`.
- Correctness rests on an audited invariant: a held `LP_DEAD` slot can only be
  reaped by the gated drain (a new tuple can never reuse it first), so a stale
  entry's address can never alias a live row before the entry is retired.

---

## What changed vs. mainline

A focused diff on top of `REL_18_STABLE`
(`git diff upstream/REL_18_STABLE..progresql-18`):

| Area | Files | Why |
|------|-------|-----|
| Catalog | `catalog/pg_index.h` (`indnuniqatts`), `catalog/pg_index_partition.{h,c}` ((index,partseq)→partition map), `catalog/pg_spanning_drainq.{h,c}` (deferred-vacuum queue), `catversion.h`, `catalog/index.c`, `catalog/heap.c` | New index metadata; partseq allocation/resolution; drain queue; build / reindex / drop hooks; int4 discriminator stamp |
| Index AM (nbtree) | `access/nbtree/nbtinsert.c`, `nbtree.c`, `nbtsort.c`, `access/index/genam.c`, `indexam.c` | Cross-partition uniqueness check; build over partitions; coalesced multi-partseq drain; skip heap-probing pre-split deletion; key-description |
| Commands / GUC | `commands/indexcmds.c`, `commands/tablecmds.c`, `commands/vacuum.c`, `parser/gram.y`, `parser/parse_utilcmd.c`, `utils/misc/guc_tables.c` | `GLOBAL` syntax; spanning-index creation / propagation; DROP / DETACH / ATTACH / REINDEX; `spanning_defer_vacuum` GUC |
| Executor | `executor/execIndexing.c`, `executor/nodeModifyTable.c`, `nodes/execnodes.h` | Per-row insert hook + per-statement cache; UPDATE handling |
| VACUUM / autovacuum | `access/heap/vacuumlazy.c`, `postmaster/autovacuum.c`, `utils/cache/relcache.c` | Deferred enqueue + coalesced drain + `pg_drain_spanning_index()`; autovacuum drain work item; HOT-blocking spanning keys on leaves |
| Dump / restore | `utils/adt/ruleutils.c`, `bin/pg_dump/pg_dump.{c,h}` | Clip discriminator + emit `GLOBAL` so dumps replay as spanning indexes |
| Planner | `optimizer/util/plancat.c` | Hide spanning indexes from path generation |
| Tests | `src/test/regress/{sql,expected}/progresql*`, `parallel_schedule` | Feature, DDL lifecycle, partseq, GLOBAL, HOT, vacuum collision, OID reuse, drain, dump round-trip |

## Status

- Based on **PostgreSQL 18** (`REL_18_STABLE`).
- **240/240** core regression tests pass, including the ProgreSQL suites
  (`progresql`, `progresql_ddl`, `progresql_partseq`, `progresql_global`,
  `progresql_hot`, `progresql_vacuum_collision`, `progresql_oid_reuse`,
  `progresql_drain`, `progresql_dumpdef`).
- Warning-clean under PostgreSQL's standard strict flags.
- Branch layout: `master` tracks upstream PostgreSQL; **`progresql-18`** carries
  the spanning-index feature (this is the active experimental line).

## Tracking upstream

This repo keeps `postgres/postgres` as the `upstream` remote. To move onto a
newer PG18 minor:

```sh
git remote add upstream https://github.com/postgres/postgres.git   # once
git fetch upstream
git rebase upstream/REL_18_STABLE progresql-18
```

## Limitations

- The feature is opt-in via the `GLOBAL` keyword; plain partitioned tables are
  untouched. (Combining `INHERITS` with `PARTITION BY` is rejected, exactly as in
  stock PostgreSQL — `GLOBAL` is the only way to request a spanning index.)
- The deferred coalesced VACUUM drain is behind `spanning_defer_vacuum` (default
  off) and currently applies to spanning leaves without local indexes.
- Multi-level (sub-)partitioning is not supported under a spanning index: every
  partition must be a storage-bearing leaf. This is rejected at DDL — both
  sub-partitioning a partition of a spanning-indexed root, and creating a
  `GLOBAL` index on an already multi-level tree.
- The per-statement cache is exactly that — per statement; it is rebuilt for
  each top-level DML.
- This is a research fork, not a supported product.

## License

ProgreSQL is distributed under the **PostgreSQL License**, the same terms as
upstream PostgreSQL. See [`COPYRIGHT`](./COPYRIGHT). ProgreSQL's modifications
are offered under the same license.
