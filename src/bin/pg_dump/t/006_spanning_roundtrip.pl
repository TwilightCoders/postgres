# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: verify that a cross-partition (spanning) PRIMARY KEY / UNIQUE
# created with the GLOBAL keyword survives a pg_dump -> restore round-trip.
# The dumped DDL must re-emit GLOBAL (and must NOT leak the internal partseq /
# tableoid discriminator column), and the restored database must still enforce
# cross-partition uniqueness.  A regression here is the one real silent
# data-loss path: a leaked discriminator restores as a plain index and quietly
# drops cross-partition uniqueness.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $src = PostgreSQL::Test::Cluster->new('src');
$src->init;
$src->start;

# A spanning PRIMARY KEY (table constraint) and a standalone spanning UNIQUE
# index -- the two distinct dump paths (dumpConstraint and pg_get_indexdef).
$src->safe_psql('postgres', <<'SQL');
CREATE TABLE events (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE events_2024 PARTITION OF events
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE events_2025 PARTITION OF events
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
INSERT INTO events VALUES (1, '2024-06-01'), (2, '2025-06-01');

CREATE TABLE tag (tag_id int NOT NULL, label text NOT NULL, ts timestamptz NOT NULL)
    PARTITION BY RANGE (ts);
CREATE TABLE tag_a PARTITION OF tag FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE tag_b PARTITION OF tag FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
CREATE UNIQUE INDEX tag_label_uq ON tag (label) GLOBAL;
INSERT INTO tag VALUES (1, 'x', '2024-03-01'), (2, 'y', '2025-03-01');
SQL

# 1. plain-text dump and inspect the emitted SQL
my $plain = "$tempdir/plain.sql";
$src->command_ok([ 'pg_dump', '--no-sync', '-f', $plain, 'postgres' ],
	'plain dump succeeds');
my $txt = slurp_file($plain);
like($txt, qr/PRIMARY KEY \(id\) GLOBAL/,
	'spanning PRIMARY KEY is dumped with the GLOBAL marker');
like($txt, qr/CREATE UNIQUE INDEX tag_label_uq .*GLOBAL/,
	'standalone spanning UNIQUE index is dumped with the GLOBAL marker');
unlike($txt, qr/\bpartseq\b/,
	'dump does not leak the internal partseq discriminator column');
unlike($txt, qr/PRIMARY KEY \(id, tableoid\)/,
	'dump does not leak a tableoid discriminator into the key list');

# 2. restore into a fresh cluster
my $dst = PostgreSQL::Test::Cluster->new('dst');
$dst->init;
$dst->start;
$dst->command_ok(
	[ 'psql', '-v', 'ON_ERROR_STOP=1', '-f', $plain, 'postgres' ],
	'restore of plain dump into a fresh cluster succeeds');

# 3. the restored catalog still marks both indexes spanning
is( $dst->safe_psql('postgres',
		q{SELECT indnuniqatts > 0 FROM pg_index WHERE indexrelid = 'events_pkey'::regclass}),
	't', 'restored PRIMARY KEY is still a spanning index (indnuniqatts > 0)');
is( $dst->safe_psql('postgres',
		q{SELECT indnuniqatts > 0 FROM pg_index WHERE indexrelid = 'tag_label_uq'::regclass}),
	't', 'restored UNIQUE index is still a spanning index (indnuniqatts > 0)');

# 4. cross-partition uniqueness is still enforced after restore -- the real
#    durability check.  id=1 already lives in the 2024 partition; re-inserting
#    it into the 2025 partition must be rejected.
my ($rc, $out, $err) = $dst->psql('postgres',
	q{INSERT INTO events VALUES (1, '2025-07-01')});
isnt($rc, 0, 'cross-partition duplicate PRIMARY KEY rejected after restore');
like($err, qr/duplicate key value/,
	'PRIMARY KEY rejection is a unique violation');

($rc, $out, $err) = $dst->psql('postgres',
	q{INSERT INTO tag VALUES (3, 'x', '2025-07-01')});
isnt($rc, 0, 'cross-partition duplicate UNIQUE rejected after restore');

# 5. custom-format dump + pg_restore round-trip enforces uniqueness too
my $custom = "$tempdir/custom.dump";
$src->command_ok([ 'pg_dump', '-Fc', '--no-sync', '-f', $custom, 'postgres' ],
	'custom-format dump succeeds');
my $dst2 = PostgreSQL::Test::Cluster->new('dst2');
$dst2->init;
$dst2->start;
$dst2->command_ok([ 'pg_restore', '-d', 'postgres', $custom ],
	'pg_restore of custom-format dump succeeds');
($rc, $out, $err) = $dst2->psql('postgres',
	q{INSERT INTO events VALUES (2, '2024-07-01')});
isnt($rc, 0,
	'cross-partition duplicate rejected after pg_restore (custom format)');

done_testing();
