--
-- UPDATE and DELETE that identify rows by ctid.
--
-- A ctid is unique only within one segment, so a ctid alone matches one row
-- on every segment.  The Postgres planner rejects an UPDATE or DELETE that
-- uses a relation's ctid without its gp_segment_id.  GPORCA leaves such
-- statements to the planner, so both optimizers raise the same error and no
-- row changes.  With optimizer_trace_fallback on, the GPORCA output shows
-- which statements it left to the planner and that it still plans the ones
-- that use gp_segment_id with ctid.
--
-- start_ignore
CREATE SCHEMA ctid_dml;
-- end_ignore
SET search_path TO ctid_dml;
SET optimizer_trace_fallback = on;

CREATE TABLE t (id int, v int, note text) DISTRIBUTED BY (id);
INSERT INTO t SELECT g, g % 4, 'x' FROM generate_series(1, 20) g;
CREATE TABLE d (id int, v int) DISTRIBUTED BY (id);
INSERT INTO d SELECT g, g % 3 FROM generate_series(1, 9) g;
CREATE TABLE s (tid tid, sid int) DISTRIBUTED RANDOMLY;
INSERT INTO s SELECT ctid, gp_segment_id FROM t WHERE id = 7;
CREATE TABLE r (id int, v int) DISTRIBUTED RANDOMLY;
INSERT INTO r SELECT g, g FROM generate_series(1, 10) g;
CREATE TABLE ao (id int, v int) WITH (appendonly = true) DISTRIBUTED BY (id);
INSERT INTO ao SELECT g, g FROM generate_series(1, 10) g;
CREATE TABLE pt (id int, k int) DISTRIBUTED BY (id)
  PARTITION BY RANGE (k) (START (0) END (4) EVERY (1));
INSERT INTO pt SELECT g, g % 4 FROM generate_series(1, 16) g;

-- The rows these tests are about: one row per segment shares a ctid.
SELECT count(*) > 1 AS ctid_not_unique FROM t WHERE ctid = '(0,1)';

-- ctid without gp_segment_id: rejected
DELETE FROM t WHERE ctid = '(0,1)';
UPDATE t SET note = 'y' WHERE ctid = '(0,2)';
UPDATE t SET id = id + 100 WHERE ctid = '(0,1)';
DELETE FROM t WHERE ctid::text = '(0,1)' OR id = 99;
DELETE FROM ao WHERE ctid = '(33554432,2)';
DELETE FROM pt WHERE ctid = '(0,1)';
DELETE FROM r WHERE ctid = '(0,1)';
PREPARE del_ctid(tid) AS DELETE FROM t WHERE ctid = $1;
EXECUTE del_ctid('(0,1)');
DEALLOCATE del_ctid;
CREATE FUNCTION del_by_ctid(tid) RETURNS void LANGUAGE plpgsql AS
  $$ BEGIN DELETE FROM t WHERE ctid = $1; END $$;
SELECT del_by_ctid('(0,1)');

-- ctid taken from a subquery, a join or a CTE: rejected
DELETE FROM d WHERE ctid = (SELECT ctid FROM d WHERE v = 0 ORDER BY id LIMIT 1);
DELETE FROM d WHERE ctid IN (SELECT ctid FROM (SELECT ctid, row_number() OVER (PARTITION BY v ORDER BY id) AS rn FROM d) x WHERE rn > 1);
DELETE FROM d WHERE ctid NOT IN (SELECT min(ctid) FROM d GROUP BY v);
DELETE FROM d a WHERE EXISTS (SELECT 1 FROM d b WHERE a.v = b.v AND a.ctid > b.ctid);
DELETE FROM t WHERE ctid IN (SELECT ctid FROM t WHERE v = 1 LIMIT 2);
DELETE FROM t USING t t2 WHERE t.ctid = t2.ctid AND t2.id = 7;
DELETE FROM t USING s WHERE t.ctid = s.tid;
DELETE FROM t USING s WHERE t.ctid = s.tid AND s.sid IS NOT NULL;
UPDATE t SET note = 'y' FROM s WHERE t.ctid = s.tid;
WITH x AS (SELECT tid FROM s) DELETE FROM t WHERE ctid IN (SELECT tid FROM x);
DELETE FROM t WHERE id IN (SELECT id FROM t t2 WHERE t2.ctid = '(0,1)');

-- Nothing was changed.
SELECT count(*), sum(id), sum(CASE WHEN note <> 'x' THEN 1 ELSE 0 END) AS changed FROM t;
SELECT count(*) FROM d;
SELECT count(*) FROM pt;

-- gp_segment_id together with ctid: allowed, and GPORCA plans it (no fallback)
BEGIN;
DELETE FROM t USING s WHERE t.ctid = s.tid AND t.gp_segment_id = s.sid;
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
-- The shape REFRESH MATERIALIZED VIEW CONCURRENTLY uses.
BEGIN;
DELETE FROM t WHERE ctid = ANY (SELECT s.tid FROM s WHERE s.tid = t.ctid AND s.sid = t.gp_segment_id);
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;

-- ctid only in the SET list or in RETURNING: not a row lookup, allowed
BEGIN;
UPDATE t SET note = (ctid IS NOT NULL)::text WHERE id = 3;
DELETE FROM t WHERE id = 4 RETURNING ctid IS NOT NULL AS had_ctid;
ROLLBACK;

-- GPORCA's check is wider than the planner's: it also counts a ctid read
-- without gp_segment_id inside a subquery.  Here the planner accepts the
-- statement, so GPORCA leaving it to the planner costs only GPORCA's plan.
-- The second statement also gets the planner's NOTICE for the subquery.
BEGIN;
DELETE FROM t WHERE gp_segment_id = (SELECT gp_segment_id FROM t WHERE id = 8)
                AND ctid = (SELECT ctid FROM t WHERE id = 8);
SELECT count(*) AS left_rows, sum(CASE WHEN id = 8 THEN 1 ELSE 0 END) AS id_8 FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id = (SELECT max(id) FROM t t2 WHERE t2.ctid = '(0,1)');
ROLLBACK;

RESET optimizer_trace_fallback;
-- start_ignore
DROP SCHEMA ctid_dml CASCADE;
-- end_ignore
