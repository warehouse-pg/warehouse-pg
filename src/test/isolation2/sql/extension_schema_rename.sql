-- ALTER SCHEMA RENAME must wait while CREATE EXTENSION or ALTER EXTENSION
-- UPDATE uses the schema; otherwise a concurrent rename can make the rest of
-- the extension script reach objects in another schema (CVE-2026-6467).
-- Here an open transaction's CREATE EXTENSION holds the schema lock.

CREATE SCHEMA ext_rename_s;
CREATE FUNCTION wait_until_schema_rename_waits() RETURNS bool AS $$
declare
retries int; /* in func */
begin /* in func */
  retries := 1200; /* in func */
  loop /* in func */
    if exists (select 1 from pg_locks l join pg_namespace n on l.objid = n.oid /* in func */
               where l.locktype = 'object' and not l.granted /* in func */
                 and l.classid = 'pg_namespace'::regclass /* in func */
                 and n.nspname = 'ext_rename_s') then /* in func */
      return true; /* in func */
    end if; /* in func */
    if retries <= 0 then /* in func */
      return false; /* in func */
    end if; /* in func */
    perform pg_sleep(0.1); /* in func */
    retries := retries - 1; /* in func */
  end loop; /* in func */
end; /* in func */
$$ language plpgsql;

1: BEGIN;
1: DROP EXTENSION gp_inject_fault;
1: CREATE EXTENSION gp_inject_fault SCHEMA ext_rename_s;
2&: ALTER SCHEMA ext_rename_s RENAME TO ext_rename_s2;
3: SELECT wait_until_schema_rename_waits();
1: ROLLBACK;
2<:
SELECT nspname FROM pg_namespace WHERE nspname LIKE 'ext_rename_s%';
-- The rolled-back CREATE EXTENSION left gp_inject_fault where it was.
SELECT e.extname, n.nspname FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'gp_inject_fault';

DROP SCHEMA ext_rename_s2;
DROP FUNCTION wait_until_schema_rename_waits();
