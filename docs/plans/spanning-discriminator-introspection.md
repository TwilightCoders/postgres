# The discriminator in `indkey`: why standard tooling misreads a spanning index

**Status: analysed, not implemented. The obvious fix does not work — see Option B.**

A spanning index carries its partition discriminator as a trailing key column,
and that column is `tableoid`, a **system** column at attnum **-6**:

```
 idx     | indisprimary | indnatts | indnkeyatts | indnuniqatts | indkey
 mf_pkey | t            |        2 |           2 |            1 | 1 -6
```

Stock PostgreSQL rejects `CREATE INDEX ... (tableoid)` outright — `index creation
on system columns is not supported` — so a negative attnum in `indkey` is a state
vanilla **guarantees is unreachable**. Client tooling therefore has no reason to
defend against it, and the ones that don't fail silently rather than loudly.

## The observed failure

An ORM deriving a table's primary key from `indkey` concludes the table has a
composite key `(id, tableoid)`. The Rails PostgreSQL adapter does exactly this —
its `primary_keys` query is shaped like

```sql
SELECT a.attname FROM (
  SELECT indrelid, indkey, generate_subscripts(indkey, 1) idx
    FROM pg_index WHERE indrelid = $1::regclass AND indisprimary
) i JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = i.indkey[i.idx]
ORDER BY i.idx
```

— joining on `indkey[idx]` with no attnum filter and no `indnkeyatts` slice. The
resulting model emits

```sql
WHERE "t"."id" = $1 AND "t"."tableoid" IS NULL
```

for every `reload` / `update` / `destroy`. `tableoid` is never NULL, so those
statements match zero rows **while reporting success**: `update!` returns true
and nothing is written. Silent no-op writes are close to the worst failure shape
available, and the cost is discovery, not remediation — the application-side fix
is one line (`self.primary_key = 'id'`).

Note that the information tooling needs is already present and correct:
`indnuniqatts` is the user-facing key count. It is simply not the field tooling
reads.

## Option A — document it, and publish the filter *(shipped, 0.2.5)*

Tooling that filters system attnums gets the right answer today. Verified against
the same index:

```sql
-- no filter        -> id, tableoid   (the bug)
JOIN pg_attribute a ON a.attnum = ANY(i.indkey)
-- AND a.attnum > 0 -> id             (correct)
```

The filter is a **no-op against stock PostgreSQL**, which can never have a
negative attnum there, so it is safe to carry upstream into a shared tool rather
than maintained as a fork-specific branch. Documented in the README's
introspection contract, alongside `pg_index_global_columns()` as the preferred
answer.

This is cheap and correct but does not help a user who meets the bug before the
documentation.

## Option B — carry the discriminator as a non-key attribute

The intuitive fix, and the one proposed downstream: lower `indnkeyatts` to the
user-facing count and carry the discriminator as an INCLUDE-style non-key
attribute, so `indnkeyatts` reports what tooling expects.

**This does not fix the reported problem.** `indkey` lists *all* `indnatts`
columns, key and non-key alike. Measured on stock PostgreSQL:

```sql
CREATE UNIQUE INDEX inc_i ON inc (a) INCLUDE (b);
--  indnatts | indnkeyatts | indkey
--         2 |           1 | 1 2        <-- the non-key column is still in indkey
```

So the discriminator would remain at `indkey[2]`, the Rails query would still
join on it, and the phantom composite key would still appear. Option B only helps
tooling that *slices* `indkey` by `indnkeyatts` — which is the well-behaved
minority, and not the case that motivated the change.

Its cost, by contrast, is large. The discriminator being a key column is load
bearing across **72 call sites in 22 files**, including five nbtree files
(`nbtinsert`, `nbtpage`, `nbtree`, `nbtsort`, plus `vacuumlazy`), `execIndexing`,
`catalog/index.c`, `plancat`, and `ruleutils`. It changes the on-disk index tuple
layout (mandatory `REINDEX`) and touches the catalog (`pg_index.h`,
`postgres.bki`, `schemapg.h`), so it needs a `catversion` bump — making it a
`pg_upgrade` break, not merely a reindex.

**Recommendation: do not pursue Option B for the introspection problem.** It pays
a redesign's price and does not buy the fix.

## Option C — represent the discriminator as an expression

Expression index columns appear in `indkey` as attnum **0**. Measured:

```sql
CREATE INDEX inc_e ON inc ((a+b));
--  indnatts | indnkeyatts | indkey
--         1 |           1 | 0
```

`pg_attribute` has no attnum 0, so an attnum join *drops* the column — the Rails
query would return `id` alone, correctly, with no client change. Stock produces
expression indexes routinely, so tooling handles them.

The obstacle is honesty: `partseq` is index-local metadata from
`pg_index_partition`, not a function of the tuple, so there is no expression that
genuinely computes it. Representing it as one would mean carrying a placeholder
expression that is never evaluated (the write paths already overwrite the
discriminator datum directly). That is a catalog-level lie, and it would mislead
anything that tries to *use* the expression — `EXPLAIN`, index-only scans,
`pg_get_indexdef`. Not recommended without a much closer look at what breaks.

## Option D — fix it upstream in the tooling

Because the `attnum > 0` filter is correct against stock PostgreSQL, the durable
answer for any widely-used tool is to carry the filter there rather than to
contort the fork's on-disk representation. This costs nothing, helps every fork
user of that tool, and is defensible to the tool's maintainers on its own merits
(it guards against a state their code already assumes cannot occur).

## Recommendation

1. **Keep Option A** (shipped). It is correct and cheap.
2. **Do not implement Option B** for this purpose — measured not to fix it.
3. **Pursue Option D** opportunistically for tools that matter.
4. Revisit the representation only if a *different* motivation appears — see
   below.

## Unverified observation, recorded for whoever revisits this

If the discriminator became a non-key attribute, two leaves holding the same user
key would produce index tuples with **identical keys** rather than distinct ones.
Stock btree would then reject the second insert natively, on the same page lock
that serialises any ordinary unique index — which is the behaviour
`indnuniqatts`, the `_bt_check_unique` special-casing, and possibly the whole
cross-partition value lock (`spanning_lock.c`) exist to reconstruct.

That suggests the non-key representation might substantially *simplify* the fork
rather than merely relocate a field. It is not a reason to do it for
introspection — Option B still doesn't fix that — but it could be a reason to do
it for its own sake, and it would materially shrink the fork's diff against
upstream, which is the north star.

**This is a hypothesis, not a finding.** It has not been prototyped, and the
interactions that would need to hold up are non-trivial: btree deduplication
(disabled for indexes with INCLUDE columns, which may be exactly what makes this
safe, or may not), suffix truncation of non-key attributes at page splits, the
partseq-filtered VACUUM bulkdelete, and whether the leaf-only availability of
non-key attributes is compatible with every site that currently reads the
discriminator. Any attempt should start with a throwaway prototype answering
"does stock uniqueness actually do the right thing here", before touching
anything shipped.
