-- The attribute versions of all relations with a row type.  test_gpdb.sh
-- runs this before and after the upgrade and compares the output; a plain
-- dump does not contain the versions.
SELECT n.nspname, c.relname, a.attnum,
       COALESCE((SELECT substr(o, length('pg_attver=') + 1)
                 FROM unnest(a.attoptions) o
                 WHERE o LIKE 'pg_attver=%'), '0')
FROM pg_class c
     JOIN pg_namespace n ON n.oid = c.relnamespace
     JOIN pg_attribute a ON a.attrelid = c.oid
WHERE c.relkind IN ('r', 'v', 'm', 'c', 'f') AND a.attnum > 0
  AND n.nspname !~ '^pg_' AND n.nspname <> 'information_schema'
ORDER BY n.nspname, c.relname, a.attnum;
