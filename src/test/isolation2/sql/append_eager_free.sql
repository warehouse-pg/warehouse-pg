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
-- Same answers, each computed by a separate scan.
0U: SELECT k, (SELECT count(*) FROM ef_part WHERE b >= k) cnt,
	(SELECT sum(c1 + c20) FROM ef_part WHERE b >= k) total
	FROM generate_series(0, 29, 4) k ORDER BY k;

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

DROP TABLE ef_part;
DROP FUNCTION ef_executor_kb(text);
