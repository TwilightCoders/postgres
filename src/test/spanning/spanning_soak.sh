#!/usr/bin/env bash
#-------------------------------------------------------------------------
# spanning_soak.sh -- concurrency + corruption stress harness for ProgreSQL
#                     spanning (cross-partition GLOBAL UNIQUE/PK) indexes.
#
# This is the scale/soak counterpart to the deterministic suites (regress,
# isolation, recovery/subscription/pg_upgrade TAP).  It follows the PostgreSQL
# practice of "stress, then verify with an independent oracle": a functional
# test only catches what it asserts, so silent corruption is caught by running
# amcheck AND an application-level uniqueness invariant after the load -- ideally
# against an --enable-cassert build.
#
# It spins up its OWN throwaway cluster, hammers a spanning-indexed partitioned
# table with concurrent INSERT / batch-INSERT / HOT-UPDATE / DELETE /
# cross-partition-MOVE, then verifies:
#   1. no cross-partition duplicates on EITHER spanning key (an int PK and a
#      text UNIQUE -- the text key exercises the collation-aware value-lock hash)
#   2. amcheck bt_index_check + bt_index_parent_check on every spanning index
#   3. (optional --crash) the above still hold after a kill -9 + crash recovery
#      mid-load, on a durable (fsync) cluster.
#
# Exit status is 0 only if every oracle passes.
#
# Usage:
#   spanning_soak.sh [--clients N] [--jobs N] [--secs N] [--partitions N]
#                    [--idspace N] [--crash] [--keep] [--bindir DIR]
#
# Defaults: 12 clients, 4 jobs, 60s, 8 partitions, idspace 5000.
#
# src/test/spanning/spanning_soak.sh
#-------------------------------------------------------------------------
set -uo pipefail

# Defaults are tuned to be a reliable corruption DETECTOR, not a gentle soak: a
# tight key space + high client count maximizes same-user-key cross-partition
# collisions (the #34 race).  At these settings the pre-fix code fails within
# ~30s; widen --idspace / drop --clients for a milder, more "realistic" soak.
CLIENTS=16; JOBS=4; SECS=60; PARTS=4; IDSPACE=500; CRASH=0; KEEP=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="$SCRIPT_DIR/../../../build/install/bin"

while [ $# -gt 0 ]; do
  case "$1" in
    --clients) CLIENTS=$2; shift 2;;
    --jobs) JOBS=$2; shift 2;;
    --secs) SECS=$2; shift 2;;
    --partitions) PARTS=$2; shift 2;;
    --idspace) IDSPACE=$2; shift 2;;
    --crash) CRASH=1; shift;;
    --keep) KEEP=1; shift;;
    --bindir) BINDIR=$2; shift 2;;
    *) echo "unknown arg: $1"; exit 2;;
  esac
done

BINDIR="$(cd "$BINDIR" && pwd)" || { echo "bindir not found"; exit 2; }
PG_CTL="$BINDIR/pg_ctl"; INITDB="$BINDIR/initdb"; PSQL="$BINDIR/psql"; PGBENCH="$BINDIR/pgbench"
for b in "$PG_CTL" "$INITDB" "$PSQL" "$PGBENCH"; do
  [ -x "$b" ] || { echo "missing binary: $b"; exit 2; }
done

RUN="/tmp/spanning_soak.$$"
DATA="$RUN/data"; SOCK="$RUN/sock"; LOG="$RUN/pg.log"
mkdir -p "$SOCK"

cleanup() {
  "$PG_CTL" -D "$DATA" stop -m immediate >/dev/null 2>&1
  # reap any orphaned aux child still pinning the shmem segment
  for p in $(ps -o pid,command 2>/dev/null | grep "$DATA" | grep -v grep | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
  done
  [ "$KEEP" = "1" ] || rm -rf "$RUN"
}
trap cleanup EXIT

psql_q() { "$PSQL" -h "$SOCK" -U volte -d postgres -tA "$@"; }

start_cluster() {
  local durable_opts=""
  if [ "$CRASH" = "1" ]; then
    durable_opts="-c fsync=on -c synchronous_commit=on -c full_page_writes=on"
  else
    durable_opts="-c fsync=off -c synchronous_commit=off"
  fi
  # autovacuum OFF on purpose: under heavy churn a spanning user-key's dead
  # entries must be allowed to accumulate so the run grows across btree pages --
  # that is exactly the geometry the cross-partition uniqueness race needs.  With
  # aggressive autovacuum the run is kept on one page (where the stock page lock
  # already serializes), masking the bug.  A real workload hits this transiently
  # between vacuum cycles; the harness just makes it reliable.
  "$PG_CTL" -D "$DATA" \
    -o "-c unix_socket_directories=$SOCK -c listen_addresses='' $durable_opts \
        -c deadlock_timeout=50ms -c max_connections=64 \
        -c autovacuum=off" \
    -l "$LOG" start -w >/dev/null 2>&1
}

wait_ready() {
  local i
  for i in $(seq 1 60); do psql_q -c "select 1" >/dev/null 2>&1 && return 0; sleep 0.5; done
  echo "cluster did not become ready"; tail -20 "$LOG"; return 1
}

echo "=== ProgreSQL spanning soak: clients=$CLIENTS jobs=$JOBS secs=$SECS parts=$PARTS idspace=$IDSPACE crash=$CRASH ==="

"$INITDB" -D "$DATA" -U volte -A trust >/dev/null 2>&1 || { echo "initdb failed"; exit 1; }
start_cluster; wait_ready || exit 1

# warn if not a cassert build (assertions are a primary oracle)
if psql_q -c "show debug_assertions" 2>/dev/null | grep -q on; then
  echo "  (cassert build: assertions active -- good)"
else
  echo "  WARNING: not an --enable-cassert build; assertion oracle is disabled"
fi

# ---- schema: two spanning indexes (int PK + text UNIQUE), N LIST partitions ----
{
  echo "DROP TABLE IF EXISTS st;"
  echo "CREATE TABLE st (id int NOT NULL, tag text NOT NULL, part int NOT NULL,"
  echo "    payload int NOT NULL DEFAULT 0,"
  echo "    PRIMARY KEY (id) GLOBAL, UNIQUE (tag) GLOBAL) PARTITION BY LIST (part);"
  for p in $(seq 0 $((PARTS-1))); do
    echo "CREATE TABLE st_$p PARTITION OF st FOR VALUES IN ($p);"
  done
  echo "DROP TABLE IF EXISTS soakcfg;"
  echo "CREATE TABLE soakcfg (idspace int, parts int);"
  echo "INSERT INTO soakcfg VALUES ($IDSPACE, $PARTS);"
  cat <<'PLPGSQL'
CREATE OR REPLACE FUNCTION op() RETURNS void LANGUAGE plpgsql AS $$
DECLARE
  cfg soakcfg; k int; p int; rnd float8;
BEGIN
  SELECT * INTO cfg FROM soakcfg LIMIT 1;
  k := 1 + (random()*(cfg.idspace-1))::int;
  p := (random()*(cfg.parts-1))::int;
  rnd := random();
  -- Mix weighted toward the dup-producing ops (INSERT + cross-partition MOVE of
  -- a contended key) so the harness reliably exercises the cross-partition
  -- uniqueness race; DELETE/batch/HOT are lighter, for path coverage.
  IF rnd < 0.38 THEN                                 -- INSERT (single)  ~38%
    BEGIN INSERT INTO st(id,tag,part) VALUES (k,'t'||k,p);
    EXCEPTION WHEN unique_violation THEN NULL; END;
  ELSIF rnd < 0.76 THEN                              -- cross-partition MOVE ~38%
    BEGIN UPDATE st SET part = p WHERE id = k;
    EXCEPTION WHEN unique_violation THEN NULL; END;
  ELSIF rnd < 0.90 THEN                              -- DELETE  ~14%
    DELETE FROM st WHERE id = k;
  ELSIF rnd < 0.96 THEN                              -- batch INSERT (multi-row) ~6%
    BEGIN
      INSERT INTO st(id,tag,part)
      SELECT g, 't'||g, (random()*(cfg.parts-1))::int
      FROM generate_series(k, k+4) g
      ON CONFLICT DO NOTHING;
    EXCEPTION WHEN unique_violation THEN NULL; END;
  ELSE                                               -- HOT update (non-key)  ~4%
    UPDATE st SET payload = payload + 1 WHERE id = k;
  END IF;
END $$;
PLPGSQL
} | "$PSQL" -h "$SOCK" -U volte -d postgres -q -v ON_ERROR_STOP=1 >/tmp/spanning_soak_setup.$$ 2>&1 \
  || { echo "schema setup failed"; cat /tmp/spanning_soak_setup.$$; exit 1; }
rm -f /tmp/spanning_soak_setup.$$
echo "SELECT op();" > "$RUN/op.sql"

run_load() {   # $1 = duration
  "$PGBENCH" -h "$SOCK" -U volte -d postgres -n -f "$RUN/op.sql" \
    -c "$CLIENTS" -j "$JOBS" -T "$1" 2>&1 | \
    grep -E "tps|number of (transactions|failed)" | sed 's/^/  pgbench: /'
}

verify() {     # echoes FAIL lines; returns nonzero on any failure
  local rc=0
  local dup_id dup_tag
  dup_id=$(psql_q -c "SELECT count(*) FROM (SELECT id FROM st GROUP BY id HAVING count(*)>1) d;")
  dup_tag=$(psql_q -c "SELECT count(*) FROM (SELECT tag FROM st GROUP BY tag HAVING count(*)>1) d;")
  echo "  invariant: duplicate ids=$dup_id  duplicate tags=$dup_tag"
  [ "$dup_id" = "0" ] || { echo "  FAIL: cross-partition duplicate id(s)"; rc=1; }
  [ "$dup_tag" = "0" ] || { echo "  FAIL: cross-partition duplicate tag(s)"; rc=1; }
  # amcheck every spanning index
  psql_q -c "CREATE EXTENSION IF NOT EXISTS amcheck;" >/dev/null 2>&1
  local idx
  for idx in $(psql_q -c "SELECT indexrelid::regclass::text FROM pg_index WHERE indrelid='st'::regclass AND indnuniqatts>0 ORDER BY 1;"); do
    if "$PSQL" -h "$SOCK" -U volte -d postgres -v ON_ERROR_STOP=1 -qc \
         "SELECT bt_index_check('$idx'::regclass); SELECT bt_index_parent_check('$idx'::regclass);" >/tmp/amck.$$ 2>&1; then
      echo "  amcheck $idx: OK"
    else
      echo "  FAIL: amcheck $idx"; cat /tmp/amck.$$ | sed 's/^/    /'; rc=1
    fi
    rm -f /tmp/amck.$$
  done
  return $rc
}

RC=0
if [ "$CRASH" = "1" ]; then
  echo "--- crash-torture: load, kill -9 mid-run, recover, verify ---"
  run_load "$SECS" &
  LOADPID=$!
  sleep $((SECS/2))
  PM=$(head -1 "$DATA/postmaster.pid")
  echo "  kill -9 postmaster ($PM) mid-load"
  kill -9 "$PM" 2>/dev/null
  for p in $(ps -o pid,command | grep "$DATA" | grep -v grep | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
  wait "$LOADPID" 2>/dev/null
  rm -f "$DATA/postmaster.pid"
  echo "  restarting (crash recovery)..."
  start_cluster; wait_ready || exit 1
  echo "  recovered; verifying..."
  verify || RC=1
else
  run_load "$SECS"
  verify || RC=1
fi

DEADLOCKS=$(grep -c "deadlock detected" "$LOG" 2>/dev/null); DEADLOCKS=${DEADLOCKS:-0}
CRASHES=$(grep -cE "was terminated by signal (11|6)|TRAP: failed" "$LOG" 2>/dev/null); CRASHES=${CRASHES:-0}
echo "  deadlocks=$DEADLOCKS  unexpected-crashes=$CRASHES  rows=$(psql_q -c 'SELECT count(*) FROM st;')"
[ "$CRASHES" = "0" ] || { echo "  FAIL: backend crash during run"; RC=1; }

if [ "$RC" = "0" ]; then echo "=== PASS ==="; else echo "=== FAIL ==="; fi
exit $RC
