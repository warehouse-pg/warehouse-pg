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
-- Make this table larger to encourage planner to put it on outer side
CREATE TABLE test_local_small (
    id int,
    name text
) DISTRIBUTED REPLICATED;

INSERT INTO test_local_small 
SELECT i, 'name_' || i 
FROM generate_series(1, 100) i;

-- Test 3: Nested Loop Join - Material node should be automatically inserted
-- Because we disabled hash/merge joins, planner will use Nested Loop
-- and because no_rescan_test_fdw doesn't provide ReScanForeignScan,
-- a Material node will be automatically inserted
-- Increase seq_page_cost to make local table scan more expensive,
-- encouraging planner to put it on outer side
SET seq_page_cost = 10;

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
-- This should return 10 rows (IDs 1-10 match)
SELECT l.id, l.name, f.data
FROM test_local_small l
INNER JOIN test_no_rescan_ft f ON l.id = f.id
ORDER BY l.id;

-- Reset seq_page_cost
RESET seq_page_cost;

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

-- Execute the LATERAL join - should return 10 rows
SELECT l.id, l.name, f.data
FROM test_local_small l,
     LATERAL (SELECT * FROM test_no_rescan_ft f WHERE f.id = l.id) f
ORDER BY l.id;

-- Test 6: Verify that without rescan the query still works
-- (Material node buffers the data)
-- Should return count = 10
SELECT count(*)
FROM test_local_small l1
INNER JOIN test_local_small l2 ON l1.id = l2.id
INNER JOIN test_no_rescan_ft f ON l1.id = f.id;

-- Reset planner settings
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET optimizer_enable_hashjoin;
RESET optimizer_enable_mergejoin;
RESET enable_material;
RESET enable_nestloop;

-- Cleanup
DROP TABLE test_local_small;
DROP FOREIGN TABLE test_no_rescan_ft;
DROP SERVER no_rescan_server;
DROP EXTENSION no_rescan_test_fdw;
