-- Staged Scan: what the plan looks like, what the switches do, and whether the
-- answers hold.
--
-- Part 1 prints real EXPLAIN output. That part is deliberately verbose: the
-- expected file doubles as the reference for what Staged Scan looks like in a
-- plan, so it should be readable on its own without running anything.
--
-- Parts 2 and 3 trade verbosity for breadth, reporting booleans from two
-- helpers so a wide matrix of cases stays legible:
--
--   gi_planned(q)  did the planner choose Staged Scan? (looks for the EXPLAIN
--                  line, so it also pins that EXPLAIN reports what will run)
--   gi_same(q)     same answer with the feature on and off? q yields one text
--                  value, normally a hash.
--
-- Companions: staged_scan_aoco.sql covers executor behaviour and block
-- skipping; staged_scan_datumstream_split.sql covers the storage primitive.

SET optimizer = off;			-- the Postgres planner's decisions; staged_scan_orca.sql has GPORCA's

-- 'clus' matches a contiguous band of row numbers, 'scat' every 100th row.
CREATE TABLE gp_main(a int, b bigint, c text, clus int, scat int, flag bool, amt numeric, d date)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED BY (a);
INSERT INTO gp_main
  SELECT i, i * 2, 'payload ' || i,
         CASE WHEN i BETWEEN 20000 AND 20200 THEN 7 ELSE 1 END,
         i % 100,
         (i % 3 = 0),
         (i % 977)::numeric / 7,
         date '2020-01-01' + (i % 900)
    FROM generate_series(1, 200000) i;
ANALYZE gp_main;

--
-- ================= 1. What the plan looks like =================
--
-- Chosen: one extra line under Seq Scan naming the column that drives Phase A.
-- The predicate itself stays on the Filter line; the Staged Scan line
-- says which column is read first to find the surviving rows.
--
EXPLAIN (COSTS OFF) SELECT c, b FROM gp_main WHERE clus = 7;

-- Turned off by the GUC: the line disappears and the plan is an ordinary scan.
SET whpg_enable_staged_scan = off;
EXPLAIN (COSTS OFF) SELECT c, b FROM gp_main WHERE clus = 7;
RESET whpg_enable_staged_scan;

-- Chosen for count(*) too, although nothing is left for Phase B to fetch:
-- Phase A reads clus and tests it block by block, and no tuple is formed for
-- a rejected row, which is cheaper than the ordinary scan's row at a time.
EXPLAIN (COSTS OFF) SELECT count(*) FROM gp_main WHERE clus = 7;

-- Chosen with two stages: the line lists the columns in stage order. The
-- first is read in full by Phase A; the second is fetched only for the rows
-- that survived it, and its clause decided before any projected column is
-- touched.
EXPLAIN (COSTS OFF) SELECT c FROM gp_main WHERE clus = 7 AND scat = 0;

-- The cost model orders the stages, not the predicate: written the other way
-- round the plan is the same.
EXPLAIN (COSTS OFF) SELECT c FROM gp_main WHERE scat = 0 AND clus = 7;

-- A clause spanning two columns is decided at the later of the two stages.
EXPLAIN (COSTS OFF) SELECT c FROM gp_main WHERE clus = 7 AND scat + clus = 7;

-- Under OR neither column can drop a row on its own, so Phase A reads both
-- (parenthesized: one stage, two columns). staged_scan_or covers this shape.
-- (a = X, a single row, rather than scat = 0: 1% of the rows scattered over
-- this 100k-row table is a survivor in every varblock, which the cost model
-- declines.)
EXPLAIN (COSTS OFF) SELECT c FROM gp_main WHERE clus = 7 OR a = 12345;

-- Declined: the predicate is not selective enough to pay for Phase A.
EXPLAIN (COSTS OFF) SELECT c FROM gp_main WHERE scat < 50;

-- Chosen on a join input, independently per relation.
CREATE TABLE gp_dim(a int, label text) USING ao_column DISTRIBUTED BY (a);
INSERT INTO gp_dim SELECT i, 'lab' || i FROM generate_series(1, 5000) i;
ANALYZE gp_dim;
EXPLAIN (COSTS OFF)
  SELECT m.c, d.label FROM gp_main m JOIN gp_dim d ON m.a = d.a WHERE m.clus = 7;

-- GPORCA builds its own plans; the same decision is made over its finished
-- plan (stagedscan_decide_orca_plan()), so the scan shows the same way there.
-- staged_scan_orca.sql covers ORCA in depth.
SET optimizer = on;
EXPLAIN (COSTS OFF) SELECT c, b FROM gp_main WHERE clus = 7;
SET optimizer = off;

--
-- ================= 2. Planner decision matrix =================
--
CREATE FUNCTION gi_planned(q text) RETURNS bool AS $BODY$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF line LIKE '%Staged Seq Scan%' THEN
            RETURN true;
        END IF;
    END LOOP;
    RETURN false;
END $BODY$ LANGUAGE plpgsql;

CREATE FUNCTION gi_same(q text) RETURNS bool AS $BODY$
DECLARE r_on text; r_off text;
BEGIN
    PERFORM set_config('whpg_enable_staged_scan', 'on', false);
    EXECUTE q INTO r_on;
    PERFORM set_config('whpg_enable_staged_scan', 'off', false);
    EXECUTE q INTO r_off;
    PERFORM set_config('whpg_enable_staged_scan', 'on', false);
    RETURN r_on IS NOT DISTINCT FROM r_off;
END $BODY$ LANGUAGE plpgsql;

CREATE FUNCTION gi_stages(q text) RETURNS text AS $BODY$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF line LIKE '%Stages:%' THEN
            RETURN trim(substring(line from 'Stages: (.*)$'));
        END IF;
    END LOOP;
    RETURN '-';
END $BODY$ LANGUAGE plpgsql;

-- Query shape.
SELECT gi_planned('SELECT c FROM gp_main WHERE clus = 7')              AS single_col_qual,
       gi_planned('SELECT count(*) FROM gp_main WHERE clus = 7')       AS count_star,
       gi_planned('SELECT c FROM gp_main WHERE clus = 7 AND scat = 0') AS two_col_qual,
       gi_planned('SELECT clus FROM gp_main WHERE clus = 7')           AS filter_col_only,
       gi_planned('SELECT c FROM gp_main')                             AS no_qual;

-- Multi-column shapes. count(*) with two filter columns is still worth it:
-- the second column is fetched for Phase A's survivors only, where the
-- ordinary scan reads both in lockstep.
SELECT gi_planned('SELECT count(*) FROM gp_main WHERE clus = 7 AND scat = 0')        AS two_col_count_star,
       gi_planned('SELECT c FROM gp_main WHERE clus = 7 AND scat = 0 AND flag')      AS three_col,
       gi_planned('SELECT c FROM gp_main WHERE clus + scat = 7')                     AS only_a_two_col_clause,
       gi_planned('SELECT c FROM gp_main WHERE clus = 7 OR a = 12345')               AS or_qual,
       gi_planned('SELECT c FROM gp_main WHERE (clus = 7 OR scat = 0) AND b = 4000') AS or_under_and;

-- Stage order is a cost decision. clus = 7 keeps 0.1% of the rows and scat < 50
-- half of them, so clus leads however the predicate is written.
SELECT gi_stages('SELECT c FROM gp_main WHERE clus = 7 AND scat < 50') AS cost_written_clus_first,
       gi_stages('SELECT c FROM gp_main WHERE scat < 50 AND clus = 7') AS cost_written_scat_first;

-- Selectivity boundary. whpg_staged_scan_min_selectivity defaults to 0.10;
-- past it Phase A would be abandoned partway through, which costs more than
-- never having started.
SELECT gi_planned('SELECT c FROM gp_main WHERE scat < 1')  AS sel_1pct,
       gi_planned('SELECT c FROM gp_main WHERE scat < 5')  AS sel_5pct,
       gi_planned('SELECT c FROM gp_main WHERE scat < 9')  AS sel_9pct,
       gi_planned('SELECT c FROM gp_main WHERE scat < 11') AS sel_11pct,
       gi_planned('SELECT c FROM gp_main WHERE scat < 50') AS sel_50pct;

-- Moving the threshold moves the boundary with it.
SET whpg_staged_scan_min_selectivity = 0.30;
SELECT gi_planned('SELECT c FROM gp_main WHERE scat < 11') AS sel_11_thresh_30,
       gi_planned('SELECT c FROM gp_main WHERE scat < 50') AS sel_50_thresh_30;
SET whpg_staged_scan_min_selectivity = 0.02;
SELECT gi_planned('SELECT c FROM gp_main WHERE scat < 1')  AS sel_1_thresh_02,
       gi_planned('SELECT c FROM gp_main WHERE scat < 5')  AS sel_5_thresh_02;
RESET whpg_staged_scan_min_selectivity;

-- The master switch.
SET whpg_enable_staged_scan = off;
SELECT gi_planned('SELECT c FROM gp_main WHERE clus = 7') AS guc_off;
RESET whpg_enable_staged_scan;

-- Table shapes that are out of scope.
CREATE TABLE gp_heap(a int, c text, clus int) DISTRIBUTED BY (a);
INSERT INTO gp_heap SELECT i, 'x' || i, i % 1000 FROM generate_series(1, 20000) i;
CREATE TABLE gp_aorow(a int, c text, clus int) USING ao_row DISTRIBUTED BY (a);
INSERT INTO gp_aorow SELECT i, 'x' || i, i % 1000 FROM generate_series(1, 20000) i;
ANALYZE gp_heap; ANALYZE gp_aorow;

SELECT gi_planned('SELECT c FROM gp_heap WHERE clus = 7')  AS heap_table,
       gi_planned('SELECT c FROM gp_aorow WHERE clus = 7') AS ao_row_table;

-- A block directory never puts a table out of scope: an index on another
-- column leaves the seq scan and its Staged Scan alone, and so does a
-- directory asked for with the blockdirectory storage option on a table that
-- has no index.
CREATE TABLE gp_blkdir(a int, c text, clus int) USING ao_column DISTRIBUTED BY (a);
INSERT INTO gp_blkdir SELECT i, 'x' || i,
       CASE WHEN i BETWEEN 5000 AND 5050 THEN 7 ELSE 1 END
  FROM generate_series(1, 50000) i;
ANALYZE gp_blkdir;
SELECT gi_planned('SELECT c FROM gp_blkdir WHERE clus = 7') AS before_index;
CREATE INDEX gp_blkdir_idx ON gp_blkdir(a);
ANALYZE gp_blkdir;
SELECT gi_planned('SELECT c FROM gp_blkdir WHERE clus = 7') AS after_index;
CREATE TABLE gp_blkdir_opt(a int, c text, clus int) USING ao_column
  WITH (blockdirectory = true) DISTRIBUTED BY (a);
INSERT INTO gp_blkdir_opt SELECT * FROM gp_blkdir;
ANALYZE gp_blkdir_opt;
SELECT gi_planned('SELECT c FROM gp_blkdir_opt WHERE clus = 7') AS option_directory;

--
-- ================= 3. Same answer with the feature on and off =================
--
-- Predicate forms.
SELECT gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE clus = 7')             AS eq,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE clus <> 1')            AS neq,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE a < 500')              AS lt,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE a BETWEEN 10 AND 900') AS between_,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE a IN (5, 50, 500)')    AS in_list;

-- Column types.
SELECT gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE c LIKE ''payload 12345''') AS like_,
       gi_same('SELECT count(*)::text FROM gp_main WHERE flag')                                       AS bool_col,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE amt < 1')              AS numeric_col,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE d = date ''2020-01-05''') AS date_col,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE b = 4000')             AS bigint_col;

-- Expression and function quals.
SELECT gi_same('SELECT count(*)::text FROM gp_main WHERE clus + 0 = 7')            AS expr_qual,
       gi_same('SELECT count(*)::text FROM gp_main WHERE abs(clus) = 7')           AS func_qual,
       gi_same('SELECT count(*)::text FROM gp_main WHERE upper(c) = ''PAYLOAD 5''') AS text_func_qual;

-- Multi-column predicates.
SELECT gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE clus = 7 AND scat = 0')          AS two_col,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE scat = 0 AND clus = 7')          AS two_col_reversed,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE clus = 7 AND scat = 0 AND flag') AS three_col,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND scat = 0')                                AS two_col_count_star,
       gi_same('SELECT md5(string_agg(scat::text, '','' ORDER BY a)) FROM gp_main WHERE clus = 7 AND scat < 50') AS stage_col_projected;

SELECT gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE clus = 7 AND scat + clus = 7')    AS clause_needs_phase_a_col,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_main WHERE clus = 7 AND scat + b = 40007')    AS two_col_clause,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND (scat = 0 OR flag)')                       AS or_under_and,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND scat IS NULL')                             AS stage_is_null,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND scat > 20 AND scat < 30 AND d > date ''2020-06-01''') AS many_clauses;

-- No row passes the second stage, or every row does.
SELECT gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND scat = 999')  AS stage_2_rejects_all,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND scat >= 0')   AS stage_2_keeps_all;

-- Degenerate cardinalities.
CREATE TABLE gp_one(a int, c text, clus int) USING ao_column DISTRIBUTED BY (a);
INSERT INTO gp_one VALUES (1, 'only', 7);
CREATE TABLE gp_empty(a int, c text, clus int) USING ao_column DISTRIBUTED BY (a);
ANALYZE gp_one; ANALYZE gp_empty;

SELECT gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 99999')     AS no_match,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus >= 0')        AS all_match,
       gi_same('SELECT md5(string_agg(c, '','')) FROM gp_one WHERE clus = 7') AS single_row,
       gi_same('SELECT count(*)::text FROM gp_empty WHERE clus = 7')        AS empty_table;

-- NULLs, deletes, updates.
INSERT INTO gp_main(a, b, c, clus, scat) VALUES (300001, NULL, NULL, 7, NULL);
SELECT gi_same('SELECT count(*)::text || coalesce(md5(string_agg(c, '','' ORDER BY a)), ''-'') FROM gp_main WHERE clus = 7') AS with_nulls,
       gi_same('SELECT count(*)::text FROM gp_main WHERE scat IS NULL')     AS is_null,
       gi_same('SELECT count(*)::text FROM gp_main WHERE scat IS NOT NULL') AS is_not_null;

DELETE FROM gp_main WHERE a % 7 = 0;
UPDATE gp_main SET c = c || '-upd' WHERE a BETWEEN 20050 AND 20100;
SELECT gi_same('SELECT count(*)::text || coalesce(md5(string_agg(c, '','' ORDER BY a)), ''-'') FROM gp_main WHERE clus = 7') AS after_delete_update;

-- Several INSERTs, then a hole in the row numbers left by an aborted
-- transaction.
CREATE TABLE gp_multiseg(a int, c text, clus int) USING ao_column DISTRIBUTED BY (a);
INSERT INTO gp_multiseg SELECT i, 'p' || i, CASE WHEN i BETWEEN 100 AND 200 THEN 7 ELSE 1 END FROM generate_series(1, 20000) i;
INSERT INTO gp_multiseg SELECT i, 'p' || i, CASE WHEN i BETWEEN 100 AND 200 THEN 7 ELSE 1 END FROM generate_series(20001, 40000) i;
INSERT INTO gp_multiseg SELECT i, 'p' || i, CASE WHEN i BETWEEN 100 AND 200 THEN 7 ELSE 1 END FROM generate_series(40001, 60000) i;
ANALYZE gp_multiseg;
SELECT gi_same('SELECT count(*)::text || md5(string_agg(c, '','' ORDER BY a)) FROM gp_multiseg WHERE clus = 7') AS multi_segfile;

BEGIN;
INSERT INTO gp_multiseg SELECT i, 'aborted' || i, 7 FROM generate_series(60001, 61000) i;
ABORT;
INSERT INTO gp_multiseg SELECT i, 'after-abort' || i, CASE WHEN i BETWEEN 62000 AND 62100 THEN 7 ELSE 1 END
  FROM generate_series(61001, 63000) i;
ANALYZE gp_multiseg;
SELECT gi_same('SELECT count(*)::text || md5(string_agg(c, '','' ORDER BY a)) FROM gp_multiseg WHERE clus = 7') AS rownum_holes,
       gi_same('SELECT count(*)::text || md5(string_agg(c, '','' ORDER BY a)) FROM gp_multiseg WHERE clus = 7 AND c LIKE ''p1%''') AS rownum_holes_two_col,
       (SELECT count(*) FROM gp_multiseg WHERE c LIKE 'aborted%') = 0 AS no_aborted_rows;

-- Storage variants.
CREATE TABLE gp_none(a int, c text, clus int) USING ao_column DISTRIBUTED BY (a);
CREATE TABLE gp_zlib(a int, c text, clus int)
  USING ao_column WITH (compresstype = zlib, compresslevel = 1) DISTRIBUTED BY (a);
CREATE TABLE gp_rle(a int, c text, clus int)
  USING ao_column WITH (compresstype = rle_type, compresslevel = 1) DISTRIBUTED BY (a);
CREATE TABLE gp_small(a int, c text, clus int)
  USING ao_column WITH (blocksize = 8192) DISTRIBUTED BY (a);
INSERT INTO gp_none SELECT i, 'p' || i, CASE WHEN i BETWEEN 5000 AND 5100 THEN 7 ELSE 1 END FROM generate_series(1, 50000) i;
INSERT INTO gp_zlib  SELECT * FROM gp_none;
INSERT INTO gp_rle   SELECT * FROM gp_none;
INSERT INTO gp_small SELECT * FROM gp_none;
ANALYZE gp_none; ANALYZE gp_zlib; ANALYZE gp_rle; ANALYZE gp_small;

SELECT gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_none WHERE clus = 7')  AS compress_none,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_zlib WHERE clus = 7')  AS compress_zlib,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_rle WHERE clus = 7')   AS compress_rle,
       gi_same('SELECT md5(string_agg(c, '','' ORDER BY a)) FROM gp_small WHERE clus = 7') AS blocksize_8k;

-- Values larger than a varblock take the LOB path.
CREATE TABLE gp_lob(a int, big text, clus int) USING ao_column DISTRIBUTED BY (a);
INSERT INTO gp_lob SELECT i, repeat(md5(i::text), 4000),
       CASE WHEN i BETWEEN 40 AND 45 THEN 7 ELSE 1 END
  FROM generate_series(1, 300) i;
ANALYZE gp_lob;
SELECT gi_same('SELECT md5(string_agg(md5(big), '','' ORDER BY a)) FROM gp_lob WHERE clus = 7') AS lob_values,
       gi_same('SELECT sum(length(big))::text FROM gp_lob WHERE clus = 7')                     AS lob_lengths,
       gi_same('SELECT count(*)::text FROM gp_lob WHERE clus = 7 AND big LIKE ''%a%''')   AS lob_as_stage_2;

-- Schema changes: a column added later is missing for the older rows.
CREATE TABLE gp_alter(a int, c text, clus int) USING ao_column DISTRIBUTED BY (a);
INSERT INTO gp_alter SELECT i, 'p' || i, CASE WHEN i BETWEEN 500 AND 550 THEN 7 ELSE 1 END FROM generate_series(1, 20000) i;
ALTER TABLE gp_alter ADD COLUMN added int DEFAULT 42;
INSERT INTO gp_alter SELECT i, 'p' || i, 7, 99 FROM generate_series(20001, 20100) i;
ANALYZE gp_alter;
SELECT gi_same('SELECT md5(string_agg(c || ''/'' || added, '','' ORDER BY a)) FROM gp_alter WHERE clus = 7') AS missing_as_projected,
       gi_same('SELECT count(*)::text FROM gp_alter WHERE added = 42') AS missing_as_filter,
       gi_same('SELECT count(*)::text FROM gp_alter WHERE added = 99') AS added_as_filter,
       gi_same('SELECT count(*)::text FROM gp_alter WHERE clus = 7 AND added = 42') AS missing_as_stage_2,
       gi_same('SELECT count(*)::text FROM gp_alter WHERE clus = 7 AND added = 99') AS added_as_stage_2;
ALTER TABLE gp_alter DROP COLUMN c;
ANALYZE gp_alter;
SELECT gi_same('SELECT sum(added)::text FROM gp_alter WHERE clus = 7') AS after_drop_column;

-- Interaction with the rest of the executor.
SELECT gi_same('SELECT count(*)::text FROM gp_main m JOIN gp_multiseg s ON m.a = s.a WHERE m.clus = 7') AS join_,
       gi_same('SELECT md5(string_agg(t, '','' ORDER BY t)) FROM (SELECT DISTINCT c AS t FROM gp_main WHERE clus = 7) x') AS distinct_,
       gi_same('SELECT md5(string_agg(c, '','')) FROM (SELECT c FROM gp_main WHERE clus = 7 ORDER BY a LIMIT 10) x') AS order_limit,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 AND a IN (SELECT a FROM gp_multiseg)') AS subquery;

SELECT gi_same('WITH x AS (SELECT c, a FROM gp_main WHERE clus = 7) SELECT md5(string_agg(c, '','' ORDER BY a)) FROM x') AS cte,
       gi_same('SELECT md5(string_agg(c || r::text, '','')) FROM (SELECT c, row_number() OVER (ORDER BY a) r FROM gp_main WHERE clus = 7) x') AS window_,
       gi_same('SELECT count(*)::text FROM gp_main WHERE clus = 7 GROUP BY scat ORDER BY 1 LIMIT 1') AS group_by;

-- A prepared statement reuses a cached plan; the flag has to survive that.
PREPARE gi_prep(int) AS SELECT count(*) FROM gp_main WHERE clus = $1;
EXECUTE gi_prep(7);
EXECUTE gi_prep(7);
EXECUTE gi_prep(7);
EXECUTE gi_prep(7);
EXECUTE gi_prep(7);
EXECUTE gi_prep(7);
DEALLOCATE gi_prep;

-- From the sixth execution on a generic plan may be used, whose qual reads
-- the parameter through a Param rather than a Const. The stage quals cannot
-- evaluate a Param, so such a plan must not be a Staged Scan plan (the
-- custom plans before it may be).
PREPARE gi_prep1(int) AS SELECT count(c) FROM gp_main WHERE clus = $1;
EXECUTE gi_prep1(7);
EXECUTE gi_prep1(7);
EXECUTE gi_prep1(7);
EXECUTE gi_prep1(7);
EXECUTE gi_prep1(7);
EXECUTE gi_prep1(7);
EXECUTE gi_prep1(7);
DEALLOCATE gi_prep1;
PREPARE gi_prep2(int) AS SELECT count(c) FROM gp_main WHERE clus = $1 AND scat = 0;
EXECUTE gi_prep2(7);
EXECUTE gi_prep2(7);
EXECUTE gi_prep2(7);
EXECUTE gi_prep2(7);
EXECUTE gi_prep2(7);
EXECUTE gi_prep2(7);
EXECUTE gi_prep2(7);
DEALLOCATE gi_prep2;
SET plan_cache_mode = force_generic_plan;
PREPARE gi_prep3(int) AS SELECT count(c) FROM gp_main WHERE clus = $1 AND scat = 0;
EXPLAIN (COSTS OFF) EXECUTE gi_prep3(7);
EXECUTE gi_prep3(7);
DEALLOCATE gi_prep3;
RESET plan_cache_mode;

-- A cursor fetches through the same scan in pieces.
BEGIN;
DECLARE gi_cur CURSOR FOR SELECT c FROM gp_main WHERE clus = 7 ORDER BY a;
FETCH 3 FROM gi_cur;
FETCH 2 FROM gi_cur;
CLOSE gi_cur;
COMMIT;

-- Extended statistics shape the stage order. Selectivities along a candidate
-- order are estimated cumulatively (all clauses decided so far are handed to
-- clauselist_selectivity together), so a functional dependency recorded by
-- CREATE STATISTICS tells the planner that a clause implied by an earlier one
-- rejects nothing, and it stops paying to decide it early.
--   a = i % 10000 (20 rows per value), b = a / 100 (100 values, fully
--   determined by a), c scattered over 3 values. Taken independently b = 0
--   (1%) looks far more selective than c = 1 (33%) and is staged before it;
--   with the dependency a -> b known, b = 0 is implied by a = 7 and c goes
--   first, so b is fetched only for the rows c lets through.
CREATE TABLE gp_dep(id int, a int, b int, c int, pad text)
  USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED BY (id);
INSERT INTO gp_dep SELECT i, i % 10000, (i % 10000) / 100, (i * 7919) % 3, md5(i::text)
  FROM generate_series(1, 200000) i;
ANALYZE gp_dep;
SELECT gi_stages('SELECT pad FROM gp_dep WHERE a = 7 AND b = 0 AND c = 1') AS without_ext_stats;
CREATE STATISTICS gp_dep_ab (dependencies) ON a, b FROM gp_dep;
ANALYZE gp_dep;
SELECT gi_stages('SELECT pad FROM gp_dep WHERE a = 7 AND b = 0 AND c = 1') AS with_ext_stats;
SELECT gi_same('SELECT count(*) || '':'' || md5(string_agg(pad, '','' ORDER BY id)) FROM gp_dep WHERE a = 7 AND b = 0 AND c = 1') AS same_with_ext_stats;
DROP TABLE gp_dep;

-- A whole-row reference projects every column, the ideal case for a
-- clustered predicate; a pseudoconstant clause is not part of the scan's qual
-- and does not change the decision.
SELECT gi_planned('SELECT t FROM gp_main t WHERE clus = 7')               AS whole_row,
       gi_planned('SELECT t, c FROM gp_main t WHERE clus = 7')            AS whole_row_and_col,
       gi_planned('SELECT c FROM gp_main WHERE clus = 7 AND now() > ''2000-01-01''') AS pseudoconstant;
SELECT gi_same('SELECT count(*) || '':'' || md5(string_agg(t::text, '','' ORDER BY a)) FROM gp_main t WHERE clus = 7') AS whole_row_same;

-- Row-level security orders the quals: a user's clause stays behind the
-- policy's unless it is leakproof, so a leaky function never sees a row the
-- policy hides. Staged Scan stages the clauses in its own order, so with a
-- policy in force it is used only when every user clause is leakproof
-- (int4eq is; textlike is not). The owner bypasses the policy and keeps both.
CREATE ROLE regress_staged_rls;
GRANT SELECT ON gp_main TO regress_staged_rls;
ALTER TABLE gp_main ENABLE ROW LEVEL SECURITY;
CREATE POLICY gp_main_scat0 ON gp_main FOR SELECT USING (scat = 0);
SELECT gi_planned('SELECT a FROM gp_main WHERE clus = 7')             AS owner_leakproof,
       gi_planned('SELECT a FROM gp_main WHERE c LIKE ''payload 2000%''') AS owner_leaky;
SET ROLE regress_staged_rls;
SELECT gi_planned('SELECT a FROM gp_main WHERE clus = 7')             AS rls_leakproof,
       gi_planned('SELECT a FROM gp_main WHERE c LIKE ''payload 2000%''') AS rls_leaky;
SELECT gi_same('SELECT count(*) || '':'' || coalesce(md5(string_agg(c, '','' ORDER BY a)), ''-'') FROM gp_main WHERE clus = 7') AS rls_same;
RESET ROLE;
DROP POLICY gp_main_scat0 ON gp_main;
ALTER TABLE gp_main DISABLE ROW LEVEL SECURITY;
REVOKE SELECT ON gp_main FROM regress_staged_rls;
DROP ROLE regress_staged_rls;

DROP FUNCTION gi_planned(text);
DROP FUNCTION gi_stages(text);
DROP FUNCTION gi_same(text);
DROP TABLE gp_main, gp_dim, gp_heap, gp_aorow, gp_blkdir, gp_blkdir_opt, gp_one, gp_empty,
           gp_multiseg, gp_none, gp_zlib, gp_rle, gp_small, gp_lob, gp_alter;
