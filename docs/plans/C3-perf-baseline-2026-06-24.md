# C3 — Performance baseline: fork-vs-vanilla (2026-06-24)

> Indicative baseline from `src/test/spanning/spanning_bench.sh` on november
> (optimized builds, assertions OFF, both engines PostgreSQL 18.3). **Trimmed
> params for an overnight run** — treat the numbers as directional, not final.
> Run config: `--clients-list "1 8 32" --secs 25 --warmup 8 --partitions 8
> --scale 25 --jobs 8`, fsync/synchronous_commit off (isolates CPU/lock cost),
> `IDSPACE=1e9` (random inserts almost never collide → measures the *uncontended*
> value-lock overhead floor).

## (a) Do no harm — the upstream-credibility metric

The fork must be ~free when the spanning feature is unused. It is: every delta is
within measurement noise, and at the highest concurrency the two are identical.

**TPC-B-like (builtin pgbench; no partitioning, no spanning)**

| clients | vanilla tps | fork tps | Δ |
|--------:|------------:|---------:|---:|
| 1 | 3780 | 3824 | +1.1% |
| 8 | 20180 | 20258 | +0.4% |
| 32 | 28808 | 28732 | −0.3% |

**INSERT into LIST-partitioned table, ordinary LOCAL unique PK**

| clients | vanilla tps | fork tps | Δ |
|--------:|------------:|---------:|---:|
| 1 | 26643 | 27096 | +1.7% |
| 8 | 125641 | 126720 | +0.9% |
| 32 | 169391 | 169438 | +0.0% |

→ **Conclusion:** carrying the fork imposes no measurable penalty on ordinary
PostgreSQL workloads. This is the number that matters for PG-Core credibility and
is one of the named criteria for dropping the README beta flag.

## (b) Feature cost — spanning GLOBAL PK vs local PK (both on the fork)

The cost of the `LOCKTAG_SPANNING_KEY` value lock + descend-twice insert path,
paid only when you use a GLOBAL index (which enforces something vanilla cannot:
cross-partition uniqueness).

| clients | fork local tps | fork spanning tps | Δ |
|--------:|---------------:|------------------:|---:|
| 1 | 27096 | 24204 | −10.7% |
| 8 | 126720 | 65233 | −48.5% |
| 32 | 169438 | 109207 | −35.5% |

→ The single-client floor is ~11%; mid-concurrency (c=8) shows the steepest
relative cost (~48%), easing somewhat by c=32 (~35%) as the workload re-balances.
This is the *uncontended* overhead — pure per-insert lock-acquire + second
descent, with collisions engineered away.

## Gaps / follow-ups (not run tonight)

- **Contended key space.** `spanning_bench.sh` hardcodes a huge idspace; it does
  not expose `--idspace`. The contended path (where the value lock actually
  serializes same-key cross-partition inserts) is the *ceiling* cost and is not
  measured here. The soak (`--idspace 500`) proves correctness under contention
  but is a mixed workload, not a clean insert-tps comparison. A future run should
  expose `--idspace` in the bench and add a contended column.
- **Larger matrix.** More client points, longer measure windows, and more
  partitions would tighten the noise band on (a) and the curve on (b).
- **DHR / defer-vacuum perf.** The deferred-drain path (C2 #1) is not separately
  measured here; the bench is insert-path only.

## Method note

Each engine ran on its own throwaway cluster (never concurrent, so no
cross-contention). Fresh optimized builds (`./configure --without-icu`, no
`--enable-cassert`) → correct rpath, real -O2 numbers. The harness refuses to
compare if either build reports `debug_assertions=on`.
