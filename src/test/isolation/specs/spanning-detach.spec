# ProgreSQL: spanning UNIQUE vs concurrent DETACH PARTITION.
#
# Non-concurrent DETACH takes AccessExclusiveLock on the partitioned root, so a
# concurrent insert routed through the root block-waits until the DETACH
# commits or aborts.  Spanning-entry retirement is deferred to the detaching
# transaction's pre-commit, so the waiter must observe:
#   - DETACH COMMITS -> the detached partition's key is freed; the waiter's
#     insert of that key into another partition succeeds.
#   - DETACH ABORTS  -> the partition stays attached; the key stays enforced;
#     the waiter's insert fails with a duplicate-key violation.
# This is the concurrency companion to the abort-safety regress coverage in
# progresql_ddl (Section 6).

setup
{
  CREATE TABLE spans (id bigint NOT NULL, ts timestamptz NOT NULL,
      PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
  CREATE TABLE spans_a PARTITION OF spans
      FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
  CREATE TABLE spans_b PARTITION OF spans
      FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
  INSERT INTO spans VALUES (1, '2024-06-01');   -- key 1 lives in spans_a
}

teardown
{
  DROP TABLE spans;
  DROP TABLE IF EXISTS spans_a;   -- standalone after a committed detach
}

session s1
setup     { BEGIN; }
step d1   { ALTER TABLE spans DETACH PARTITION spans_a; }
step c1   { COMMIT; }
step a1   { ABORT; }

session s2
setup     { BEGIN; }
# insert the same global key (1) into the OTHER partition, spans_b
step i2   { INSERT INTO spans VALUES (1, '2025-06-01'); }
step c2   { COMMIT; }

session checker
step show { SELECT id, ts FROM spans ORDER BY id, ts; }

# DETACH commits: spans_a (and its key 1) leaves the table; the blocked insert
# then succeeds -> key 1 now lives only in spans_b.
permutation d1 i2 c1 c2 show

# DETACH aborts: spans_a stays attached, key 1 stays enforced; the blocked
# insert fails -> the one surviving row is the original in spans_a.
permutation d1 i2 a1 c2 show
