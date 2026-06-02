-- C1-D will add this to parallel_schedule; RED until the discriminator flip.
--
-- Demonstrates the OID-reuse hazard the partseq discriminator eliminates.
-- Under the tableoid discriminator (pre-C1-D), a partition that is attached
-- after another relation has recycled a dropped partition's OID can alias a
-- stale spanning-index entry, producing a false cross-partition conflict (or
-- silently missing a real one).  Under the partseq discriminator (C1-D), each
-- partition has an index-local, never-reused id, so a recycled pg_class OID can
-- never alias a spanning entry.  This test asserts NO false conflict.
--
-- It is intentionally NOT in parallel_schedule until C1-D makes it pass.

CREATE TABLE oir (id bigint NOT NULL, ts timestamptz NOT NULL,
    PRIMARY KEY (id) GLOBAL) PARTITION BY RANGE (ts);
CREATE TABLE oir_2024 PARTITION OF oir FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE oir_2025 PARTITION OF oir FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

INSERT INTO oir VALUES (1, '2025-06-01');   -- lands in oir_2025

-- Remove oir_2025; its spanning entries must be fully cleaned up.
ALTER TABLE oir DETACH PARTITION oir_2025;
DROP TABLE oir_2025;

-- Churn the OID counter so a new relation can reuse oir_2025's old OID, then
-- attach a fresh partition over the same range.
CREATE TABLE oir_2025 PARTITION OF oir FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

-- Re-inserting id=1 (which the dropped partition held) must succeed: there is
-- no live row with id=1 anywhere in the tree.  A stale tableoid-keyed entry
-- would wrongly reject this; partseq makes it impossible.
INSERT INTO oir VALUES (1, '2025-07-01');

SELECT count(*) AS rows_with_id_1 FROM oir WHERE id = 1;

DROP TABLE oir CASCADE;
