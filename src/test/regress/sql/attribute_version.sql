--
-- Tests related to attribute versioning
--
-- This simulates minor and major version upgrades leaving some composite data
-- around, verifying that we don't falsely error out, nor miss erroring out
-- when necessary.
--
-- Where an attribute is altered to a different type, it changes from bytea
-- to text, for the reasons explained in rowtypes_alter.sql.
--

-- make_tuple_unversioned() and clear_attribute_versions() are created in
-- create_function_1

-- Catalog updates only reach the node they are issued on, so the versions
-- are removed by a C function that is run on the coordinator and on every
-- segment.
CREATE FUNCTION make_type_unversioned(p_rt regtype)
  RETURNS void
  LANGUAGE plpgsql
  STRICT
  AS $$
DECLARE
   p_rc regclass;
BEGIN
   SELECT typrelid INTO STRICT p_rc FROM pg_type WHERE typtype = 'c' AND oid = p_rt;

   PERFORM clear_attribute_versions(p_rc);
   PERFORM clear_attribute_versions(p_rc) FROM gp_dist_random('gp_id');
END;
$$;

CREATE FUNCTION type_versions(p_rt regtype, OUT rc regclass, OUT rt regtype, OUT attnum int2, OUT attname name, OUT attversion int)
  RETURNS SETOF RECORD
  LANGUAGE plpgsql
  STRICT
  AS $$
DECLARE
   p_rc regclass;
   p_r record;
   p_lastver numeric = 0;
   p_lastnum int = 0;
   p_ver int;
   p_attversion numeric;
BEGIN
   SELECT typrelid INTO STRICT p_rc FROM pg_type WHERE typtype = 'c' AND oid = p_rt;

   FOR p_r IN SELECT * FROM pg_attribute pa WHERE attrelid = p_rc ORDER BY pa.attnum LOOP
     p_attversion = COALESCE((SELECT pg_catalog.substr(o, length('pg_attver') + 2)::numeric FROM pg_catalog.unnest(p_r.attoptions) o WHERE o LIKE 'pg_attver=%'), 0);

     IF p_attversion = 0 THEN
       p_ver = 0;
     ELSIF p_attversion = p_lastver THEN
       p_ver = p_lastnum;
     ELSE
       p_lastnum = p_lastnum + 1;
       p_lastver = p_attversion;
       p_ver = p_lastnum;
     END IF;

     rc = p_r.attrelid;
     rt = p_rt;
     attnum = p_r.attnum;
     attname = p_r.attname;
     attversion = p_ver;

     RETURN NEXT;
   END LOOP;
END;
$$;


--
-- Simulate a table with one unversioned attribute (i.e. created before an
-- upgrade), with a column added after the upgrade.
--

CREATE TYPE rowold_comp AS (c1 int);
CREATE TABLE rowold_tbl (key text, c rowold_comp);


-- insert tuples with 1 column
-- WHPG: on this branch the values of such an INSERT are also read on their
-- way to the segments, which rejects a datum without a version for a type
-- that has one.  The check is switched off while the simulated old values
-- are stored; they are stored as they are, without a version.
SET composite_type_mismatch = disabled;
INSERT INTO rowold_tbl
  VALUES
    ('1-cols-unver', make_tuple_unversioned(ROW(1)::rowold_comp)),
    ('1-cols-wrongver', ROW(1)::rowold_comp)
;
RESET composite_type_mismatch;

ALTER TYPE rowold_comp ADD ATTRIBUTE c2 bytea;

-- insert tuples with 2 columns
SET composite_type_mismatch = disabled;
INSERT INTO rowold_tbl
  VALUES
    ('2-cols-unver', make_tuple_unversioned(ROW(2, '\x4649584d45')::rowold_comp)),
    ('2-cols-wrongver', ROW(2, '\x4649584d45')::rowold_comp)
;
RESET composite_type_mismatch;

SELECT * FROM type_versions('rowold_comp');
SELECT make_type_unversioned('rowold_comp');
SELECT * FROM type_versions('rowold_comp');

-- insert a 2 column tuple formed while the type is unversioned, i.e. after a
-- minor version update but before any DDL; it is stamped with version 0
INSERT INTO rowold_tbl
  VALUES ('2-cols-ver0', ROW(2, '\x4649584d45')::rowold_comp);

ALTER TYPE rowold_comp ADD ATTRIBUTE c3 int;

SELECT * FROM type_versions('rowold_comp');

-- insert tuples with 3 columns
SET composite_type_mismatch = disabled;
INSERT INTO rowold_tbl
  VALUES
    ('3-cols-unver', make_tuple_unversioned(ROW(3, '\x4649584d45', 333)::rowold_comp)),
    ('3-cols-ver', ROW(3, '\x4649584d45', 333)::rowold_comp)
;
RESET composite_type_mismatch;

-- Should be able to deform unversioned 1 & 2 column tuples, they were created before
-- the "upgrade"
SELECT c FROM rowold_tbl WHERE key = '1-cols-unver';
SELECT c FROM rowold_tbl WHERE key = '2-cols-unver';
-- The same holds for the 2 column tuple stamped with version 0
SELECT c FROM rowold_tbl WHERE key = '2-cols-ver0';

-- But an unversioned 3 column tuple shouldn't be deformable, as it only could
-- have been created after the upgrade.
SELECT c FROM rowold_tbl WHERE key = '3-cols-unver';

-- Should not be able to access versioned 1 & 2 column tuples, they have the
-- wrong versions (from before we reset the versions)
SELECT c FROM rowold_tbl WHERE key = '1-cols-wrongver';
SELECT c FROM rowold_tbl WHERE key = '2-cols-wrongver';

-- But the versioned 3 column tuple is from after the versions were reset and
-- thus should be deformable.
SELECT c FROM rowold_tbl WHERE key = '3-cols-ver';


--
-- Test that if we alter a column after an upgrade, we can't deform
-- unversioned tuples that include that column.
--

-- via plpgsql
-- WHPG: PL/pgSQL has no transaction control on this branch, so the block's
-- changes are rolled back by the surrounding transaction.
BEGIN;
DO $$
DECLARE
  r1 record;
  r2 record;
BEGIN
  SELECT c INTO r1 FROM rowold_tbl WHERE key = '1-cols-unver';
  SELECT c INTO r2 FROM rowold_tbl WHERE key = '2-cols-unver';

  DROP TABLE rowold_tbl;
  ALTER TYPE rowold_comp ALTER ATTRIBUTE c2 TYPE text;

  -- should work
  RAISE NOTICE 'r1: %', r1;

  -- should fail
  BEGIN
    RAISE NOTICE 'r2: %', r2;
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'r2 failed: %', sqlerrm;
  END;

END;
$$;
ROLLBACK;


-- via cursors
BEGIN;

-- Cursors cannot be rewound here, so each one returns three copies of its
-- row and every FETCH below reads the next copy.
DECLARE c1_unver CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl, generate_series(1, 3) WHERE key = '1-cols-unver';
DECLARE c1_wrongver CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl, generate_series(1, 3) WHERE key = '1-cols-wrongver';

DECLARE c2_unver CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl, generate_series(1, 3) WHERE key = '2-cols-unver';
DECLARE c2_wrongver CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl, generate_series(1, 3) WHERE key = '2-cols-wrongver';
DECLARE c2_ver0 CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl, generate_series(1, 3) WHERE key = '2-cols-ver0';

DECLARE c3_ver CURSOR WITH HOLD FOR
  SELECT c FROM rowold_tbl, generate_series(1, 3) WHERE key = '3-cols-ver';
COMMIT;


BEGIN;

-- Should succeed, is versioned and the version is [for now] correct.
FETCH NEXT FROM c3_ver;


DROP TABLE rowold_tbl;
ALTER TYPE rowold_comp ALTER ATTRIBUTE c2 TYPE text;

SAVEPOINT after_ddl;

-- Should succeed, c1 should still have version 0 after the ALTER of c2.
FETCH NEXT FROM c1_unver;

-- Should fail, from before version reset
FETCH NEXT FROM c1_wrongver;
ROLLBACK TO SAVEPOINT after_ddl;

-- Should error out, was changed, unversioned is interpreted as version 0,
-- which does not match anymore
FETCH NEXT FROM c2_unver;
ROLLBACK TO SAVEPOINT after_ddl;

-- Should fail, from before version reset
FETCH NEXT FROM c2_wrongver;
ROLLBACK TO SAVEPOINT after_ddl;

-- Should error out like c2_unver, version 0 does not match anymore
FETCH NEXT FROM c2_ver0;
ROLLBACK TO SAVEPOINT after_ddl;

-- Should error out, schema was changed, version is wrong
FETCH NEXT FROM c3_ver;
ROLLBACK TO SAVEPOINT after_ddl;

ROLLBACK;

-- Now that the DDL is rolled back, we should be able to again look at cursors
-- that failed due to the DDL
FETCH NEXT FROM c2_unver;
FETCH NEXT FROM c3_ver;
FETCH NEXT FROM c2_ver0;


--
-- A datum of a type without attributes stays readable after attributes are
-- added.
--
CREATE TYPE rowzero_comp AS ();
CREATE TABLE rowzero_tbl (c rowzero_comp);
INSERT INTO rowzero_tbl VALUES (ROW()::rowzero_comp);
ALTER TYPE rowzero_comp ADD ATTRIBUTE a int;
SELECT c, (c).a FROM rowzero_tbl;


--
-- An unversioned datum that is neither toasted nor stored reaches
-- DatumGetHeapTupleHeader() as-is, so it has to be copied before being
-- marked as version 0.
--
CREATE TYPE rowdirect_comp AS (a int, b int);
SELECT make_type_unversioned('rowdirect_comp');
SELECT (make_tuple_unversioned(ROW(1, 2)::rowdirect_comp)).b;

-- If the type's attributes are versioned, an unversioned datum is rejected,
-- even though it was formed with the current definition.
CREATE TYPE rowdirect_ver_comp AS (a int, b int);
SELECT (make_tuple_unversioned(ROW(1, 2)::rowdirect_ver_comp)).b;


--
-- Test the composite_type_mismatch GUC
--
CREATE TYPE rowignore_comp AS (a bytea, b bytea);
BEGIN;
DECLARE c_ignore CURSOR WITH HOLD FOR
  SELECT ROW('\x4649584d45', '\x464958544f4f')::rowignore_comp
  FROM generate_series(1, 3);
COMMIT;
ALTER TYPE rowignore_comp ALTER ATTRIBUTE a TYPE bytea;
SHOW composite_type_mismatch;
SET composite_type_mismatch = warn;
FETCH NEXT FROM c_ignore;
SET composite_type_mismatch = disabled;
FETCH NEXT FROM c_ignore;
RESET composite_type_mismatch;
FETCH NEXT FROM c_ignore;

CREATE ROLE regress_av_nosuper;
SET ROLE regress_av_nosuper;
SET composite_type_mismatch = disabled;
RESET ROLE;


--
-- Constants of a relation's own row type, formed while the relation is
-- being created
--
CREATE TABLE rowself_def (a int, b int DEFAULT ('(5,6)'::rowself_def).a);
INSERT INTO rowself_def (a) VALUES (1);
SELECT * FROM rowself_def;


--
-- The cache of SQL functions is keyed on the result rowtype, compared with
-- equalRowTypes().  Altering a column to the type it already has changes only
-- the version, which still has to invalidate the cached result descriptor.
--
-- Note: In versions before 18 SQL functions don't perform caching.
--
CREATE TABLE rowfunc_tbl (a int, b int);
CREATE FUNCTION rowfunc() RETURNS rowfunc_tbl
  LANGUAGE sql AS $$SELECT 1, 2$$;
SELECT (rowfunc()).b;
ALTER TABLE rowfunc_tbl ALTER COLUMN b TYPE int;
SELECT (rowfunc()).b;
SELECT (rowfunc()).b;
\c -
SELECT (rowfunc()).b;


----
-- clean up
----

CLOSE ALL;

DROP TABLE IF EXISTS rowold_tbl;
DROP TYPE IF EXISTS rowold_comp;
DROP TABLE IF EXISTS rowzero_tbl;
DROP TYPE IF EXISTS rowzero_comp;
DROP TYPE IF EXISTS rowdirect_comp;
DROP TYPE IF EXISTS rowdirect_ver_comp;
DROP TYPE IF EXISTS rowignore_comp;
DROP FUNCTION IF EXISTS rowfunc();
DROP TABLE IF EXISTS rowfunc_tbl;
DROP TABLE IF EXISTS rowself_def;

DROP FUNCTION IF EXISTS make_type_unversioned(p_rt regtype);
DROP FUNCTION IF EXISTS type_versions(p_rt regtype);

DROP ROLE regress_av_nosuper;
