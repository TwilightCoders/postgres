# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: crash recovery of cross-partition ("spanning") unique indexes.
#
# The fork adds no custom WAL or resource managers: a spanning index is an
# ordinary btree on the partitioned root, and cross-partition uniqueness reuses
# btree's page-locked _bt_check_unique.  Crash recovery is therefore expected to
# be correct "by reuse" of the standard btree+heap WAL.  This test crashes the
# server with an immediate stop and, after WAL replay, asserts:
#   1. committed rows and their spanning-index entries survive the crash;
#   2. cross-partition uniqueness is still enforced (no duplicate slips in);
#   3. an in-flight (uncommitted) insert is rolled back by recovery and leaves
#      no phantom entry that blocks or duplicates a later insert of that key;
#   4. the pg_index_partition discriminator map is intact after recovery.
#
# A second scenario covers finding #8: TRUNCATE of a spanning partition re-maps
# it to a fresh partseq (a WAL-logged catalog change), so after a crash that may
# lose the non-durable LP_DEAD retirement hint, the truncated heap's stale
# entries are unresolvable and cannot alias a reused TID in the refilled heap.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('span_crash');
$node->init;
$node->start;

# A spanning PRIMARY KEY across two range partitions, populated and committed.
$node->safe_psql(
	'postgres', q{
    CREATE TABLE s (id bigint NOT NULL, ts timestamptz NOT NULL,
        PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
    CREATE TABLE s_a PARTITION OF s
        FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
    CREATE TABLE s_b PARTITION OF s
        FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
    INSERT INTO s SELECT g, '2024-06-01'::timestamptz
        FROM generate_series(1, 500) g;        -- keys 1..500   -> s_a
    INSERT INTO s SELECT g, '2025-06-01'::timestamptz
        FROM generate_series(501, 1000) g;     -- keys 501..1000 -> s_b
});

is( $node->safe_psql('postgres', 'SELECT count(*) FROM s'),
	'1000', 'committed rows present before crash');

# Sanity: cross-partition uniqueness is enforced before the crash (key 1 lives
# in s_a; inserting it into s_b must fail).
my ($pre_ret, $pre_out, $pre_err) = $node->psql('postgres',
	q{INSERT INTO s VALUES (1, '2025-03-01')});
isnt($pre_ret, 0, 'pre-crash: cross-partition duplicate rejected');
like($pre_err, qr/duplicate key value/, 'pre-crash: duplicate-key error');

# Open an UNCOMMITTED insert of a brand-new key (2000) into s_b in a background
# session and never commit it.  The crash must discard it, leaving no phantom
# index entry.  (A new key does not conflict, so the INSERT succeeds in-txn.)
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN');
$bg->query_safe(q{INSERT INTO s VALUES (2000, '2025-07-01')});

# Crash: immediate stop performs no shutdown checkpoint, so recovery must replay
# WAL from the last checkpoint.  The background session dies with the server.
$node->stop('immediate');
$node->start;

# 1. Committed rows survived WAL replay; the uncommitted key 2000 did not.
is( $node->safe_psql('postgres', 'SELECT count(*) FROM s'),
	'1000', 'post-crash: committed rows survived, uncommitted insert rolled back');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM s WHERE id = 2000'),
	'0', 'post-crash: uncommitted key 2000 absent');

# 2. Cross-partition uniqueness still enforced after recovery.
my ($d_ret, $d_out, $d_err) = $node->psql('postgres',
	q{INSERT INTO s VALUES (1, '2025-04-01')});
isnt($d_ret, 0, 'post-crash: cross-partition duplicate still rejected');
like($d_err, qr/duplicate key value/, 'post-crash: duplicate-key error preserved');

# 3. No phantom entry: the rolled-back key 2000 is freely re-insertable.
is( $node->safe_psql('postgres',
		q{INSERT INTO s VALUES (2000, '2025-07-01') RETURNING id}),
	'2000', 'post-crash: rolled-back key 2000 re-insertable (no phantom entry)');

# ...and a freshly re-inserted key is itself enforced (entry really landed).
my ($r_ret, $r_out, $r_err) = $node->psql('postgres',
	q{INSERT INTO s VALUES (2000, '2024-08-01')});
isnt($r_ret, 0, 'post-crash: re-inserted key 2000 now enforced cross-partition');

# 4. The discriminator map survived: one row per partition for s_pkey.
is( $node->safe_psql(
		'postgres', q{
		SELECT count(DISTINCT indpartrelid)
		  FROM pg_index_partition
		 WHERE indpartidxid = 's_pkey'::regclass}),
	'2', 'post-crash: pg_index_partition has a row per partition');

# A distinct new key in either partition still works (index fully functional).
is( $node->safe_psql('postgres',
		q{INSERT INTO s VALUES (5000, '2024-09-01') RETURNING id}),
	'5000', 'post-crash: distinct new key insertable');

$bg->quit;

# ---------------------------------------------------------------------------
# Finding #8: TRUNCATE retirement is crash-durable.
#
# TRUNCATE of a spanning partition resets the partition's relfilenode (heap TIDs
# restart at (0,1)) and must retire the partition's old spanning entries.  If
# retirement were only a non-durable LP_DEAD page hint AND the partition kept its
# partseq, a crash that lost the hint would resurrect a stale entry that still
# RESOLVED to the refilled partition and aliased a reused TID -> a spurious
# duplicate-key error (or silently lost enforcement).  The fix re-maps the
# partition to a fresh partseq -- a WAL-logged catalog change -- so any
# resurrected stale entry (keyed on the old, now-unmapped partseq) is
# unresolvable and is harmlessly skipped by _bt_check_unique.
# ---------------------------------------------------------------------------
$node->safe_psql(
	'postgres', q{
    CREATE TABLE t (id bigint NOT NULL, ts timestamptz NOT NULL,
        PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
    CREATE TABLE t_a PARTITION OF t
        FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
    CREATE TABLE t_b PARTITION OF t
        FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
    INSERT INTO t SELECT g, '2024-06-01'::timestamptz FROM generate_series(1, 100) g;
    INSERT INTO t SELECT g, '2025-06-01'::timestamptz FROM generate_series(101, 200) g;
});

my $partseq_before = $node->safe_psql(
	'postgres', q{
    SELECT indpartseq FROM pg_index_partition
     WHERE indpartidxid = 't_pkey'::regclass AND indpartrelid = 't_a'::regclass});

# TRUNCATE t_a (commits the re-map to a fresh partseq), then crash with no
# shutdown checkpoint so the deferred LP_DEAD retirement hint is not guaranteed
# on disk -- exactly the window finding #8 depends on.
$node->safe_psql('postgres', 'TRUNCATE t_a');
$node->stop('immediate');
$node->start;

# The re-map to a fresh partseq is a committed catalog change, so it survived
# WAL replay (the old number is retired, never reused).
my $partseq_after = $node->safe_psql(
	'postgres', q{
    SELECT indpartseq FROM pg_index_partition
     WHERE indpartidxid = 't_pkey'::regclass AND indpartrelid = 't_a'::regclass});
isnt($partseq_after, $partseq_before,
	'post-crash: TRUNCATE re-mapped the partition to a fresh partseq (durable)');

is($node->safe_psql('postgres', 'SELECT count(*) FROM t_a'),
	'0', 'post-crash: truncated partition is empty');
is($node->safe_psql('postgres', 'SELECT count(*) FROM t_b'),
	'100', 'post-crash: sibling partition intact');

# THE TEST: refilling the previously-truncated keys must succeed.  A resurrected
# stale entry (if the LP_DEAD hint was lost) is keyed on the old, now-unmapped
# partseq, so it is unresolvable and cannot alias the reused (0,1..) TIDs.
my ($ti_ret, $ti_out, $ti_err) = $node->psql('postgres',
	q{INSERT INTO t SELECT g, '2024-07-01'::timestamptz FROM generate_series(1, 100) g});
is($ti_ret, 0,
	'post-crash: refill of truncated keys succeeds (no spurious unique violation)');

# Cross-partition uniqueness is still enforced on the refilled partition.
my ($tu_ret, $tu_out, $tu_err) = $node->psql('postgres',
	q{INSERT INTO t VALUES (1, '2025-08-01')});
isnt($tu_ret, 0,
	'post-crash: cross-partition duplicate rejected after truncate+refill');
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'200', 'post-crash: row counts correct after truncate+refill');

$node->stop;
done_testing();
