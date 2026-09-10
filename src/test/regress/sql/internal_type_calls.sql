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

-- the make_op chokepoint: an operator taking/returning internal must not
-- be usable from SQL either
CREATE OPERATOR ==== (leftarg = internal, rightarg = internal, procedure = numeric_avg_combine);
PREPARE p_makeop(internal, internal) AS SELECT $1 ==== $2;
DROP OPERATOR ==== (internal, internal);

-- unknown literals must not be coercible to internal, so this must not
-- match any function
SELECT numeric_avg_combine('abc', 'def');

-- casts to or from internal are rejected, via all three cast syntaxes
SELECT 'foo'::internal;
SELECT NULL::internal;
SELECT CAST(123 AS internal);
-- CoerceViaIO: casting internal to/from another type via an explicit
-- intermediate type must be rejected too, not just the direct cast
SELECT 'foo'::text::internal;
PREPARE p_coerceio(internal) AS SELECT $1::text;
-- function-style cast syntax "internal(x)" is a separate parser path from
-- "::"/CAST (it goes through ParseComplexProjection, not can_coerce_type),
-- and is stopped only because internal_in() is unconditional and non-strict
SELECT internal('foo');
SELECT internal(NULL);

-- multi-stage aggregation still works: the planner and executor invoke
-- the internal-typed transition/combine functions themselves
CREATE TABLE internal_calls_t(a int, b numeric) DISTRIBUTED BY (a);
INSERT INTO internal_calls_t SELECT g, g * 1.5 FROM generate_series(1, 1000) g;
INSERT INTO internal_calls_t VALUES (1001, NULL);
ANALYZE internal_calls_t;

-- force two-stage (partial + combine) aggregation under both the legacy
-- planner and ORCA, so the internal-typed combine functions are actually
-- exercised regardless of which optimizer this test runs under
SET optimizer_force_multistage_agg = on;

-- numeric_combine / numeric_avg_combine paths
SELECT avg(b), sum(b), count(b), stddev(b), var_samp(b) FROM internal_calls_t;
-- all-NULL input: combine functions must return honest NULL states
SELECT avg(b), sum(b) FROM internal_calls_t WHERE b IS NULL;
-- a genuinely all-NULL group (a = 1001 is alone on the "true" side), not
-- just a NULL row hiding inside an otherwise non-NULL group
SELECT (a > 1000) AS grp, avg(b), sum(b) FROM internal_calls_t GROUP BY 1 ORDER BY 1;
-- int8_avg_combine / numeric_poly_combine paths (var_pop(int4) is the
-- overload that actually uses numeric_poly_combine; var_pop(int8) uses
-- plain numeric_combine, already covered above)
SELECT avg(a::int8), sum(a::int8), avg(a::int2), var_pop(a) FROM internal_calls_t;

RESET optimizer_force_multistage_agg;

DROP TABLE internal_calls_t;
