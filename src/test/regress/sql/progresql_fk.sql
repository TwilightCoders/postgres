-- FOREIGN KEY references to a spanning (GLOBAL) primary/unique key (#33).
--
-- A spanning index is a single btree on the partitioned ROOT whose key is
-- (user_cols..., partseq); it has NO per-partition child index.  Two things make
-- an FK referencing it work:
--   1. transformFkeyCheckAttrs matches the referenced columns against the index's
--      leading indnuniqatts USER key (ignoring the trailing partseq).
--   2. the referenced-side action triggers, normally created per leaf against the
--      leaf's child index, instead reuse the root spanning index OID at every
--      partition level (fkReferencedPartitionIndex) -- the action query targets
--      the referencing table, not the PK index, so the OID is only metadata.
-- Asserts the full FK contract: insert-time CHECK, and ON DELETE/UPDATE actions
-- enforced both through the partitioned root AND on direct-leaf operations, with
-- no orphans; plus ATTACH of a new referenced partition and a multi-column key.
-- Each operated-on parent id is referenced by exactly one child so the action
-- under test is the one exercised (no cross-FK interference).

CREATE TABLE p (id int NOT NULL, k int NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY LIST (k);
CREATE TABLE p0 PARTITION OF p FOR VALUES IN (0);
CREATE TABLE p1 PARTITION OF p FOR VALUES IN (1);
INSERT INTO p VALUES (1,1),(2,1),(3,0),(4,1),(5,0);

-- FK creation against a spanning PK now succeeds (was "no unique constraint
-- matching given keys").  One child per ON DELETE action.
CREATE TABLE cc (cid int PRIMARY KEY, pid int REFERENCES p(id) ON DELETE CASCADE);
CREATE TABLE cn (cid int PRIMARY KEY,
    pid int REFERENCES p(id) ON DELETE SET NULL ON UPDATE CASCADE);
CREATE TABLE cr (cid int PRIMARY KEY, pid int REFERENCES p(id) ON DELETE RESTRICT);

-- The constraint records the spanning index as its supporting unique index, at
-- the root AND at each per-leaf sub-constraint (no per-leaf child index exists).
SELECT conname, conrelid::regclass AS on_tbl, confrelid::regclass AS refs,
       conindid::regclass AS via_index
  FROM pg_constraint WHERE conname LIKE 'cr_pid_fkey%' AND contype = 'f'
  ORDER BY conname;

-- CHECK: a referencing row needs an existing parent key (cross-partition: the
-- parent rows live in different leaves; the spanning index resolves them all).
INSERT INTO cc VALUES (20,1);    -- parent id=1 in p1
INSERT INTO cr VALUES (41,3);    -- parent id=3 in p0
INSERT INTO cc VALUES (99,999);  -- ERROR: no such parent key

-- ON DELETE SET NULL via the partitioned ROOT (id=2, referenced only by cn).
INSERT INTO cn VALUES (30,2);
DELETE FROM p WHERE id = 2;
SELECT cid, pid AS pid_after_root_setnull FROM cn WHERE cid = 30;   -- NULL

-- ON DELETE CASCADE via a DIRECT-LEAF delete (id=4 in p1, referenced only by cc).
INSERT INTO cc VALUES (22,4);
DELETE FROM p1 WHERE id = 4;
SELECT count(*) AS cc22_after_leaf_cascade FROM cc WHERE cid = 22;  -- 0

-- ON UPDATE CASCADE via a DIRECT-LEAF update (id=5 in p0, referenced only by cn).
INSERT INTO cn VALUES (31,5);
UPDATE p0 SET id = 55 WHERE id = 5;
SELECT cid, pid AS pid_after_leaf_updcascade FROM cn WHERE cid = 31;  -- 55

-- ON DELETE RESTRICT blocks deleting a still-referenced parent, direct-leaf too
-- (id=3 in p0, referenced by cr).
DELETE FROM p0 WHERE id = 3;     -- ERROR: still referenced from cr

-- ATTACH a new referenced partition: the cloned FK enforces it immediately.
CREATE TABLE p2 (id int NOT NULL, k int NOT NULL, PRIMARY KEY (id));
INSERT INTO p2 VALUES (6,2);
ALTER TABLE p ATTACH PARTITION p2 FOR VALUES IN (2);
INSERT INTO cc VALUES (23,6);    -- references the freshly attached partition's row
INSERT INTO cc VALUES (98,7);    -- ERROR: id=7 not present (CHECK on attached part)
DELETE FROM p2 WHERE id = 6;     -- CASCADE fires on the attached partition
SELECT count(*) AS cc23_after_attach_cascade FROM cc WHERE cid = 23;  -- 0

-- No orphans anywhere on the referencing side.
SELECT (SELECT count(*) FROM cc c WHERE NOT EXISTS (SELECT 1 FROM p WHERE p.id = c.pid)) AS cc_orphans,
       (SELECT count(*) FROM cn c WHERE c.pid IS NOT NULL
          AND NOT EXISTS (SELECT 1 FROM p WHERE p.id = c.pid)) AS cn_orphans,
       (SELECT count(*) FROM cr c WHERE NOT EXISTS (SELECT 1 FROM p WHERE p.id = c.pid)) AS cr_orphans;

-- Multi-column spanning key: FK references the full user key (a,b).
CREATE TABLE mp (a int NOT NULL, b int NOT NULL, k int NOT NULL,
    PRIMARY KEY (a,b) GLOBAL) PARTITION BY LIST (k);
CREATE TABLE mp0 PARTITION OF mp FOR VALUES IN (0);
CREATE TABLE mp1 PARTITION OF mp FOR VALUES IN (1);
INSERT INTO mp VALUES (1,1,0),(1,2,1);
CREATE TABLE mc (mcid int PRIMARY KEY, a int, b int,
    FOREIGN KEY (a,b) REFERENCES mp(a,b) ON DELETE CASCADE);
INSERT INTO mc VALUES (1,1,1);   -- ok
INSERT INTO mc VALUES (2,1,9);   -- ERROR: (1,9) not present
DELETE FROM mp1 WHERE a=1 AND b=2;   -- unreferenced; fine
DELETE FROM mp0 WHERE a=1 AND b=1;   -- CASCADE -> mc(1) gone
SELECT count(*) AS mc_rows FROM mc;  -- 0
DROP TABLE mc, mp CASCADE;

DROP TABLE cc, cn, cr, p CASCADE;    -- drops p and its partitions (p0, p1, attached p2)
