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
$node->stop;
done_testing();
