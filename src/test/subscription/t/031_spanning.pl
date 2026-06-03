# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: logical replication must maintain spanning (GLOBAL) indexes on the
# subscriber.  The apply worker writes through ExecSimpleRelationInsert/Update
# and initial table sync through CopyFrom -- none of which is ExecInsert/
# ExecUpdate -- so each must perform spanning-index maintenance itself.
# Otherwise a subscriber's spanning index is left empty and cross-partition
# uniqueness is silently lost there.  This test replicates INSERTs (initial COPY
# sync + streaming) and a heavy cold UPDATE into a spanning-indexed partitioned
# table, then proves the subscriber STILL rejects a cross-partition duplicate --
# i.e. its spanning index really was maintained.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $pub = PostgreSQL::Test::Cluster->new('publisher');
$pub->init(allows_streaming => 'logical');
$pub->start;
my $sub = PostgreSQL::Test::Cluster->new('subscriber');
$sub->init;
$sub->start;

my $ddl = q{
    CREATE TABLE t (id bigint NOT NULL, ts timestamptz NOT NULL, payload text,
        PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
    CREATE TABLE t_a PARTITION OF t FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
    CREATE TABLE t_b PARTITION OF t FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
    -- A spanning (GLOBAL) PK lives on the root; the leaves have no local PK, so
    -- it cannot serve as their replica identity.  Use FULL so UPDATEs replicate.
    ALTER TABLE t_a REPLICA IDENTITY FULL;
    ALTER TABLE t_b REPLICA IDENTITY FULL;
};
$pub->safe_psql('postgres', $ddl);
$sub->safe_psql('postgres', $ddl);

# A pre-existing publisher row, carried to the subscriber by initial COPY sync.
$pub->safe_psql('postgres', "INSERT INTO t VALUES (1, '2024-06-01', 'v0');");

my $connstr = $pub->connstr . ' dbname=postgres';
$pub->safe_psql('postgres', "CREATE PUBLICATION pub FOR ALL TABLES;");
$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$connstr' PUBLICATION pub;");
$pub->wait_for_catchup('sub');
$sub->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');"
) or die "timed out waiting for initial sync";

my ($rc, $out, $err);

# 1. Initial COPY sync carried the row AND maintained the spanning index: a
#    local cross-partition duplicate on the subscriber must be rejected.
is($sub->safe_psql('postgres', "SELECT count(*) FROM t"),
	'1', 'subscriber: initial sync carried the row');
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO t VALUES (1, '2025-07-01', 'dup')");
isnt($rc, 0, 'subscriber: cross-partition dup rejected after initial COPY sync');
like($err, qr/duplicate key value/, 'COPY-sync maintained the spanning index');

# 2. Streaming INSERT (apply_handle_insert -> ExecSimpleRelationInsert).
$pub->safe_psql('postgres', "INSERT INTO t VALUES (2, '2025-06-01', 'v0');");
$pub->wait_for_catchup('sub');
is($sub->safe_psql('postgres', "SELECT count(*) FROM t WHERE id = 2"),
	'1', 'subscriber: streamed insert applied');
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO t VALUES (2, '2024-07-01', 'dup')");
isnt($rc, 0,
	'subscriber: cross-partition dup rejected after streamed INSERT');

# 3. Streaming cold UPDATE churn (apply_handle_update -> ExecSimpleRelationUpdate).
#    Enough non-key updates to force off-page (cold) moves on the subscriber.
$pub->safe_psql('postgres',
	q{DO $$ BEGIN FOR i IN 1..300 LOOP UPDATE t SET payload = 'v'||i WHERE id = 1; END LOOP; END $$;}
);
$pub->wait_for_catchup('sub');
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO t VALUES (1, '2025-08-01', 'dup')");
isnt($rc, 0,
	'subscriber: cross-partition dup rejected after replicated UPDATE churn');

# No duplicates anywhere on the subscriber.
is( $sub->safe_psql(
		'postgres',
		"SELECT count(*) FROM (SELECT id FROM t GROUP BY id HAVING count(*) > 1) d"
	),
	'0',
	'subscriber: no cross-partition duplicates after replication');

$sub->stop('fast');
$pub->stop('fast');
done_testing();
