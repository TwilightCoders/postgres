# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: crash recovery of a spanning (GLOBAL) index over a table-INHERITS
# hierarchy ("inherit at the top, bucket the leaves").  A spanning index on an
# inheritance root is an ordinary btree maintained through the standard
# heap+btree WAL, exactly like the declarative case (049_spanning_crash); this
# test asserts the same crash-safety properties hold when the leaves are
# inheritance children rather than declarative partitions:
#   1. committed rows + their spanning entries survive replay;
#   2. cross-leaf uniqueness (across time buckets AND across typed children) is
#      still enforced after recovery;
#   3. an in-flight (uncommitted) insert is rolled back, leaving no phantom entry;
#   4. the pg_index_partition discriminator map is intact after recovery.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('span_inherit_crash');
$node->init;
$node->start;

# Inheritance root (ent) with two typed children (msg, fct), each bucketed by
# time via inheritance children; one GLOBAL PK on the root spans them all.
$node->safe_psql(
	'postgres', q{
    CREATE TABLE ent (id bigint NOT NULL, ts date NOT NULL, kind text);
    CREATE TABLE msg (body text) INHERITS (ent);
    CREATE TABLE msg_a (CHECK (ts >= '2024-01-01' AND ts < '2025-01-01')) INHERITS (msg);
    CREATE TABLE msg_b (CHECK (ts >= '2025-01-01' AND ts < '2026-01-01')) INHERITS (msg);
    CREATE TABLE fct (claim text) INHERITS (ent);
    CREATE TABLE fct_a (CHECK (ts >= '2024-01-01' AND ts < '2025-01-01')) INHERITS (fct);
    INSERT INTO msg_a(id,ts,kind,body) SELECT g,'2024-06-01','m','v' FROM generate_series(1,400) g;
    INSERT INTO msg_b(id,ts,kind,body) SELECT g,'2025-06-01','m','v' FROM generate_series(401,800) g;
    INSERT INTO fct_a(id,ts,kind,claim) SELECT g,'2024-07-01','f','v' FROM generate_series(801,1000) g;
    CREATE UNIQUE INDEX ent_id_g ON ent (id) GLOBAL;
});

is($node->safe_psql('postgres', 'SELECT count(*) FROM ent'),
	'1000', 'committed rows present before crash');

# Pre-crash: cross-leaf uniqueness enforced (key 1 lives in msg_a).
my ($pre_ret, $pre_out, $pre_err) = $node->psql('postgres',
	q{INSERT INTO msg_b(id,ts,kind,body) VALUES (1,'2025-03-01','m','dup')});
isnt($pre_ret, 0, 'pre-crash: cross-leaf duplicate rejected');

# Capture the discriminator-map size for this index (root + intermediates +
# leaves all get a partseq); compare it after recovery rather than hardcoding.
my $map_before = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_index_partition WHERE indpartidxid = 'ent_id_g'::regclass});

# Uncommitted insert of a brand-new key into a leaf; the crash must discard it.
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN');
$bg->query_safe(q{INSERT INTO msg_b(id,ts,kind,body) VALUES (5000,'2025-07-01','m','phantom')});

$node->stop('immediate');
$node->start;

# 1. Committed rows survived; uncommitted key 5000 did not.
is($node->safe_psql('postgres', 'SELECT count(*) FROM ent'),
	'1000', 'post-crash: committed rows survived, uncommitted insert rolled back');
is($node->safe_psql('postgres', 'SELECT count(*) FROM ent WHERE id = 5000'),
	'0', 'post-crash: uncommitted key 5000 absent');

# 2a. Cross-leaf (cross-time) uniqueness still enforced.
my ($d_ret, $d_out, $d_err) = $node->psql('postgres',
	q{INSERT INTO msg_b(id,ts,kind,body) VALUES (1,'2025-04-01','m','dup')});
isnt($d_ret, 0, 'post-crash: cross-time duplicate still rejected');

# 2b. Cross-TYPE uniqueness still enforced (key 801 is a fct; reject as a msg).
my ($t_ret, $t_out, $t_err) = $node->psql('postgres',
	q{INSERT INTO msg_a(id,ts,kind,body) VALUES (801,'2024-08-01','m','dup')});
isnt($t_ret, 0, 'post-crash: cross-type duplicate still rejected');

# 3. No phantom entry: the rolled-back key 5000 is freely re-insertable, and
#    once re-inserted it is itself enforced.
is($node->safe_psql('postgres',
		q{INSERT INTO msg_b(id,ts,kind,body) VALUES (5000,'2025-07-01','m','real') RETURNING id}),
	'5000', 'post-crash: rolled-back key 5000 re-insertable (no phantom entry)');
my ($r_ret, $r_out, $r_err) = $node->psql('postgres',
	q{INSERT INTO msg_a(id,ts,kind,body) VALUES (5000,'2024-08-01','m','dup')});
isnt($r_ret, 0, 'post-crash: re-inserted key 5000 now enforced cross-leaf');

# 4. The discriminator map survived intact.
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_index_partition WHERE indpartidxid = 'ent_id_g'::regclass}),
	$map_before, 'post-crash: pg_index_partition map intact');

# No duplicates anywhere.
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM (SELECT id FROM ent GROUP BY id HAVING count(*) > 1) d}),
	'0', 'post-crash: no cross-leaf duplicates');

$bg->quit;
$node->stop;
done_testing();
