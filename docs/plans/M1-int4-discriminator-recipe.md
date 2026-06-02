# E6 / M1 — Make the discriminator a real int4 column (retire the tableoid carrier)

> Output of the m1-scout investigation, 2026-06-01. The trailing spanning-index
> key column is PHYSICALLY the `tableoid` system column (attno -6, type `oid`,
> opclass `oid_ops`) but the code stores/reads an int32 partseq and the DDL
> cleanup scankey uses `F_INT4EQ` — a true int4-vs-oid_ops mismatch that "works"
> only because partseq is small-positive. `\d` shows "tableoid". pgsql-hackers
> will reject overloading the `tableoid` attno. Dilip-2025 uses a real appended
> 4-byte int key column — aligning with it is the credibility play.

## Recommendation: option (b1). Effort M (~2–3 dev-days). No on-disk change.
Keep attno -6 as the value CARRIER (the trailing value is always overwritten
post-`FormIndexDatum` anyway, so `slot_getsysattr(-6)` runs harmlessly), but
stamp the index column's `pg_attribute` as honest `int4`/`int4_ops` so
introspection, opclass, comparator, and scankey all agree. int4 and oid are
byte-identical on disk (4-byte, byval, int-aligned) → index tuple layout
UNCHANGED, WAL/amcheck untouched, pg_upgrade not broken; existing spanning
indexes need REINDEX to relabel their pg_attribute (same REINDEX-migration story
already documented). Rejected: (a) expression index — partseq isn't derivable
from the heap tuple; (c) new heap system column — XL, 15+ files, wrong semantics
(per-index not per-row); (d) consistent-oid interim — leaves the dishonest name,
not worth a separate step.

## Exact change-set (every site)
SHAPE / TYPE / OPCLASS / NAME (the b1 edit surface):
- `indexcmds.c:1199` — `ii_IndexAttrNumbers[N] = TableOidAttributeNumber` (leave; -6 is the carrier)
- `indexcmds.c:1210` — `GetDefaultOpClass(OIDOID,...)` → `GetDefaultOpClass(INT4OID,...)`
- `indexcmds.c:1215` — column name `"tableoid"` → `"partseq"`
- `catalog/index.c` `ConstructTupleDescriptor` (~347-372, 440-449) — add a branch
  that stamps the spanning trailing column's pg_attribute as INT4OID (typlen 4,
  byval, TYPALIGN_INT). int4_ops has no `opckeytype`, so the type must be set
  directly here (the opclass change alone won't change the stored attribute type,
  which is copied from `SystemAttributeDefinition(-6)` = oid).
- `catversion.h` — bump.

WRITE (value injection — already int4, keep):
- `execIndexing.c:1395` `Int32GetDatum(se->partseq)` (hot path), build
  `indexcmds.c:2960`, ATTACH `tablecmds.c:2223`.

READ / MATCH (already int4, keep):
- `nbtinsert.c:592-598,689-696` (`DatumGetInt32` + `SpanningResolvePartseqRelid`),
  `nbtree.c:1558,1587-1591` (vacuum gate), `tablecmds.c:2073` cleanup scankey
  `F_INT4EQ` (now CONSISTENT with int4_ops once the column is int4).

DOC/COMMENT:
- `genam.c:244` comment "appended tableoid" → "appended partseq".
- The relcache HOT-blocking helper (E7) reads the user-key cols, unaffected.

## Residual risks
- `opckeytype` interaction: int4_ops has none, so stamp INT4OID directly in
  ConstructTupleDescriptor; verify `_bt_mkscankey`/`btint4cmp` end-to-end.
- `FormIndexDatum` for attno -6 still runs `slot_getsysattr(-6)` then overwrites;
  document the invariant (or move to a full synthetic attno = option b2, L).
- REINDEX gating: an un-REINDEXed old index (oid_ops-declared) reloaded after the
  code change would have a declared-type/comparator mismatch — enforce
  "REINDEX spanning roots after applying this" in the migration note.
- amcheck not yet partseq-aware (deferrable).

## Note
Do this AFTER or alongside DHR (the DHR drain reads the discriminator via
`index_getattr` + decode; if M1 lands, the read stays `DatumGetInt32` on the now-
honest int4 column — minimal coupling). Either order works; M1 is independent.
