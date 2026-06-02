# ProgreSQL: cross-partition ("spanning") UNIQUE enforcement under concurrency.
#
# A spanning index is ONE shared btree on the partitioned root, so two sessions
# inserting the same key into DIFFERENT leaf partitions both land on the same
# index pages.  Cross-partition uniqueness is therefore enforced by reusing
# btree's page-locked _bt_check_unique: the second inserter block-waits on the
# first inserter's XID, then either raises a duplicate-key error (first
# committed) or proceeds (first aborted).  This spec proves that claim -- the
# property the README "correct by reuse" reassurance rests on.

setup
{
  CREATE TABLE spans (id bigint NOT NULL, ts timestamptz NOT NULL,
      PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
  CREATE TABLE spans_2024 PARTITION OF spans
      FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
  CREATE TABLE spans_2025 PARTITION OF spans
      FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
}

teardown
{
  DROP TABLE spans;
}

session s1
setup     { BEGIN; }
# routes to spans_2024
step i1   { INSERT INTO spans VALUES (1, '2024-06-01'); }
step c1   { COMMIT; }
step a1   { ABORT; }

session s2
setup     { BEGIN; }
# same global key (1), routes to the OTHER partition spans_2025
step i2   { INSERT INTO spans VALUES (1, '2025-06-01'); }
# same global key (1) routed to the SAME partition as s1 (intra-partition guard)
step i2s  { INSERT INTO spans VALUES (1, '2024-09-01'); }
step c2   { COMMIT; }

session checker
step show { SELECT id, ts FROM spans ORDER BY id, ts; }

# Cross-partition conflict, first inserter COMMITS: s2 blocks on s1, then must
# fail with a duplicate-key violation -> exactly one row survives.
permutation i1 i2 c1 c2 show

# Cross-partition conflict, first inserter ABORTS: s2's blocked insert proceeds
# and commits -> the surviving row is s2's.
permutation i1 i2 a1 c2 show

# Intra-partition conflict (regression guard): spanning must not have weakened
# ordinary same-partition uniqueness under concurrency.
permutation i1 i2s c1 c2 show
