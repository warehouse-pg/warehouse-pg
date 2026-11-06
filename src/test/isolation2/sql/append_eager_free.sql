-- A planner Append over AO/AOCO partitions releases each partition's scan
-- once it has moved on to the next one.  AO/AOCO scans keep private read
-- buffers (one per column for AOCO), so otherwise an Append over many such
-- partitions holds all of them until the end of the query.
--
-- The Append must still be able to reopen a released scan when it is
-- rescanned.  Rescans of the Append itself, with no Motion in between, are
-- only reachable in utility mode, hence this isolation2 test.

CREATE TABLE ef_part (a int, b int,
	c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int, c9 int, c10 int,
	c11 int, c12 int, c13 int, c14 int, c15 int, c16 int, c17 int, c18 int, c19 int, c20 int)
	USING ao_column DISTRIBUTED BY (a) PARTITION BY RANGE (b);

-- Two levels: 15 mid-level partitions, each with an AOCO and an AO leaf.
DO $$
DECLARE i int; /* in func */
BEGIN /* in func */
	FOR i IN 0..14 LOOP /* in func */
		EXECUTE format('CREATE TABLE ef_part_%s PARTITION OF ef_part FOR VALUES FROM (%s) TO (%s) PARTITION BY RANGE (b)', i, i * 2, i * 2 + 2); /* in func */
		EXECUTE format('CREATE TABLE ef_part_%s_co PARTITION OF ef_part_%s FOR VALUES FROM (%s) TO (%s) USING ao_column', i, i, i * 2, i * 2 + 1); /* in func */
		EXECUTE format('CREATE TABLE ef_part_%s_ao PARTITION OF ef_part_%s FOR VALUES FROM (%s) TO (%s) USING ao_row', i, i, i * 2 + 1, i * 2 + 2); /* in func */
	END LOOP; /* in func */
END $$;

INSERT INTO ef_part SELECT i, i % 30, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i
	FROM generate_series(1, 30000) i;

-- Peak executor memory of the QE slice, from EXPLAIN ANALYZE.
CREATE FUNCTION ef_executor_kb(query text) RETURNS int AS
$$
DECLARE /* in func */
	l text; /* in func */
	kb int; /* in func */
BEGIN /* in func */
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || query LOOP /* in func */
		IF l ~ '\(slice1\)\s+Executor memory' THEN /* in func */
			kb := substring(l FROM '([0-9]+)K bytes max'); /* in func */
		END IF; /* in func */
	END LOOP; /* in func */
	RETURN kb; /* in func */
END; /* in func */
$$
LANGUAGE plpgsql;

1: SET optimizer = off;

-- Scanning all 30 leaves needs about as much memory as scanning just one.
-- Without the early release it needs roughly 30 times as much.
1: SELECT ef_executor_kb('SELECT sum(c1+c2+c3+c4+c5+c6+c7+c8+c9+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ef_part')
	< 3 * ef_executor_kb('SELECT sum(c1+c2+c3+c4+c5+c6+c7+c8+c9+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ef_part WHERE b = 0') AS one_partition_at_a_time;

1: SELECT b, count(*), sum(c1), sum(c20) FROM ef_part GROUP BY b ORDER BY b;

-- Rescans: the inner Append is rescanned once per outer row.  Its first pass
-- releases each leaf's scan; the rescans must reopen them.
0U: SET optimizer = off;
0U: SELECT o.k, s.cnt, s.total
	FROM generate_series(0, 29, 4) o(k),
	LATERAL (SELECT count(*) cnt, sum(c1 + c20) total FROM ef_part WHERE b >= o.k) s
	ORDER BY o.k;
-- Same answers from a single pass over the Append (a join under a
-- Materialize), with no rescan of it.
0U: SELECT o.k, count(*), sum(c1 + c20)
	FROM generate_series(0, 29, 4) o(k) JOIN ef_part ON b >= o.k
	GROUP BY o.k ORDER BY o.k;

-- A cursor fetched in small chunks moves on to the next leaf, and releases
-- the previous one, in a later FETCH than the one that read that leaf.  Every
-- row must still come back exactly once, in leaf order.
0U: BEGIN;
0U: DECLARE c_fwd CURSOR FOR SELECT b, a FROM ef_part WHERE a < 40;
0U: FETCH 4 FROM c_fwd;
0U: FETCH 4 FROM c_fwd;
0U: FETCH 4 FROM c_fwd;
0U: FETCH 4 FROM c_fwd;
0U: CLOSE c_fwd;
0U: COMMIT;
0Uq:

-- The same for index and bitmap scans of the leaves.  Their scans keep the
-- AO/AOCO fetch descriptors, with the same kind of read buffers.
CREATE INDEX ON ef_part (a);
ANALYZE ef_part;

1: SET enable_seqscan = off;
1: SET enable_bitmapscan = off;
1: SELECT ef_executor_kb('SELECT sum(c1+c2+c3+c4+c5+c6+c7+c8+c9+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ef_part WHERE a < 30000')
	< 3 * ef_executor_kb('SELECT sum(c1+c2+c3+c4+c5+c6+c7+c8+c9+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ef_part WHERE a < 30000 AND b = 0') AS index_one_partition_at_a_time;
1: SET enable_indexscan = off;
1: SET enable_bitmapscan = on;
1: SELECT ef_executor_kb('SELECT sum(c1+c2+c3+c4+c5+c6+c7+c8+c9+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ef_part WHERE a < 30000')
	< 3 * ef_executor_kb('SELECT sum(c1+c2+c3+c4+c5+c6+c7+c8+c9+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ef_part WHERE a < 30000 AND b = 0') AS bitmap_one_partition_at_a_time;
1q:

-- Rescans of released index and bitmap scans, checked against a single pass
-- over a sequential scan.
0U: SET optimizer = off;
0U: SET enable_seqscan = off;
0U: SET enable_bitmapscan = off;
0U: SELECT o.k, s.cnt, s.total
	FROM generate_series(0, 29, 4) o(k),
	LATERAL (SELECT count(*) cnt, sum(c1 + c20) total FROM ef_part WHERE a < 20000 AND b >= o.k) s
	ORDER BY o.k;
0U: SET enable_indexscan = off;
0U: SET enable_bitmapscan = on;
0U: SELECT o.k, s.cnt, s.total
	FROM generate_series(0, 29, 4) o(k),
	LATERAL (SELECT count(*) cnt, sum(c1 + c20) total FROM ef_part WHERE a < 20000 AND b >= o.k) s
	ORDER BY o.k;
-- Here the Append is under a parameterized NestLoop, and the bitmap index
-- scans take o.k as a run-time key.
0U: SET enable_hashjoin = off;
0U: SET enable_mergejoin = off;
0U: SET enable_material = off;
0U: SELECT count(*), sum(o.k), sum(c1 + c20)
	FROM generate_series(1, 200, 7) o(k) JOIN ef_part ON a = o.k;
0U: RESET enable_hashjoin;
0U: RESET enable_mergejoin;
0U: RESET enable_material;
-- A bitmap scan can't go backwards, so the cursor still releases leaves.
0U: BEGIN;
0U: DECLARE c_bm CURSOR FOR SELECT b, a FROM ef_part WHERE a < 40;
0U: FETCH 4 FROM c_bm;
0U: FETCH 4 FROM c_bm;
0U: FETCH 4 FROM c_bm;
0U: FETCH 4 FROM c_bm;
0U: CLOSE c_bm;
0U: COMMIT;
0U: SET enable_seqscan = on;
0U: SET enable_bitmapscan = off;
0U: SELECT o.k, count(*), sum(c1 + c20)
	FROM generate_series(0, 29, 4) o(k) JOIN ef_part ON a < 20000 AND b >= o.k
	GROUP BY o.k ORDER BY o.k;
0U: SELECT count(*), sum(o.k), sum(c1 + c20)
	FROM generate_series(1, 200, 7) o(k) JOIN ef_part ON a = o.k;
0Uq:

-- WHPG: a released leaf is marked squelched, so squelching the Append
-- later skips it; the Append must still squelch the SubPlans in the leaf's
-- quals, and the Motions under them.  Here only the first branch's qual has
-- a SubPlan, and the per-segment Limit stops the Append in the second
-- branch, after the first one was released.  The queries must finish
-- normally.
CREATE TABLE ef_sq_a1 (x int, z int) USING ao_row DISTRIBUTED BY (x);
CREATE TABLE ef_sq_a2 (x int, z int) USING ao_row DISTRIBUTED BY (x);
CREATE TABLE ef_sq_b (y int, z int) DISTRIBUTED BY (y);
INSERT INTO ef_sq_a1 SELECT i, 1 FROM generate_series(1, 30) i;
INSERT INTO ef_sq_a2 SELECT i, 1 FROM generate_series(1, 30000) i;
INSERT INTO ef_sq_b SELECT i, 1 FROM generate_series(1, 1000) i;
ANALYZE ef_sq_a1, ef_sq_a2, ef_sq_b;

1: SET optimizer = off;
-- A hashed SubPlan, with the Motion right under it.
1: EXPLAIN (COSTS OFF) SELECT count(x) FROM (SELECT x FROM ef_sq_a1
	WHERE x > 1000 OR x IN (SELECT y FROM ef_sq_b)
	UNION ALL SELECT x FROM ef_sq_a2 LIMIT 100) s;
1: SELECT count(x) FROM (SELECT x FROM ef_sq_a1
	WHERE x > 1000 OR x IN (SELECT y FROM ef_sq_b)
	UNION ALL SELECT x FROM ef_sq_a2 LIMIT 100) s;
-- A correlated SubPlan, with a Materialize over the Motion.
1: EXPLAIN (COSTS OFF) SELECT count(x) FROM (SELECT x FROM ef_sq_a1
	WHERE x >= (SELECT y FROM ef_sq_b WHERE ef_sq_b.z = ef_sq_a1.z LIMIT 1)
	UNION ALL SELECT x FROM ef_sq_a2 LIMIT 100) s;
1: SELECT count(x) FROM (SELECT x FROM ef_sq_a1
	WHERE x >= (SELECT y FROM ef_sq_b WHERE ef_sq_b.z = ef_sq_a1.z LIMIT 1)
	UNION ALL SELECT x FROM ef_sq_a2 LIMIT 100) s;
1q:

DROP TABLE ef_part;
DROP TABLE ef_sq_a1, ef_sq_a2, ef_sq_b;
DROP FUNCTION ef_executor_kb(text);
