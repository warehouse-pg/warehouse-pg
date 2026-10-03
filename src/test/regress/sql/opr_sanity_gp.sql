-- Additional GPDB-specific sanity checks.


-- Test that all GPDB-hashable datatypes, i.e. those that you can use as the
-- distribution key for a table, are also mergejoinable. (We use the same
-- PathKey structure internally that is used to represent sort ordering, to
-- represent the distribution columns.)
--
--- NB: This list of datatypes should match that in IsGreenplumDbHashable
select * from pg_operator where oprname='=' and oprleft = oprright
and not oprcanmerge
and oprleft IN (
  'int2'::regtype,
  'int4'::regtype,
  'int8'::regtype,
  'float4'::regtype,
  'float8'::regtype,
  'numeric'::regtype,
  'char'::regtype,
  'bpchar '::regtype,
  'text'::regtype,
  'pg_node_tree'::regtype,
  'varchar'::regtype,
  'bytea'::regtype,
  'name'::regtype,
  'oid'::regtype,
  'tid'::regtype,
  'regproc'::regtype,
  'regprocedure'::regtype,
  'regoper'::regtype,
  'regoperator'::regtype,
  'regclass'::regtype,
  'regtype'::regtype,
  'timestamp'::regtype,
  'timestamptz'::regtype,
  'date'::regtype,
  'time'::regtype,
  'timetz '::regtype,
  'interval'::regtype,
  'inet'::regtype,
  'cidr'::regtype,
  'macaddr'::regtype,
  'bit'::regtype,
  'varbit'::regtype,
  'bool'::regtype,
  'anyarray'::regtype,
  'oidvector'::regtype,
  'money'::regtype);

-- Everything in pg_catalog that PUBLIC may not execute or read. PostgreSQL's
-- system_views.sql and ours revoke a number of privileged functions and views
-- from PUBLIC; the expected output is that complete set, so a lost REVOKE
-- shows up as a vanished row and a new one as an added row.
select p.proname, pg_get_function_identity_arguments(p.oid) as args
from pg_proc p
where p.pronamespace = 'pg_catalog'::regnamespace
  and not has_function_privilege('public', p.oid, 'EXECUTE')
order by 1, 2;
select c.relname
from pg_class c
where c.relnamespace = 'pg_catalog'::regnamespace
  and c.relkind in ('r', 'v', 'm', 'S')
  and not has_table_privilege('public', c.oid, 'SELECT')
order by 1;
