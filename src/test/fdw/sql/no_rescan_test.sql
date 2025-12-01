-- Test script for no_rescan_test_fdw
-- This demonstrates automatic materialization when FDW doesn't support rescan

-- Configure planner to use Nested Loop Join so we can see Material nodes
SET enable_hashjoin = off;     -- Disable Hash Join
SET enable_mergejoin = off;    -- Disable Merge Join
SET optimizer_enable_hashjoin = off;     -- Disable Hash Join
SET optimizer_enable_mergejoin = off;    -- Disable Merge Join
SET enable_material = on;      -- Ensure Material nodes are allowed
SET enable_nestloop = on;      -- Ensure Nested Loop is allowed

-- Create the extension
CREATE EXTENSION no_rescan_test_fdw;

-- Create server
CREATE SERVER no_rescan_server FOREIGN DATA WRAPPER no_rescan_test_fdw;

-- Create foreign table
-- The FDW will generate 10 rows with (id, data) columns
CREATE FOREIGN TABLE test_no_rescan_ft (
    id int,
    data text
) SERVER no_rescan_server;

-- Test 1: Simple scan (no rescan needed)
-- This should work fine without any Material node
SELECT * FROM test_no_rescan_ft ORDER BY id;

-- Test 2: Verify the foreign scan works
EXPLAIN (COSTS OFF) SELECT * FROM test_no_rescan_ft;

-- Note: EXPLAIN triggers BeginForeignScan/EndForeignScan for cost estimation
-- This is normal behavior and shows "scanned 0 of 10 rows" because EXPLAIN
-- doesn't actually fetch tuples, only initializes the scan

-- Create a small local table for join testing
-- Use DISTRIBUTED REPLICATED to avoid Motion nodes interfering with join choice
CREATE TABLE test_local_small (
    id int,
    name text
) DISTRIBUTED REPLICATED;

INSERT INTO test_local_small VALUES
    (1, 'one'),
    (2, 'two'),
    (3, 'three'),
    (4, 'four'),
    (5, 'five');

-- Test 3: Nested Loop Join - Material node should be automatically inserted
-- Because we disabled hash/merge joins, planner will use Nested Loop
-- and because no_rescan_test_fdw doesn't provide ReScanForeignScan,
-- a Material node will be automatically inserted
EXPLAIN (COSTS OFF)
SELECT l.id, l.name, f.data
FROM test_local_small l
INNER JOIN test_no_rescan_ft f ON l.id = f.id
ORDER BY l.id;

-- Expected plan
-- Gather Motion
--   -> Sort
--        -> Nested Loop
--             -> Seq Scan on test_local_small l
--             -> Material                    <-- Automatically inserted!
--                  -> Foreign Scan on test_no_rescan_ft f

-- Test 4: Execute the join to verify it works correctly
-- This should return 5 rows (only IDs 1-5 match)
SELECT l.id, l.name, f.data
FROM test_local_small l
INNER JOIN test_no_rescan_ft f ON l.id = f.id
ORDER BY l.id;

-- The foreign scan will be executed once and materialized
-- Even though test_local_small has 5 rows, the Material node buffers the
-- foreign scan results, so we don't need to rescan

-- Test 5: Alternative - using LATERAL join which naturally requires rescan
-- LATERAL joins require the inner side to be rescanned for each outer row
-- The Material node allows this even though the FDW doesn't support rescan
EXPLAIN (COSTS OFF)
SELECT l.id, l.name, f.data
FROM test_local_small l,
     LATERAL (SELECT * FROM test_no_rescan_ft f WHERE f.id = l.id) f
ORDER BY l.id;

-- Execute the LATERAL join - should return 5 rows
SELECT l.id, l.name, f.data
FROM test_local_small l,
     LATERAL (SELECT * FROM test_no_rescan_ft f WHERE f.id = l.id) f
ORDER BY l.id;

-- Test 6: More complex join scenario
CREATE TABLE test_local_medium (
    id int,
    category text
) DISTRIBUTED REPLICATED;  -- Use replicated to avoid Motion interference

INSERT INTO test_local_medium
SELECT i, 'category_' || (i % 3)
FROM generate_series(1, 8) i;

-- This should also show Material node with Nested Loop
EXPLAIN (COSTS OFF)
SELECT m.id, m.category, f.data
FROM test_local_medium m
INNER JOIN test_no_rescan_ft f ON m.id = f.id
WHERE m.id <= 7
ORDER BY m.id;

-- Execute the query - should return 7 rows (IDs 1-7)
SELECT m.id, m.category, f.data
FROM test_local_medium m
INNER JOIN test_no_rescan_ft f ON m.id = f.id
WHERE m.id <= 7
ORDER BY m.id;

-- Test 7: Verify that without rescan the query still works
-- (Material node buffers the data)
-- Should return count = 5
SELECT count(*)
FROM test_local_small l1
INNER JOIN test_local_small l2 ON l1.id = l2.id
INNER JOIN test_no_rescan_ft f ON l1.id = f.id;

-- Test 8: Correlated subquery limitation - execution time error
-- A correlated subquery in the SELECT list creates a SubPlan that must rescan
-- the foreign table for each outer row. SubPlans are planned independently,
-- so the foreign table scan doesn't know it will need rescanning until execution.
-- This is a known limitation: FDWs without ReScanForeignScan cannot be used
-- in correlated subqueries.
--
-- ORCA has its trick to avoid correlated subquery and rescan.
EXPLAIN (COSTS OFF)
SELECT l.id, l.name,
       (SELECT f.data FROM test_no_rescan_ft f WHERE f.id = l.id LIMIT 1) as fdata
FROM test_local_small l
ORDER BY l.id;

-- Execution fails because each SubPlan execution requires rescanning the
-- foreign table with different parameter values, which is impossible without
-- the ReScanForeignScan callback.
SELECT l.id, l.name,
       (SELECT f.data FROM test_no_rescan_ft f WHERE f.id = l.id LIMIT 1) as fdata
FROM test_local_small l
ORDER BY l.id;

-- Test 9: LATERAL query - planner avoids parameterization
-- Even though this uses LATERAL syntax, the planner can convert it to a
-- regular nested loop join with a filter condition (l.id = f.id), avoiding
-- the need for a parameterized foreign scan path. A Material node is inserted
-- to buffer the foreign scan results for rescanning.
-- This works because the foreign scan itself doesn't need parameters - the
-- filtering happens after the scan.
EXPLAIN (COSTS OFF)
SELECT l.id, l.name, f.data
FROM test_local_small l,
     LATERAL (SELECT * FROM test_no_rescan_ft f WHERE f.id = l.id) f
WHERE l.id < 3
ORDER BY l.id;

-- Executes successfully: Material buffers all foreign scan results, then
-- rescans from the buffer for each outer row while applying the join filter.
SELECT l.id, l.name, f.data
FROM test_local_small l,
     LATERAL (SELECT * FROM test_no_rescan_ft f WHERE f.id = l.id) f
WHERE l.id < 3
ORDER BY l.id;

-- Reset planner settings
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;
RESET enable_nestloop;

-- Cleanup
DROP TABLE test_local_small;
DROP TABLE test_local_medium;
DROP FOREIGN TABLE test_no_rescan_ft;
DROP SERVER no_rescan_server;
DROP EXTENSION no_rescan_test_fdw;
