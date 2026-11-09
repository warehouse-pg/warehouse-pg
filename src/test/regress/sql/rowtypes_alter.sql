--
-- Test deforming composite datums with a descriptor they were not formed
-- with. This file tests scenarios involving a single session.
--
-- To make these tests stable and not crash - even if we do not [yet] prevent
-- unsafe deforming - we change columns from bytea to text. Both are varlena
-- types, thereby preventing out-of-range accesses.  To make it easy to detect
-- that something is wrong, we use the bytea values '\x4649584d45', which
-- deforms to 'FIXME' if wrongly interpreted as text (and '\x464958544f4f' as
-- 'FIXTOO').

-- Executes DDL from within a query
CREATE FUNCTION rta_exec(cmd text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE cmd;
END;
$$;


--
-- Portals
--

-- A cursor over a constant of the row type, altered before the first fetch
CREATE TABLE rta_tab (a int, b bytea);
BEGIN;
DECLARE rta_cur CURSOR FOR
  SELECT '(1,\\x4649584d45)'::rta_tab FROM generate_series(1, 3) g;
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
FETCH ALL FROM rta_cur;
ROLLBACK;
DROP TABLE rta_tab;

-- A holdable cursor's materialized rows outlive the table they were read
-- from, which, if set, would otherwise would prevent the ALTER.
CREATE TABLE rta_ref (b bytea);
CREATE TABLE rta_tab (id int, r rta_ref);
INSERT INTO rta_tab VALUES (1, ROW('\x4649584d45'));
DECLARE rta_cur CURSOR WITH HOLD FOR SELECT * FROM rta_tab;
DROP TABLE rta_tab;
ALTER TABLE rta_ref ALTER COLUMN b TYPE text;
DO $$
DECLARE
  c refcursor := 'rta_cur';
  v record;
BEGIN
  FETCH c INTO v;
  RAISE NOTICE 'v.r.b: %', (v.r).b;
END;
$$;
CLOSE rta_cur;
DROP TABLE rta_ref;

-- A CTE scanned before and after the ALTER
BEGIN;
CREATE TABLE rta_tab (a int, b bytea);
DECLARE rta_cur CURSOR FOR
  WITH w AS (SELECT '(1,\\x4649584d45)'::rta_tab AS r)
  SELECT r::text FROM w
  UNION ALL
  SELECT r::text FROM w;
FETCH NEXT FROM rta_cur;
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
FETCH NEXT FROM rta_cur;
ROLLBACK;

-- Same for a composite type, with a row fetched before the ALTER
BEGIN;
CREATE TYPE rta_type AS (a int, b bytea);
DECLARE rta_cur CURSOR FOR
  SELECT '(1,\\x4649584d45)'::rta_type FROM generate_series(1, 3);
FETCH NEXT FROM rta_cur;
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
FETCH NEXT FROM rta_cur;
ROLLBACK;

-- Holdable cursors are only materialized at commit, after the ALTER
CREATE TABLE rta_tab (a int, b bytea);
BEGIN;
DECLARE rta_cur_const CURSOR WITH HOLD FOR
  SELECT '(1,\\x4649584d45)'::rta_tab FROM generate_series(1, 3);
DECLARE rta_cur_row CURSOR WITH HOLD FOR
  SELECT ROW(g, '\x4649584d45'::bytea)::rta_tab FROM generate_series(1, 3) g;
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
COMMIT;
FETCH NEXT FROM rta_cur_const;
FETCH NEXT FROM rta_cur_row;
CLOSE rta_cur_const;
CLOSE rta_cur_row;
DROP TABLE rta_tab;

-- Holdable cursor over a range over the row type
CREATE TABLE rta_tab (a text, b bytea);
CREATE TYPE rta_range AS RANGE (SUBTYPE = rta_tab);
DECLARE rta_cur CURSOR WITH HOLD FOR
  SELECT rta_range('(aaa,\\x4649584d45)', '(bbb,\\x4649584d45)')
  FROM generate_series(1, 3);
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
FETCH NEXT FROM rta_cur;
CLOSE rta_cur;
DROP TYPE rta_range;
DROP TABLE rta_tab;


--
-- Cached plans
--
-- These depend on the cached plan surviving the ALTER. To allow for that,
-- disable debug_discard_caches, which would lead to always replanning if > 0.
--

-- Prepared statements embedding constants of the row type.  The first ALTER
-- keeps the column's type.  With more than one VALUES row, the field access
-- is not folded into a constant at plan time.
CREATE TABLE rta_tab (a int, b bytea);
PREPARE rta_const AS SELECT '(1,\\x4649584d45)'::rta_tab;
PREPARE rta_field AS
  SELECT (r).b FROM (VALUES ('(1,\\x4649584d45)'::rta_tab),
                            ('(2,\\x464958544f4f)'::rta_tab)) v(r);
EXECUTE rta_const;
EXECUTE rta_field;
ALTER TABLE rta_tab ALTER COLUMN b TYPE bytea;
EXECUTE rta_const;
EXECUTE rta_field;
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
EXECUTE rta_const;
EXECUTE rta_field;
DEALLOCATE rta_const;
DEALLOCATE rta_field;
DROP TABLE rta_tab;

-- Same for a composite type
CREATE TYPE rta_type AS (a int, b bytea);
PREPARE rta_const AS SELECT '(1,\\x4649584d45)'::rta_type;
PREPARE rta_field AS
  SELECT (r).b FROM (VALUES ('(1,\\x4649584d45)'::rta_type),
                            ('(2,\\x464958544f4f)'::rta_type)) v(r);
EXECUTE rta_const;
EXECUTE rta_field;
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE bytea;
EXECUTE rta_const;
EXECUTE rta_field;
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
EXECUTE rta_const;
EXECUTE rta_field;
DEALLOCATE rta_const;
DEALLOCATE rta_field;
DROP TYPE rta_type;

-- Prepared statement embedding a range over the row type
CREATE TABLE rta_tab (a text, b bytea);
CREATE TYPE rta_range AS RANGE (SUBTYPE = rta_tab);
PREPARE rta_const AS
  SELECT rta_range('(aaa,\\x4649584d45)', '(bbb,\\x4649584d45)');
EXECUTE rta_const;
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
EXECUTE rta_const;
DEALLOCATE rta_const;
DROP TYPE rta_range;
DROP TABLE rta_tab;

-- A constant of the row type passed as an argument to a PL/pgSQL function
CREATE TYPE rta_type AS (a int, b bytea);
CREATE FUNCTION rta_func(r rta_type) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
  RETURN r::text;
END;
$$;
PREPARE rta_const AS SELECT rta_func('(1,\\x4649584d45)'::rta_type);
EXECUTE rta_const;
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
EXECUTE rta_const;
DEALLOCATE rta_const;
DROP FUNCTION rta_func(rta_type);
DROP TYPE rta_type;

-- A constant in a PL/pgSQL statement's cached plan, reused after the ALTER
CREATE TABLE rta_tab (a int, b bytea);
DO $$
DECLARE
  t text;
BEGIN
  FOR i IN 1..2 LOOP
    SELECT '(1,\\x4649584d45)'::rta_tab INTO t;
    RAISE NOTICE '%: %', i, t;
    IF i = 1 THEN
      ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
    END IF;
  END LOOP;
END;
$$;
DROP TABLE rta_tab;

-- A constant in a SQL function's cached plan, reused after the ALTER.  The
-- SET clause keeps the function from being inlined.
--
-- Note: In versions before 18 SQL functions don't perform caching.
CREATE TYPE rta_type AS (a int, b bytea);
CREATE FUNCTION rta_func() RETURNS text LANGUAGE sql
  SET search_path = public
  AS $$SELECT '(1,\\x4649584d45)'::rta_type::text$$;
SELECT rta_func();
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
SELECT rta_func();
DROP FUNCTION rta_func();
DROP TYPE rta_type;

-- PL/pgSQL RETURN NEXT of a record variable
CREATE TYPE rta_type AS (a int, b bytea);
CREATE FUNCTION rta_func() RETURNS SETOF rta_type LANGUAGE plpgsql AS $$
DECLARE
  v rta_type;
BEGIN
  v := ROW(1, '\x4649584d45'::bytea)::rta_type;
  RETURN NEXT v;
END;
$$;
SELECT b FROM rta_func();
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
SELECT b FROM rta_func();
DROP FUNCTION rta_func();
DROP TYPE rta_type;

-- PL/pgSQL RETURN NEXT of an expression
CREATE TYPE rta_type AS (a int, b bytea);
CREATE FUNCTION rta_func() RETURNS SETOF rta_type LANGUAGE plpgsql AS $$
BEGIN
  RETURN NEXT ROW(1, '\x4649584d45'::bytea)::rta_type;
END;
$$;
SELECT b FROM rta_func();
ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
SELECT b FROM rta_func();
DROP FUNCTION rta_func();
DROP TYPE rta_type;

-- PL/pgSQL coercion of a function result to a different row type.  Altering
-- both types keeps the descriptors identical, so no attribute mapping is
-- done.
CREATE TYPE rta_src AS (a int, b bytea);
CREATE TYPE rta_dst AS (a int, b bytea);
CREATE FUNCTION rta_func() RETURNS rta_dst LANGUAGE plpgsql AS $$
BEGIN
  RETURN ROW(1, '\x4649584d45'::bytea)::rta_src;
END;
$$;
SELECT rta_func();
ALTER TYPE rta_src ALTER ATTRIBUTE b TYPE text;
ALTER TYPE rta_dst ALTER ATTRIBUTE b TYPE text;
SELECT rta_func();
DROP FUNCTION rta_func();
DROP TYPE rta_src, rta_dst;

-- PL/pgSQL trigger returning a row expression
CREATE TABLE rta_tab (a int, b bytea);
CREATE FUNCTION rta_trig() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RETURN ROW(1, '\x4649584d45'::bytea)::rta_tab;
END;
$$;
CREATE TRIGGER rta_trig BEFORE INSERT ON rta_tab
  FOR EACH ROW EXECUTE PROCEDURE rta_trig();
INSERT INTO rta_tab VALUES (0, NULL);
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
INSERT INTO rta_tab VALUES (0, NULL);
SELECT * FROM rta_tab;
DROP TABLE rta_tab;
DROP FUNCTION rta_trig();

-- PL/pgSQL trigger returning a record variable
CREATE TABLE rta_tab (a int, b bytea);
CREATE FUNCTION rta_trig() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
  v rta_tab;
BEGIN
  v := ROW(1, '\x4649584d45'::bytea)::rta_tab;
  RETURN v;
END;
$$;
CREATE TRIGGER rta_trig BEFORE INSERT ON rta_tab
  FOR EACH ROW EXECUTE PROCEDURE rta_trig();
INSERT INTO rta_tab VALUES (0, NULL);
ALTER TABLE rta_tab ALTER COLUMN b TYPE text;
INSERT INTO rta_tab VALUES (0, NULL);
SELECT * FROM rta_tab;
DROP TABLE rta_tab;
DROP FUNCTION rta_trig();


--
-- PL/pgSQL variables
--

-- A variable assigned field-wise, returned after the ALTER
CREATE TYPE rta_type AS (b bytea);
CREATE FUNCTION rta_func() RETURNS rta_type LANGUAGE plpgsql AS $$
DECLARE
  r rta_type;
BEGIN
  SELECT '\x4649584d45'::bytea INTO r.b;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RETURN r;
END;
$$;
SELECT r, record_send(r) FROM (SELECT rta_func() AS r OFFSET 0) s;
DROP FUNCTION rta_func();
DROP TYPE rta_type;

-- A variable returned from a trigger after its type was altered.  The
-- table's row type has the layout the variable's type had before the ALTER.
CREATE TABLE rta_tab (a int, b bytea);
CREATE TYPE rta_type AS (a int, b bytea);
CREATE FUNCTION rta_trig() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
  v rta_type;
BEGIN
  v := ROW(1, '\x4649584d45'::bytea)::rta_type;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RETURN v;
END;
$$;
CREATE TRIGGER rta_trig BEFORE INSERT ON rta_tab
  FOR EACH ROW EXECUTE PROCEDURE rta_trig();
INSERT INTO rta_tab VALUES (0, NULL);
SELECT * FROM rta_tab;
DROP TABLE rta_tab;
DROP FUNCTION rta_trig();
DROP TYPE rta_type;

-- Same for a function result, coerced to another row type
CREATE TYPE rta_src AS (a int, b bytea);
CREATE TYPE rta_dst AS (a int, b bytea);
CREATE FUNCTION rta_func() RETURNS rta_dst LANGUAGE plpgsql AS $$
DECLARE
  v rta_src;
BEGIN
  v := ROW(1, '\x4649584d45'::bytea)::rta_src;
  ALTER TYPE rta_src ALTER ATTRIBUTE b TYPE text;
  RETURN v;
END;
$$;
SELECT rta_func();
DROP FUNCTION rta_func();
DROP TYPE rta_src, rta_dst;

-- Altering the first of two attributes
CREATE TYPE rta_type AS (a bytea, b bytea);
DO $$
DECLARE
  r rta_type;
BEGIN
  r := ROW('\x4649584d45', '\x464958544f4f');
  RAISE NOTICE 'before alter: %', r;
  ALTER TYPE rta_type ALTER ATTRIBUTE a TYPE text;
  RAISE NOTICE 'after alter: %', r;
END;
$$;
DROP TYPE rta_type;

-- Altering an attribute followed by a dropped attribute
CREATE TYPE rta_type AS (a bytea, b bytea);
DO $$
DECLARE
  r rta_type;
BEGIN
  r := ROW('\x4649584d45', '\x464958544f4f');
  ALTER TYPE rta_type DROP ATTRIBUTE b;
  RAISE NOTICE 'after drop: %', r;
  ALTER TYPE rta_type ALTER ATTRIBUTE a TYPE text;
  RAISE NOTICE 'after alter: %', r;
END;
$$;
DROP TYPE rta_type;

-- A datum not containing the altered attribute is safe to deform
CREATE TYPE rta_type AS (a bytea);
DO $$
DECLARE
  r rta_type;
BEGIN
  r := ROW('\x4649584d45');
  RAISE NOTICE 'before alter: %', r;
  ALTER TYPE rta_type ADD ATTRIBUTE b bytea;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RAISE NOTICE 'after alter: %', r;
END;
$$;
DROP TYPE rta_type;

-- A datum not containing a later attribute, after an earlier attribute it
-- does contain was altered
CREATE TYPE rta_type AS (a bytea);
DO $$
DECLARE
  r rta_type;
BEGIN
  r := ROW('\x4649584d45');
  ALTER TYPE rta_type ADD ATTRIBUTE b int;
  ALTER TYPE rta_type ALTER ATTRIBUTE a TYPE text;
  RAISE NOTICE 'after alter: %', r;
END;
$$;
DROP TYPE rta_type;

-- Conversion of a child's row type to the parent's that does not need to
-- map any attributes
CREATE TABLE rta_parent (a text, b bytea);
CREATE TABLE rta_child () INHERITS (rta_parent);
DO $$
DECLARE
  r rta_child;
BEGIN
  r := '(aaa,\\x4649584d45)'::rta_child;
  ALTER TABLE rta_parent ALTER COLUMN b TYPE text;
  RAISE NOTICE 'r: %', (r::rta_parent)::text;
END;
$$;
DROP TABLE rta_parent, rta_child;


--
-- Assignment between PL/pgSQL record variables whose descriptors were
-- acquired before and after the ALTER
--

-- The source row comes straight from the executor
CREATE TYPE rta_type AS (b bytea);
CREATE TABLE rta_src (b bytea);
INSERT INTO rta_src VALUES ('\x4649584d45');
DO $$
DECLARE
  r1 rta_type;
  r2 rta_type;
BEGIN
  SELECT * INTO r1 FROM rta_src;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  r2 := ROW('new')::rta_type;
  r2 := r1;
  RAISE NOTICE 'r2.b: %', r2.b;
END;
$$;
DROP TABLE rta_src;
DROP TYPE rta_type;

-- Same, but stored rather than read
CREATE TYPE rta_type AS (b bytea);
CREATE TABLE rta_src (b bytea);
INSERT INTO rta_src VALUES ('\x4649584d45');
DO $$
DECLARE
  r1 rta_type;
  r2 rta_type;
BEGIN
  SELECT * INTO r1 FROM rta_src;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  -- after the ALTER, as otherwise the ALTER would be prevented
  CREATE TABLE rta_sink (c rta_type);
  r2 := ROW('new')::rta_type;
  BEGIN
    r2 := r1;
    INSERT INTO rta_sink VALUES (r2);
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'insert or assign failed: %', sqlerrm;
  END;
END;
$$;
SELECT (c).b FROM rta_sink;
DROP TABLE rta_sink;
DROP TABLE rta_src;
DROP TYPE rta_type;

-- A source variable assigned before the ALTER
CREATE TYPE rta_type AS (b bytea);
DO $$
DECLARE
  r1 rta_type;
  r2 rta_type;
  t text;
BEGIN
  r1 := ROW('\x4649584d45')::rta_type;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  r2 := ROW('new')::rta_type;
  r2 := r1;
  RAISE NOTICE 'r2.b: %', r2.b;
  SELECT (r2).b::text INTO t;
  RAISE NOTICE 'via SQL: %', t;
END;
$$;
DROP TYPE rta_type;

-- A target variable assigned before the ALTER, the source after it
CREATE TYPE rta_type AS (b text);
DO $$
DECLARE
  r1 rta_type;
  r2 rta_type;
BEGIN
  r2 := ROW('old')::rta_type;
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE bytea;
  r1 := ROW('\x4649584d45')::rta_type;
  r2 := r1;
  RAISE NOTICE 'r2.b: %', r2.b;
END;
$$;
DROP TYPE rta_type;


--
-- Reuse of an attribute number, after the attribute's addition was rolled
-- back
--

-- Prepared statement, prepared in a rolled back transaction.  See earlier
-- comment about debug_discard_caches.
CREATE TYPE rta_type AS (a int);
BEGIN;
ALTER TYPE rta_type ADD ATTRIBUTE b bytea;
PREPARE rta_stmt AS SELECT '(1,\\x4649584d45)'::rta_type;
EXECUTE rta_stmt;
ROLLBACK;
EXECUTE rta_stmt;
ALTER TYPE rta_type ADD ATTRIBUTE b text;
EXECUTE rta_stmt;
DEALLOCATE rta_stmt;
DROP TYPE rta_type;

-- Same, with a rolled back subtransaction
CREATE TYPE rta_type AS (a int);
BEGIN;
SAVEPOINT s;
ALTER TYPE rta_type ADD ATTRIBUTE b bytea;
PREPARE rta_stmt AS SELECT '(1,\\x4649584d45)'::rta_type;
EXECUTE rta_stmt;
ROLLBACK TO SAVEPOINT s;
ALTER TYPE rta_type ADD ATTRIBUTE b text;
EXECUTE rta_stmt;
COMMIT;
DEALLOCATE rta_stmt;
DROP TYPE rta_type;

-- Same in a function
CREATE TYPE rta_type AS (a int);
CREATE FUNCTION rta_func() RETURNS void LANGUAGE plpgsql AS $$
DECLARE
  r record;
BEGIN
  BEGIN
    ALTER TYPE rta_type ADD ATTRIBUTE b bytea;
    r := '(1,\\x4649584d45)'::rta_type;
    RAISE NOTICE 'b is bytea: %', r;
    RAISE raise_exception;
  EXCEPTION WHEN raise_exception THEN
    ALTER TYPE rta_type ADD ATTRIBUTE b text;
    RAISE NOTICE 'b is text: %', r;
  END;
END;
$$;
SELECT rta_func();
DROP FUNCTION rta_func();
DROP TYPE rta_type;

-- Same in a set returning function, with the datum nested in a table's row
-- type
CREATE TYPE rta_type AS (a int);
CREATE TABLE rta_tab (c rta_type);
CREATE FUNCTION rta_func() RETURNS TABLE (a int, c rta_tab)
LANGUAGE plpgsql AS $$
BEGIN
  a := 1;
  BEGIN
    ALTER TYPE rta_type ADD ATTRIBUTE b bytea;
    c := ROW(ROW(1, '\x4649584d45'::bytea));
    RETURN NEXT;
    RAISE raise_exception;
  EXCEPTION WHEN raise_exception THEN
    RETURN NEXT;
    ALTER TYPE rta_type ADD ATTRIBUTE b text;
    RETURN NEXT;
  END;
END;
$$;
SELECT * FROM rta_func();
DROP FUNCTION rta_func();
DROP TABLE rta_tab;
DROP TYPE rta_type;

-- Same in a function, with the datum nested in a variable of a table's row
-- type
CREATE TYPE rta_type AS (a int);
CREATE TABLE rta_tab (c rta_type);
CREATE FUNCTION rta_func() RETURNS void LANGUAGE plpgsql AS $$
DECLARE
  r rta_tab;
BEGIN
  BEGIN
    ALTER TYPE rta_type ADD ATTRIBUTE b bytea;
    r := ROW(ROW(1, '\x4649584d45'::bytea));
    RAISE NOTICE 'b is bytea: %', r;
    RAISE raise_exception;
  EXCEPTION WHEN raise_exception THEN
    ALTER TYPE rta_type ADD ATTRIBUTE b text;
    RAISE NOTICE 'b is text: %', r;
  END;
END;
$$;
SELECT rta_func();
DROP FUNCTION rta_func();
DROP TABLE rta_tab;
DROP TYPE rta_type;


--
-- ALTER by a function, while the query calling it is executing
--

-- Constants formed before the ALTER, read after it
CREATE TABLE rta_tab (a int, b bytea);
SELECT '(1,\\x4649584d45)'::rta_tab,
       rta_exec('ALTER TABLE rta_tab ALTER COLUMN b TYPE text'),
       '(2,\\x464958544f4f)'::rta_tab;
DROP TABLE rta_tab;

-- Same for a composite type, in a prepared statement
CREATE TYPE rta_type AS (a int, b bytea);
PREPARE rta_stmt AS
  SELECT NULL, NULL
  UNION ALL
  SELECT NULL, rta_exec('ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text')
  UNION ALL
  SELECT '(1,\\x4649584d45)'::rta_type::text, NULL;
EXECUTE rta_stmt;
DEALLOCATE rta_stmt;
DROP TYPE rta_type;


--
-- ALTER while a single expression is being evaluated, after the descriptor
-- has been looked up but before the datum built with it is read
--

-- A volatile function as a field of a ROW() expression
CREATE TYPE rta_type AS (x int, b bytea);
CREATE FUNCTION rta_func() RETURNS int LANGUAGE plpgsql AS $$
BEGIN
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RETURN 1;
END;
$$;
SELECT (ROW(rta_func(), '\x4649584d45'::bytea)::rta_type)::text;
DROP FUNCTION rta_func();
DROP TYPE rta_type;

-- A volatile function as json_populate_record()'s argument, altering the
-- type between two rows
CREATE TYPE rta_type AS (b bytea);
CREATE FUNCTION rta_func(n int) RETURNS json LANGUAGE plpgsql AS $$
BEGIN
  IF n = 2 THEN
    ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  END IF;
  RETURN '{"b": "\\x4649584d45"}'::json;
END;
$$;
SELECT n, json_populate_record(NULL::rta_type, rta_func(n))::text
  FROM generate_series(1, 2) g(n);
DROP FUNCTION rta_func(int);
DROP TYPE rta_type;

-- A domain typed attribute, whose CHECK constraint is evaluated while
-- record_in() builds the datum
CREATE FUNCTION rta_func() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RETURN true;
END;
$$;
CREATE DOMAIN rta_dom AS boolean CHECK (rta_func());
CREATE TYPE rta_type AS (b bytea, gate rta_dom);
-- The first cast to text is to avoid the RANDOMIZE_ALLOCATED_MEMORY logic in
-- coerce_type(), which would otherwise trigger and make the test unstable.
SELECT ('(\\x4649584d45,true)'::text::rta_type)::text;
DROP TYPE rta_type;
DROP DOMAIN rta_dom;
DROP FUNCTION rta_func();

-- A prepared statement comparing an indexed column with a parameter of the
-- row type.  The ALTER is performed by a domain CHECK constraint on another
-- parameter, i.e. between the coercion of the two parameters.  That order
-- needs an index scan, which coerces both parameters before comparing.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE TYPE rta_key AS (b text);
CREATE TYPE rta_type AS (b bytea);
CREATE FUNCTION rta_func(int) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RETURN true;
END;
$$;
CREATE DOMAIN rta_dom AS integer CHECK (rta_func(VALUE));
CREATE TABLE rta_tab (k rta_key, gate integer);
CREATE INDEX ON rta_tab (k, gate);
-- a key matching the parameter only if it is misread
INSERT INTO rta_tab VALUES (ROW(convert_from('\x4649584d45', 'UTF8')), 1);
PREPARE rta_stmt(text, integer) AS
  SELECT 1 FROM rta_tab WHERE k = $1::rta_type AND gate = $2::rta_dom;
EXPLAIN (COSTS OFF) EXECUTE rta_stmt('(\\x4649584d45)', 1);
EXECUTE rta_stmt('(\\x4649584d45)', 1);
DEALLOCATE rta_stmt;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE rta_tab;
DROP DOMAIN rta_dom;
DROP FUNCTION rta_func(int);
DROP TYPE rta_type;
DROP TYPE rta_key;


--
-- Out-of-line and compressed field values
--
-- Forming a datum from a tuple with out-of-line values takes separate paths,
-- which deform the tuple and form it anew.  Some of them also decompress
-- in-line compressed values.
--

-- comp is stored compressed in-line, ext out-of-line
CREATE TABLE rta_toast (id int, b bytea, comp text, ext text);
ALTER TABLE rta_toast ALTER COLUMN comp SET STORAGE MAIN,
  ALTER COLUMN ext SET STORAGE EXTERNAL;
SELECT pg_relation_size(reltoastrelid) = 0 AS toast_empty
  FROM pg_class WHERE oid = 'rta_toast'::regclass;
INSERT INTO rta_toast VALUES (1, '\x4649584d45', repeat('c', 3000), repeat('e', 3000));
SELECT (SELECT pg_relation_size(reltoastrelid) > 0 FROM pg_class
         WHERE oid = 'rta_toast'::regclass) AS ext_out_of_line
  FROM rta_toast;

-- The result of a SQL function returning a row with such values, formed by
-- heap_copy_tuple_as_datum(), read before and after the ALTER
CREATE TYPE rta_type AS (id int, b bytea, comp text, ext text);
CREATE FUNCTION rta_toast_row() RETURNS rta_type LANGUAGE sql
  AS $$SELECT * FROM rta_toast$$;
SELECT (r).id, (r).b, length((r).comp), length((r).ext)
  FROM (SELECT rta_toast_row() AS r OFFSET 0) s;
SELECT split_part(r::text, ',', 2),
       rta_exec('ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text'),
       split_part(r::text, ',', 2)
  FROM (SELECT rta_toast_row() AS r OFFSET 0) s;
DROP FUNCTION rta_toast_row();
DROP TYPE rta_type;

-- A ROW() expression over such values, inlined by HeapTupleHeaderGetDatum()
-- with the type's current descriptor.  A function in the ROW() alters the
-- type after the descriptor the row is formed with was looked up.
CREATE TYPE rta_type AS (x int, b bytea, ext text);
CREATE FUNCTION rta_func() RETURNS int LANGUAGE plpgsql AS $$
BEGIN
  ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text;
  RETURN 1;
END;
$$;
SELECT (r).b, length((r).ext)
  FROM (SELECT ROW(0, b, ext)::rta_type AS r FROM rta_toast OFFSET 0) s;
SELECT split_part(r::text, ',', 2)
  FROM (SELECT ROW(rta_func(), b, ext)::rta_type AS r FROM rta_toast OFFSET 0) s;
DROP FUNCTION rta_func();
DROP TYPE rta_type;

DROP TABLE rta_toast;


DROP FUNCTION rta_exec(text);
