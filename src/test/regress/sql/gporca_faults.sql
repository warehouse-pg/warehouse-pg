--
-- ORCA tests which require gp_fault_injector
--
-- start_matchsubs
-- m/\(cost=.*\)/
-- s/\(cost=.*\)//
-- end_matchsubs

CREATE SCHEMA gporca_faults;
SET search_path = gporca_faults, public;

CREATE TABLE foo (a int, b int);
INSERT INTO foo VALUES (1,1);

-- test interruption requests to optimization
select gp_inject_fault('opt_relcache_translator_catalog_access', 'reset', 1);
select gp_inject_fault('opt_relcache_translator_catalog_access', 'interrupt', 1);
select count(*) from foo;

-- WHPG: an interrupted optimization releases its log buffer (10 MB) instead
-- of leaving it in the session's optimizer memory context (counted rather
-- than compared, so that a build without GPORCA gives the same answer)
select count(*) as held from pg_backend_memory_contexts where name = 'GPORCA Top-level Memory Context' and total_bytes >= 1024 * 1024;

-- Ensure that ORCA is not called on any process other than the coordinator QD
CREATE FUNCTION func1_nosql_vol(x int) RETURNS int AS $$
BEGIN
  RETURN $1 +1;
END
$$ LANGUAGE plpgsql VOLATILE;

-- Query that runs the function on an additional non-QD coordinator slice
-- Include the EXPLAIN to ensure that this happens in the plan.
EXPLAIN SELECT * FROM func1_nosql_vol(5), foo;

select gp_inject_fault('opt_relcache_translator_catalog_access', 'reset', 1);
select gp_inject_fault('opt_relcache_translator_catalog_access', 'interrupt', 1);
SELECT * FROM func1_nosql_vol(5), foo;

-- The fault should *not* be hit above when optimizer = off, to reset it now.
SELECT gp_inject_fault('opt_relcache_translator_catalog_access', 'reset', 1);
