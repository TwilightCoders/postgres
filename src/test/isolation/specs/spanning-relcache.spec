# ProgreSQL: adding a spanning (GLOBAL) index to a populated partitioned table
# must invalidate the LEAF relcaches in other backends (#42).
#
# When a spanning index is added, every leaf partition's hot-blocking attribute
# set gains the spanning key columns (progresql_add_spanning_hotblocking_attrs),
# so a spanning-key UPDATE on a leaf must be forced COLD (non-HOT) to keep the
# spanning entry maintained for the new tuple version.  ALTER TABLE / CREATE
# INDEX invalidate the partitioned root but not its leaves; a backend that
# cached a leaf's rd_hotblockingattr BEFORE the index was added would otherwise
# keep the stale (pre-spanning) set, treat the UPDATE as HOT, drop the spanning
# entry, and silently admit a cross-partition duplicate.
# BuildSpanningIndexFromPartitions invalidates each leaf to close that window.

setup
{
  CREATE TABLE relinval (k int NOT NULL, p int NOT NULL, pad int NOT NULL DEFAULT 0)
      PARTITION BY LIST (p);
  CREATE TABLE relinval0 PARTITION OF relinval FOR VALUES IN (0);
  CREATE TABLE relinval1 PARTITION OF relinval FOR VALUES IN (1);
  -- A local index forces the leaf hot-blocking bitmap to be COMPUTED and CACHED;
  -- an index-less leaf takes a non-caching fast path, so nothing would go stale.
  CREATE INDEX relinval0_pad ON relinval0 (pad);
  CREATE INDEX relinval1_pad ON relinval1 (pad);
  INSERT INTO relinval VALUES (100, 0, 0);
}

teardown { DROP TABLE relinval; }

session s1
# Cache leaf relinval0's hot-blocking bitmap in s1's backend (pre-spanning: no k).
step s1cache  { UPDATE relinval SET pad = pad + 1 WHERE k = 100; }
# Spanning-key UPDATE.  It must be COLD now that s2 added the spanning index,
# i.e. s1 must have processed the leaf-relcache invalidation.  If the leaf cache
# is stale this is wrongly HOT and the spanning entry for k=200 is never created.
step s1upd    { UPDATE relinval SET k = 200 WHERE k = 100; }
# Cross-partition probe: k=200 is live in relinval0, so a k=200 insert into the
# OTHER partition must be rejected by the spanning index.
step s1dup    { INSERT INTO relinval VALUES (200, 1, 0); }
# No duplicate spanning key may survive.
step s1check  { SELECT k, count(*) FROM relinval GROUP BY k HAVING count(*) > 1; }

session s2
step s2add    { ALTER TABLE relinval ADD CONSTRAINT relinval_uq UNIQUE (k) GLOBAL; }

permutation s1cache s2add s1upd s1dup s1check
