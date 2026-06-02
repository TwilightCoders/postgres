-- E2: dump/restore durability of spanning indexes.
--
-- pg_get_indexdef / pg_get_constraintdef (and pg_dump, which builds PK/UNIQUE
-- constraint DDL itself) must clip the trailing discriminator key column and
-- emit the GLOBAL marker.  Otherwise a dump replays as a plain index/constraint
-- over (user_cols, discriminator) and silently loses cross-partition uniqueness
-- (the discriminator differs per partition, so the composite key is always
-- "unique").  This test pins the round-trippable definitions.

CREATE TABLE dd_base (id bigint NOT NULL, ts timestamptz NOT NULL, tag text);
CREATE TABLE dd (PRIMARY KEY (id)) INHERITS (dd_base) PARTITION BY RANGE (ts);
CREATE TABLE dd1 PARTITION OF dd FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE dd2 PARTITION OF dd FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
-- a standalone spanning UNIQUE index too (covers pg_get_indexdef directly)
CREATE UNIQUE INDEX dd_tag_uq ON dd (tag) GLOBAL;

-- Definitions must show only user columns + GLOBAL, never "tableoid".
SELECT pg_get_indexdef('dd_pkey'::regclass);
SELECT pg_get_indexdef('dd_tag_uq'::regclass);
SELECT pg_get_constraintdef(oid)
  FROM pg_constraint WHERE conrelid = 'dd'::regclass AND contype = 'p';

-- Both indexes are spanning.
SELECT c.relname, i.indnuniqatts
  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
  WHERE i.indrelid = 'dd'::regclass ORDER BY c.relname;

-- Replay round-trip: drop the standalone index and recreate it from its own
-- dumped definition; it must come back spanning (indnuniqatts > 0), proving the
-- emitted DDL reconstructs the spanning property rather than a plain index.
SELECT pg_get_indexdef('dd_tag_uq'::regclass) AS def \gset
DROP INDEX dd_tag_uq;
SELECT :'def' \gexec
SELECT indnuniqatts FROM pg_index WHERE indexrelid = 'dd_tag_uq'::regclass;

-- And the recreated index still enforces cross-partition uniqueness.
INSERT INTO dd VALUES (1, '2024-06-01', 'x');
INSERT INTO dd VALUES (2, '2025-06-01', 'x');   -- ERROR: duplicate key (tag)=(x)

DROP TABLE dd CASCADE;
DROP TABLE dd_base;
