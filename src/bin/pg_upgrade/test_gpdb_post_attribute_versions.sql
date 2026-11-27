-- Composite values stored before the upgrade, read on the upgraded cluster.
-- See test_gpdb_pre_attribute_versions.sql for the objects.  Run by
-- test_gpdb.sh when the segments have been upgraded too; its output is
-- compared with test_gpdb_post_attribute_versions.out.
\set VERBOSITY terse
SET search_path = upgrade_attver, public;

-- composite datums readable after pg_upgrade
SELECT c FROM rowver_tbl ORDER BY i;
SELECT c FROM rowver_ao ORDER BY i;
SELECT c FROM rowver_aoco ORDER BY i;
-- zero-version datum
SELECT c FROM rowunver_tbl;
-- datum with dropped last column
SELECT r FROM rowdrop_tbl;
-- datum with dropped last attribute of composite type
SELECT c FROM rowdropt_tbl;
-- datum with dropped zero-version column
SELECT r FROM rowunvdrop_tbl;
-- attribute options of zero-version column preserved
SELECT attoptions FROM pg_attribute
WHERE attrelid = 'rowunvdrop_src'::regclass AND attname = 'c';
-- view-rowtype datum
SELECT r FROM rowview_tbl;
-- self-referencing partition key and bound readable
SELECT regexp_replace(pg_get_partkeydef('rowself'::regclass) || ' ' ||
                      pg_get_expr(relpartbound, oid), '\s+', ' ', 'g')
FROM pg_class WHERE relname = 'rowself_1';
-- routing by self-referencing partition key
BEGIN;
INSERT INTO rowself VALUES (11,12,1);
SELECT count(*) FROM rowself_1;
ROLLBACK;
-- self-referencing view usable
SELECT a FROM rowselfv;
-- datums of a partitioned table's and of a leaf's row type
SELECT r, leaf FROM rowpart_tbl;
-- the same versions on every node
SELECT count(*) AS differing_between_nodes FROM (
  SELECT attrelid, attnum
  FROM (SELECT attrelid, attnum, attoptions FROM pg_attribute WHERE attrelid >= 16384
        UNION ALL
        SELECT attrelid, attnum, attoptions FROM gp_dist_random('pg_attribute') WHERE attrelid >= 16384) x
  GROUP BY 1, 2
  HAVING count(DISTINCT COALESCE((SELECT o FROM unnest(attoptions) o WHERE o LIKE 'pg_attver=%'), '-')) > 1
) y;

-- Check changed versions as well as readability.  Hold cursors retain the
-- datums after dropping their table, allowing the type to be altered.
BEGIN;
DECLARE prefix CURSOR WITH HOLD FOR SELECT c FROM rowver_tbl WHERE i = 1;
DECLARE whole CURSOR WITH HOLD FOR SELECT c FROM rowver_tbl WHERE i = 2;
COMMIT;
BEGIN;
DROP TABLE rowver_tbl, rowver_ao, rowver_aoco;
ALTER TYPE rowver_comp ALTER ATTRIBUTE b TYPE bigint;
-- unchanged prefix readable after altering upgraded type
FETCH FIRST FROM prefix;
-- changed attribute rejected after altering upgraded type
FETCH FIRST FROM whole;
ROLLBACK;
CLOSE prefix;
CLOSE whole;

-- unversioned prefix readable after pg_upgrade
SELECT c FROM rowold_tbl WHERE key = 'before-alter';
-- unversioned datum containing versioned attribute rejected
SELECT c FROM rowold_tbl WHERE key = 'after-alter';

BEGIN;
DECLARE prefix CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl WHERE key = 'prefix';
DECLARE whole CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl WHERE key = 'before-alter';
COMMIT;
BEGIN;
DROP TABLE rowold_tbl;
ALTER TYPE rowold_comp ALTER ATTRIBUTE b TYPE bigint;
-- unversioned prefix readable after altering upgraded type
FETCH FIRST FROM prefix;
-- unversioned datum with changed attribute rejected
FETCH FIRST FROM whole;
ROLLBACK;
CLOSE prefix;
CLOSE whole;
