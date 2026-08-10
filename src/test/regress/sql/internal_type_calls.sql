--
-- Test that functions taking or returning type "internal" cannot be
-- called or constructed from SQL, and that aggregate combine functions
-- (which WarehousePG exercises heavily via multi-stage aggregation)
-- still work correctly.  (CVE-2026-14680)
--

-- functions returning type internal must not be callable from SQL
SELECT internal_in('foo');

-- functions accepting type internal must not be callable from SQL,
-- even when the argument is an internal-typed parameter
PREPARE p_internal(internal) AS SELECT numeric_avg_serialize($1);

-- unknown literals must not be coercible to internal, so this must not
-- match any function
SELECT numeric_avg_combine('abc', 'def');

-- casts to or from internal are rejected
SELECT 'foo'::internal;
SELECT NULL::internal;
SELECT CAST(123 AS internal);

-- multi-stage aggregation still works: the planner and executor invoke
-- the internal-typed transition/combine functions themselves
CREATE TABLE internal_calls_t(a int, b numeric) DISTRIBUTED BY (a);
INSERT INTO internal_calls_t SELECT g, g * 1.5 FROM generate_series(1, 1000) g;
INSERT INTO internal_calls_t VALUES (1001, NULL);

-- numeric_combine / numeric_avg_combine paths
SELECT avg(b), sum(b), count(b), stddev(b), var_samp(b) FROM internal_calls_t;
-- all-NULL input: combine functions must return honest NULL states
SELECT avg(b), sum(b) FROM internal_calls_t WHERE b IS NULL;
SELECT a % 3 AS grp, avg(b), sum(b) FROM internal_calls_t GROUP BY 1 ORDER BY 1;
-- int8_avg_combine / numeric_poly_combine paths
SELECT avg(a::int8), sum(a::int8), avg(a::int2), var_pop(a::int8) FROM internal_calls_t;
-- array_agg_combine path
SELECT array_agg(a ORDER BY a) FROM internal_calls_t WHERE a <= 3;

DROP TABLE internal_calls_t;
