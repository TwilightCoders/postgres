-- Determinism & completeness of FK-clone fanout over a spanning (GLOBAL)
-- inheritance root.
--
-- An ordinary inheritance root has no single physical table for a referenced-
-- side FK trigger to point at, so a FOREIGN KEY that targets a spanning root is
-- enforced by CLONING the constraint once per storage-bearing leaf, plus the
-- base constraint on the root itself.  This file pins the three properties that
-- make that sound -- and that an earlier investigation briefly mis-read as a
-- defect ("the same base FKs produced different clone counts") when the real
-- cause was two DIFFERENT schemas being compared, not non-determinism:
--
--   1. FANOUT is exactly base + one clone per current leaf.
--   2. The clone set is ORDER-INDEPENDENT: applying the FK BEFORE any leaf
--      exists yields the identical set as applying it AFTER the leaves.
--   3. A leaf added AFTER the FK acquires its clone and enforces (no hole),
--      and the base constraint definition stays clean / re-clonable -- so a
--      pg_dump that emits only the base FK restores to the same set.  (The
--      actual dump -> restore half is pinned in 006_spanning_roundtrip.pl.)

-- Two builds of the same shape, opposite orders.
-- Path X: leaves first, then the FK.
CREATE SCHEMA fkx;
CREATE TABLE fkx.root (id bigint NOT NULL, val text);
CREATE TABLE fkx.leaf_a () INHERITS (fkx.root);
CREATE TABLE fkx.leaf_b () INHERITS (fkx.root);
CREATE TABLE fkx.leaf_c () INHERITS (fkx.root);
ALTER TABLE fkx.root ADD CONSTRAINT root_id_uq UNIQUE (id) GLOBAL;
CREATE TABLE fkx.ref1 (id bigint PRIMARY KEY, root_ref bigint);
ALTER TABLE fkx.ref1 ADD CONSTRAINT fk_r
    FOREIGN KEY (root_ref) REFERENCES fkx.root(id) ON DELETE CASCADE;

-- Path Y: FK first (zero leaves present), then the leaves.
CREATE SCHEMA fky;
CREATE TABLE fky.root (id bigint NOT NULL, val text);
ALTER TABLE fky.root ADD CONSTRAINT root_id_uq UNIQUE (id) GLOBAL;
CREATE TABLE fky.ref1 (id bigint PRIMARY KEY, root_ref bigint);
ALTER TABLE fky.ref1 ADD CONSTRAINT fk_r
    FOREIGN KEY (root_ref) REFERENCES fky.root(id) ON DELETE CASCADE;
CREATE TABLE fky.leaf_a () INHERITS (fky.root);
CREATE TABLE fky.leaf_b () INHERITS (fky.root);
CREATE TABLE fky.leaf_c () INHERITS (fky.root);

-- (1) FANOUT: base + one clone per leaf == 1 + 3 == 4, the same on each path.
SELECT n.nspname AS path, count(*) AS fk_constraints
  FROM pg_constraint c JOIN pg_namespace n ON n.oid = c.connamespace
 WHERE c.contype = 'f' AND n.nspname IN ('fkx','fky')
 GROUP BY n.nspname ORDER BY 1;

-- (2) ORDER-INDEPENDENCE: the two clone sets are identical.  Project each path
-- to (referenced relname, on-delete, on-update), strip the schema, and assert
-- neither path has a tuple the other lacks.
WITH proj AS (
  SELECT n.nspname AS path,
         regexp_replace((c.confrelid::regclass)::text, '^fk[xy]\.', '') AS refs,
         c.confdeltype, c.confupdtype
    FROM pg_constraint c JOIN pg_namespace n ON n.oid = c.connamespace
   WHERE c.contype = 'f' AND n.nspname IN ('fkx','fky')
)
SELECT NOT EXISTS (
    (SELECT refs, confdeltype, confupdtype FROM proj WHERE path = 'fkx'
     EXCEPT
     SELECT refs, confdeltype, confupdtype FROM proj WHERE path = 'fky')
    UNION ALL
    (SELECT refs, confdeltype, confupdtype FROM proj WHERE path = 'fky'
     EXCEPT
     SELECT refs, confdeltype, confupdtype FROM proj WHERE path = 'fkx')
  ) AS clone_set_order_independent;

-- (3) the set references each leaf AND the root (path X, sorted).
SELECT regexp_replace((confrelid::regclass)::text, '^fkx\.', '') AS references_rel
  FROM pg_constraint
 WHERE contype = 'f' AND connamespace = 'fkx'::regnamespace
 ORDER BY 1;

-- (4) ENFORCEMENT is real: a present parent resolves into its leaf, a missing
-- one is rejected by the base constraint (no parent in any leaf).
INSERT INTO fkx.leaf_a(id, val) VALUES (1, 'a');
INSERT INTO fkx.ref1 VALUES (10, 1);     -- ok: parent id=1 lives in leaf_a
INSERT INTO fkx.ref1 VALUES (11, 999);   -- ERROR: no such parent in any leaf

-- (5) COMPLETENESS: a leaf added AFTER the FK acquires its clone and enforces.
CREATE TABLE fkx.leaf_d () INHERITS (fkx.root);
SELECT count(*) AS clones_referencing_leaf_d
  FROM pg_constraint
 WHERE contype = 'f' AND confrelid = 'fkx.leaf_d'::regclass;   -- 1
INSERT INTO fkx.leaf_d(id, val) VALUES (2, 'd');
INSERT INTO fkx.ref1 VALUES (12, 2);     -- ok: references the after-added leaf's row
DELETE FROM fkx.leaf_d WHERE id = 2;     -- ON DELETE CASCADE fires on the new leaf
SELECT count(*) AS ref12_after_cascade FROM fkx.ref1 WHERE id = 12;  -- 0

-- (6) the BASE FK definition is clean and re-clonable -- exactly what pg_dump
-- emits, with no leaf leaking into it -- so the emit/re-clone round-trip lands
-- back on the identical set.
SELECT pg_get_constraintdef(oid) AS base_fk_def
  FROM pg_constraint
 WHERE conname = 'fk_r' AND connamespace = 'fkx'::regnamespace
   AND confrelid = 'fkx.root'::regclass;

DROP SCHEMA fkx CASCADE;
DROP SCHEMA fky CASCADE;
