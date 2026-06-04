-- #34 regression: cross-partition uniqueness must hold even when a single
-- user key's entries span more than one btree leaf page.
--
-- A spanning (GLOBAL) index is one btree on the partitioned ROOT whose entries
-- sort by (user_cols..., partseq).  Stock btree enforces uniqueness by holding
-- the write lock on the one leaf page a key belongs to and scanning rightward
-- from the first page the value could be on.  Two things break for a spanning
-- index and are fixed in _bt_doinsert:
--
--   1. The uniqueness descent used the FULL key (including the trailing
--      partseq), so inserting (uk, partseq=hi) could land PAST a live
--      (uk, partseq=lo) sitting on a left sibling, which the rightward-only
--      scan never revisits -> a duplicate admitted with NO concurrency.  The
--      fix descends the check with the user-key prefix so it lands on the
--      leftmost page of the user-key run.
--   2. Concurrent inserters of the same user key into different partitions land
--      on different (uk, partseq) pages and so never share a buffer lock; a
--      short-duration value lock (LOCKTAG_SPANNING_KEY) serializes them.  That
--      property is exercised by the isolation specs and the pgbench soak; this
--      test covers the concurrency-independent component (1) deterministically.
--
-- The setup below moves a few hot keys across partitions hundreds of times with
-- autovacuum disabled, accumulating dead (key, old-partseq) entries so each hot
-- key's run grows across leaf pages, then asserts cross-partition uniqueness
-- still holds for those keys.  Before the fix this admitted silent duplicates.

CREATE TABLE spanrun (id int NOT NULL, part int NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY LIST (part);
CREATE TABLE spanrun0 PARTITION OF spanrun FOR VALUES IN (0) WITH (autovacuum_enabled = off);
CREATE TABLE spanrun1 PARTITION OF spanrun FOR VALUES IN (1) WITH (autovacuum_enabled = off);
CREATE TABLE spanrun2 PARTITION OF spanrun FOR VALUES IN (2) WITH (autovacuum_enabled = off);
CREATE TABLE spanrun3 PARTITION OF spanrun FOR VALUES IN (3) WITH (autovacuum_enabled = off);

-- A handful of hot keys, each moved across partitions many times.  Every
-- cross-partition move is delete+insert, leaving a dead spanning entry for the
-- old partseq; with autovacuum off these accumulate and the per-key run grows
-- across btree pages.
DO $$
DECLARE k int; r int;
BEGIN
  FOR k IN 1..20 LOOP
    INSERT INTO spanrun VALUES (k, 0);
  END LOOP;
  FOR r IN 1..400 LOOP
    FOR k IN 1..20 LOOP
      UPDATE spanrun SET part = (r + k) % 4 WHERE id = k;
    END LOOP;
  END LOOP;
END $$;

-- Every hot key is live in exactly one partition.
SELECT count(*) AS live_rows FROM spanrun;
SELECT count(*) AS duplicate_ids
FROM (SELECT id FROM spanrun GROUP BY id HAVING count(*) > 1) d;

-- Cross-partition uniqueness must still be enforced for every live key, even
-- though its run now spans multiple pages.  Insert each existing key into a
-- DIFFERENT partition than the one it currently lives in; every attempt must be
-- rejected with a unique violation.  Count the rejections.
DO $$
DECLARE
  rec      record;
  otherp   int;
  rejected int := 0;
  admitted int := 0;
BEGIN
  FOR rec IN SELECT id, part FROM spanrun ORDER BY id LOOP
    otherp := (rec.part + 1) % 4;
    BEGIN
      INSERT INTO spanrun VALUES (rec.id, otherp);
      admitted := admitted + 1;     -- should never happen
    EXCEPTION WHEN unique_violation THEN
      rejected := rejected + 1;
    END;
  END LOOP;
  RAISE NOTICE 'cross-partition re-insert: % rejected, % wrongly admitted',
    rejected, admitted;
END $$;

-- Final integrity: still no duplicates after the re-insert attempts.
SELECT count(*) AS duplicate_ids_final
FROM (SELECT id FROM spanrun GROUP BY id HAVING count(*) > 1) d;

DROP TABLE spanrun;
