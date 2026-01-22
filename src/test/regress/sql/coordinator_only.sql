-- ==================================================================
-- Test DISTRIBUTED COORDINATOR ONLY tables
-- ==================================================================

-- ==================================================================
-- 0. Test Setup (auxiliary tables)
-- ==================================================================

-- Helper distributed table used by joins and CTAS tests
CREATE TABLE IF NOT EXISTS distributed_table_aux (
    c1 INT,
    c2 INT
) DISTRIBUTED BY (c1);

INSERT INTO distributed_table_aux
SELECT i, 18 + (i % 50)
FROM generate_series(1, 20) i;

CREATE TABLE distributed_table_aux_r AS
SELECT * FROM distributed_table_aux
DISTRIBUTED REPLICATED;

-- ==================================================================
-- 1. Basic Table Creation and Metadata
-- ==================================================================

-- Create a basic coordinator-only table
CREATE TABLE coordinator_only_heap (c1 INT, c2 TEXT, c3 NUMERIC) DISTRIBUTED COORDINATOR ONLY;
\d+ coordinator_only_heap

-- Verify the distribution policy in catalog
SELECT policytype, numsegments, distkey, distclass
FROM gp_distribution_policy
WHERE localoid = 'coordinator_only_heap'::regclass;

-- Create coordinator-only table with different storage types
CREATE TABLE coordinator_only_ao (c1 INT, c2 VARCHAR(50))
    USING ao_row
    DISTRIBUTED COORDINATOR ONLY;

\d+ coordinator_only_ao

SELECT policytype, numsegments, distkey, distclass
FROM gp_distribution_policy
WHERE localoid = 'coordinator_only_ao'::regclass;

CREATE TABLE coordinator_only_aoco (c1 BIGINT, c2 FLOAT, c3 DATE)
    USING ao_column
    DISTRIBUTED COORDINATOR ONLY;

\d+ coordinator_only_aoco

SELECT policytype, numsegments, distkey, distclass
FROM gp_distribution_policy
WHERE localoid = 'coordinator_only_aoco'::regclass;

-- Test data operations on AO table
INSERT INTO coordinator_only_ao VALUES (1001, 'ao_test_1'), (1002, 'ao_test_2');
INSERT INTO coordinator_only_ao SELECT i, 'ao_row_' || i FROM generate_series(1003, 1008) i;
SELECT gp_segment_id, * FROM coordinator_only_ao ORDER BY c1;

-- Test data operations on AOCO table
INSERT INTO coordinator_only_aoco VALUES (2001, 12.34, '2025-01-15'), (2002, 56.78, '2025-02-20');
INSERT INTO coordinator_only_aoco
SELECT i, i * 3.14, ('2025-01-01'::date + (i - 2000) * interval '1 day')::date
FROM generate_series(2003, 2012) i;
SELECT gp_segment_id, * FROM coordinator_only_aoco ORDER BY c1;

-- VACUUM and ANALYZE on AO tables
VACUUM coordinator_only_ao;
ANALYZE coordinator_only_ao;
SELECT tablename, attname FROM pg_stats WHERE tablename = 'coordinator_only_ao' ORDER BY attname;

VACUUM coordinator_only_aoco;
ANALYZE coordinator_only_aoco;
SELECT tablename, attname FROM pg_stats WHERE tablename = 'coordinator_only_aoco' ORDER BY attname;

-- ==================================================================
-- 2. Data Insertion and Verification
-- ==================================================================

-- INSERT with VALUES
INSERT INTO coordinator_only_heap VALUES (100, 'alpha', 10.5), (200, 'beta', 20.7);

-- INSERT with SELECT
INSERT INTO coordinator_only_heap
SELECT i, 'data_' || i, i * 1.5
FROM generate_series(300, 315) i;

-- Verify all data is on coordinator (gp_segment_id = -1)
SELECT gp_segment_id, count(*) FROM coordinator_only_heap GROUP BY gp_segment_id ORDER BY gp_segment_id;
SELECT gp_segment_id, count(*) FROM gp_dist_random('coordinator_only_heap') GROUP BY gp_segment_id ORDER BY gp_segment_id;

SELECT gp_segment_id, count(*) FROM coordinator_only_ao GROUP BY gp_segment_id ORDER BY gp_segment_id;
SELECT gp_segment_id, count(*) FROM gp_dist_random('coordinator_only_ao') GROUP BY gp_segment_id ORDER BY gp_segment_id;

SELECT gp_segment_id, count(*) FROM coordinator_only_aoco GROUP BY gp_segment_id ORDER BY gp_segment_id;
SELECT gp_segment_id, count(*) FROM gp_dist_random('coordinator_only_aoco') GROUP BY gp_segment_id ORDER BY gp_segment_id;

-- Verify physical storage location
SELECT pg_relation_size('coordinator_only_heap') > 0 AS has_data_on_coordinator;

-- ==================================================================
-- 3. COPY Operations
-- ==================================================================

-- COPY FROM stdin
COPY coordinator_only_heap FROM stdin WITH delimiter ',';
400,gamma,33.3
500,delta,44.4
600,epsilon,55.5
\.

-- COPY TO stdout
COPY coordinator_only_heap TO stdout;

-- COPY with ON SEGMENT (should work for now)
COPY coordinator_only_heap(c1) FROM PROGRAM 'seq <SEGID> 7' ON SEGMENT;

-- ==================================================================
-- 4. Table Maintenance Operations
-- ==================================================================

-- ANALYZE
ANALYZE coordinator_only_heap;
SELECT tablename, attname, n_distinct
FROM pg_stats
WHERE tablename = 'coordinator_only_heap'
ORDER BY attname;

-- VACUUM
VACUUM coordinator_only_heap;

-- DELETE
DELETE FROM coordinator_only_heap WHERE c1 < 200;
SELECT COUNT(*) FROM coordinator_only_heap;

-- UPDATE
UPDATE coordinator_only_heap SET c3 = c3 * 2 WHERE c1 > 300;
SELECT * FROM coordinator_only_heap ORDER BY c1 LIMIT 8;

-- TRUNCATE
TRUNCATE TABLE coordinator_only_heap;
SELECT COUNT(*) FROM coordinator_only_heap;

-- ==================================================================
-- 5. Index Operations
-- ==================================================================

-- Repopulate for index tests
INSERT INTO coordinator_only_heap
SELECT i, 'row_' || i, i * 0.75
FROM generate_series(1, 800) i;

-- Create various index types
CREATE UNIQUE INDEX idx_coordinator_only_unique ON coordinator_only_heap(c1);
CREATE INDEX idx_coordinator_only_btree ON coordinator_only_heap(c2);

-- Verify index usage
EXPLAIN SELECT * FROM coordinator_only_heap WHERE c1 = 42;
SELECT * FROM coordinator_only_heap WHERE c1 = 42;

EXPLAIN SELECT * FROM coordinator_only_heap WHERE c2 = 'row_123';
SELECT * FROM coordinator_only_heap WHERE c2 = 'row_123';

-- ==================================================================
-- 6. CREATE TABLE AS SELECT (CTAS)
-- ==================================================================

-- CTAS from a regular distributed table to distributed
CREATE TABLE ctas_from_distributed_to_distributed AS
SELECT * FROM distributed_table_aux
WHERE c2 > 25
DISTRIBUTED BY (c1);
\d+ ctas_from_distributed_to_distributed

-- CTAS from a regular distributed table to coordinator-only, should fail
CREATE TABLE ctas_from_distributed AS
SELECT * FROM distributed_table_aux
WHERE c2 > 25
DISTRIBUTED COORDINATOR ONLY;

-- CTAS from a coordinator-only table to coordinator-only, should fail
CREATE TABLE ctas_from_coordinator_only_to_only AS
SELECT * FROM coordinator_only_heap
WHERE c1 > 25
DISTRIBUTED COORDINATOR ONLY;

-- CTAS from coordinator-only table (should create hash-distributed table by default)
CREATE TABLE ctas_from_coordinator_only_no_distribution AS
SELECT * FROM coordinator_only_heap
WHERE c1 BETWEEN 50 AND 150;
\d+ ctas_from_coordinator_only_no_distribution

-- ==================================================================
-- 7. CREATE TABLE LIKE
-- ==================================================================

-- LIKE should inherit DISTRIBUTED COORDINATOR ONLY
CREATE TABLE like_coordinator_only (LIKE coordinator_only_heap);
\d+ like_coordinator_only

-- ==================================================================
-- 8. Query Planning and Execution
-- ==================================================================

-- Simple scan should execute only on coordinator
EXPLAIN SELECT * FROM coordinator_only_heap;

-- Join with distributed table (should broadcast coordinator-only table)
EXPLAIN SELECT * FROM coordinator_only_heap c, distributed_table_aux d
WHERE c.c1 = d.c2;

-- Aggregation on coordinator-only table
EXPLAIN SELECT COUNT(*), AVG(c1), MAX(c3) FROM coordinator_only_heap;
SELECT COUNT(*), AVG(c1)::NUMERIC(10,2), MAX(c3) FROM coordinator_only_heap;

-- Subquery
EXPLAIN SELECT * FROM distributed_table_aux
WHERE c2 IN (SELECT c1 FROM coordinator_only_heap WHERE c1 < 500);

-- ==================================================================
-- 9. Materialized Views
-- ==================================================================

-- Create matview with explicit DISTRIBUTED COORDINATOR ONLY, should fail
CREATE MATERIALIZED VIEW mv_coordinator_only_explicit AS
SELECT i FROM generate_series(1, 20) i
DISTRIBUTED COORDINATOR ONLY;

-- Matview from coordinator-only table (should infer hash distribution)
CREATE MATERIALIZED VIEW mv_from_coordinator_only AS
SELECT c1, c2 FROM coordinator_only_heap WHERE c1 BETWEEN 100 AND 200;
\d+ mv_from_coordinator_only;

-- ==================================================================
-- 10. Constraints
-- ==================================================================

-- Create table with constraints
CREATE TABLE coordinator_only_constraints (
    c1 SERIAL PRIMARY KEY,
    c2 VARCHAR(80) NOT NULL,
    c3 INT CHECK (c3 BETWEEN 1 AND 999),
    UNIQUE(c2)
) DISTRIBUTED COORDINATOR ONLY;

INSERT INTO coordinator_only_constraints (c2, c3) VALUES ('item_a', 50);
INSERT INTO coordinator_only_constraints (c2, c3) VALUES ('item_b', 150);
INSERT INTO coordinator_only_constraints (c2, c3) VALUES ('item_c', 250);

-- Should fail - duplicate c2
INSERT INTO coordinator_only_constraints (c2, c3) VALUES ('item_a', 75);

-- Should fail - check constraint violation
INSERT INTO coordinator_only_constraints (c2, c3) VALUES ('item_d', 1000);

-- ==================================================================
-- 11. Error Cases - Operations Not Supported
-- ==================================================================

-- Cannot use COPY with error logging on coordinator-only tables
COPY coordinator_only_heap FROM PROGRAM 'seq 1 25'
LOG ERRORS SEGMENT REJECT LIMIT 5;

-- Cannot ALTER distribution policy
ALTER TABLE coordinator_only_heap SET DISTRIBUTED RANDOMLY;

-- Cannot change existing replicated table to coordinator-only
ALTER TABLE distributed_table_aux_r SET DISTRIBUTED COORDINATOR ONLY;

-- Cannot EXPAND coordinator-only tables
ALTER TABLE coordinator_only_heap EXPAND TABLE;

-- ==================================================================
-- 12. Error Cases - External Tables
-- ==================================================================

-- Readable external tables cannot specify DISTRIBUTED clause
CREATE EXTERNAL WEB TABLE ext_readable_coordinator_only(x TEXT)
EXECUTE e'echo 1' FORMAT 'text'
DISTRIBUTED COORDINATOR ONLY;

-- Writable external tables cannot use DISTRIBUTED COORDINATOR ONLY
CREATE WRITABLE EXTERNAL WEB TABLE ext_writable_coordinator_only(x TEXT)
EXECUTE 'cat > /dev/null' FORMAT 'text'
DISTRIBUTED COORDINATOR ONLY;

-- ==================================================================
-- 13. Error Cases - Inheritance
-- ==================================================================

-- Cannot inherit from coordinator-only table
CREATE TABLE inherit_from_coordinator_only(k INT)
INHERITS (coordinator_only_heap);

-- ==================================================================
-- 14. Error Cases - Functions on Segments
-- ==================================================================

-- Functions accessing coordinator-only tables should error on segments
CREATE OR REPLACE FUNCTION get_coordinator_only_data()
RETURNS SETOF coordinator_only_heap AS $$
BEGIN
    RETURN QUERY SELECT * FROM coordinator_only_heap;
END;
$$ LANGUAGE plpgsql EXECUTE ON ALL SEGMENTS;

SELECT * FROM get_coordinator_only_data();

-- Functions accessing coordinator-only tables should work on the coordinator
CREATE OR REPLACE FUNCTION get_coordinator_only_data_on_coordinator()
RETURNS SETOF coordinator_only_heap AS $$
BEGIN
    RETURN QUERY SELECT * FROM coordinator_only_heap;
END;
$$ LANGUAGE plpgsql EXECUTE ON COORDINATOR;

SELECT count(*) FROM get_coordinator_only_data_on_coordinator();

-- ==================================================================
-- 15. Partitioned Tables (should not be allowed)
-- ==================================================================

-- Coordinator-only tables cannot be partitioned
CREATE TABLE coordinator_only_partitioned (
    c1 INT,
    c2 DATE
) DISTRIBUTED COORDINATOR ONLY
PARTITION BY RANGE (c2)
(
    START ('2025-01-01'::DATE) END ('2025-12-31'::DATE) EVERY (INTERVAL '3 months')
);

-- ==================================================================
-- 16. Cleanup
-- ==================================================================

-- Before cleanup: verify coordinator-only tables exist in segments catalog
SELECT gp_segment_id, count(*)
FROM gp_dist_random('pg_class')
WHERE relname IN ('coordinator_only_heap', 'coordinator_only_ao', 'coordinator_only_aoco',
                  'like_coordinator_only', 'coordinator_only_constraints')
GROUP BY gp_segment_id
ORDER BY gp_segment_id;

-- Left two coordinator-only tables for following pg_upgrade and gpcheckcat tests
--  DROP TABLE IF EXISTS coordinator_only_heap CASCADE;
DROP TABLE IF EXISTS coordinator_only_ao CASCADE;
--  DROP TABLE IF EXISTS coordinator_only_aoco CASCADE;
DROP TABLE IF EXISTS like_coordinator_only CASCADE;
DROP TABLE IF EXISTS coordinator_only_constraints CASCADE;
DROP FUNCTION IF EXISTS get_coordinator_only_data() CASCADE;
DROP FUNCTION IF EXISTS get_coordinator_only_data_on_coordinator() CASCADE;

-- Extra cleanup for auxiliary and optional objects
DROP TABLE IF EXISTS distributed_table_aux CASCADE;
DROP TABLE IF EXISTS distributed_table_aux_r CASCADE;

-- After cleanup: verify coordinator-only tables no longer exist in segments catalog
SELECT gp_segment_id, count(*)
FROM gp_dist_random('pg_class')
WHERE relname IN ('coordinator_only_heap', 'coordinator_only_ao', 'coordinator_only_aoco',
                  'like_coordinator_only', 'coordinator_only_constraints')
GROUP BY gp_segment_id
ORDER BY gp_segment_id;
