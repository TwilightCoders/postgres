# Backup & Disaster Recovery runbook

How to run ProgreSQL in production with a supported, vanilla-PostgreSQL safety
net. The guiding principle: **confine the fork to the primary; make durability
ride on stock PostgreSQL.**

## Why this works

A spanning (`GLOBAL`) index is *index-level state plus a small catalog*. The
trailing `partseq` discriminator lives in the **index key**, not in the heap
tuple, and it is allocated fresh per partition on whatever cluster builds the
index. Therefore:

- **Logical decoding emits ordinary row changes.** The heap tuples are standard
  (just your user columns), so a *stock* PostgreSQL subscriber applies them with
  no knowledge that the source had a cross-partition index.
- **The index is fully derivable from the data.** Nothing in the spanning index
  is information you can't reconstruct by re-loading the rows. So a vanilla data
  copy is a complete backup, and rebuilding a ProgreSQL master is just "load the
  data and let the index get maintained."

This is validated by the `subscription/031_spanning` TAP test and by an
end-to-end master→replica→rebuild drill (results at the bottom).

## Architecture

```
   writes ─▶  ProgreSQL PRIMARY  ──logical replication──▶  stock PostgreSQL REPLICA
              (enforces GLOBAL                              (faithful data copy;
               uniqueness, partitioning)                    back it up with ANY
                                                            standard PG tooling)
```

- The **primary** is the only node running the fork. It enforces partitioning and
  cross-partition uniqueness.
- The **replica** is unmodified PostgreSQL. It does not (and cannot) have a
  spanning index, so it does not re-enforce cross-partition uniqueness — it
  doesn't need to; the primary already did, so the data flowing in is clean. The
  replica's job is to be a durable, tool-friendly copy.
- Back up the *replica* with whatever you already trust: `pg_dump`, pgBackRest,
  Barman, WAL-G, base backups + PITR. None of them need to understand the fork.

### Two shapes for the replica

1. **Same partitioned shape, minus `GLOBAL`.** A faithful structural copy. It
   won't have cross-partition uniqueness (stock PG can't put a PK that excludes
   the partition key on a partitioned table — that's the whole reason spanning
   indexes exist), but it holds all the data.
2. **A single flat (non-partitioned) table with an ordinary `PRIMARY KEY`.** If
   the replica doesn't need partitioning, this gives it *real* uniqueness
   enforcement on a 100%-vanilla node. Publish from the primary with
   `publish_via_partition_root = true` and the subscriber sees one logical table.

The drill below uses shape (2).

## Required setting: `REPLICA IDENTITY FULL`

A spanning-PK **leaf** has no local primary key, so logical decoding has no key to
log for `UPDATE`/`DELETE`. Set the leaves to `REPLICA IDENTITY FULL` or those
changes won't replicate (INSERT replicates without it — and a silently
INSERT-only replica is the dangerous failure mode, so do this up front):

```sql
ALTER TABLE my_part_0 REPLICA IDENTITY FULL;
ALTER TABLE my_part_1 REPLICA IDENTITY FULL;
-- ... every leaf partition; do it for new partitions at ATTACH time too.
```

Cost: `REPLICA IDENTITY FULL` logs the entire old row (not just a key) for every
`UPDATE`/`DELETE`, increasing WAL volume. Fine for moderate write rates; measure
it for very heavy ones.

## Setup

On the **primary** (`wal_level = logical`):

```sql
-- your spanning schema (kept in source control / migrations)
CREATE TABLE t (id int NOT NULL, k int NOT NULL, payload text,
    PRIMARY KEY (id) GLOBAL) PARTITION BY LIST (k);
CREATE TABLE t0 PARTITION OF t FOR VALUES IN (0);
CREATE TABLE t1 PARTITION OF t FOR VALUES IN (1);
ALTER TABLE t0 REPLICA IDENTITY FULL;
ALTER TABLE t1 REPLICA IDENTITY FULL;

CREATE PUBLICATION pub FOR TABLE t WITH (publish_via_partition_root = true);
```

### Table-inheritance (`INHERITS`) hierarchies

If the spanning index is on a **table-inheritance** root (typed children, time-
bucketed leaves) rather than a declaratively partitioned root, the publication
side differs: `publish_via_partition_root` does **not** apply — inheritance
children are *separate* logical-replication relations, and publishing the parent
alone replicates only the parent's own rows. Publish every storage-bearing leaf
explicitly (or use `FOR ALL TABLES`), and set `REPLICA IDENTITY FULL` on each
leaf (they have no local PK, same as partitions):

```sql
-- inheritance root + typed children + bucketed leaves
ALTER TABLE msg_2026_01 REPLICA IDENTITY FULL;   -- every leaf
ALTER TABLE fct_2026_01 REPLICA IDENTITY FULL;
CREATE PUBLICATION pub FOR TABLE msg_2026_01, fct_2026_01 /* , ... every leaf */;
-- or simply:  CREATE PUBLICATION pub FOR ALL TABLES;
```

The subscriber maintains its spanning index on initial COPY sync, streamed
INSERT, and replicated UPDATE — covered by the `subscription/032_spanning_inherit`
TAP test. This is also the per-leaf shape a fork↔fork multi-master mesh uses
(each node publishes its leaves with `origin = none` for loop avoidance).

On the **stock-PostgreSQL replica**:

```sql
-- flat backup table (shape 2)
CREATE TABLE t (id int PRIMARY KEY, k int, payload text);
CREATE SUBSCRIPTION sub
    CONNECTION 'host=PRIMARY port=5432 dbname=app user=repl'
    PUBLICATION pub;   -- initial COPY sync runs automatically
```

## Monitoring (so the "backup" is actually a backup)

Logical replication is asynchronous and does **not** ship DDL. Watch for drift:

- **Lag**: `pg_stat_subscription` on the replica; `pg_stat_replication` /
  `pg_replication_slots` on the primary. Alert on growing lag or an inactive slot.
- **DDL drift**: any new partition / column / table on the primary must be applied
  to the replica too, or its rows have nowhere to land. Gate schema changes on a
  "apply to replica" step.
- **Row-count reconciliation**: periodic `count(*)` (or checksums) per table,
  primary vs replica, as a coarse "are we actually in sync" check.

## Restore runbook (catastrophic loss of the primary)

The replica gives you **data**; your migrations give you the **`GLOBAL` schema**
(a `pg_dump` of the *vanilla* replica has no `GLOBAL` keyword, so don't restore
the schema from it).

1. Stand up a fresh **ProgreSQL** instance.
2. Apply your ProgreSQL schema DDL from source control — tables, partitions, and
   the `GLOBAL` constraints/indexes. (Empty schema first, or build the index after
   load — both work.)
3. Load **data only** from the replica:
   ```sh
   pg_dump -h REPLICA --data-only -t t app | psql -h NEW_PRIMARY app
   ```
   Rows route to partitions and the spanning index is maintained as they land
   (or, if you created the `GLOBAL` index after loading, it is built and its
   cross-partition uniqueness is verified by the build).
4. Verify: row counts match the replica; a deliberate cross-partition duplicate is
   rejected; `bt_index_parent_check()` on the spanning index is clean.
5. Repoint writers at the new primary; re-establish the replica (new subscription).

**Rehearse this** on a schedule against a throwaway instance — restoring from the
vanilla replica touches nothing in production, so there's no excuse not to.

### Caveats / RPO

- Async replication means you lose whatever hadn't replicated before the failure
  (your RPO is the replication lag at the moment of loss).
- The data on the replica is already uniqueness-clean (the primary enforced it on
  the way in). If you ever *promote the vanilla replica to be a primary* directly,
  new writes there are no longer cross-partition-uniqueness-checked — promote it
  to a *ProgreSQL* master (the restore runbook) if you need the guarantee back.

## End-to-end drill (proof)

A live drill (`ProgreSQL master → vanilla flat replica → kill master → rebuild a
fresh ProgreSQL master from the replica's data`) confirmed the whole loop:

```
master: 1000 rows, cross-partition dup REJECTED (enforced)
replica (flat, id PRIMARY KEY): initial COPY sync → 1000 rows
live INSERT/UPDATE/DELETE on master → replica tracked exactly (REPLICA IDENTITY FULL)
CATASTROPHE: master killed
fresh ProgreSQL: apply GLOBAL schema, load data-only from replica → 1000 rows
verify: row count matches ✔  cross-partition dup REJECTED ✔  0 in-data dups ✔
        amcheck of rebuilt spanning index: OK ✔
```

The reusable drill script lives with the soak harness conventions; the logical-rep
path is also covered deterministically by `src/test/subscription/t/031_spanning.pl`.
