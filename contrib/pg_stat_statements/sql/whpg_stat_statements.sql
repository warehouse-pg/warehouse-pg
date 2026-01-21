CREATE EXTENSION pg_stat_statements;

--
-- simple and compound statements
--
SET pg_stat_statements.track_utility = FALSE;
SELECT whpg_stat_statements_reset();

SELECT 1 AS "int";

SELECT 'hello'
  -- multiline
  AS "text";

SELECT 'world' AS "text";

-- transaction
BEGIN;
SELECT 1 AS "int";
SELECT 'hello' AS "text";
COMMIT;

-- compound transaction
BEGIN \;
SELECT 2.0 AS "float" \;
SELECT 'world' AS "text" \;
COMMIT;

-- compound with empty statements and spurious leading spacing
\;\;   SELECT 3 + 3 \;\;\;   SELECT ' ' || ' !' \;\;   SELECT 1 + 4 \;;

-- non ;-terminated statements
SELECT 1 + 1 + 1 AS "add" \gset
SELECT :add + 1 + 1 AS "add" \;
SELECT :add + 1 + 1 AS "add" \gset

-- set operator
SELECT 1 AS i UNION SELECT 2 ORDER BY i;

-- ? operator
select '{"a":1, "b":2}'::jsonb ? 'b';

-- cte
WITH t(f) AS (
  VALUES (1.0), (2.0)
)
  SELECT f FROM t ORDER BY f;

-- prepared statement with parameter
PREPARE pgss_test (int) AS SELECT $1, 'test' LIMIT 1;
EXECUTE pgss_test(1);
DEALLOCATE pgss_test;

SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- CRUD: INSERT SELECT UPDATE DELETE on test table
--
SELECT whpg_stat_statements_reset();

-- utility "create table" should not be shown
CREATE TEMP TABLE test (a int, b char(20));

INSERT INTO test VALUES(generate_series(1, 10), 'aaa');
UPDATE test SET b = 'bbb' WHERE a > 7;
DELETE FROM test WHERE a > 9;

-- explicit transaction
BEGIN;
UPDATE test SET b = '111' WHERE a = 1 ;
COMMIT;

BEGIN \;
UPDATE test SET b = '222' WHERE a = 2 \;
COMMIT ;

UPDATE test SET b = '333' WHERE a = 3 \;
UPDATE test SET b = '444' WHERE a = 4 ;

BEGIN \;
UPDATE test SET b = '555' WHERE a = 5 \;
UPDATE test SET b = '666' WHERE a = 6 \;
COMMIT ;

-- many INSERT values
INSERT INTO test (a, b) VALUES (1, 'a'), (2, 'b'), (3, 'c');

-- SELECT with constants
SELECT * FROM test WHERE a > 5 ORDER BY a ;

SELECT *
  FROM test
  WHERE a > 9
  ORDER BY a ;

-- SELECT without constants
SELECT * FROM test ORDER BY a, b;

-- SELECT with IN clause
SELECT * FROM test WHERE a IN (1, 2, 3, 4, 5) ORDER BY a, b;

SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- pg_stat_statements.track = none
--
SET pg_stat_statements.track = 'none';
SELECT whpg_stat_statements_reset();

SELECT 1 AS "one";
SELECT 1 + 1 AS "two";

SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- pg_stat_statements.track = top
--
SET pg_stat_statements.track = 'top';
SELECT whpg_stat_statements_reset();

DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END;
$$;

-- PL/pgSQL function
CREATE FUNCTION PLUS_TWO(i INTEGER) RETURNS INTEGER AS $$
DECLARE
  r INTEGER;
BEGIN
  SELECT (i + 1 + 1.0)::INTEGER INTO r;
  RETURN r;
END; $$ LANGUAGE plpgsql;

SELECT PLUS_TWO(3);
SELECT PLUS_TWO(7);

-- SQL function --- use LIMIT to keep it from being inlined
CREATE FUNCTION PLUS_ONE(i INTEGER) RETURNS INTEGER AS
$$ SELECT (i + 1.0)::INTEGER LIMIT 1 $$ LANGUAGE SQL;

SELECT PLUS_ONE(8);
SELECT PLUS_ONE(10);

SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- pg_stat_statements.track = all
--
SET pg_stat_statements.track = 'all';
SELECT whpg_stat_statements_reset();

-- we drop and recreate the functions to avoid any caching funnies
DROP FUNCTION PLUS_ONE(INTEGER);
DROP FUNCTION PLUS_TWO(INTEGER);

-- PL/pgSQL function
CREATE FUNCTION PLUS_TWO(i INTEGER) RETURNS INTEGER AS $$
DECLARE
  r INTEGER;
BEGIN
  SELECT (i + 1 + 1.0)::INTEGER INTO r;
  RETURN r;
END; $$ LANGUAGE plpgsql;

SELECT PLUS_TWO(-1);
SELECT PLUS_TWO(2);

-- SQL function --- use LIMIT to keep it from being inlined
CREATE FUNCTION PLUS_ONE(i INTEGER) RETURNS INTEGER AS
$$ SELECT (i + 1.0)::INTEGER LIMIT 1 $$ LANGUAGE SQL;

SELECT PLUS_ONE(3);
SELECT PLUS_ONE(1);

SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- utility commands
--
SET pg_stat_statements.track_utility = TRUE;
SELECT whpg_stat_statements_reset();

SELECT 1;
CREATE INDEX test_b ON test(b);
DROP TABLE test \;
DROP TABLE IF EXISTS test \;
DROP FUNCTION PLUS_ONE(INTEGER);
DROP TABLE IF EXISTS test \;
DROP TABLE IF EXISTS test \;
DROP FUNCTION IF EXISTS PLUS_ONE(INTEGER);
DROP FUNCTION PLUS_TWO(INTEGER);

SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- Track user activity and reset them
--
SELECT whpg_stat_statements_reset();
CREATE ROLE regress_stats_user1;
CREATE ROLE regress_stats_user2;

SET ROLE regress_stats_user1;

SELECT 1 AS "ONE";
SELECT 1+1 AS "TWO";

RESET ROLE;
SET ROLE regress_stats_user2;

SELECT 1 AS "ONE";
SELECT 1+1 AS "TWO";

RESET ROLE;
SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- Don't reset anything if any of the parameter is NULL
--
SELECT whpg_stat_statements_reset(NULL);
SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- remove query ('SELECT $1+$2 AS "TWO"') executed by regress_stats_user2
-- in the current_database
--
SELECT whpg_stat_statements_reset(
	(SELECT r.oid FROM pg_roles AS r WHERE r.rolname = 'regress_stats_user2'),
	(SELECT d.oid FROM pg_database As d where datname = current_database()),
	(SELECT s.queryid FROM whpg_stat_statements_aggregated AS s
				WHERE s.query = 'SELECT $1+$2 AS "TWO"' LIMIT 1));
SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- remove query ('SELECT $1 AS "ONE"') executed by two users
--
SELECT whpg_stat_statements_reset(0,0,s.queryid)
	FROM whpg_stat_statements_aggregated AS s WHERE s.query = 'SELECT $1 AS "ONE"';
SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- remove query of a user (regress_stats_user1)
--
SELECT whpg_stat_statements_reset(r.oid)
		FROM pg_roles AS r WHERE r.rolname = 'regress_stats_user1';
SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- reset all
--
SELECT whpg_stat_statements_reset(0,0,0);
SELECT query, calls, rows FROM whpg_stat_statements_aggregated ORDER BY query COLLATE "C";

--
-- cleanup
--
DROP ROLE regress_stats_user1;
DROP ROLE regress_stats_user2;

--
-- Test whpg_stat_statements view (coordinator + segments)
--
SELECT whpg_stat_statements_reset();

CREATE TEMP TABLE test_dist (id int, val text) DISTRIBUTED BY (id);
INSERT INTO test_dist SELECT i, 'data' FROM generate_series(1, 100) i;
SELECT count(*) FROM test_dist;

-- Check that whpg_stat_statements shows coordinator (gp_segment_id = -1) and segments
-- Use rows > 0 for segments since exact distribution may vary
-- Note: calls column is excluded because with Postgres planner, the entry segment
-- (which runs both generate_series and INSERT slices) will show calls=2, while other
-- segments show calls=1. The entry segment varies by gp_session_id % numsegments.
SELECT gp_segment_id, query,
       CASE WHEN gp_segment_id = -1 THEN rows ELSE (rows > 0)::int END AS rows_check
FROM whpg_stat_statements
WHERE query LIKE '%test_dist%'
  AND query NOT LIKE '%whpg_stat_statements%'
ORDER BY gp_segment_id, query COLLATE "C";

DROP TABLE test_dist;

--
-- Test with AO (Append-Optimized) table
--
SELECT whpg_stat_statements_reset();

CREATE TEMP TABLE test_ao (id int, val text) WITH (appendoptimized=true) DISTRIBUTED BY (id);
INSERT INTO test_ao SELECT i, 'aodata' FROM generate_series(1, 100) i;
SELECT count(*) FROM test_ao;

-- Segments should have rows > 0 for INSERT, coordinator dispatches (rows = 0)
-- Note: calls column excluded due to entry segment double-counting (see test_dist comment)
SELECT gp_segment_id, query,
       CASE WHEN gp_segment_id = -1 THEN rows ELSE (rows > 0)::int END AS rows_check
FROM whpg_stat_statements
WHERE query LIKE '%test_ao%'
  AND query NOT LIKE '%whpg_stat_statements%'
ORDER BY gp_segment_id, query COLLATE "C";

DROP TABLE test_ao;

--
-- Test with replicated table
--
SELECT whpg_stat_statements_reset();

CREATE TEMP TABLE test_rep (id int, val text) DISTRIBUTED REPLICATED;
INSERT INTO test_rep SELECT i, 'repdata' FROM generate_series(1, 50) i;
SELECT count(*) FROM test_rep;

-- Replicated table: all segments get all rows (50 each for INSERT)
-- Note: calls column excluded due to entry segment double-counting (see test_dist comment)
-- Note: For replicated tables, SELECT can run on any segment, so we filter it out
-- from segment results to avoid non-deterministic output
SELECT gp_segment_id, query, rows
FROM whpg_stat_statements
WHERE query LIKE '%test_rep%'
  AND query NOT LIKE '%whpg_stat_statements%'
  AND NOT (gp_segment_id >= 0 AND query LIKE '%SELECT%count%')
ORDER BY gp_segment_id, query COLLATE "C";

DROP TABLE test_rep;

--
-- Test with materialized view
--
SELECT whpg_stat_statements_reset();

-- Use regular table (not temp) since materialized views cannot reference temp tables
CREATE TABLE test_mv_base (id int, val text) DISTRIBUTED BY (id);
INSERT INTO test_mv_base SELECT i, 'mvdata' || i FROM generate_series(1, 50) i;

-- Create materialized view
CREATE MATERIALIZED VIEW test_matview AS SELECT id, val FROM test_mv_base WHERE id <= 25 DISTRIBUTED BY (id);

-- Query the materialized view
SELECT count(*) FROM test_matview;

-- Refresh the materialized view
REFRESH MATERIALIZED VIEW test_matview;

-- Query again after refresh
SELECT count(*) FROM test_matview;

-- Check stats for materialized view operations
-- Note: calls column excluded due to entry segment double-counting (see test_dist comment)
SELECT gp_segment_id, query, rows
FROM whpg_stat_statements
WHERE (query LIKE '%test_matview%' OR query LIKE '%test_mv_base%')
  AND query NOT LIKE '%whpg_stat_statements%'
ORDER BY gp_segment_id, query COLLATE "C";

DROP MATERIALIZED VIEW test_matview;
DROP TABLE test_mv_base;

--
-- Test multi-user scenario with distributed table
-- This verifies that whpg_stat_statements views correctly join segment stats
-- with coordinator stats by (userid, dbid, queryid), not just queryid.
-- If the JOIN only uses queryid, stats from different users would be incorrectly mixed.
--
SELECT whpg_stat_statements_reset();

CREATE ROLE regress_stats_user1;
CREATE ROLE regress_stats_user2;
GRANT ALL ON SCHEMA public TO regress_stats_user1, regress_stats_user2;

-- Create a distributed table that both users will query
CREATE TABLE test_multiuser (id int, val text) DISTRIBUTED BY (id);
INSERT INTO test_multiuser SELECT i, 'data' FROM generate_series(1, 10) i;
GRANT ALL ON test_multiuser TO regress_stats_user1, regress_stats_user2;

-- Reset stats after setup
SELECT whpg_stat_statements_reset();

-- User 1 runs a query on the distributed table
SET ROLE regress_stats_user1;
SELECT * FROM test_multiuser ORDER BY id;
RESET ROLE;

-- User 2 runs the same query (same queryid, different userid)
SET ROLE regress_stats_user2;
SELECT * FROM test_multiuser ORDER BY id;
RESET ROLE;

-- Check whpg_stat_statements: each user should have separate entries
SELECT
    (SELECT rolname FROM pg_roles WHERE oid = userid) as username,
    gp_segment_id,
    query,
    calls
FROM whpg_stat_statements
WHERE query LIKE '%test_multiuser%'
ORDER BY username, gp_segment_id;

-- Check whpg_stat_statements_aggregated: should show 2 separate entries (one per user)
SELECT
    (SELECT rolname FROM pg_roles WHERE oid = userid) as username,
    query,
    calls,
    rows
FROM whpg_stat_statements_aggregated
WHERE query LIKE '%test_multiuser%'
ORDER BY username;

-- Cleanup
DROP TABLE test_multiuser;
REVOKE ALL ON SCHEMA public FROM regress_stats_user1, regress_stats_user2;
DROP ROLE regress_stats_user1;
DROP ROLE regress_stats_user2;

--
-- Test with table function (RTE_TABLEFUNCTION)
-- This tests that JumbleRangeTable handles RTE_TABLEFUNCTION correctly
--
SELECT whpg_stat_statements_reset();

CREATE TABLE test_tf (a int, b text) DISTRIBUTED BY (a);
INSERT INTO test_tf VALUES (1, 'one'), (2, 'two'), (3, 'three');

-- Create two table functions with the same signature but different names
CREATE FUNCTION test_tf_func_a(input anytable) RETURNS SETOF test_tf
    AS '$libdir/regress', 'multiset_example' LANGUAGE C;
CREATE FUNCTION test_tf_func_b(input anytable) RETURNS SETOF test_tf
    AS '$libdir/regress', 'multiset_example' LANGUAGE C;

-- Query using both table functions with identical subqueries
-- These should have different queryids because different functions are called
SELECT a, b FROM test_tf_func_a(TABLE(SELECT a, b FROM test_tf SCATTER BY a)) ORDER BY a;
SELECT a, b FROM test_tf_func_b(TABLE(SELECT a, b FROM test_tf SCATTER BY a)) ORDER BY a;

-- Check that different table functions are tracked separately
SELECT query, calls
FROM whpg_stat_statements_aggregated
WHERE query LIKE 'SELECT a, b FROM test_tf_func_%'
ORDER BY query COLLATE "C";

DROP FUNCTION test_tf_func_a(anytable);
DROP FUNCTION test_tf_func_b(anytable);
DROP TABLE test_tf;

DROP EXTENSION pg_stat_statements;
