-- Staged Scan: two-phase AOCO scan.
--
-- Phase A scans only the filter column and records the surviving row numbers;
-- Phase B materializes the other columns for just those rows, skipping any
-- varblock that holds no survivor.
--
-- The contract this test defends is that turning Staged Scan on must never
-- change a query's answer, and that it actually skips blocks when the
-- survivors are clustered.
--
-- Note: no CREATE SCHEMA / search_path here, because that would hide
-- gp_inject_fault(); and no SQL wrapper functions over the table, because a
-- function that reads a relation cannot be dispatched to a QE slice.

SET optimizer = off;			-- v1 is Postgres-planner only

CREATE FUNCTION gi_arm(faultname text, tab text) RETURNS bool AS $$
    SELECT bool_and(gp_inject_fault(faultname, 'skip', '', '', tab,
                                    1, -1, 0, dbid) = 'Success:')
      FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
$$ LANGUAGE sql;

CREATE FUNCTION gi_hits(faultname text) RETURNS bigint AS $$
    SELECT sum(substring(gp_inject_fault(faultname, 'status', dbid)
                         from 'num times hit:''(\d+)''')::bigint)::bigint
      FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
$$ LANGUAGE sql;

CREATE FUNCTION gi_reset(faultname text) RETURNS bool AS $$
    SELECT bool_and(gp_inject_fault(faultname, 'reset', dbid) = 'Success:')
      FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
$$ LANGUAGE sql;

-- 'clus' matches a narrow band of row numbers; 'scat' matches a comparable
-- number of rows spread evenly over the whole table.
CREATE TABLE staged_gi(a int, b bigint, c text, clus int, scat int)
  USING ao_column DISTRIBUTED BY (a);
INSERT INTO staged_gi
  SELECT i, i * 2, 'payload ' || i,
         CASE WHEN i BETWEEN 50000 AND 50200 THEN 7 ELSE 1 END,
         i % 1000
    FROM generate_series(1, 200000) i;

--
-- 1. Staged Scan must not change results. Each pair below is run with the
--    feature on and then off; identical rows in the expected file are the
--    assertion.
--
SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7;

-- scattered predicate
SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE scat = 7;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE scat = 7;

-- no match at all
SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(a) sa FROM staged_gi WHERE clus = 99999;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa FROM staged_gi WHERE clus = 99999;

-- everything matches: must fall back cleanly and still be correct
SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(a) sa FROM staged_gi WHERE clus >= 0;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa FROM staged_gi WHERE clus >= 0;

-- a range predicate, and one on the distribution key
SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE a BETWEEN 1000 AND 1200;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE a BETWEEN 1000 AND 1200;

-- NULL handling: IS NULL and a predicate that never matches NULLs
INSERT INTO staged_gi VALUES (300001, NULL, NULL, 7, NULL);
SET whpg_enable_staged_scan = on;
SELECT count(*) n, count(c) nc FROM staged_gi WHERE clus = 7;
SELECT count(*) n FROM staged_gi WHERE scat IS NULL;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, count(c) nc FROM staged_gi WHERE clus = 7;
SELECT count(*) n FROM staged_gi WHERE scat IS NULL;

--
-- 2. Phase B really skips blocks when the survivors are clustered.
--
--    The table has no index yet, hence no block directory: every block between
--    two survivors has its header read and its content stepped over.
SET whpg_enable_staged_scan = on;
SELECT gi_arm('StagedScan_PhaseB_BlockSkipped', 'staged_gi'),
       gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_gi');

SELECT count(c), sum(a) FROM staged_gi WHERE clus = 7;

SELECT gi_hits('StagedScan_PhaseB_BlockSkipped') > 0 AS clustered_skipped_blocks,
       gi_hits('StagedScan_PhaseB_BlockSkipped') >
       gi_hits('StagedScan_PhaseB_BlockDecompressed') AS skipped_more_than_read;

--    (psql variables, not a table: a query over a table would run the
--    injector helpers on a QE, where they return nothing.)
SELECT gi_hits('StagedScan_PhaseB_BlockSkipped') AS walk_skipped,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') AS walk_decompressed \gset
SELECT gi_reset('StagedScan_PhaseB_BlockSkipped'),
       gi_reset('StagedScan_PhaseB_BlockDecompressed');

--    EXPLAIN ANALYZE bills the same blocks without an injector: "Staged Scan
--    Blocks: D decompressed, S skipped". The line is per statement, so one
--    more run of the query must report exactly the counts captured above.
CREATE FUNCTION gi_blocks(q text) RETURNS text AS $BODY$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
        IF line LIKE '%Stage Blocks:%' THEN
            RETURN trim(substring(line from 'Stage Blocks: (.*)$'));
        END IF;
    END LOOP;
    RETURN '-';
END $BODY$ LANGUAGE plpgsql;
SELECT gi_blocks('SELECT count(c), sum(a) FROM staged_gi WHERE clus = 7')
       = format('%s decompressed, %s skipped', :walk_decompressed, :walk_skipped)
       AS blocks_line_matches_walk;

--    An index on an unrelated column gives the table a block directory (and
--    must leave the seq scan and its Staged Scan alone, checked in 4). With
--    the directory Phase B seeks straight to the survivors' blocks: the same
--    blocks are decompressed, but the ones in between are neither read nor
--    stepped over, so far fewer headers are visited. The answer is the same.
CREATE INDEX staged_gi_a_idx ON staged_gi(a);
SELECT gi_arm('StagedScan_PhaseB_Seek', 'staged_gi'),
       gi_arm('StagedScan_PhaseB_BlockSkipped', 'staged_gi'),
       gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_gi');

SELECT count(c), sum(a) FROM staged_gi WHERE clus = 7;

SELECT gi_hits('StagedScan_PhaseB_Seek') > 0 AS clustered_seeked,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') = :walk_decompressed AS same_blocks_decompressed,
       gi_hits('StagedScan_PhaseB_Seek') + gi_hits('StagedScan_PhaseB_BlockSkipped')
         < :walk_skipped AS fewer_headers_visited;

--    With the directory the bill is the same to the block: the blocks a seek
--    jumps past are never read, so the injector never sees them, but EXPLAIN
--    counts them from the directory's entries and reports what the header
--    walk reported.
SELECT gi_blocks('SELECT count(c), sum(a) FROM staged_gi WHERE clus = 7')
       = format('%s decompressed, %s skipped', :walk_decompressed, :walk_skipped)
       AS blocks_line_matches_walk_after_index;

SELECT gi_reset('StagedScan_PhaseB_Seek'),
       gi_reset('StagedScan_PhaseB_BlockSkipped'),
       gi_reset('StagedScan_PhaseB_BlockDecompressed');

--    EXPLAIN shows the scan as "Staged Seq Scan" with the Phase A column on
--    its own line; ANALYZE adds how many rows Phase A kept out of how many it
--    scanned, summed over the segments. Timing and buffers are left out so the
--    output is stable.
EXPLAIN (COSTS OFF) SELECT count(c) FROM staged_gi WHERE clus = 7;
CREATE FUNCTION gi_survivors(q text) RETURNS text AS $BODY$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
        IF line LIKE '%Stage Survivors:%' THEN
            RETURN trim(substring(line from 'Stage Survivors: (.*)$'));
        END IF;
    END LOOP;
    RETURN '-';
END $BODY$ LANGUAGE plpgsql;
SELECT gi_survivors('SELECT count(c) FROM staged_gi WHERE clus = 7') AS survivors_of_rows,
       gi_survivors('SELECT count(c) FROM staged_gi WHERE clus = 7 AND scat = 0') AS survivors_two_stages;

--    "Rows Removed by Filter" keeps its meaning: the rows the predicate
--    removed, whichever phase did it, so it reads the same with the feature
--    on and off -- for one stage, for two, and (section 4 below) once rows
--    have been deleted, which an ordinary scan never shows to its filter.
CREATE FUNCTION gi_removed(q text) RETURNS text AS $BODY$
DECLARE line text; on_txt text := '-'; off_txt text := '-';
BEGIN
    PERFORM set_config('whpg_enable_staged_scan', 'on', false);
    FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
        IF line LIKE '%Rows Removed by Filter:%' THEN on_txt := trim(substring(line from 'Rows Removed by Filter: (.*)$')); END IF;
    END LOOP;
    PERFORM set_config('whpg_enable_staged_scan', 'off', false);
    FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
        IF line LIKE '%Rows Removed by Filter:%' THEN off_txt := trim(substring(line from 'Rows Removed by Filter: (.*)$')); END IF;
    END LOOP;
    PERFORM set_config('whpg_enable_staged_scan', 'on', false);
    RETURN on_txt || CASE WHEN on_txt = off_txt THEN ' (same off)' ELSE ' (off: ' || off_txt || ')' END;
END $BODY$ LANGUAGE plpgsql;
SELECT gi_removed('SELECT count(c) FROM staged_gi WHERE clus = 7')              AS removed_one_stage,
       gi_removed('SELECT count(c) FROM staged_gi WHERE clus = 7 AND scat = 0') AS removed_two_stages;

--
-- 3. Nothing to fetch after Phase A.
--
--    count(*) projects no other column: the scan is still staged, Phase A
--    alone finds the survivors, and Phase B has nothing to decompress.
SELECT gi_arm('StagedScan_PhaseA_Survivor', 'staged_gi');
SELECT gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_gi');
SELECT count(*) FROM staged_gi WHERE clus = 7;
SELECT gi_hits('StagedScan_PhaseA_Survivor') > 0 AS staged_for_count_star,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') = 0 AS no_phase_b_for_count_star;
SELECT gi_reset('StagedScan_PhaseA_Survivor');
SELECT gi_reset('StagedScan_PhaseB_BlockDecompressed');

--    A two-column predicate is staged: the second column is fetched for
--    Phase A's survivors only, its clause evaluated, and the projected
--    columns fetched for the rows that pass. So the second column skips
--    blocks, rows are rejected between the two, and the projected columns see
--    a smaller survivor set than Phase A produced.
SELECT gi_arm('StagedScan_PhaseA_Survivor', 'staged_gi'),
       gi_arm('StagedScan_Stage_BlockSkipped', 'staged_gi'),
       gi_arm('StagedScan_Stage_Seek', 'staged_gi'),
       gi_arm('StagedScan_Stage_BlockDecompressed', 'staged_gi'),
       gi_arm('StagedScan_Stage_Rejected', 'staged_gi'),
       gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_gi');
--    (clus = 7 holds for 201 rows plus the NULL row inserted above; exactly
--    one of them has scat = 0. Do not query staged_gi again while the faults
--    are armed: the counters would double.)
SELECT count(c) FROM staged_gi WHERE clus = 7 AND scat = 0;
SELECT gi_hits('StagedScan_PhaseA_Survivor') AS phase_a_survivors,
       gi_hits('StagedScan_Stage_Rejected') AS rejected_by_stage_2,
       gi_hits('StagedScan_PhaseA_Survivor') - gi_hits('StagedScan_Stage_Rejected') = 1 AS survivors_minus_rejected_is_answer,
       gi_hits('StagedScan_Stage_BlockSkipped') + gi_hits('StagedScan_Stage_Seek') > 0 AS stage_2_avoided_blocks,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') <= gi_hits('StagedScan_Stage_BlockDecompressed') AS projected_read_no_more_than_stage_2;
SELECT gi_reset('StagedScan_PhaseA_Survivor'),
       gi_reset('StagedScan_Stage_BlockSkipped'),
       gi_reset('StagedScan_Stage_Seek'),
       gi_reset('StagedScan_Stage_BlockDecompressed'),
       gi_reset('StagedScan_Stage_Rejected'),
       gi_reset('StagedScan_PhaseB_BlockDecompressed');

--    The same predicate must give the same answer however it is written.
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7 AND scat = 0;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE scat = 0 AND clus = 7;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7 AND scat = 0;
SET whpg_enable_staged_scan = on;

--    A block directory never puts a table out of scope, and the index on
--    another column (created in 2) leaves the seq scan and its Staged Scan
--    alone.
SELECT gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_gi');
SELECT count(c) FROM staged_gi WHERE clus = 7;
SELECT gi_hits('StagedScan_PhaseB_BlockDecompressed') > 0 AS runs_with_blkdir_and_index;
SELECT gi_reset('StagedScan_PhaseB_BlockDecompressed');
DROP INDEX staged_gi_a_idx;

--    A directory asked for with the blockdirectory storage option, on a table
--    that has no index at all, is seeked through the same way.
CREATE TABLE staged_gi_bd (LIKE staged_gi) USING ao_column
  WITH (blockdirectory = true) DISTRIBUTED BY (a);
INSERT INTO staged_gi_bd SELECT * FROM staged_gi;
ANALYZE staged_gi_bd;
SELECT gi_arm('StagedScan_PhaseB_Seek', 'staged_gi_bd'),
       gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_gi_bd');
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi_bd WHERE clus = 7;
SELECT gi_hits('StagedScan_PhaseB_Seek') > 0 AS seeks_through_option_directory,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') > 0 AS runs_with_option_directory;
SELECT gi_reset('StagedScan_PhaseB_Seek'),
       gi_reset('StagedScan_PhaseB_BlockDecompressed');
DROP TABLE staged_gi_bd;

--
-- 4. Deleted rows must not come back: Phase A consults the visimap before a
--    row enters the survivor set.
--
DELETE FROM staged_gi WHERE a % 7 = 0;
SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7;
SET whpg_enable_staged_scan = on;
--    Deleted rows that fail the predicate are not "removed by filter" in an
--    ordinary scan; Phase A settles them from the visimap's hidden count.
SELECT gi_removed('SELECT count(c) FROM staged_gi WHERE clus = 7')              AS removed_after_delete,
       gi_removed('SELECT count(c) FROM staged_gi WHERE clus = 7 AND scat = 0') AS removed_two_stages_after_delete;

--
-- 5. Phase A produces survivors in batches; Phase B consumes a batch and
--    Phase A resumes, so the memory for row numbers is bounded whatever the
--    hit count. Batches start at 8 and double up to 65536, which a table of
--    this size never reaches, so the StagedScan_PhaseA_BatchComplete fault
--    closes a batch on every survivor: each survivor goes out in a batch of
--    its own (plus at most one empty batch per segfile, closing it at its
--    end), the segfile is still read once, and the answer does not change --
--    neither for a single stage nor for a later one.
--
SET whpg_enable_staged_scan = on;
SELECT gi_arm('StagedScan_PhaseA_BatchComplete', 'staged_gi'),
       gi_arm('StagedScan_PhaseA_Survivor', 'staged_gi'),
       gi_arm('StagedScan_PhaseA_Batch', 'staged_gi'),
       gi_arm('StagedScan_PhaseA_Complete', 'staged_gi');
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7;
SELECT gi_hits('StagedScan_PhaseA_Survivor') AS survivors,
       gi_hits('StagedScan_PhaseA_Batch') - gi_hits('StagedScan_PhaseA_Survivor')
         BETWEEN 0 AND gi_hits('StagedScan_PhaseA_Complete') AS one_batch_per_survivor,
       gi_hits('StagedScan_PhaseA_Complete') AS segfiles_completed;
SELECT gi_reset('StagedScan_PhaseA_Survivor'),
       gi_reset('StagedScan_PhaseA_Batch'),
       gi_reset('StagedScan_PhaseA_Complete');
SELECT gi_survivors('SELECT count(c) FROM staged_gi WHERE clus = 7') AS survivors_batched;
SELECT count(*) n, sum(a) sa, md5(string_agg(c, ',' ORDER BY a)) h
  FROM staged_gi WHERE clus = 7 AND scat = 0;
SELECT gi_reset('StagedScan_PhaseA_BatchComplete');

--    Batches start at 8 and double up to 65536,
--    so the first row is out after 8 matches rather than after a batch that,
--    on a sparse predicate, could span the segfile. A replicated table is
--    scanned on one segment, which makes the batch count exact: 200 clustered
--    survivors behind block 64 of an 8 KB-block column go out as 8, 16, 32,
--    64 and the remaining 80 (the fifth batch runs to the end of the segfile).
--    The first batch of a single-stage predicate also keeps filling until 64
--    blocks have been crossed (the fallback checks' sample); here the band
--    sits past that point, so the first batch is exactly 8.
CREATE TABLE staged_ramp(k int, a int ENCODING (blocksize = 8192), c text)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED REPLICATED;
INSERT INTO staged_ramp
  SELECT i, CASE WHEN i BETWEEN 300000 AND 300199 THEN 7 ELSE i % 1000 + 10 END, 'payload ' || i
  FROM generate_series(1, 400000) i;
ANALYZE staged_ramp;
EXPLAIN (COSTS OFF) SELECT c FROM staged_ramp WHERE a = 7;
SELECT gi_arm('StagedScan_PhaseA_Batch', 'staged_ramp');
SELECT count(*) n, sum(k) sk, md5(string_agg(c, ',' ORDER BY k)) h FROM staged_ramp WHERE a = 7;
SELECT gi_hits('StagedScan_PhaseA_Batch') AS ramped_batches;
SELECT gi_reset('StagedScan_PhaseA_Batch');
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(k) sk, md5(string_agg(c, ',' ORDER BY k)) h FROM staged_ramp WHERE a = 7;
SET whpg_enable_staged_scan = on;
DROP TABLE staged_ramp;

--    The first batch also shows how the survivors spread over Phase A's
--    varblocks. A predicate that keeps a row in nearly every block leaves
--    Phase B nothing to skip, so Phase A falls back to the ordinary scan
--    before emitting a row (once it has crossed enough blocks to tell a
--    dense band from a dense table). 1.8M rows: b = 0 keeps 1% of them, 80
--    per block; the a band keeps as many rows in a handful of blocks (and
--    projects b, so that there is a Phase B at all).
CREATE TABLE staged_dense(a bigint, b int, c text)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1)
  DISTRIBUTED BY (a);
INSERT INTO staged_dense SELECT i, i % 100, 'row ' || i FROM generate_series(1, 1800000) i;
ANALYZE staged_dense;

SELECT gi_arm('StagedScan_PhaseA_DenseFallback', 'staged_dense'),
       gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_dense');
SELECT count(*) n, sum(a) sa FROM staged_dense WHERE b = 0;
SELECT gi_hits('StagedScan_PhaseA_DenseFallback') AS segments_fell_back,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') AS phase_b_blocks;
--    ... and with no Phase B there is no bill: EXPLAIN ANALYZE omits the line.
SELECT gi_blocks('SELECT count(*) n, sum(a) sa FROM staged_dense WHERE b = 0') AS blocks_line_on_fallback;
SELECT gi_reset('StagedScan_PhaseA_DenseFallback'),
       gi_reset('StagedScan_PhaseB_BlockDecompressed');
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(a) sa FROM staged_dense WHERE b = 0;
SET whpg_enable_staged_scan = on;

SELECT gi_arm('StagedScan_PhaseA_DenseFallback', 'staged_dense'),
       gi_arm('StagedScan_PhaseB_BlockDecompressed', 'staged_dense');
SELECT count(*) n, sum(b) sb FROM staged_dense WHERE a BETWEEN 1200000 AND 1218000;
SELECT gi_hits('StagedScan_PhaseA_DenseFallback') AS segments_fell_back,
       gi_hits('StagedScan_PhaseB_BlockDecompressed') > 0 AS phase_b_ran;
SELECT gi_reset('StagedScan_PhaseA_DenseFallback'),
       gi_reset('StagedScan_PhaseB_BlockDecompressed');
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(b) sb FROM staged_dense WHERE a BETWEEN 1200000 AND 1218000;
SET whpg_enable_staged_scan = on;
DROP TABLE staged_dense;

--    On a predicate that keeps nearly every row the survivor array (65536
--    rows) fills within a few blocks, long before 64 of them have passed, and
--    so does it on a clustered band of 65536 rows or more at the start of the
--    file; the rate over those blocks is the same for both. A full first
--    batch is judged only when it holds more than four times what the planner
--    estimated for the whole segment file. Replicated tables are scanned on
--    one segment, one segment file each.
--    staged_null: k IS NULL with a null_frac of 0 in the statistics (taken
--    before the NULL rows went in) is estimated at one row and keeps 100000
--    of 101000: the first batch falls back.
--    The same batch arms the multi-stage check of Phase A against the
--    estimate, and the density check, here reached with the gate raised to
--    50% so that a predicate keeping a third of the rows passes the
--    selectivity recheck: staged_den's k = 7 is absent from the statistics.
--    staged_band: k <= 70000 keeps the first 70000 of 1M rows, the 7% the
--    statistics predict; the array fills on the band's first blocks, and the
--    scan stays staged.
CREATE FUNCTION gi_fallback(q text) RETURNS text AS $BODY$
DECLARE line text; fb text := '-'; sv text := '-';
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
        IF line LIKE '%Stage Survivors:%' THEN sv := trim(substring(line from 'Stage Survivors: (.*)$')); END IF;
        IF line LIKE '%Stage Fallback:%' THEN fb := trim(substring(line from 'Stage Fallback: (.*)$')); END IF;
    END LOOP;
    RETURN sv || ' / fallback ' || fb;
END $BODY$ LANGUAGE plpgsql;
CREATE TABLE staged_null(k int, b int)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED REPLICATED;
INSERT INTO staged_null SELECT i, i FROM generate_series(1, 1000) i;
ANALYZE staged_null;
INSERT INTO staged_null SELECT NULL, i FROM generate_series(1, 100000) i;
SELECT gi_fallback('SELECT b FROM staged_null WHERE k IS NULL') AS null_full_batch;
SELECT count(*) n, sum(b) sb FROM staged_null WHERE k IS NULL;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(b) sb FROM staged_null WHERE k IS NULL;
SET whpg_enable_staged_scan = on;
SELECT gi_arm('StagedScan_PhaseA_EstimateFallback', 'staged_null');
SELECT gi_fallback('SELECT b FROM staged_null WHERE k IS NULL AND b % 100 = 5') AS null_two_stages;
SELECT gi_hits('StagedScan_PhaseA_EstimateFallback') AS estimate_fallbacks;
SELECT gi_reset('StagedScan_PhaseA_EstimateFallback');

CREATE TABLE staged_den(k int, b int)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED REPLICATED;
INSERT INTO staged_den SELECT i % 1000 + 10, i FROM generate_series(1, 1000) i;
ANALYZE staged_den;
INSERT INTO staged_den SELECT CASE WHEN i % 3 = 0 THEN 7 ELSE 8 END, i FROM generate_series(1, 300000) i;
SET whpg_staged_scan_min_selectivity = 0.5;
SELECT gi_arm('StagedScan_PhaseA_DenseFallback', 'staged_den');
SELECT gi_fallback('SELECT b FROM staged_den WHERE k = 7') AS den_full_batch;
SELECT gi_hits('StagedScan_PhaseA_DenseFallback') AS dense_fallbacks;
SELECT gi_reset('StagedScan_PhaseA_DenseFallback');
RESET whpg_staged_scan_min_selectivity;
SELECT count(*) n, sum(b) sb FROM staged_den WHERE k = 7;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(b) sb FROM staged_den WHERE k = 7;
SET whpg_enable_staged_scan = on;

CREATE TABLE staged_band(k int, b int)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED REPLICATED;
INSERT INTO staged_band SELECT i, i FROM generate_series(1, 1000000) i;
ANALYZE staged_band;
SELECT gi_fallback('SELECT b FROM staged_band WHERE k <= 70000') AS band_full_batch;
SELECT count(*) n, sum(b) sb FROM staged_band WHERE k <= 70000;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(b) sb FROM staged_band WHERE k <= 70000;
SET whpg_enable_staged_scan = on;
DROP TABLE staged_null, staged_den, staged_band;
DROP FUNCTION gi_fallback(text);

--
-- 6. Compression types: skipping depends on the varblock header layout, not
--    the codec, so each must behave identically.
--
CREATE TABLE staged_zstd(a int, c text, d int)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1)
  DISTRIBUTED BY (a);
INSERT INTO staged_zstd
  SELECT i, 'payload ' || i, CASE WHEN i BETWEEN 5000 AND 5100 THEN 7 ELSE 1 END
    FROM generate_series(1, 50000) i;

CREATE TABLE staged_rle(a int, c text, d int)
  USING ao_column WITH (compresstype = rle_type, compresslevel = 1)
  DISTRIBUTED BY (a);
INSERT INTO staged_rle
  SELECT i, 'payload ' || i, CASE WHEN i BETWEEN 5000 AND 5100 THEN 7 ELSE 1 END
    FROM generate_series(1, 50000) i;

SET whpg_enable_staged_scan = on;
SELECT count(c) n_zstd, md5(string_agg(c, ',' ORDER BY a)) h FROM staged_zstd WHERE d = 7;
SELECT count(c) n_rle, md5(string_agg(c, ',' ORDER BY a)) h FROM staged_rle WHERE d = 7;
SET whpg_enable_staged_scan = off;
SELECT count(c) n_zstd, md5(string_agg(c, ',' ORDER BY a)) h FROM staged_zstd WHERE d = 7;
SELECT count(c) n_rle, md5(string_agg(c, ',' ORDER BY a)) h FROM staged_rle WHERE d = 7;

--
-- 7. ALTER TABLE ADD COLUMN leaves the new column missing for the old rows.
--    As a projected column Phase B must take the default without touching the
--    segfile; as a filter column Staged Scan declines.
--
ALTER TABLE staged_zstd ADD COLUMN e int DEFAULT 42;

SET whpg_enable_staged_scan = on;
SELECT count(*) n, sum(e) sum_e FROM staged_zstd WHERE d = 7;
SELECT count(c) n_filter_on_missing FROM staged_zstd WHERE e = 42;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(e) sum_e FROM staged_zstd WHERE d = 7;
SELECT count(c) n_filter_on_missing FROM staged_zstd WHERE e = 42;

--
-- 8. Values larger than a block are stored as large-content blocks, whose
--    length a header-only read cannot tell. Under a block directory -- here
--    one built with gp_blockdirectory_entry_min_range, so that it has fewer
--    entries than blocks and a seek lands short of the row -- Phase B still
--    finds each survivor's value.
--
SET gp_blockdirectory_entry_min_range = 65536;
CREATE TABLE staged_lob(a int, k int, big text)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED BY (a);
CREATE INDEX staged_lob_a_idx ON staged_lob(a);
INSERT INTO staged_lob
  SELECT i, CASE WHEN i % 50 = 7 THEN 7 ELSE 1 END,
         (SELECT string_agg(md5((i * 1000 + j)::text), '') FROM generate_series(1, 1000 + i % 600) j)
    FROM generate_series(1, 300) i;
RESET gp_blockdirectory_entry_min_range;
ANALYZE staged_lob;

SET whpg_enable_staged_scan = on;
EXPLAIN (COSTS OFF) SELECT a, big FROM staged_lob WHERE k = 7;
SELECT count(*) n, sum(length(big)) len, md5(string_agg(md5(big), ',' ORDER BY a)) h FROM staged_lob WHERE k = 7;
SET whpg_enable_staged_scan = off;
SELECT count(*) n, sum(length(big)) len, md5(string_agg(md5(big), ',' ORDER BY a)) h FROM staged_lob WHERE k = 7;

RESET whpg_enable_staged_scan;
DROP TABLE staged_lob;
DROP TABLE staged_gi;
DROP TABLE staged_zstd;
DROP TABLE staged_rle;
DROP FUNCTION gi_survivors(text);
DROP FUNCTION gi_removed(text);
DROP FUNCTION gi_arm(text, text);
DROP FUNCTION gi_hits(text);
DROP FUNCTION gi_reset(text);
