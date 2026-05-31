-- ProgreSQL C1: pg_index_partition partseq allocation, stability, and cleanup.
--
-- partseq is the index-local partition sequence number recorded in
-- pg_index_partition for each partition that joins a spanning index's domain.
-- Properties exercised here:
--   * assigned 1..N as partitions join, on both the CREATE TABLE ... PARTITION
--     OF path and the ALTER TABLE ... ATTACH path;
--   * removed when a partition leaves the tree (DETACH) and when the whole
--     spanning index is dropped, with no dangling pg_class references;
--   * never reused: a partition joining after a DETACH gets a fresh number;
--   * preserved across REINDEX.

CREATE TABLE psq_base (id bigint NOT NULL, ts timestamptz NOT NULL);
CREATE TABLE psq (PRIMARY KEY (id)) INHERITS (psq_base) PARTITION BY RANGE (ts);

-- PARTITION OF path: each new partition gets the next partseq.
CREATE TABLE psq_2024 PARTITION OF psq FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE psq_2025 PARTITION OF psq FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

-- ATTACH path: a standalone (empty) table gets the next partseq.
CREATE TABLE psq_2026 (id bigint NOT NULL, ts timestamptz NOT NULL);
ALTER TABLE psq ATTACH PARTITION psq_2026 FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');

SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

-- Cross-partition uniqueness is enforced over the whole tree.
INSERT INTO psq VALUES (1, '2024-06-01');
INSERT INTO psq VALUES (1, '2025-06-01');  -- duplicate id in another partition: ERROR

-- DETACH removes the departing partition's map row (no dangling reference).
ALTER TABLE psq DETACH PARTITION psq_2025;

SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

-- A partition joining after the DETACH gets a fresh partseq; the detached
-- partition's number (2) is never reused.
CREATE TABLE psq_2027 (id bigint NOT NULL, ts timestamptz NOT NULL);
ALTER TABLE psq ATTACH PARTITION psq_2027 FOR VALUES FROM ('2027-01-01') TO ('2028-01-01');

SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

-- REINDEX preserves the existing partseq assignments.
REINDEX TABLE psq;

SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

-- Dropping the whole tree removes every map row (no dangling pg_class refs).
DROP TABLE psq CASCADE;

SELECT count(*) AS dangling_rows
FROM pg_index_partition
WHERE indpartidxid NOT IN (SELECT oid FROM pg_class)
   OR indpartrelid NOT IN (SELECT oid FROM pg_class);

DROP TABLE psq_base;
DROP TABLE psq_2025;  -- the detached partition, now standalone
