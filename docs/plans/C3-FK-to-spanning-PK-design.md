# C3 / #33 — FOREIGN KEY references to a spanning (GLOBAL) primary key

> Scoping + design for letting a `REFERENCES p(id)` foreign key target a spanning
> (`GLOBAL`) PRIMARY KEY / UNIQUE constraint on a partitioned root.
>
> Status: **DESIGN / SCOPING ONLY — not implemented.** Drafted 2026-06-13 after
> the DHR staircase (#38 + #39) shipped.  This is a genuine feature (a new
> capability), not a surgical fix; it expands the fork's surface, so it wants a
> deliberate build under review, not an autonomous slam-in.  **Do not ship a
> partial implementation** — see the data-integrity hazard below.

## Current behaviour
`CREATE TABLE c (..., pid int REFERENCES p(id))` where `p` has
`PRIMARY KEY (id) GLOBAL` fails at constraint creation:

```
ERROR:  there is no unique constraint matching given keys for referenced table "p"
```

The spanning index *is* a unique constraint on `id`, but the FK validator does
not recognise it.

## Why — two distinct gaps

### Gap 1 — the matcher (small)
`transformFkeyCheckAttrs` (tablecmds.c) gates on
`indexStruct->indnkeyatts == numattrs`.  A spanning index's key is
`(user_cols…, partseq)`, so `indnkeyatts` counts the **discriminator** too
(e.g. 2 for a 1-column user key), while `numattrs` is the user-column count
(1) → no match.  The column-match loop likewise scans all `indnkeyatts` columns.

Fix: when the index is spanning (`IndexFormIsSpanning`), compare and match against
the **leading `indnuniqatts`** columns (the user-visible key), ignoring the
trailing partseq.  Small, surgical.

### Gap 2 — the referenced-side action triggers (the hard part)
This is why the matcher fix alone is **not enough and is in fact dangerous**.

FK enforcement has two sides:
- **Referencing side (CHECK):** on INSERT/UPDATE of the child, verify the parent
  key exists.  Done by SPI (`SELECT 1 FROM p x WHERE x.id = $1 FOR KEY SHARE`).
  This would **just work** for a spanning PK: `WHERE id = $1` is a leading-column
  scan of the spanning btree on the root, returning the single global match.
- **Referenced side (ACTION):** on DELETE/UPDATE of the **parent**, enforce
  RESTRICT/CASCADE/SET NULL on children.  For a partitioned referenced table,
  `addFkRecurseReferenced` (tablecmds.c:~10978) recurses over the parent's
  partitions and, per partition, does
  `partIndexId = index_get_partition(partRel, indexOid)` and hangs an action
  trigger on that **per-leaf child index** — `elog(ERROR, "index for %u not found
  in partition")` if there is none (tablecmds.c:11011).

A spanning index has **no per-leaf children** — it is one btree on the root — so
this recursion hard-errors.  Vanilla's whole referenced-side model assumes the
referenced unique index is a *partitioned* index with a child on every leaf; a
spanning index breaks that assumption.

**Hazard:** if Gap 1 is fixed but Gap 2 is not, FK *creation* succeeds and the
insert-time CHECK works, but parent-side DELETE/UPDATE is **not enforced** →
orphaned child rows / silent FK violation.  That is a data-integrity hole; a
half-built FK is worse than none.  Ship both gaps or neither.

## Recommended approach (Gap 2)
A spanning index is the rare case where a **root-level** action trigger is both
possible and simpler than vanilla's per-leaf model: the spanning btree supports an
efficient point/leading-column lookup on the root, which the per-leaf model exists
precisely to avoid needing.  So for a spanning referenced index:

- In `addFkRecurseReferenced`, detect `RelationIsSpanning(index)` and **do not
  recurse** over the parent partitions.  Instead create a single action trigger on
  the partitioned **root** keyed on the spanning index (the root's
  `ri_triggers` action query probes `p` by the user key, which the spanning index
  serves).  One `pg_constraint`/trigger pair, not one-per-leaf.
- Confirm the RI action queries (`ri_restrict`, `ri_Cascade_*`, `ri_set`) issued
  against the partitioned root resolve the parent row via the spanning index and
  take the correct row lock across partitions.  These are SPI queries on `p`, so
  they should compose with the existing partitioned-table RI plumbing once the
  trigger is on the root rather than the leaves.
- DETACH/ATTACH of a parent partition must not need per-leaf FK trigger fixups
  (there are none) — verify the ATTACH/DETACH paths that normally re-point
  per-leaf FK triggers are correctly skipped for a spanning-referenced FK.

## Enforcement details to verify
- `RI_FKey_check` / `ri_PerformCheck` query plans actually choose the spanning
  index for `WHERE id = $1` (leading-column scan); add an index hint test.
- `FOR KEY SHARE` row locking on the partitioned root through the spanning index
  locks the right leaf tuple.
- ON DELETE / ON UPDATE: RESTRICT, NO ACTION, CASCADE, SET NULL, SET DEFAULT —
  each exercised across partitions.
- Self-referential and multi-column spanning keys (user key > 1 column +
  partseq).

## Test plan (TDD)
- regress: create FK → spanning PK; INSERT child with present/absent parent key
  (CHECK); DELETE/UPDATE parent under each action; cross-partition parent rows;
  multi-column user key; pg_dump/restore of the FK (definition round-trips);
  pg_upgrade preserves it.
- isolation: concurrent child INSERT vs parent DELETE (the FK lock interaction
  through the spanning index).
- Negative: clear behaviour if the spanning index is later dropped/replaced.

## Sizing
M–L.  Gap 1 is S.  Gap 2 is the bulk: a focused change in the referenced-side FK
recursion (`addFkRecurseReferenced` + `addFkConstraint`/trigger creation) to use a
root-level trigger for spanning indexes, plus enforcement verification and a broad
FK-semantics test matrix.  Corruption/integrity-sensitive (it is a data-integrity
constraint); build under review with the full test matrix green before shipping.

## Interim option (not done)
A clean early ERROR — detect the spanning user-key match in `transformFkeyCheckAttrs`
and raise "foreign keys referencing a spanning (GLOBAL) constraint are not yet
supported" instead of the misleading "no unique constraint matching given keys".
Improves diagnostics without shipping a partial FK.  Deferred (cosmetic; avoid
touching FK code until the real feature lands).
