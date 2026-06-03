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

CREATE TABLE psq (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);

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

-- Detaching the HIGHEST-numbered partition must NOT free its number for reuse.
-- The original MAX(indpartseq)+1 allocator scanned only surviving rows, so
-- detaching the top partition lowered the maximum and the next joiner reused the
-- freed number -- silently aliasing a departed partition's stored partseq to a
-- later one.  The persistent pg_spanning_seq counter is never lowered, so the
-- number is never reused.  (A non-highest DETACH, exercised above, passes either
-- way and so does not cover this; this case is the regression guard.)
ALTER TABLE psq DETACH PARTITION psq_2027;  -- frees the highest partseq (4)

CREATE TABLE psq_2028 (id bigint NOT NULL, ts timestamptz NOT NULL);
ALTER TABLE psq ATTACH PARTITION psq_2028 FOR VALUES FROM ('2028-01-01') TO ('2029-01-01');

-- psq_2028 must get 5 (the counter's high-water mark), never the freed 4.
SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

-- The counter persists the high-water mark independent of surviving rows.
SELECT spseqnext FROM pg_spanning_seq WHERE spseqidxid = 'psq_pkey'::regclass;

-- TRUNCATE re-maps the (still-attached) partition to a FRESH partseq, retiring
-- the old number: the truncated heap's stale spanning entries become
-- unresolvable, which is what makes TRUNCATE crash-durable (a lost LP_DEAD hint
-- can no longer resurrect a *resolving* entry that would alias a reused TID).
-- psq_2024 holds partseq 1; after TRUNCATE it must get 6 (the counter), not keep 1.
TRUNCATE psq_2024;
SELECT indpartseq, indpartrelid::regclass
FROM pg_index_partition
WHERE indpartidxid = 'psq_pkey'::regclass
ORDER BY indpartseq;

SELECT spseqnext FROM pg_spanning_seq WHERE spseqidxid = 'psq_pkey'::regclass;

-- Uniqueness still holds after the re-map: a new key routes to psq_2024 under
-- its fresh partseq and a cross-partition duplicate is still rejected.
INSERT INTO psq VALUES (100, '2024-03-01');
INSERT INTO psq VALUES (100, '2026-03-01');  -- duplicate across partitions: ERROR

-- Dropping the whole tree removes every map row (no dangling pg_class refs),
-- and the counter row too (no dangling spseqidxid).
DROP TABLE psq CASCADE;

SELECT count(*) AS dangling_rows
FROM pg_index_partition
WHERE indpartidxid NOT IN (SELECT oid FROM pg_class)
   OR indpartrelid NOT IN (SELECT oid FROM pg_class);

SELECT count(*) AS dangling_seq_rows
FROM pg_spanning_seq
WHERE spseqidxid NOT IN (SELECT oid FROM pg_class);

DROP TABLE psq_2025;  -- a detached partition, now standalone
DROP TABLE psq_2027;  -- the highest-detached partition, now standalone
