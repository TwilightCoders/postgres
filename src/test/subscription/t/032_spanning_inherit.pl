# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: logical replication must maintain a spanning (GLOBAL) index on the
# subscriber when the spanned table is a table-INHERITS hierarchy.  Unlike a
# declarative partitioned table (031_spanning), inheritance children are
# *separate* logical-replication relations -- there is no publish_via_partition_
# root -- so the publication lists them via FOR ALL TABLES and the apply worker
# writes each leaf directly.  Each apply (initial COPY sync + streamed
# INSERT/UPDATE) must perform spanning-index maintenance on the subscriber, or
# its spanning index is left empty and cross-leaf uniqueness silently lost.
# The subscriber must still reject a cross-time AND a cross-type duplicate.

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

# Inheritance root + two typed children + time-bucket leaves; one GLOBAL PK on
# the root.  Leaves have no local PK (the spanning index can't be their replica
# identity), so set REPLICA IDENTITY FULL to replicate UPDATEs/DELETEs.
my $ddl = q{
    CREATE TABLE ent (id bigint NOT NULL, ts date NOT NULL, kind text, payload text);
    CREATE TABLE msg (body text) INHERITS (ent);
    CREATE TABLE msg_a (CHECK (ts >= '2024-01-01' AND ts < '2025-01-01')) INHERITS (msg);
    CREATE TABLE msg_b (CHECK (ts >= '2025-01-01' AND ts < '2026-01-01')) INHERITS (msg);
    CREATE TABLE fct (claim text) INHERITS (ent);
    CREATE TABLE fct_a (CHECK (ts >= '2024-01-01' AND ts < '2025-01-01')) INHERITS (fct);
    ALTER TABLE msg_a REPLICA IDENTITY FULL;
    ALTER TABLE msg_b REPLICA IDENTITY FULL;
    ALTER TABLE fct_a REPLICA IDENTITY FULL;
    CREATE UNIQUE INDEX ent_id_g ON ent (id) GLOBAL;
};
$pub->safe_psql('postgres', $ddl);
$sub->safe_psql('postgres', $ddl);

# A pre-existing publisher row in a leaf, carried by initial COPY sync; and one
# in a different typed child to exercise cross-type enforcement.
$pub->safe_psql('postgres', q{
    INSERT INTO msg_a(id,ts,kind,body) VALUES (1,'2024-06-01','m','v0');
    INSERT INTO fct_a(id,ts,kind,claim) VALUES (50,'2024-06-02','f','v0');
});

my $connstr = $pub->connstr . ' dbname=postgres';
$pub->safe_psql('postgres', "CREATE PUBLICATION pub FOR ALL TABLES;");
$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$connstr' PUBLICATION pub;");
$pub->wait_for_catchup('sub');
$sub->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');"
) or die "timed out waiting for initial sync";

my ($rc, $out, $err);

# 1. Initial COPY sync carried the rows AND built the subscriber's spanning
#    index: a local cross-leaf dup on the subscriber must be rejected.
is($sub->safe_psql('postgres', "SELECT count(*) FROM ent"),
	'2', 'subscriber: initial sync carried the rows');
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO msg_b(id,ts,kind,body) VALUES (1,'2025-07-01','m','dup')");
isnt($rc, 0, 'subscriber: cross-time dup rejected after initial COPY sync');
# cross-type: key 50 came in via fct_a; reject it as a msg
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO msg_a(id,ts,kind,body) VALUES (50,'2024-07-01','m','dup')");
isnt($rc, 0, 'subscriber: cross-type dup rejected after initial COPY sync');

# 2. Streamed INSERT (apply_handle_insert -> ExecSimpleRelationInsert).
$pub->safe_psql('postgres', "INSERT INTO msg_b(id,ts,kind,body) VALUES (2,'2025-06-01','m','v0');");
$pub->wait_for_catchup('sub');
is($sub->safe_psql('postgres', "SELECT count(*) FROM ent WHERE id = 2"),
	'1', 'subscriber: streamed insert applied');
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO msg_a(id,ts,kind,body) VALUES (2,'2024-07-01','m','dup')");
isnt($rc, 0, 'subscriber: cross-leaf dup rejected after streamed INSERT');

# 3. Streamed cold UPDATE churn (apply_handle_update -> ExecSimpleRelationUpdate).
$pub->safe_psql('postgres',
	q{DO $$ BEGIN FOR i IN 1..300 LOOP UPDATE msg_a SET payload = 'v'||i WHERE id = 1; END LOOP; END $$;}
);
$pub->wait_for_catchup('sub');
($rc, $out, $err) =
  $sub->psql('postgres', "INSERT INTO msg_b(id,ts,kind,body) VALUES (1,'2025-08-01','m','dup')");
isnt($rc, 0, 'subscriber: cross-leaf dup rejected after replicated UPDATE churn');

# No duplicates anywhere on the subscriber.
is($sub->safe_psql('postgres',
		"SELECT count(*) FROM (SELECT id FROM ent GROUP BY id HAVING count(*) > 1) d"),
	'0', 'subscriber: no cross-leaf duplicates after replication');

$sub->stop('fast');
$pub->stop('fast');
done_testing();
