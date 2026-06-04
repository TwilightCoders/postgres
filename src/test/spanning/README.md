# Spanning-index stress / soak harness

`spanning_soak.sh` is the scale/concurrency counterpart to the deterministic
spanning suites (`src/test/regress/sql/progresql_*`, the isolation specs
`spanning-unique` / `spanning-detach`, and the recovery / subscription /
pg_upgrade TAP tests). It exists because **a functional test only catches what it
asserts** — silent corruption has to be caught by an independent oracle run over
a stressed system. It is what was used to find, and then verify the fix for, the
cross-partition uniqueness race (#34).

## What it does

Spins up a throwaway cluster, hammers a spanning-indexed partitioned table with
concurrent DML via `pgbench`, then checks **two independent oracles**:

1. **The application invariant** — no cross-partition duplicates on *either*
   spanning key. The table carries an `int` `PRIMARY KEY (id) GLOBAL` **and** a
   `text` `UNIQUE (tag) GLOBAL` (with `tag = 't'||id`), so one logical key is
   enforced by two spanning indexes — and the text key exercises the
   collation-aware hash in the `LOCKTAG_SPANNING_KEY` value lock.
2. **`amcheck`** — `bt_index_check` + `bt_index_parent_check` on every spanning
   index, to catch *structural / ordering* corruption.

> **Why both?** `amcheck` validates that the btree is well-ordered on its actual
> key `(user_cols…, partseq)`. A cross-partition duplicate is well-ordered at that
> level — the violation is only at the *user-key* level — so **`amcheck` passes
> even with duplicates present** (confirmed: the pre-fix code fails the invariant
> while `amcheck` reports OK). The invariant catches the uniqueness violation;
> `amcheck` catches the page/ordering corruption a faulty insert-positioning fix
> would cause. Neither alone is sufficient.

The workload mix is weighted toward the dup-producing operations (single INSERT +
cross-partition MOVE of a contended key), with DELETE / batch-INSERT / HOT-UPDATE
for path coverage. **Autovacuum is disabled on purpose**: under heavy churn a hot
key's dead spanning entries must be allowed to accumulate so its run grows across
btree leaf pages — that is the geometry the race needs (two same-user-key inserts
into different partitions landing on *different* pages, so the stock leaf-page
write lock no longer serializes them). Aggressive autovacuum keeps the run on one
page and masks the bug; a real workload hits the condition transiently between
vacuum cycles.

## Usage

```sh
# default: reliable detector (16 clients, 60s, idspace 500, 4 partitions)
src/test/spanning/spanning_soak.sh

# crash-recovery torture: kill -9 mid-load on a durable cluster, recover, verify
src/test/spanning/spanning_soak.sh --crash

# milder, more "realistic" soak (wider key space, fewer clients)
src/test/spanning/spanning_soak.sh --clients 8 --idspace 50000 --secs 300
```

Flags: `--clients N --jobs N --secs N --partitions N --idspace N --crash --keep
--bindir DIR`. Exit status is 0 only if every oracle passes. Run against an
`--enable-cassert` build so backend assertions are a third oracle.

The defaults are tuned to be a **detector**, not a gentle soak: a tight key space
+ high client count maximizes same-key cross-partition collisions. Widen
`--idspace` / drop `--clients` for a longer realistic run.

## Validated as a real oracle

The harness was confirmed to actually catch the bug (a soak that can't fail is
theater):

| build | config | result |
|---|---|---|
| pre-fix `9d92004062` | idspace 300, 16 clients, 30s | **43 dup ids / 43 dup tags → FAIL** |
| pre-fix `9d92004062` | idspace 100, 24 clients, 30s | **24 dups → FAIL** |
| fix `1d94d3320e` | idspace 100, 24 clients | 0 dups, amcheck OK → PASS |
| fix `1d94d3320e` | idspace 300, 16 clients | 0 dups, amcheck OK → PASS |
| fix `1d94d3320e` | `--crash` (kill -9 mid-load) | 0 dups after recovery → PASS |

## How this maps to how PostgreSQL itself is tested at scale

PostgreSQL does not run beta in production. It (a) runs the deterministic suites
on a ~100-machine buildfarm + Cirrus CI for every commit; (b) makes races
deterministic with the isolation tester and **injection points**
(`--enable-injection-points`); (c) **stresses, then verifies with independent
oracles** — `amcheck`, page checksums, assertions — rather than trusting "it
didn't crash"; (d) fuzzes with `sqlsmith`; (e) runs torture builds
(`--enable-cassert`, `debug_discard_caches` / `CLOBBER_CACHE_ALWAYS`, valgrind,
ASAN/UBSan); and (f) ships a long opt-in beta. This harness is item (c) for the
spanning feature. A natural complement (item b) is a `--enable-injection-points`
isolation test that wedges two backends into the exact check/insert window for a
*deterministic* concurrency proof — not yet added.
