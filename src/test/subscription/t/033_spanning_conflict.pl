# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# ProgreSQL: a cross-partition (spanning / GLOBAL) uniqueness conflict hit by the
# logical-replication apply worker must be CLASSIFIED as a conflict
# (confl_insert_exists / confl_update_exists in pg_stat_subscription_stats), the
# same as an ordinary unique index -- not raised as a raw, unclassified error.
#
# A spanning leaf has no local unique index; the only thing enforcing uniqueness
# is the spanning index on the partitioned root, which the apply worker maintains
# out-of-band (ExecInsertSpanningIndexTuples).  Before the fix that path raised a
# bare unique violation OUTSIDE CheckAndReportConflict, so the apply worker logged
# an opaque apply_error and retried forever -- the conflict never reached PG18's
# conflict detection (all confl_* stayed 0), so it was neither observable nor
# resolvable.  This test pins parity with a normal unique index: both classify.
#
# A control table with an ordinary PRIMARY KEY (on its own subscription) proves
# the harness actually observes classification; the spanning table (on its own
# subscription) is the regression under test.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $pub = PostgreSQL::Test::Cluster->new('publisher');
$pub->init(allows_streaming => 'logical');
$pub->start;
my $sub = PostgreSQL::Test::Cluster->new('subscriber');
$sub->init(allows_streaming => 'logical');
$sub->start;

# Control: an ordinary table with a normal PRIMARY KEY.
# Spanning: a partitioned table whose PRIMARY KEY is GLOBAL (spanning); its leaves
# carry no local unique index, so uniqueness is enforced only by the root's
# spanning index -- and cross-partition, so a leaf-local conflict check can't see
# it.  RI FULL on the leaves (the spanning PK can't be a replica identity).
my $ddl = q{
    CREATE TABLE c (id int PRIMARY KEY, v text);

    CREATE TABLE t (id bigint NOT NULL, ts timestamptz NOT NULL, v text,
        PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
    CREATE TABLE t_a PARTITION OF t FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
    CREATE TABLE t_b PARTITION OF t FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
    ALTER TABLE t_a REPLICA IDENTITY FULL;
    ALTER TABLE t_b REPLICA IDENTITY FULL;
};
$pub->safe_psql('postgres', $ddl);
$sub->safe_psql('postgres', $ddl);

# One publication + subscription per table so each conflict counter is isolated.
$pub->safe_psql('postgres', "CREATE PUBLICATION p_ctrl FOR TABLE c;");
$pub->safe_psql('postgres', "CREATE PUBLICATION p_span FOR TABLE t;");
my $connstr = $pub->connstr . ' dbname=postgres';
$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION s_ctrl CONNECTION '$connstr' PUBLICATION p_ctrl;");
$sub->safe_psql('postgres',
	"CREATE SUBSCRIPTION s_span CONNECTION '$connstr' PUBLICATION p_span;");
$sub->wait_for_subscription_sync($pub, 's_ctrl');
$sub->wait_for_subscription_sync($pub, 's_span');

# Wait until the apply worker for $subname has attempted the conflicting change
# (apply_error_count moves off zero regardless of whether the conflict is
# classified), then return the confl_insert_exists counter.  Pre-fix the spanning
# path bumps only apply_error_count; post-fix it also bumps confl_insert_exists.
sub confl_insert_after_attempt
{
	my ($subname) = @_;
	$sub->poll_query_until('postgres',
		"SELECT apply_error_count > 0 OR confl_insert_exists > 0 FROM pg_stat_subscription_stats WHERE subname = '$subname'"
	) or die "apply worker for $subname never attempted the conflicting change";
	return $sub->safe_psql('postgres',
		"SELECT confl_insert_exists FROM pg_stat_subscription_stats WHERE subname = '$subname'"
	);
}

# --- Control: a normal INSERT/INSERT conflict is classified (vanilla PG18). ---
# A local row, then a replicated insert of the same key -> insert_exists.
$sub->safe_psql('postgres', "INSERT INTO c VALUES (1, 'local');");
$pub->safe_psql('postgres', "INSERT INTO c VALUES (1, 'remote');");
cmp_ok(confl_insert_after_attempt('s_ctrl'), '>=', 1,
	'control: ordinary unique conflict is classified as confl_insert_exists');

# --- Spanning: a CROSS-PARTITION conflict must be classified the same way. ---
# Local id=100 in t_a; a replicated insert of id=100 into t_b collides only on the
# root spanning index.  Pre-fix this is an unclassified apply_error; post-fix it
# is confl_insert_exists.
$sub->safe_psql('postgres', "INSERT INTO t VALUES (100, '2024-06-01', 'local');");
$pub->safe_psql('postgres', "INSERT INTO t VALUES (100, '2025-06-01', 'remote');");
cmp_ok(confl_insert_after_attempt('s_span'), '>=', 1,
	'spanning: cross-partition conflict is classified as confl_insert_exists');

$sub->stop('fast');
$pub->stop('fast');
done_testing();
