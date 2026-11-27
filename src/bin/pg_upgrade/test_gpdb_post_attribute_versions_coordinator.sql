-- The checks on the objects of test_gpdb_pre_attribute_versions.sql that the
-- coordinator can answer alone.  Run by test_gpdb.sh on the upgraded cluster
-- also when only the coordinator has been upgraded; its output is compared
-- with test_gpdb_post_attribute_versions_coordinator.out.
\set VERBOSITY terse
SET search_path = upgrade_attver, public;

-- attribute options of zero-version column preserved
SELECT attoptions FROM pg_attribute
WHERE attrelid = 'rowunvdrop_src'::regclass AND attname = 'c';
-- self-referencing partition key and bound readable
SELECT regexp_replace(pg_get_partkeydef('rowself'::regclass) || ' ' ||
                      pg_get_expr(relpartbound, oid), '\s+', ' ', 'g')
FROM pg_class WHERE relname = 'rowself_1';
-- self-referencing view usable
SELECT a FROM rowselfv;
