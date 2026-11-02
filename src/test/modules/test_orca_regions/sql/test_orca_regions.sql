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

RESET test_orca_regions.mode;
DROP TABLE t, u;
