# ProgreSQL: spanning UNIQUE vs DETACH PARTITION ... CONCURRENTLY.
#
# The non-concurrent companion (spanning-detach.spec) covers DETACH under the
# root AccessExclusiveLock, where a colliding insert simply block-waits for the
# DETACH to commit or abort.  This spec covers the CONCURRENTLY path, which
# takes only ShareUpdateExclusiveLock on the root and runs in two transactions
# with a wait-for-snapshots barrier between them.  Spanning-entry retirement for
# the detached partition is deferred to the final detach transaction's
# pre-commit, so the invariant under test is: the detached partition's global
# key stays enforced until the detach actually completes, and is freed
# (reusable in another partition) exactly once it does -- with cross-partition
# uniqueness never violated in between.
#
# DETACH ... CONCURRENTLY cannot run inside a transaction block, so the detacher
# session (s2) uses autocommit steps; the technique follows the upstream
# detach-partition-concurrently-*.spec suite.

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
  DROP TABLE IF EXISTS spans_a;   -- standalone after a completed detach
}

# detacher: DETACH ... CONCURRENTLY must be autocommit (no surrounding BEGIN)
session s2
step det   { ALTER TABLE spans DETACH PARTITION spans_a CONCURRENTLY; }

# colliding inserter / snapshot holder
session s1
step s1b    { BEGIN; }
step s1read { SELECT count(*) FROM spans; }
# the same global key (1) into the OTHER partition, spans_b
step ins_b  { INSERT INTO spans VALUES (1, '2025-06-01'); }
step s1c    { COMMIT; }

session checker
step show { SELECT tableoid::regclass AS part, id FROM spans ORDER BY id, part;
           SELECT relpartbound IS NULL AS spans_a_detached
             FROM pg_class WHERE relname = 'spans_a'; }

# (1) Retirement-on-concurrent-detach: a completed concurrent detach removes
# spans_a (and its key 1) from the table, so the same key then inserts cleanly
# into spans_b.
permutation det ins_b show

# (2) Enforced-while-attached: with spans_a still present the key is enforced,
# so inserting key 1 into spans_b collides; the later detach then removes
# spans_a, leaving no key 1 anywhere.
permutation ins_b det show

# (3) Wait-then-complete: the detacher must wait for s1's open snapshot before
# it can finish.  Once s1 commits the detach completes, the key is retired, and
# the post-detach insert of key 1 into spans_b succeeds.
permutation s1b s1read det s1c ins_b show
