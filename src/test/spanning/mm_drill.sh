#!/usr/bin/env bash
# fork<->fork master-master logical replication drill for ProgreSQL spanning indexes.
# Proves (or bounds) bidirectional apply keeping the GLOBAL/spanning index correct.
# Key fix vs v1: leaf-level publication (publish_via_partition_root=false) + leaves
# REPLICA IDENTITY FULL — because the spanning index (PK, contains the partseq system
# column) can NEVER serve as a logical-rep replica identity. Leaves have no local index,
# so the PK-fallback is empty and FULL is honored.
set -uo pipefail

PREFIX="${PROGRESQL_PREFIX:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/build/install}"
BIN="$PREFIX/bin"
ROOT="/tmp/mm-drill"; A="$ROOT/A"; B="$ROOT/B"
PA=5530; PB=5531; LOGA="$ROOT/a.log"; LOGB="$ROOT/b.log"
export PGHOST=127.0.0.1 PGUSER=postgres

pass=0; fail=0
ok(){ echo "  ✅ PASS: $*"; pass=$((pass+1)); }
no(){ echo "  ❌ FAIL: $*"; fail=$((fail+1)); }
hdr(){ echo; echo "════════════════════════════════════════════"; echo "  $*"; echo "════════════════════════════════════════════"; }
qa(){ "$BIN/psql" -p $PA -d app -At -c "$1" 2>&1; }
qb(){ "$BIN/psql" -p $PB -d app -At -c "$1" 2>&1; }

cleanup(){ "$BIN/pg_ctl" -D "$A" stop -m immediate >/dev/null 2>&1; "$BIN/pg_ctl" -D "$B" stop -m immediate >/dev/null 2>&1; rm -rf "$ROOT"; }
trap cleanup EXIT
rm -rf "$ROOT"; mkdir -p "$ROOT"

# wait until BOTH nodes' count(*) on <tbl> == <expected> (deterministic, no false-positive)
converge(){ local tbl=$1 want=$2 i ca cb
  for i in $(seq 1 80); do ca=$(qa "select count(*) from $tbl"); cb=$(qb "select count(*) from $tbl")
    [ "$ca" = "$want" ] && [ "$cb" = "$want" ] && return 0; sleep 0.5; done; return 1; }
errcount(){ grep -ciE 'violates unique|duplicate key' "$LOGA" "$LOGB" 2>/dev/null | awk -F: '{s+=$2} END{print s+0}'; }
amck(){ local port=$1 lbl=$2 list err
  list=$("$BIN/psql" -p $port -d app -At -c "select coalesce(string_agg(format('bt_index_parent_check(%L,false,true)',c.oid::regclass::text),','),'') from pg_class c join pg_index i on i.indexrelid=c.oid where i.indisvalid and i.indnuniqatts>0 and c.relkind='i'")
  [ -z "$list" ] && { echo "    ($lbl: no spanning idx)"; return 1; }
  err=$("$BIN/psql" -p $port -d app -At -c "select $list" 2>&1)
  echo "$err" | grep -qiE 'error|corrupt' && { echo "    $lbl amcheck: $err"; return 1; }; return 0; }
resync(){ # drop+recreate both subs clean (after an intentional break)
  qa "DROP SUBSCRIPTION IF EXISTS sub_from_b" >/dev/null 2>&1; qb "DROP SUBSCRIPTION IF EXISTS sub_from_a" >/dev/null 2>&1; sleep 1
  qa "CREATE SUBSCRIPTION sub_from_b CONNECTION 'host=127.0.0.1 port=$PB dbname=app user=postgres' PUBLICATION pub WITH (origin=none, copy_data=false)" >/dev/null 2>&1
  qb "CREATE SUBSCRIPTION sub_from_a CONNECTION 'host=127.0.0.1 port=$PA dbname=app user=postgres' PUBLICATION pub WITH (origin=none, copy_data=false)" >/dev/null 2>&1; sleep 2; }

hdr "SETUP — two fork instances (A:$PA, B:$PB), wal_level=logical"
"$BIN/initdb" -D "$A" -U postgres --no-sync >/dev/null 2>&1; "$BIN/initdb" -D "$B" -U postgres --no-sync >/dev/null 2>&1
for D in "$A" "$B"; do cat >> "$D/postgresql.conf" <<EOF
wal_level=logical
max_wal_senders=10
max_replication_slots=10
max_logical_replication_workers=10
max_worker_processes=16
track_commit_timestamp=on
listen_addresses='127.0.0.1'
EOF
done
echo "port=$PA" >> "$A/postgresql.conf"; echo "port=$PB" >> "$B/postgresql.conf"
"$BIN/pg_ctl" -D "$A" -l "$LOGA" -w start >/dev/null 2>&1; "$BIN/pg_ctl" -D "$B" -l "$LOGB" -w start >/dev/null 2>&1
"$BIN/createdb" -p $PA app; "$BIN/createdb" -p $PB app
qa "CREATE EXTENSION amcheck" >/dev/null; qb "CREATE EXTENSION amcheck" >/dev/null
SCHEMA=$(cat <<'SQL'
CREATE TABLE raw_log (id uuid NOT NULL DEFAULT uuidv7(), conversation_id uuid NOT NULL, ts timestamptz NOT NULL, node text NOT NULL, payload text, PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE raw_log_01 PARTITION OF raw_log FOR VALUES FROM ('2026-01-01') TO ('2026-02-01');
CREATE TABLE raw_log_02 PARTITION OF raw_log FOR VALUES FROM ('2026-02-01') TO ('2026-03-01');
CREATE TABLE raw_log_03 PARTITION OF raw_log FOR VALUES FROM ('2026-03-01') TO ('2026-04-01');
ALTER TABLE raw_log_01 REPLICA IDENTITY FULL; ALTER TABLE raw_log_02 REPLICA IDENTITY FULL; ALTER TABLE raw_log_03 REPLICA IDENTITY FULL;
CREATE TABLE facts (id uuid NOT NULL DEFAULT uuidv7(), natural_key text NOT NULL, ts timestamptz NOT NULL, value text, PRIMARY KEY (id) GLOBAL, UNIQUE (natural_key) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE facts_01 PARTITION OF facts FOR VALUES FROM ('2026-01-01') TO ('2026-02-01');
CREATE TABLE facts_02 PARTITION OF facts FOR VALUES FROM ('2026-02-01') TO ('2026-03-01');
CREATE TABLE facts_03 PARTITION OF facts FOR VALUES FROM ('2026-03-01') TO ('2026-04-01');
ALTER TABLE facts_01 REPLICA IDENTITY FULL; ALTER TABLE facts_02 REPLICA IDENTITY FULL; ALTER TABLE facts_03 REPLICA IDENTITY FULL;
CREATE TABLE seqmsg (conversation_id uuid NOT NULL, sequence_num int NOT NULL, ts timestamptz NOT NULL, node text, UNIQUE (conversation_id, sequence_num) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE seqmsg_01 PARTITION OF seqmsg FOR VALUES FROM ('2026-01-01') TO ('2026-02-01');
CREATE TABLE seqmsg_02 PARTITION OF seqmsg FOR VALUES FROM ('2026-02-01') TO ('2026-03-01');
ALTER TABLE seqmsg_01 REPLICA IDENTITY FULL; ALTER TABLE seqmsg_02 REPLICA IDENTITY FULL;
SQL
)
qa "$SCHEMA" >/dev/null && qb "$SCHEMA" >/dev/null && echo "  schema (spanning + time-partitioned, leaves FULL) on A and B."

hdr "BIDIRECTIONAL REPLICATION — origin=none, leaf-level (the working model)"
qa "CREATE PUBLICATION pub FOR TABLE raw_log, facts, seqmsg" >/dev/null   # publish_via_partition_root defaults false -> leaf-level
qb "CREATE PUBLICATION pub FOR TABLE raw_log, facts, seqmsg" >/dev/null
resync
echo "  A sub: $(qa "select subname||':'||subenabled from pg_subscription")  |  B sub: $(qb "select subname||':'||subenabled from pg_subscription")"

# ───────────────────────────────────────────
hdr "SCENARIO 1 — bidirectional apply keeps the spanning index correct"
qa "INSERT INTO raw_log(conversation_id,ts,node,payload) SELECT gen_random_uuid(),(timestamptz '2026-01-15'+(g%3)*interval '1 month'),'A','a'||g FROM generate_series(1,150) g" >/dev/null
qb "INSERT INTO raw_log(conversation_id,ts,node,payload) SELECT gen_random_uuid(),(timestamptz '2026-01-15'+(g%3)*interval '1 month'),'B','b'||g FROM generate_series(1,150) g" >/dev/null
if converge raw_log 300; then
  ok "both nodes converge to the union (300/300) under bidirectional apply"
  da=$(qa "select count(*) from (select id from raw_log group by id having count(*)>1) z"); db=$(qb "select count(*) from (select id from raw_log group by id having count(*)>1) z")
  [ "$da" = "0" ] && [ "$db" = "0" ] && ok "uniqueness invariant: 0 cross-partition dup ids on both nodes" || no "dup ids A=$da B=$db"
  ma=$(qa "select count(distinct node) from raw_log"); mb=$(qb "select count(distinct node) from raw_log")
  [ "$ma" = "2" ] && [ "$mb" = "2" ] && ok "both A- and B-origin rows present on both nodes (genuinely bidirectional)" || no "origin mix A=$ma B=$mb"
  amck $PA A && amck $PB B && ok "amcheck (bt_index_parent_check, structural; heapallindexed guarded off for spanning) clean on both nodes" || no "amcheck flagged corruption"
else no "raw_log did not converge to 300 (A=$(qa 'select count(*) from raw_log') B=$(qb 'select count(*) from raw_log'))"; fi

# ───────────────────────────────────────────
hdr "SCENARIO 2 — raw corpus: append-only converges; same-id-on-two-nodes is NOT silently deduped"
b=$(qa "select count(*) from raw_log")
qa "INSERT INTO raw_log(conversation_id,ts,node,payload) VALUES (gen_random_uuid(),'2026-02-10','A','x1'),(gen_random_uuid(),'2026-03-10','A','x2')" >/dev/null
qb "INSERT INTO raw_log(conversation_id,ts,node,payload) VALUES (gen_random_uuid(),'2026-01-10','B','y1')" >/dev/null
converge raw_log $((b+3)) && ok "origin-disjoint UUIDv7 appends converge to union, zero conflicts (+3)" || no "expected +3"
e0=$(errcount); DUP=$(qa "select uuidv7()")
qa "INSERT INTO raw_log(id,conversation_id,ts,node,payload) VALUES ('$DUP',gen_random_uuid(),'2026-02-11','A','dupA')" >/dev/null
qb "INSERT INTO raw_log(id,conversation_id,ts,node,payload) VALUES ('$DUP',gen_random_uuid(),'2026-02-11','B','dupB')" >/dev/null
sleep 3
if [ "$(errcount)" -gt "$e0" ]; then
  ok "same-id authored on two nodes raises an apply ERROR (PK violation) — NOT a silent discard (corrects the 'idempotent dedup' assumption)"
  echo "     → append-only safety = origin-disjoint authorship as a HARD invariant + origin=none, not dedup-on-apply."
else no "expected a PK-violation apply error for same-id-on-two-nodes"; fi
qa "TRUNCATE raw_log" >/dev/null; qb "TRUNCATE raw_log" >/dev/null; resync   # clean slate for later

# ───────────────────────────────────────────
hdr "SCENARIO 3 — the one real conflict: same natural_key, different UUID, cross-partition"
e0=$(errcount)
qa "INSERT INTO facts(natural_key,ts,value) VALUES ('NK1','2026-01-20','from-A')" >/dev/null   # facts_01
qb "INSERT INTO facts(natural_key,ts,value) VALUES ('NK1','2026-03-20','from-B')" >/dev/null   # facts_03 (different partition)
echo "  each node accepted its own NK1 locally (A facts=$(qa 'select count(*) from facts'), B facts=$(qb 'select count(*) from facts'))"
sleep 4
if [ "$(errcount)" -gt "$e0" ]; then
  ok "cross-apply of same-natural-key/different-UUID is REJECTED by the spanning UNIQUE on apply (cross-partition), subscription halts"
  ref=$(grep -iE 'violates unique' "$LOGA" "$LOGB" 2>/dev/null | grep -iE 'natural_key|facts' | tail -1 | sed 's/.*ERROR/ERROR/' | cut -c1-100)
  echo "     log: ...$ref"
  echo "     → the failure mode is DETECTED (loud), never silently merged. Single-authority-for-facts avoids it entirely:"
else no "expected a spanning-UNIQUE apply violation"; fi
qa "TRUNCATE facts" >/dev/null; qb "TRUNCATE facts" >/dev/null; resync
qa "INSERT INTO facts(natural_key,ts,value) SELECT 'F'||g,(timestamptz '2026-01-15'+(g%3)*interval '1 month'),'v'||g FROM generate_series(1,60) g" >/dev/null
if converge facts 60; then
  da=$(qa "select count(*) from (select natural_key from facts group by natural_key having count(*)>1) z")
  [ "$da" = "0" ] && ok "single-groomer (only A writes facts) converges clean: 60/60, 0 natural_key dups" || no "groomer dup=$da"
  amck $PA A && amck $PB B && ok "amcheck clean on facts spanning indexes after single-groomer apply" || no "amcheck flagged facts"
else no "facts did not converge under single-groomer"; fi

# ───────────────────────────────────────────
hdr "SCENARIO 4 — UUIDv7 (safe) vs node-assigned sequence_num (collides)"
CONV=$(qa "select gen_random_uuid()")
qa "INSERT INTO raw_log(conversation_id,ts,node,payload) VALUES ('$CONV','2026-02-01','A','mA1'),('$CONV','2026-02-02','A','mA2')" >/dev/null
qb "INSERT INTO raw_log(conversation_id,ts,node,payload) VALUES ('$CONV','2026-02-03','B','mB1'),('$CONV','2026-02-04','B','mB2')" >/dev/null
if converge "raw_log where conversation_id='$CONV'" 4; then ok "4a: concurrent same-conversation appends under UUIDv7 converge conflict-free (4/4)"; else no "4a uuidv7"; fi
e0=$(errcount)
qa "INSERT INTO seqmsg(conversation_id,sequence_num,ts,node) VALUES ('$CONV',1,'2026-01-05','A')" >/dev/null   # seqmsg_01
qb "INSERT INTO seqmsg(conversation_id,sequence_num,ts,node) VALUES ('$CONV',1,'2026-02-06','B')" >/dev/null   # seqmsg_02 (cross-partition)
sleep 4
[ "$(errcount)" -gt "$e0" ] && ok "4b: node-assigned (conversation_id,sequence_num) UNIQUE COLLIDES cross-apply, cross-partition — confirms schema must use UUIDv7, not node-local seq" || no "4b expected a seq collision"

# ───────────────────────────────────────────
hdr "MTI PROBE — can the spanning index compose with Postgres INHERITS? (single-node)"
echo "  probe 1 (INHERITS child that is itself PARTITION BY):"
echo "     -> $(qa "CREATE TABLE eb(id uuid NOT NULL, ts timestamptz NOT NULL, kind text); CREATE TABLE em(body text, PRIMARY KEY(id) GLOBAL) INHERITS (eb) PARTITION BY RANGE (ts)" | tail -1)"
echo "  probe 2 (INHERITS from a declaratively-partitioned root):"
echo "     -> $(qa "CREATE TABLE e2(id uuid NOT NULL, ts timestamptz NOT NULL) PARTITION BY RANGE (ts); CREATE TABLE e2c () INHERITS (e2)" | tail -1)"
echo "  probe 3 (does GLOBAL even enforce across an INHERITS hierarchy?):"
qa "CREATE TABLE ip(id uuid NOT NULL, k text); CREATE TABLE ic(extra text) INHERITS (ip); CREATE UNIQUE INDEX ipk ON ip (k) GLOBAL" >/dev/null 2>&1
qa "INSERT INTO ip(id,k) VALUES (gen_random_uuid(),'dup')" >/dev/null 2>&1
p3=$(qa "INSERT INTO ic(id,k,extra) VALUES (gen_random_uuid(),'dup','child')" 2>&1)
echo "$p3" | grep -qiE 'duplicate|unique|violat' && echo "     -> ENFORCED across hierarchy: $p3" || echo "     -> NOT enforced across INHERITS children (child dup accepted) — GLOBAL only spans declarative partitions"

hdr "RESULT"; echo "  PASS=$pass  FAIL=$fail"
[ "$fail" = "0" ] && echo "  ⇒ master↔master HOLDS for the spanning core (scenarios 1–4) with leaf-level pub + leaves FULL." || echo "  ⇒ see failures above."
