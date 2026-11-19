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
-- distributed by its join column with t
CREATE TABLE u2 (c int, d int) DISTRIBUTED BY (d);
INSERT INTO u2 SELECT i, i % 4 FROM generate_series(1, 20) i;
ANALYZE t;
ANALYZE u;
ANALYZE u2;

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
-- only t moves, to u2's segments: the region puts each row where u2's
-- matching rows are, by the motion's hash
EXPLAIN (COSTS OFF) SELECT t.a, u2.c FROM t JOIN u2 ON t.b = u2.d;
SELECT t.a, u2.c FROM t JOIN u2 ON t.b = u2.d ORDER BY 1, 2;
-- with one row per payload, and all of a segment's rows in one
SET test_orca_regions.batch_rows = 1;
SELECT count(*), sum(t.a), sum(u2.c) FROM t JOIN u2 ON t.b = u2.d;
SELECT * FROM t ORDER BY a;
SET test_orca_regions.batch_rows = 1000;
SELECT count(*), sum(t.a), sum(u2.c) FROM t JOIN u2 ON t.b = u2.d;
SELECT * FROM t ORDER BY a;
RESET test_orca_regions.batch_rows;
-- a CTE's producer and consumers: the rows cross the shared scans
-- encoded, and the consumers' regions decode them
SET test_orca_regions.mode = ipc_cte;
EXPLAIN (COSTS OFF) WITH c AS (SELECT * FROM t) SELECT * FROM c c1 JOIN c c2 USING (a) WHERE c1.a < 5;
WITH c AS (SELECT * FROM t) SELECT * FROM c c1 JOIN c c2 USING (a) WHERE c1.a < 5 ORDER BY 1;
-- the last child of a sequence: the sequence passes the rows on, to the
-- motion above it, or as the leaf of a region (on the coordinator)
SET test_orca_regions.mode = ipc_sequence;
EXPLAIN (COSTS OFF) WITH c AS (SELECT * FROM t WHERE a < 5) SELECT * FROM c c1 JOIN c c2 USING (a);
WITH c AS (SELECT * FROM t WHERE a < 5) SELECT * FROM c c1 JOIN c c2 USING (a) ORDER BY 1;
EXPLAIN (COSTS OFF) WITH c AS (SELECT i AS a FROM generate_series(1, 5) i) SELECT * FROM c c1 JOIN c c2 USING (a);
WITH c AS (SELECT i AS a FROM generate_series(1, 5) i) SELECT * FROM c c1 JOIN c c2 USING (a) ORDER BY 1;
SET test_orca_regions.mode = ipc;

-- the executor checks the segments a region sends rows to: those of the
-- motion
DO $$
DECLARE
	seg int;
	n bigint;
BEGIN
	FOREACH seg IN ARRAY ARRAY[-1, 0, 2, 3] LOOP
		EXECUTE format('SET test_orca_regions.segment = %s', seg);
		BEGIN
			-- planned anew, for the segment
			EXECUTE 'SELECT count(*) FROM t JOIN u2 ON t.b = u2.d' INTO n;
			RAISE NOTICE 'segment %: no error', seg;
		EXCEPTION WHEN internal_error THEN
			-- without the segment that raised it
			RAISE NOTICE 'segment %: %', seg, split_part(SQLERRM, ' (', 1);
		END;
	END LOOP;
END
$$;
RESET test_orca_regions.segment;

-- a limit over a region over a motion squelches the motion when it has
-- its row, instead of waiting for the senders, which block on a full
-- interconnect: the CustomScan's children are squelched too
CREATE TABLE big (a int, b int) DISTRIBUTED BY (a);
INSERT INTO big SELECT i, i FROM generate_series(1, 100000) i;
ANALYZE big;
SELECT disable_xform('CXformSplitLimit');
EXPLAIN (COSTS OFF) SELECT * FROM big LIMIT 1;
SET statement_timeout = '60s';
DO $$
DECLARE
	r record;
BEGIN
	-- its output varies
	FOR r IN EXPLAIN (ANALYZE, COSTS OFF) SELECT * FROM big LIMIT 1 LOOP
	END LOOP;
END
$$;
RESET statement_timeout;
SELECT enable_xform('CXformSplitLimit');
-- large payloads: all of a sender's rows for a segment in one, of hundreds
-- of kB, across a redistribute and a gather motion
SET test_orca_regions.batch_rows = 1000000;
EXPLAIN (COSTS OFF) SELECT count(*), sum(b1.a), sum(b2.b) FROM big b1 JOIN big b2 ON b1.b = b2.a;
SELECT count(*), sum(b1.a), sum(b2.b) FROM big b1 JOIN big b2 ON b1.b = b2.a;
EXPLAIN (COSTS OFF) SELECT count(*), sum(n), sum(a) FROM (SELECT a, row_number() OVER () n FROM big) s;
SELECT count(*), sum(n), sum(a) FROM (SELECT a, row_number() OVER () n FROM big) s;
RESET test_orca_regions.batch_rows;
DROP TABLE big;

-- a region with two leaves: it reads the inner one before the outer, as
-- a join does when both have motions in them
SET test_orca_regions.mode = join;
EXPLAIN (COSTS OFF) SELECT t.a, u.c FROM t JOIN u ON t.b = u.d;
SELECT t.a, u.c FROM t JOIN u ON t.b = u.d ORDER BY 1, 2;
-- a region replacing the materialize of a nested loop's inner side is
-- rescanned for each outer row: it keeps its leaf's rows, as the motion in
-- the leaf can't be rescanned
SET test_orca_regions.mode = spool;
EXPLAIN (COSTS OFF) SELECT count(*), sum(t.a), sum(u.c) FROM t, u WHERE t.a < u.c;
SELECT count(*), sum(t.a), sum(u.c) FROM t, u WHERE t.a < u.c;
SET test_orca_regions.mode = off;
SELECT count(*), sum(t.a), sum(u.c) FROM t, u WHERE t.a < u.c;

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
-- a region's operators between its root and its leaves aren't translated,
-- so none may be a motion or a scan
SET test_orca_regions.mode = span_motion;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = span_scan;
EXPLAIN (COSTS OFF) SELECT * FROM t LIMIT 1;
-- nor the root of another region, which would never be translated
SET test_orca_regions.mode = nested_root;
EXPLAIN (COSTS OFF) SELECT * FROM (SELECT * FROM t ORDER BY a LIMIT 5) s ORDER BY b;
-- nor have a subplan (under a scalar), which the leaf can't be either: its
-- SubPlan expression would never be translated
SET optimizer_enforce_subplans = on;
SET test_orca_regions.mode = subplan_leaf;
EXPLAIN (COSTS OFF) SELECT * FROM (SELECT * FROM t ORDER BY a LIMIT 5) s WHERE b < (SELECT max(d) FROM u WHERE u.c = s.a);
EXPLAIN (COSTS OFF) SELECT * FROM (SELECT * FROM t ORDER BY a LIMIT 5) s WHERE a IN (SELECT c FROM u);
RESET optimizer_enforce_subplans;
-- an ipc region's (segment, payload) rows may only reach other regions:
-- not the operator above the motion, the client, or a CTE's consumers
SET test_orca_regions.mode = ipc_unread;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = ipc_root;
EXPLAIN (COSTS OFF) SELECT * FROM t;
SET test_orca_regions.mode = ipc_cte_unread;
EXPLAIN (COSTS OFF) WITH c AS (SELECT * FROM t) SELECT * FROM c c1 JOIN c c2 USING (a) WHERE c1.a < 5;
-- nor the operator above a sequence, or the client
SET test_orca_regions.mode = ipc_sequence_unread;
EXPLAIN (COSTS OFF) WITH c AS (SELECT * FROM t WHERE a < 5) SELECT * FROM c c1 JOIN c c2 USING (a);
EXPLAIN (COSTS OFF) WITH c AS (SELECT i AS a FROM generate_series(1, 5) i) SELECT * FROM c c1 JOIN c c2 USING (a);


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
-- nor index scans, or a dynamic scan of its partitions unless its root is
-- read the same way: ORCA plans neither
CREATE INDEX e_a ON e (a);
CREATE TABLE pe (a int, b int) DISTRIBUTED BY (a) PARTITION BY RANGE (a);
CREATE TABLE pe1 PARTITION OF pe FOR VALUES FROM (0) TO (15) USING test_orca_ext;
CREATE TABLE pe2 PARTITION OF pe FOR VALUES FROM (15) TO (40) USING test_orca_ext;
SET optimizer_trace_fallback = off;
INSERT INTO pe SELECT i, i % 3 FROM generate_series(1, 30) i;
SET optimizer_trace_fallback = on;
ANALYZE pe;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM e WHERE a = 5;
SELECT * FROM e WHERE a = 5;
EXPLAIN (COSTS OFF) SELECT count(*) FROM pe WHERE b = 1;
SELECT count(*) FROM pe WHERE b = 1;
RESET test_orca_regions.mode;
EXPLAIN (COSTS OFF) SELECT * FROM e WHERE a = 5;
SET test_orca_regions.mode = scan;
RESET enable_seqscan;
DROP TABLE pe;
DROP INDEX e_a;

-- its size and column statistics from the extension, which the hooks look
-- up with ORCA off (the module raises an error otherwise). ORCA caches
-- none of its metadata, so each query sees the hooks' answers of the time:
-- 10 rows, then 1000 rows, of which b = 1 in 900
EXPLAIN SELECT * FROM e WHERE b = 1;
SET test_orca_regions.ext_rows = 1000;
EXPLAIN SELECT * FROM e WHERE b = 1;
SET test_orca_regions.ext_stats_from = 's';
EXPLAIN SELECT * FROM e WHERE b = 1;
RESET test_orca_regions.ext_rows;
RESET test_orca_regions.ext_stats_from;
EXPLAIN SELECT * FROM e WHERE b = 1;
-- nor what the extension says of it: once not the extension's, ORCA
-- doesn't plan its table AM
SET test_orca_regions.ext_relation = none;
EXPLAIN (COSTS OFF) SELECT * FROM e WHERE b = 1;
SET test_orca_regions.ext_relation = random;

-- regions reading it themselves, the executor evaluating the scans'
-- filters and projections
SET test_orca_regions.mode = ext_scan;
EXPLAIN (COSTS OFF, VERBOSE) SELECT a + 1 FROM e WHERE b = 1;
SELECT a + 1 FROM e WHERE b = 1 ORDER BY 1;
-- with a parameter, of a generic plan: the segments get its values. (An
-- outer query's parameter stays above the motion a subplan reads e through)
SET plan_cache_mode = force_generic_plan;
PREPARE e_by_b(int) AS SELECT count(*), sum(a) FROM e WHERE b = $1;
EXPLAIN (COSTS OFF, VERBOSE) EXECUTE e_by_b(1);
EXECUTE e_by_b(1);
EXECUTE e_by_b(2);
DEALLOCATE e_by_b;
RESET plan_cache_mode;
EXPLAIN (COSTS OFF) SELECT count(*) FROM e JOIN t ON e.a = t.a;
SELECT count(*) FROM e JOIN t ON e.a = t.a;
-- a subplan in a filter is translated once: no unused slice
SELECT disable_xform('CXformLeftSemiJoin2HashJoin');
SELECT disable_xform('CXformLeftSemiJoin2NLJoin');
SELECT disable_xform('CXformLeftSemiJoin2InnerJoin');
EXPLAIN (COSTS OFF, VERBOSE) SELECT count(*) FROM e WHERE b IN (SELECT d FROM u);
SELECT count(*) FROM e WHERE b IN (SELECT d FROM u);
-- and its scan inside a subplan, translated on its own, gets its region
-- (a subplan for a larger e)
SET test_orca_regions.ext_rows = 1000;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t WHERE a = 7 OR a IN (SELECT a FROM e WHERE b = 1);
SELECT count(*) FROM t WHERE a = 7 OR a IN (SELECT a FROM e WHERE b = 1);
RESET test_orca_regions.ext_rows;
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
DROP TABLE t, u, u2, e, s;
DROP ACCESS METHOD test_orca_ext;
