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
`(user_columns…, tableoid)` for every live row in every partition, and enforces
uniqueness on just the user columns. Cross-partition `PRIMARY KEY` / `UNIQUE`
simply works:

```sql
CREATE TABLE events_base (id bigint, kind text, ts timestamptz NOT NULL);

-- The opt-in signal: a partitioned table that ALSO inherits a base table.
CREATE TABLE events (
    PRIMARY KEY (id)              -- unique across ALL partitions, not just within one
) INHERITS (events_base)
  PARTITION BY RANGE (ts);

CREATE TABLE events_2024 PARTITION OF events
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE events_2025 PARTITION OF events
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

INSERT INTO events VALUES (1, 'a', '2024-06-01');   -- ok
INSERT INTO events VALUES (1, 'b', '2025-06-01');   -- ERROR: duplicate key (id)=(1)
                                                    --        across partitions ✔
```

The feature activates for a partitioned root that **also `INHERITS` a base
table** and declares a `PRIMARY KEY` / `UNIQUE`. That `INHERITS` + `PARTITION BY`
combination is the explicit opt-in — ordinary partitioned tables behave exactly
like stock PostgreSQL.

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
make -C src/test/regress check    # 233/233, includes `progresql` + `progresql_ddl`
```

---

## How it works (the nerd section)

### The index shape
- A new `pg_index` column, **`indnuniqatts`**, marks a spanning index: a value
  `> 0` says "this index stores N+1 key columns, but uniqueness is enforced on
  only the first `indnuniqatts`." The trailing column is the row's
  **`tableoid`** (system attno `-7`).
- So two rows that share a user key but live in *different* partitions are
  **distinct entries** in the B-tree (different `tableoid`), yet a uniqueness
  check that compares only the leading `indnuniqatts` columns still catches the
  collision.

### Uniqueness checks across partitions
- `_bt_check_unique` (`nbtinsert.c`) compares only the first `indnuniqatts`
  columns. When a candidate duplicate is found, the conflicting tuple lives in a
  *partition*, not the root — so the check reads the `tableoid` key column,
  opens that partition, and performs the heap-liveness probe there.
- Stale entries are retired with the standard `kill_prior_tuple` / `LP_DEAD`
  mechanism so aborted / deleted rows don't raise false conflicts.

### Writing the index (executor)
- After a row lands in a leaf partition, `ExecInsertSpanningIndexTuples`
  (`execIndexing.c`) writes `(user_cols…, tableoid)` into the root's spanning
  index. INSERT and COPY both route through this path.
- A **per-statement cache** on `EState` hoists the
  `get_partition_ancestors` → `table_open` → `index_open` → `BuildIndexInfo`
  resolution out of the per-row hot path (built lazily per partition, released
  by `FreeExecutorState`). On a 3-partition bench this cut bulk-INSERT spanning
  overhead ~7.5× versus the naive per-row reopen.
- `UPDATE` skips the spanning write when the unique-key columns are unchanged —
  the existing entry stays valid through the heap HOT chain — and re-roots on the
  old TID for HOT updates.

### DDL lifecycle (the hard part)
The spanning index has to stay correct across every operation that mutates the
partition tree (`tablecmds.c`, `index.c`, `heap.c`):
- **DROP / DETACH** — remove only the departing partition's entries, keyed by
  `tableoid` (not heap TID, which collides across partitions).
- **ATTACH** — backfill the attaching partition's existing rows into the
  spanning index.
- **REINDEX** — `index_build` leaves the root index empty (the root has no
  storage), so the rebuild repopulates `(key, tableoid)` from every live row in
  every partition.
- **COPY** — flows through the executor INSERT path, so it's covered for free.

### Planner
- Spanning indexes are hidden from the planner's index list (`plancat.c`): rows
  live in partitions, never in the root's (nonexistent) storage, so the planner
  must never consider scanning them.

---

## What changed vs. mainline

~27 files, ~1,670 insertions on top of `REL_18_STABLE`
(`git diff upstream/REL_18_STABLE..progresql-18`):

| Area | Files | Why |
|------|-------|-----|
| Catalog | `catalog/pg_index.h` (`indnuniqatts`), `catversion.h`, `catalog/index.c`, `catalog/heap.c` | New index metadata; build / reindex / drop hooks |
| Index AM (nbtree) | `access/nbtree/nbtinsert.c`, `nbtsort.c`, `access/index/genam.c`, `indexam.c` | Cross-partition uniqueness check; build over partitions; key-description |
| Commands | `commands/indexcmds.c`, `commands/tablecmds.c`, `parser/parse_utilcmd.c` | Spanning-index creation / propagation; DROP / DETACH / ATTACH / REINDEX lifecycle |
| Executor | `executor/execIndexing.c`, `executor/nodeModifyTable.c`, `nodes/execnodes.h` | Per-row insert hook + per-statement cache; UPDATE handling |
| Planner | `optimizer/util/plancat.c` | Hide spanning indexes from path generation |
| Tests | `src/test/regress/{sql,expected}/progresql{,_ddl}`, `parallel_schedule` | Feature + DDL-lifecycle coverage |

## Status

- Based on **PostgreSQL 18** (`REL_18_STABLE`).
- **233/233** core regression tests pass, including the two ProgreSQL suites
  (`progresql`, `progresql_ddl`).
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

- The feature is opt-in via the `INHERITS` + `PARTITION BY` pattern; plain
  partitioned tables are untouched.
- Sub-partitions are intentionally excluded from the spanning-index path.
- The per-statement cache is exactly that — per statement; it is rebuilt for
  each top-level DML.
- This is a research fork, not a supported product.

## License

ProgreSQL is distributed under the **PostgreSQL License**, the same terms as
upstream PostgreSQL. See [`COPYRIGHT`](./COPYRIGHT). ProgreSQL's modifications
are offered under the same license.
