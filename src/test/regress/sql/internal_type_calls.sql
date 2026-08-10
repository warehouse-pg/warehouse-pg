--
-- Test that functions taking or returning type "internal" cannot be
-- called or constructed from SQL.  (CVE-2026-14680)
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
