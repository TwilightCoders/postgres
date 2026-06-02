-- DHR coalesced spanning-index drain (E5).
--
-- Exercises spanning_defer_vacuum + pg_drain_spanning_index(): a leaf VACUUM
-- enqueues its dead spanning entries (leaving heap slots LP_DEAD) instead of
-- scanning the whole spanning index, and a single coalesced drain later retires
-- every queued partition's entries and reaps the now-safe slots.  Asserts the
-- deferral is correct: cross-partition uniqueness holds before and after the
-- drain, the reuse trap is closed, and a truly-deleted key can be reinserted
-- once its stale entry is drained.
--
-- autovacuum is disabled on the partitions so the drain queue state is
-- deterministic (nothing drains or re-enqueues behind the test's back).

SET spanning_defer_vacuum = on;

CREATE TABLE dr_base (id bigint NOT NULL, ts timestamptz NOT NULL);
CREATE TABLE dr (PRIMARY KEY (id)) INHERITS (dr_base) PARTITION BY RANGE (ts);
CREATE TABLE dr_a PARTITION OF dr FOR VALUES FROM ('2024-01-01') TO ('2025-01-01')
  WITH (autovacuum_enabled = false);
CREATE TABLE dr_b PARTITION OF dr FOR VALUES FROM ('2025-01-01') TO ('2026-01-01')
  WITH (autovacuum_enabled = false);

-- One live row in each partition; both land at heap TID (0,1) (partition-local
-- address spaces), the classic cross-partition TID collision.
INSERT INTO dr VALUES (10, '2024-06-01');   -- dr_a (0,1)
INSERT INTO dr VALUES (20, '2025-06-01');   -- dr_b (0,1)

-- Delete id=10 in dr_a and VACUUM: under defer this ENQUEUES rather than
-- scanning the spanning index, and holds the dead slot LP_DEAD.
DELETE FROM dr WHERE id = 10;
VACUUM dr_a;
SELECT sdq_partseq, sdq_ndead FROM pg_spanning_drainq ORDER BY sdq_partseq;

-- Insert a new key into dr_a: it must NOT reuse the held LP_DEAD slot (the
-- stale id=10 entry still points there until the drain runs).
INSERT INTO dr VALUES (30, '2024-07-01');

-- Cross-partition uniqueness on the still-live id=20 (dr_b, TID (0,1)) must hold
-- even though dr_a's drain has not run -- the partseq gate keeps the sibling's
-- live entry untouched.
INSERT INTO dr VALUES (20, '2024-08-01');   -- ERROR: duplicate key (id)=(20)

-- Coalesced drain: retire dr_a's stale id=10 entry and reap its slot.
SELECT pg_drain_spanning_index('dr_pkey'::regclass) AS retired;
SELECT count(*) AS drainq_rows FROM pg_spanning_drainq;

-- id=10 was truly deleted and now drained -> reinsert succeeds.
INSERT INTO dr VALUES (10, '2024-09-01');
-- live keys still reject duplicates.
INSERT INTO dr VALUES (20, '2024-10-01');   -- ERROR: duplicate key (id)=(20)
INSERT INTO dr VALUES (30, '2024-11-01');   -- ERROR: duplicate key (id)=(30)
INSERT INTO dr VALUES (10, '2024-12-01');   -- ERROR: duplicate key (id)=(10)

SELECT id, count(*) FROM dr GROUP BY id ORDER BY id;

-- Idempotent: a re-drain with nothing pending retires 0.
SELECT pg_drain_spanning_index('dr_pkey'::regclass) AS retired_again;

DROP TABLE dr CASCADE;
DROP TABLE dr_base;

-- Multi-partition coalescing: dead entries in several partitions are retired in
-- a SINGLE index scan.
CREATE TABLE mc_base (id bigint NOT NULL, k int NOT NULL);
CREATE TABLE mc (PRIMARY KEY (id)) INHERITS (mc_base) PARTITION BY LIST (k);
CREATE TABLE mc0 PARTITION OF mc FOR VALUES IN (0) WITH (autovacuum_enabled = false);
CREATE TABLE mc1 PARTITION OF mc FOR VALUES IN (1) WITH (autovacuum_enabled = false);
CREATE TABLE mc2 PARTITION OF mc FOR VALUES IN (2) WITH (autovacuum_enabled = false);

INSERT INTO mc SELECT g, g % 3 FROM generate_series(1, 90) g;
-- delete the first half: ids 1..45 cycle k=1,2,0,... so all 3 partitions get
-- dead rows (deleting by id%3 would hit only one partition).
DELETE FROM mc WHERE id <= 45;
VACUUM mc0;
VACUUM mc1;
VACUUM mc2;
SELECT count(*) AS dirty_partitions, sum(sdq_ndead) AS total_ndead
  FROM pg_spanning_drainq;

SELECT pg_drain_spanning_index('mc_pkey'::regclass) AS retired;
SELECT count(*) AS drainq_rows FROM pg_spanning_drainq;

-- Deleted ids reinsert (their k routes them back to the same partition);
-- a still-live id rejects a duplicate; no duplicates anywhere.
INSERT INTO mc VALUES (3, 0), (6, 0), (9, 0);   -- were deleted -> success
INSERT INTO mc VALUES (90, 0);                   -- live -> ERROR duplicate (90)
SELECT count(*) AS rows, count(*) FILTER (WHERE cnt > 1) AS dups
  FROM (SELECT id, count(*) cnt FROM mc GROUP BY id) s;

DROP TABLE mc CASCADE;
DROP TABLE mc_base;

-- Negative: draining a non-spanning index is an error.
CREATE TABLE drq_plain (x int PRIMARY KEY);
SELECT pg_drain_spanning_index('drq_plain_pkey'::regclass);
DROP TABLE drq_plain;

-- A spanning leaf WITH a local index must NOT be deferred/drained: the drain
-- reaps every LP_DEAD slot, which would orphan a live local-index entry.  Such
-- leaves use the eager path (no enqueue), so the drain never touches them and
-- the local index stays consistent with the heap.
CREATE TABLE li_base (id bigint NOT NULL, v int NOT NULL);
CREATE TABLE li (PRIMARY KEY (id)) INHERITS (li_base) PARTITION BY LIST ((id % 1));
CREATE TABLE li0 PARTITION OF li FOR VALUES IN (0) WITH (autovacuum_enabled = false);
CREATE INDEX li0_v ON li0 (v);
INSERT INTO li SELECT g, g FROM generate_series(1, 500) g;
DELETE FROM li WHERE id <= 250;
VACUUM li0;                                  -- local-index leaf -> eager path
SELECT count(*) AS drainq_after_local_idx FROM pg_spanning_drainq;   -- expect 0
SELECT pg_drain_spanning_index('li_pkey'::regclass) AS retired;      -- expect 0
INSERT INTO li SELECT g, g FROM generate_series(1, 250) g;           -- reinsert deleted
SET enable_seqscan = off;
SELECT count(*) AS li_index_rows FROM li0 WHERE v BETWEEN 1 AND 250;
RESET enable_seqscan;
SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS li_seq_rows FROM li0 WHERE v BETWEEN 1 AND 250;
RESET enable_indexscan; RESET enable_bitmapscan;
DROP TABLE li CASCADE;
DROP TABLE li_base;

-- Dup-key churn against a spanning index must not crash: the heap-probing
-- pre-split deletion passes are skipped for spanning indexes (their heapRel is
-- the AM-less partitioned root).  Reaching the end without a crash is the test.
CREATE TABLE ch_base (id bigint NOT NULL, pad text);
CREATE TABLE ch (PRIMARY KEY (id)) INHERITS (ch_base) PARTITION BY RANGE (id);
CREATE TABLE ch0 PARTITION OF ch FOR VALUES FROM (0) TO (100000)
  WITH (autovacuum_enabled = false);
DO $$
BEGIN
  FOR i IN 1..8 LOOP
    INSERT INTO ch SELECT g, repeat('x', 40) FROM generate_series(1, 1000) g;
    DELETE FROM ch WHERE id BETWEEN 1 AND 1000;
  END LOOP;
END $$;
SELECT count(*) AS ch_survived FROM ch;
DROP TABLE ch CASCADE;
DROP TABLE ch_base;

RESET spanning_defer_vacuum;
