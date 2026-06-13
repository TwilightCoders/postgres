# Changelog — ProgreSQL spanning indexes

Changes ProgreSQL adds on top of stock PostgreSQL (`REL_18_STABLE`). Vanilla
PostgreSQL behavior is unchanged unless a table opts in with the `GLOBAL` keyword.
Newest first.

## 2026-06-13

### Added
- **Foreign keys referencing a spanning (`GLOBAL`) primary/unique key** (#33).
  `REFERENCES parent (cols)` against a spanning key is accepted and fully
  enforced — insert-time existence checks plus `ON DELETE`/`ON UPDATE`
  `RESTRICT` / `NO ACTION` / `CASCADE` / `SET NULL` / `SET DEFAULT`, on operations
  through the partitioned root *and* on individual leaf partitions. New regress
  suite `progresql_fk`.
- **Backup / disaster-recovery runbook** ([`docs/disaster-recovery.md`](docs/disaster-recovery.md)):
  run the fork on the primary, logically replicate to a stock-PostgreSQL replica
  as the supported backup/DR target, and rebuild a ProgreSQL master from that
  data. Validated end-to-end.
- Soak harness knobs: `--local-index` (#38 deferred local-index oracle, with
  `heapallindexed` amcheck) and `--ddl-churn` (#41 concurrent index DROP/CREATE
  during a drain).

### Changed
- **`spanning_defer_vacuum` now defaults to `on`** (#39). Leaf VACUUM enqueues to
  the coalesced drain instead of an O(N²) per-leaf full index scan.
- **Deferred spanning vacuum now applies to leaves with local indexes** (#38),
  not just no-local-index leaves: the coalesced drain vacuums each leaf's local
  indexes for the dead set before reaping, so the deferral is universal.

### Fixed
- Deferred-drain corruption with multiple spanning indexes on one root (#40): the
  drain is now root-coordinated (frees a heap slot only after every spanning index
  on the root has retired its entry).
- Leaf relcaches are invalidated when a spanning index is built (#42), closing a
  latent stale-`rd_hotblockingattr` window.

### Verified (no code change)
- Crash recovery: `recovery/049_spanning_crash` TAP (17/17) + 25 kill-9/recover
  soak cycles.
- Logical replication: `subscription/031_spanning` TAP (7/7). `UPDATE`/`DELETE`
  replication of spanning-indexed tables requires `REPLICA IDENTITY FULL` on the
  leaves (documented).

## Earlier (campaign highlights)

### Added
- The **spanning index**: a single btree on the partitioned root keyed
  `(user_cols…, partseq)` that enforces uniqueness on the user columns across all
  partitions. Opt in via `GLOBAL` — table constraint, `CREATE UNIQUE INDEX …
  GLOBAL`, or `ALTER TABLE … ADD CONSTRAINT … GLOBAL`.
- Index-local `partseq` discriminator + `pg_index_partition` catalog (replacing an
  earlier `tableoid`-based scheme), making dump/restore and `pg_upgrade` correct
  by construction.
- DHR deferred coalesced VACUUM drain (`pg_spanning_drainq`,
  `pg_drain_spanning_index()`, autovacuum work-item trigger + launcher sweep).
- Catalog/DDL coverage: ATTACH backfill, DETACH/DROP retirement, REINDEX,
  TRUNCATE re-map, COPY, `pg_dump`/restore, `pg_upgrade`. `amcheck` made safe on
  spanning indexes.

### Fixed
- Cross-partition uniqueness race under concurrency (#34) — a value lock at the
  `_bt_doinsert` choke point.
- HOT key-change corruption (E7) — spanning keys are HOT-blocking on leaves.
- btree page-recycle crash under autovacuum (#35).
- partseq reuse on highest-partition detach (#2); drain-queue cleanup on
  DETACH/DROP (#4); TRUNCATE retirement crash-durability (#8).

See [`PRODUCTION_READINESS.md`](PRODUCTION_READINESS.md) for the full disposition
and [`docs/plans/`](docs/plans) for design records.
