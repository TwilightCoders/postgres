--
-- progresql_ddl: DDL lifecycle for spanning indexes
--
-- Verifies the spanning index correctly tracks cross-partition uniqueness
-- across all DDL paths that mutate the partition tree:
--   - DROP partition
--   - DETACH partition (non-concurrent)
--   - COPY (executor INSERT path)
--   - REINDEX
--   - ATTACH partition (with pre-existing rows)
--   - CASCADE DROP cleanup
--
-- The cleanup path is keyed on (key, tableoid), not on heap TID, so two
-- partitions whose first row both live at heap TID (0,1) do not collide.
-- REINDEX repopulates the spanning index from leaf partitions after the
-- empty rebuild on the partitioned root.  ATTACH backfills the spanning
-- index with pre-existing rows in the attaching partition.
--

-- Setup
CREATE TABLE pgddl_base (id bigint, kind text, ts timestamptz NOT NULL);

CREATE TABLE pgddl_data (
    PRIMARY KEY (id)
) INHERITS (pgddl_base)
  PARTITION BY RANGE (ts);

CREATE TABLE pgddl_2023 PARTITION OF pgddl_data
    FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');
CREATE TABLE pgddl_2024 PARTITION OF pgddl_data
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE pgddl_2025 PARTITION OF pgddl_data
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

-- Each partition's first row lands at heap TID (0,1), so the cleanup
-- callback used to false-positive across partitions.  Now it filters
-- by tableoid so only the targeted partition's entries are removed.
INSERT INTO pgddl_data VALUES (1, 'a', '2023-06-15 00:00:00+00');  -- pgddl_2023, TID (0,1)
INSERT INTO pgddl_data VALUES (2, 'b', '2024-06-15 00:00:00+00');  -- pgddl_2024, TID (0,1)
INSERT INTO pgddl_data VALUES (3, 'c', '2025-06-15 00:00:00+00');  -- pgddl_2025, TID (0,1)

-- Confirm cross-partition uniqueness before any DDL.
BEGIN;
INSERT INTO pgddl_data VALUES (1, 'dup', '2025-09-01 00:00:00+00');
ROLLBACK;

-- Section 1: DROP partition
-- Spanning entries for the dropped partition are removed; entries for
-- OTHER partitions (which share heap TIDs with the dropped one) are NOT
-- touched.

DROP TABLE pgddl_2023;
SELECT count(*) FROM pgddl_data;  -- 2 rows remain (id=2, id=3)

-- id=1 is re-insertable: only (1, pgddl_2023.OID) was cleaned.
INSERT INTO pgddl_data VALUES (1, 'a2', '2024-08-01 00:00:00+00');
SELECT id, kind FROM pgddl_data ORDER BY id;

-- (2, pgddl_2024.OID) and (3, pgddl_2025.OID) were preserved despite
-- sharing heap TID (0,1) with pgddl_2023's removed entry.
BEGIN;
INSERT INTO pgddl_data VALUES (2, 'dup-tid-collision', '2025-10-01 00:00:00+00');
ROLLBACK;
BEGIN;
INSERT INTO pgddl_data VALUES (3, 'dup-tid-collision', '2025-11-01 00:00:00+00');
ROLLBACK;

-- Section 2: DETACH partition (non-concurrent)
-- Cleanup runs before pg_inherits is updated, so get_partition_ancestors
-- still resolves the parent root and the index entries are correctly
-- removed without affecting other partitions.

INSERT INTO pgddl_data VALUES (2, 'b2', '2024-09-01 00:00:00+00');  -- pgddl_2024, TID (0,2)
INSERT INTO pgddl_data VALUES (3, 'c2', '2025-09-01 00:00:00+00');  -- pgddl_2025, TID (0,2)
SELECT count(*) FROM pgddl_data;  -- 5 rows

ALTER TABLE pgddl_data DETACH PARTITION pgddl_2024;
SELECT count(*) FROM pgddl_data;   -- pgddl_2024 rows removed from parent view
SELECT count(*) FROM pgddl_2024;   -- rows still exist in detached table

-- id=1 and id=2 (formerly in pgddl_2024) are re-insertable in the parent.
-- pgddl_2025 entries with overlapping heap TIDs are unaffected.
INSERT INTO pgddl_data VALUES (1, 'a3', '2025-02-01 00:00:00+00');
INSERT INTO pgddl_data VALUES (2, 'b3', '2025-03-01 00:00:00+00');
SELECT id, kind FROM pgddl_data ORDER BY id;

DROP TABLE pgddl_2024;

-- Section 3: COPY populates spanning index correctly
-- COPY routes each tuple through the executor INSERT path, which in turn
-- calls ExecInsertSpanningIndexTuples to update the root's spanning index.

COPY pgddl_data (id, kind, ts) FROM STDIN;
10	copy-a	2025-07-01 00:00:00+00
11	copy-b	2025-08-01 00:00:00+00
\.

SELECT count(*) FROM pgddl_data;

-- COPYed rows are in the spanning index; same-partition duplicate rejected.
BEGIN;
INSERT INTO pgddl_data VALUES (10, 'dup-copy', '2025-09-01 00:00:00+00');
ROLLBACK;

-- Section 4: REINDEX repopulates the spanning index from partitions
-- index_build leaves the rebuilt index empty (the partitioned root has no
-- storage), so reindex_index calls BuildSpanningIndexFromPartitions to
-- re-insert (key, tableoid) entries for every live row in every leaf
-- partition.  Cross-partition uniqueness is preserved across REINDEX.

REINDEX INDEX pgddl_data_pkey;

-- Both of these should fail: the spanning index is fully repopulated.
BEGIN;
INSERT INTO pgddl_data VALUES (10, 'post-reindex-dup', '2025-08-01 00:00:00+00');
ROLLBACK;
BEGIN;
INSERT INTO pgddl_data VALUES (1, 'post-reindex-dup2', '2025-04-01 00:00:00+00');
ROLLBACK;

SELECT count(*) FROM pgddl_data;

-- Section 5: ATTACH PARTITION backfills spanning index
-- AttachPartitionEnsureIndexes correctly skips spanning indexes (they live
-- only on the root), and ATExecAttachPartition then walks the attaching
-- partition's heap and inserts (user_columns..., attachOid) into each
-- spanning index.  Pre-existing rows are visible to cross-partition
-- uniqueness immediately after attach.

CREATE TABLE pgddl_attach (
    id bigint NOT NULL,
    kind text,
    ts timestamptz NOT NULL,
    CONSTRAINT pgddl_attach_pkey PRIMARY KEY (id)
);
INSERT INTO pgddl_attach VALUES (20, 'pre-attach', '2026-03-01 00:00:00+00');

ALTER TABLE pgddl_data ATTACH PARTITION pgddl_attach
    FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');

-- id=20 was inserted before attach; the backfill picks it up and a
-- duplicate insert is correctly rejected.
BEGIN;
INSERT INTO pgddl_data VALUES (20, 'gap-dup', '2025-10-01 00:00:00+00');
ROLLBACK;

-- New inserts AFTER attach are covered by the same spanning index.
INSERT INTO pgddl_data VALUES (21, 'post-attach', '2026-04-01 00:00:00+00');
BEGIN;
INSERT INTO pgddl_data VALUES (21, 'dup-post-attach', '2025-10-01 00:00:00+00');
ROLLBACK;

-- Section 6: CASCADE DROP cleans up all objects.
DROP TABLE pgddl_data CASCADE;
DROP TABLE pgddl_base;

SELECT COUNT(*) FROM pg_class WHERE relname LIKE 'pgddl%';
