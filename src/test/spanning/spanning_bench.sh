#!/usr/bin/env bash
#-------------------------------------------------------------------------
# spanning_bench.sh -- fork-vs-vanilla performance benchmark for ProgreSQL
#                      spanning (cross-partition GLOBAL UNIQUE/PK) indexes.
#
# The soak harness (spanning_soak.sh) answers "is it CORRECT under load?".
# This answers the orthogonal question: "what does it COST?".  Two axes:
#
#   (a) DO NO HARM -- does merely carrying / using the fork regress an
#       ordinary PostgreSQL workload?  This is the number that matters for
#       upstream credibility: the fork must be ~free when you don't use the
#       feature.  Measured by running an IDENTICAL workload on a vanilla
#       build and the fork build and comparing.
#         A. builtin pgbench TPC-B-like (no partitioning, no spanning)
#         B. INSERT into a LIST-partitioned table with an ordinary LOCAL
#            (per-partition) unique PK
#
#   (b) FEATURE COST -- what does a spanning GLOBAL unique index cost over
#       the ordinary local one?  This isolates the LOCKTAG_SPANNING_KEY value
#       lock + the descend-twice insert path.  Vanilla cannot create a GLOBAL
#       index, so this is fork-internal: spanning vs local, both on the fork.
#         C. INSERT into the same partitioned table with a GLOBAL PK
#
# IMPORTANT: run this against OPTIMIZED builds (NOT --enable-cassert /
# --enable-debug).  Assertion/debug builds make the numbers meaningless.
# The script refuses to compare if either build reports debug_assertions=on.
#
# Each engine gets its own throwaway cluster (only one runs at a time, so the
# two never contend).  Results are collected into a TSV and rendered as a set
# of markdown comparison tables (vanilla vs fork, with Δ%).
#
# Usage:
#   spanning_bench.sh --vanilla-bin DIR --fork-bin DIR
#                     [--clients-list "1 4 8 16 32"] [--secs N] [--warmup N]
#                     [--scale N] [--jobs N] [--partitions N] [--outfile FILE]
#
# Defaults: clients "1 4 8 16 32", 60s measure, 15s warmup, scale 25,
#           8 jobs, 4 partitions, results to stdout.
#
# src/test/spanning/spanning_bench.sh
#-------------------------------------------------------------------------
set -uo pipefail

VAN_BIN=""; FORK_BIN=""
CLIENTS_LIST="1 4 8 16 32"; SECS=60; WARMUP=15; SCALE=25; JOBS=8
PARTS=4; OUTFILE=""
IDSPACE=1000000000   # huge so random INSERTs almost never collide (~pure insert path)

while [ $# -gt 0 ]; do
  case "$1" in
    --vanilla-bin) VAN_BIN=$2; shift 2;;
    --fork-bin) FORK_BIN=$2; shift 2;;
    --clients-list) CLIENTS_LIST=$2; shift 2;;
    --secs) SECS=$2; shift 2;;
    --warmup) WARMUP=$2; shift 2;;
    --scale) SCALE=$2; shift 2;;
    --jobs) JOBS=$2; shift 2;;
    --partitions) PARTS=$2; shift 2;;
    --outfile) OUTFILE=$2; shift 2;;
    *) echo "unknown arg: $1"; exit 2;;
  esac
done

[ -n "$VAN_BIN" ] && [ -n "$FORK_BIN" ] || { echo "need --vanilla-bin and --fork-bin"; exit 2; }
for b in "$VAN_BIN/postgres" "$FORK_BIN/postgres" "$VAN_BIN/pgbench" "$FORK_BIN/pgbench"; do
  [ -x "$b" ] || { echo "missing binary: $b"; exit 2; }
done

WHO="$(id -un)"
TSV="$(mktemp /tmp/spanning_bench.XXXXXX.tsv)"   # scenario \t engine \t clients \t tps \t lat_ms
RUN=""   # current cluster run dir (set by start_cluster)

emit() { if [ -n "$OUTFILE" ]; then echo "$@" >>"$OUTFILE"; else echo "$@"; fi; }

# ---- one throwaway, perf-tuned cluster per engine (identical settings) ----
start_cluster() {   # $1 = bindir ; sets RUN/DATA/SOCK/PORT globals
  local bindir=$1
  RUN="$(mktemp -d /tmp/spanning_bench_cl.XXXXXX)"
  DATA="$RUN/data"; SOCK="$RUN/sock"; LOG="$RUN/pg.log"
  mkdir -p "$SOCK"
  "$bindir/initdb" -D "$DATA" -U "$WHO" -A trust >/dev/null 2>&1 || { echo "initdb failed"; exit 1; }
  # Identical, perf-oriented config on BOTH engines so the only difference is the
  # code.  fsync off: we are measuring CPU/lock overhead, not disk; off lowers
  # variance and the fork adds work on the CPU/lock side, not the IO side.
  # autovacuum off: a 60s window shouldn't be perturbed by background vacuum.
  "$bindir/pg_ctl" -D "$DATA" -l "$LOG" -w start \
    -o "-c unix_socket_directories=$SOCK -c listen_addresses='' \
        -c fsync=off -c synchronous_commit=off -c full_page_writes=off \
        -c shared_buffers=2GB -c max_connections=128 -c max_wal_size=16GB \
        -c checkpoint_timeout=60min -c autovacuum=off" >/dev/null 2>&1 || {
    echo "cluster start failed"; tail -20 "$LOG"; exit 1; }
}
stop_cluster() {
  "$bindir_cur/pg_ctl" -D "$DATA" stop -m immediate >/dev/null 2>&1
  rm -rf "$RUN"
}
PSQL_BIN=""; PGBENCH_BIN=""; bindir_cur=""
sql() { "$PSQL_BIN" -h "$SOCK" -U "$WHO" -d postgres -tA "$@"; }

assert_off() {   # refuse to benchmark a cassert/debug build
  local da; da=$(sql -c "show debug_assertions" 2>/dev/null)
  if [ "$da" = "on" ]; then
    echo "REFUSING: $1 build has debug_assertions=on (cassert) -- benchmark would be meaningless"; exit 1
  fi
}

# ---- run pgbench once: warm up, then measure; append a TSV row ----
# $1=scenario $2=engine $3=clients $4=mode(builtin|file) $5=scriptfile(if file)
measure() {
  local scen=$1 eng=$2 c=$3 mode=$4 script=${5:-}
  local args=(-h "$SOCK" -U "$WHO" -d postgres -n -j "$JOBS" -c "$c")
  [ "$mode" = "file" ] && args+=(-f "$script")
  # warmup (discarded)
  "$PGBENCH_BIN" "${args[@]}" -T "$WARMUP" >/dev/null 2>&1
  # measured run
  local out; out=$("$PGBENCH_BIN" "${args[@]}" -T "$SECS" 2>&1)
  local tps lat
  tps=$(echo "$out" | awk '/tps = /{print $3; exit}')
  lat=$(echo "$out" | awk '/latency average/{print $4; exit}')
  tps=${tps:-NaN}; lat=${lat:-NaN}
  printf '%s\t%s\t%s\t%s\t%s\n' "$scen" "$eng" "$c" "$tps" "$lat" >>"$TSV"
  echo "  [$eng] $scen c=$c -> tps=$tps lat=${lat}ms"
}

# ---- scenario schemas (custom INSERT workloads B and C) ----
# B: partitioned table, ordinary LOCAL unique PK (works on vanilla AND fork)
mk_local_part() {
  { echo "DROP TABLE IF EXISTS bpart;"
    echo "CREATE TABLE bpart (id bigint NOT NULL, p int NOT NULL, PRIMARY KEY (id,p)) PARTITION BY LIST (p);"
    for q in $(seq 0 $((PARTS-1))); do echo "CREATE TABLE bpart_$q PARTITION OF bpart FOR VALUES IN ($q);"; done
  } | "$PSQL_BIN" -h "$SOCK" -U "$WHO" -d postgres -q -v ON_ERROR_STOP=1
}
# C: same shape but a GLOBAL (spanning) PK -- fork only
mk_spanning_part() {
  { echo "DROP TABLE IF EXISTS spart;"
    echo "CREATE TABLE spart (id bigint NOT NULL, p int NOT NULL, PRIMARY KEY (id) GLOBAL) PARTITION BY LIST (p);"
    for q in $(seq 0 $((PARTS-1))); do echo "CREATE TABLE spart_$q PARTITION OF spart FOR VALUES IN ($q);"; done
  } | "$PSQL_BIN" -h "$SOCK" -U "$WHO" -d postgres -q -v ON_ERROR_STOP=1
}
# pgbench insert scripts (random id over a huge space -> negligible collisions)
write_scripts() {
  cat >"$RUN/ins_local.sql" <<SQL
\set id random(1, $IDSPACE)
\set p random(0, $((PARTS-1)))
INSERT INTO bpart(id,p) VALUES (:id,:p) ON CONFLICT DO NOTHING;
SQL
  cat >"$RUN/ins_spanning.sql" <<SQL
\set id random(1, $IDSPACE)
\set p random(0, $((PARTS-1)))
INSERT INTO spart(id,p) VALUES (:id,:p) ON CONFLICT DO NOTHING;
SQL
}

run_engine() {   # $1=bindir $2=engine-label  ($3=fork? "1" enables spanning scenario)
  local bindir=$1 eng=$2 is_fork=${3:-0}
  bindir_cur=$bindir; PSQL_BIN="$bindir/psql"; PGBENCH_BIN="$bindir/pgbench"
  start_cluster "$bindir"
  assert_off "$eng"
  echo "=== engine=$eng ($("$bindir/postgres" --version)) ==="
  write_scripts

  # Scenario A: builtin TPC-B-like (do-no-harm, the headline normal workload)
  "$PGBENCH_BIN" -h "$SOCK" -U "$WHO" -d postgres -i -s "$SCALE" -q >/dev/null 2>&1
  for c in $CLIENTS_LIST; do measure "A_tpcb" "$eng" "$c" builtin; done

  # Scenario B: partitioned local-unique INSERT (do-no-harm on the partitioned path)
  for c in $CLIENTS_LIST; do
    mk_local_part   # fresh empty table each run
    measure "B_part_local" "$eng" "$c" file "$RUN/ins_local.sql"
  done

  # Scenario C: partitioned spanning INSERT (feature cost) -- fork only
  if [ "$is_fork" = "1" ]; then
    for c in $CLIENTS_LIST; do
      mk_spanning_part
      measure "C_part_spanning" "$eng" "$c" file "$RUN/ins_spanning.sql"
    done
  fi

  stop_cluster
}

echo "=== spanning_bench: secs=$SECS warmup=$WARMUP scale=$SCALE jobs=$JOBS parts=$PARTS clients='$CLIENTS_LIST' ==="
run_engine "$VAN_BIN"  vanilla 0
run_engine "$FORK_BIN" fork    1

# ---- render markdown tables from the TSV ----
val() { awk -F'\t' -v s="$1" -v e="$2" -v c="$3" -v col="$4" '$1==s&&$2==e&&$3==c{print $col}' "$TSV"; }
pct() { awk -v a="$1" -v b="$2" 'BEGIN{ if(a==""||b==""||a=="NaN"||b=="NaN"||a+0==0){print "n/a"} else {printf "%+.1f%%", (b-a)/a*100} }'; }

emit ""
emit "## ProgreSQL spanning benchmark — fork vs vanilla"
emit ""
emit "Opt builds (assertions off). ${SECS}s measured (+${WARMUP}s warmup discarded), scale ${SCALE}, ${PARTS} partitions, jobs ${JOBS}. fsync/synchronous_commit off (isolating CPU/lock overhead). tps = excludes connection time; lat = mean latency (ms)."

emit ""
emit "### (a) Do no harm — builtin pgbench TPC-B-like (no partitioning, no spanning)"
emit "_Δ% is fork relative to vanilla; ~0% is the goal (the fork must be free when unused)._"
emit ""
emit "| clients | vanilla tps | fork tps | Δ tps | vanilla lat | fork lat |"
emit "|--------:|------------:|---------:|------:|------------:|---------:|"
for c in $CLIENTS_LIST; do
  vt=$(val A_tpcb vanilla "$c" 4); ft=$(val A_tpcb fork "$c" 4)
  vl=$(val A_tpcb vanilla "$c" 5); fl=$(val A_tpcb fork "$c" 5)
  emit "| $c | ${vt:-?} | ${ft:-?} | $(pct "$vt" "$ft") | ${vl:-?} | ${fl:-?} |"
done

emit ""
emit "### (a) Do no harm — INSERT into LIST-partitioned table, ordinary LOCAL unique PK"
emit ""
emit "| clients | vanilla tps | fork tps | Δ tps | vanilla lat | fork lat |"
emit "|--------:|------------:|---------:|------:|------------:|---------:|"
for c in $CLIENTS_LIST; do
  vt=$(val B_part_local vanilla "$c" 4); ft=$(val B_part_local fork "$c" 4)
  vl=$(val B_part_local vanilla "$c" 5); fl=$(val B_part_local fork "$c" 5)
  emit "| $c | ${vt:-?} | ${ft:-?} | $(pct "$vt" "$ft") | ${vl:-?} | ${fl:-?} |"
done

emit ""
emit "### (b) Feature cost — spanning (GLOBAL) PK vs local PK, both on the fork"
emit "_Δ% is spanning relative to the fork's own local-PK insert (the LOCKTAG_SPANNING_KEY + descend-twice cost)._"
emit ""
emit "| clients | fork local tps | fork spanning tps | Δ tps | local lat | spanning lat |"
emit "|--------:|---------------:|------------------:|------:|----------:|-------------:|"
for c in $CLIENTS_LIST; do
  lt=$(val B_part_local fork "$c" 4); st=$(val C_part_spanning fork "$c" 4)
  ll=$(val B_part_local fork "$c" 5); sl=$(val C_part_spanning fork "$c" 5)
  emit "| $c | ${lt:-?} | ${st:-?} | $(pct "$lt" "$st") | ${ll:-?} | ${sl:-?} |"
done
emit ""
emit "_Raw TSV: scenario, engine, clients, tps, lat_ms_"
emit '```'
if [ -n "$OUTFILE" ]; then cat "$TSV" >>"$OUTFILE"; else cat "$TSV"; fi
emit '```'

rm -f "$TSV"
echo "=== bench complete ==="
