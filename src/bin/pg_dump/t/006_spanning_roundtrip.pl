# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: verify that a cross-partition (spanning) PRIMARY KEY / UNIQUE
# created with the GLOBAL keyword survives a pg_dump -> restore round-trip.
# The dumped DDL must re-emit GLOBAL (and must NOT leak the internal partseq /
# tableoid discriminator column), and the restored database must still enforce
# cross-partition uniqueness.  A regression here is the one real silent
# data-loss path: a leaked discriminator restores as a plain index and quietly
# drops cross-partition uniqueness.
#
# It also covers a FOREIGN KEY that references a spanning INHERITS root, which is
# enforced by cloning the constraint once per leaf plus a base constraint on the
# root.  pg_dump must emit ONLY the base FK (the clones are derived); restore
# must re-clone to the IDENTICAL fanout.  That round-trip determinism is what an
# earlier investigation mis-read as "the same base FK produced different clone
# counts" -- it was two different schemas, not non-determinism.  The in-process
# determinism is pinned in regress/sql/progresql_fk_clone.sql; this is the half
# that needs a real dump/restore.

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

-- A FOREIGN KEY referencing a spanning INHERITS root.  node has two leaves, so
-- the FK is enforced by a base constraint (-> node) plus one clone per leaf
-- (-> node_a, -> node_b): fanout == 3.  pg_dump must emit only the base FK.
CREATE TABLE node (id bigint NOT NULL, ts timestamptz NOT NULL);
CREATE TABLE node_a () INHERITS (node);
CREATE TABLE node_b () INHERITS (node);
CREATE UNIQUE INDEX node_id_g ON node (id) GLOBAL;
INSERT INTO node_a VALUES (10, '2024-01-01');
INSERT INTO node_b VALUES (11, '2025-01-01');
CREATE TABLE noderef (rid int PRIMARY KEY,
    nid bigint REFERENCES node(id) ON DELETE CASCADE);
INSERT INTO noderef VALUES (1, 10), (2, 11);
SQL

# the source fans the FK out to base + one clone per leaf (3)
is( $src->safe_psql('postgres',
		q{SELECT count(*) FROM pg_constraint WHERE contype = 'f' AND conname LIKE 'noderef_nid_fkey%'}),
	'3', 'source: FK over spanning inheritance root fans out to base + one clone per leaf');

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

# the FK over a spanning inheritance root is dumped as exactly ONE base FK
# (referencing the root); the per-leaf clones are derived and must NOT be dumped
# independently, or restore would double them.
my @fk_base = ($txt =~ /FOREIGN KEY \(nid\) REFERENCES (?:public\.)?node\(id\)/g);
is(scalar @fk_base, 1,
	'FK over a spanning inheritance root is dumped as exactly one base constraint');
unlike($txt, qr/FOREIGN KEY \(nid\) REFERENCES (?:public\.)?node_[ab]\b/,
	'dump does not emit the per-leaf FK clones (they are derived)');

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

# the FK re-clones to the IDENTICAL fanout after restore (3), not a different
# count -- the round-trip determinism, proven across a real dump/restore.
is( $dst->safe_psql('postgres',
		q{SELECT count(*) FROM pg_constraint WHERE contype = 'f' AND conname LIKE 'noderef_nid_fkey%'}),
	'3', 'FK over spanning inheritance root re-clones to the identical fanout after restore');
($rc, $out, $err) = $dst->psql('postgres',
	q{INSERT INTO noderef VALUES (3, 88888)});
isnt($rc, 0,
	'restored FK over spanning inheritance root still rejects a missing parent');

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
