# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: verify that a cross-partition (spanning) index created with the
# GLOBAL keyword survives pg_upgrade.  pg_upgrade transfers the schema with
# pg_dump --binary-upgrade; if the GLOBAL marker or the indnuniqatts catalog
# flag were lost in that path, the upgraded cluster would silently downgrade the
# spanning index to a plain one and drop cross-partition uniqueness.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $oldnode = PostgreSQL::Test::Cluster->new('old_node');
$oldnode->init;
$oldnode->start;

# A spanning PRIMARY KEY and a standalone spanning UNIQUE index, populated and
# left in place so pg_upgrade has to carry them across.
$oldnode->safe_psql('postgres', <<'SQL');
CREATE TABLE m (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE m_2024 PARTITION OF m FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE m_2025 PARTITION OF m FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO m VALUES (1, '2024-06-01'), (2, '2025-06-01');

CREATE TABLE u (uid int NOT NULL, label text NOT NULL, ts timestamptz NOT NULL)
    PARTITION BY RANGE (ts);
CREATE TABLE u_a PARTITION OF u FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE u_b PARTITION OF u FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE UNIQUE INDEX u_label_uq ON u (label) GLOBAL;
INSERT INTO u VALUES (1, 'x', '2024-03-01'), (2, 'y', '2025-03-01');

-- Churn case: DETACH + re-ATTACH allocates a fresh (monotonic) partseq to
-- c_2024, putting the partseq order out of partition-descriptor (bound) order.
-- The partseq discriminators baked into the transferred index file then do NOT
-- match what a naive partition-descriptor-order rebuild would assign, so
-- pg_upgrade must preserve the exact pg_index_partition map.
CREATE TABLE c (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE c_2024 PARTITION OF c FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE c_2025 PARTITION OF c FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
ALTER TABLE c DETACH PARTITION c_2024;
ALTER TABLE c ATTACH PARTITION c_2024
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
INSERT INTO c VALUES (10, '2024-06-01'), (20, '2025-06-01');
SQL

is( $oldnode->safe_psql('postgres',
		q{SELECT indnuniqatts FROM pg_index WHERE indexrelid = 'm_pkey'::regclass}),
	'1', 'old cluster: spanning PK has indnuniqatts = 1');

# Capture the (non-trivial) source partseq map to compare after upgrade.
my $old_map = $oldnode->safe_psql('postgres',
	q{SELECT string_agg(indpartseq || ':' || indpartrelid::regclass, ',' ORDER BY indpartseq)
	  FROM pg_index_partition WHERE indpartidxid = 'c_pkey'::regclass});

my $newnode = PostgreSQL::Test::Cluster->new('new_node');
$newnode->init;
$newnode->append_conf('postgresql.conf', 'autovacuum = off');

my $oldbindir = $oldnode->config_data('--bindir');
my $newbindir = $newnode->config_data('--bindir');

# pg_upgrade requires both servers stopped; run it from a scratch dir so the
# generated files (delete_old_cluster.sh, pg_upgrade_output.d) land there.
chdir ${PostgreSQL::Test::Utils::tmp_check};
$oldnode->stop;

command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--copy',
	],
	'pg_upgrade of a cluster containing spanning indexes succeeds');

$newnode->start;

# 1. the spanning catalog flag survived the upgrade
is( $newnode->safe_psql('postgres',
		q{SELECT indnuniqatts FROM pg_index WHERE indexrelid = 'm_pkey'::regclass}),
	'1', 'upgraded cluster: spanning PK indnuniqatts survived (= 1)');
is( $newnode->safe_psql('postgres',
		q{SELECT indnuniqatts > 0 FROM pg_index WHERE indexrelid = 'u_label_uq'::regclass}),
	't', 'upgraded cluster: spanning UNIQUE index is still spanning');

# 2. the partitioned data came across
is($newnode->safe_psql('postgres', q{SELECT count(*) FROM m}),
	'2', 'upgraded cluster: partitioned data present');

# 3. cross-partition uniqueness is still enforced after the upgrade
my ($rc, $out, $err) = $newnode->psql('postgres',
	q{INSERT INTO m VALUES (1, '2025-07-01')});
isnt($rc, 0, 'upgraded cluster: cross-partition duplicate PK rejected');
like($err, qr/duplicate key value/, 'PK rejection is a unique violation');

($rc, $out, $err) = $newnode->psql('postgres',
	q{INSERT INTO u VALUES (3, 'x', '2025-07-01')});
isnt($rc, 0, 'upgraded cluster: cross-partition duplicate UNIQUE rejected');

# 4. churn case: the exact partseq map is preserved (not re-derived in
#    partition-descriptor order), and uniqueness is still enforced.
is( $newnode->safe_psql('postgres',
		q{SELECT string_agg(indpartseq || ':' || indpartrelid::regclass, ',' ORDER BY indpartseq)
		  FROM pg_index_partition WHERE indpartidxid = 'c_pkey'::regclass}),
	$old_map,
	'upgraded cluster: pg_index_partition partseq map preserved exactly (DETACH/ATTACH churn)');

($rc, $out, $err) = $newnode->psql('postgres',
	q{INSERT INTO c VALUES (10, '2025-07-01')});
isnt($rc, 0,
	'upgraded cluster: cross-partition duplicate rejected after DETACH/ATTACH churn');

done_testing();
