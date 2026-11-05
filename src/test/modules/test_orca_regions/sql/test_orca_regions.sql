--
-- Test ORCA's orca_plan_regions_hook.
--
-- The segments look up the CustomScan methods of the plans they are sent,
-- so they must load the module too.
DO $$
BEGIN
	EXECUTE format('ALTER DATABASE %I SET session_preload_libraries = %L',
				   current_database(), 'test_orca_regions');
END
$$;
\c

SET optimizer = on;
SET optimizer_trace_fallback = on;

CREATE TABLE t (a int, b int) DISTRIBUTED BY (a);
INSERT INTO t SELECT i, i % 3 FROM generate_series(1, 10) i;
INSERT INTO t VALUES (NULL, 1), (11, NULL);
CREATE TABLE u (c int, d int) DISTRIBUTED BY (c);
INSERT INTO u SELECT i, i FROM generate_series(1, 10) i;
ANALYZE t;
ANALYZE u;

-- no regions: the plan as ORCA chose it
SET test_orca_regions.mode = off;
EXPLAIN (COSTS OFF) SELECT * FROM t WHERE b = 1;

-- a region at each table scan
SET test_orca_regions.mode = scan;
EXPLAIN (COSTS OFF) SELECT * FROM t WHERE b = 1;
SELECT * FROM t WHERE b = 1 ORDER BY a;
EXPLAIN (COSTS OFF) SELECT t.a, u.c FROM t JOIN u ON t.b = u.d;
SELECT t.a, u.c FROM t JOIN u ON t.b = u.d ORDER BY 1, 2;

-- ipc regions: rows cross gather and redistribute motions as
-- (segment, payload) rows
SET test_orca_regions.mode = ipc;
EXPLAIN (COSTS OFF) SELECT * FROM t WHERE b = 1;
SELECT * FROM t WHERE b = 1 ORDER BY a;
SELECT * FROM t ORDER BY a;
EXPLAIN (COSTS OFF) SELECT t.a, u.c FROM t JOIN u ON t.b = u.d;
SELECT t.a, u.c FROM t JOIN u ON t.b = u.d ORDER BY 1, 2;

-- the PG planner doesn't call the hook
SET optimizer = off;
EXPLAIN (COSTS OFF) SELECT * FROM t WHERE b = 1;
SET optimizer = on;

-- malformed regions: the query falls back to the PG planner
SET test_orca_regions.mode = bad_version;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = no_methods;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = bad_root;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = leaf_outside;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = nested_leaves;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = duplicate_root;
EXPLAIN (COSTS OFF) SELECT * FROM t;
-- a motion can't merge rows by an opaque payload
SET test_orca_regions.mode = sorted_motion;
EXPLAIN (COSTS OFF) SELECT * FROM t ORDER BY a;
SELECT * FROM t ORDER BY a LIMIT 3;


-- no regions under row-level security: their DXL lacks the security quals
CREATE ROLE regress_orca_regions;
GRANT SELECT ON t, u TO regress_orca_regions;
ALTER TABLE t ENABLE ROW LEVEL SECURITY;
CREATE POLICY t_small ON t USING (a < 5);
SET test_orca_regions.mode = scan;
SET ROLE regress_orca_regions;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SELECT * FROM t ORDER BY a;
-- also when the table is only in a subquery
EXPLAIN (COSTS OFF) SELECT * FROM u WHERE c IN (SELECT a FROM t);
SELECT * FROM u WHERE c IN (SELECT a FROM t) ORDER BY c;
RESET ROLE;
DROP POLICY t_small ON t;
ALTER TABLE t DISABLE ROW LEVEL SECURITY;
REVOKE SELECT ON t, u FROM regress_orca_regions;
DROP ROLE regress_orca_regions;

--
-- Tables an extension reads itself: those of a table AM ORCA doesn't plan
--
CREATE ACCESS METHOD test_orca_ext TYPE TABLE HANDLER heap_tableam_handler;
CREATE TABLE e (a int, b int) USING test_orca_ext DISTRIBUTED BY (a);
INSERT INTO e SELECT i, i % 3 FROM generate_series(1, 30) i;
ANALYZE e;
-- statistics for e to have: b = 1 in 90% of the rows
CREATE TABLE s (a int, b int) DISTRIBUTED BY (a);
INSERT INTO s SELECT i, CASE WHEN i <= 90 THEN 1 ELSE i END
FROM generate_series(1, 100) i;
ANALYZE s;
-- the rows ORCA estimates a query returns
CREATE FUNCTION plan_rows(query text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
	RETURN round((plan->0->'Plan'->>'Plan Rows')::numeric);
END
$$;

RESET test_orca_regions.mode;
EXPLAIN (COSTS OFF) SELECT * FROM e;

-- the extension's: ORCA plans reads of it, which only its regions may scan
SET test_orca_regions.ext_relation = random;
EXPLAIN (COSTS OFF) SELECT * FROM e WHERE b = 1;
SET test_orca_regions.mode = scan;
EXPLAIN (COSTS OFF) SELECT * FROM e WHERE b = 1;
SELECT * FROM e WHERE b = 1 ORDER BY a;
-- randomly distributed: a join on its distribution key moves it
EXPLAIN (COSTS OFF) SELECT count(*) FROM e JOIN t ON e.a = t.a;
SELECT count(*) FROM e JOIN t ON e.a = t.a;

-- its size and column statistics from the extension, which the hooks look
-- up with ORCA off (the module raises an error otherwise)
SET optimizer_trace_fallback = off;		-- plan_rows itself falls back
SELECT plan_rows('SELECT * FROM e WHERE b = 1');
SET test_orca_regions.ext_rows = 1000;
SELECT plan_rows('SELECT * FROM e WHERE b = 1');
SET test_orca_regions.ext_stats_from = 's';
SELECT plan_rows('SELECT * FROM e WHERE b = 1');
RESET test_orca_regions.ext_rows;
RESET test_orca_regions.ext_stats_from;
SET optimizer_trace_fallback = on;

-- regions reading it themselves, the executor evaluating the scans'
-- filters and projections
SET test_orca_regions.mode = ext_scan;
EXPLAIN (COSTS OFF, VERBOSE) SELECT a + 1 FROM e WHERE b = 1;
SELECT a + 1 FROM e WHERE b = 1 ORDER BY 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM e JOIN t ON e.a = t.a;
SELECT count(*) FROM e JOIN t ON e.a = t.a;
-- a subplan in a filter is translated once: no unused slice
SELECT disable_xform('CXformLeftSemiJoin2HashJoin');
SELECT disable_xform('CXformLeftSemiJoin2NLJoin');
SELECT disable_xform('CXformLeftSemiJoin2InnerJoin');
EXPLAIN (COSTS OFF, VERBOSE) SELECT count(*) FROM e WHERE b IN (SELECT d FROM u);
SELECT count(*) FROM e WHERE b IN (SELECT d FROM u);
-- and its scan inside a subplan, translated on its own, gets its region
EXPLAIN (COSTS OFF) SELECT count(*) FROM t WHERE a = 7 OR a IN (SELECT a FROM e WHERE b = 1);
SELECT count(*) FROM t WHERE a = 7 OR a IN (SELECT a FROM e WHERE b = 1);
SELECT enable_xform('CXformLeftSemiJoin2HashJoin');
SELECT enable_xform('CXformLeftSemiJoin2NLJoin');
SELECT enable_xform('CXformLeftSemiJoin2InnerJoin');

-- the PG planner plans writes to it
INSERT INTO e VALUES (100, 1);
SELECT count(*) FROM e;
-- and reads, if the extension says so
SET test_orca_regions.ext_relation = planner;
EXPLAIN (COSTS OFF) SELECT * FROM e;

RESET test_orca_regions.mode;
RESET test_orca_regions.ext_relation;
DROP FUNCTION plan_rows(text);
DROP TABLE t, u, e, s;
DROP ACCESS METHOD test_orca_ext;
