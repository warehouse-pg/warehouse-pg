--
-- UPDATE and DELETE that read ctid.
--
-- A ctid is unique only within one segment, so a ctid alone matches one row
-- on every segment.  The Postgres planner rejects an UPDATE or DELETE that
-- uses a relation's ctid without mentioning that relation's gp_segment_id,
-- unless an equality on its whole distribution key confines it to one
-- segment; otherwise the statement runs as written.  GPORCA
-- leaves every UPDATE or DELETE that reads a ctid to the planner, so both
-- optimizers give the same answer.  With optimizer_trace_fallback on, the
-- GPORCA output shows each hand-off.
--
-- The case names (U1, P1, K1, ...) are the same as in the review of this
-- change, so a reader can match each statement to its expected behaviour.
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
CREATE TABLE m (a int, b int, v int) DISTRIBUTED BY (a, b);
INSERT INTO m SELECT g, g % 3, g FROM generate_series(1, 20) g;
CREATE TABLE r (id int, v int) DISTRIBUTED RANDOMLY;
INSERT INTO r SELECT g, g FROM generate_series(1, 10) g;
CREATE TABLE ao (id int, v int) WITH (appendonly = true) DISTRIBUTED BY (id);
INSERT INTO ao SELECT g, g FROM generate_series(1, 10) g;
CREATE TABLE pt (id int, k int) DISTRIBUTED BY (id)
  PARTITION BY RANGE (k) (START (0) END (4) EVERY (1));
INSERT INTO pt SELECT g, g % 4 FROM generate_series(1, 16) g;

-- The rows these tests are about: on the standard 3-segment cluster, ids 1, 2
-- and 5 all sit at ctid (0,1), one on each segment.
SELECT count(*) AS rows_at_0_1 FROM t WHERE ctid = '(0,1)';

--
-- ctid without gp_segment_id: rejected by both optimizers.
--
-- U1, U2, U3
DELETE FROM t WHERE ctid = '(0,1)';
UPDATE t SET note = 'y' WHERE ctid = '(0,2)';
UPDATE t SET note = 'y' WHERE ctid = '(0,1)' RETURNING id;
UPDATE t SET id = id + 100 WHERE ctid = '(0,1)';
DELETE FROM t WHERE ctid::text = '(0,1)' OR id = 99;
-- append-only, partitioned and randomly distributed tables
DELETE FROM ao WHERE ctid = '(33554432,2)';
DELETE FROM pt WHERE ctid = '(0,1)';
DELETE FROM r WHERE ctid = '(0,1)';
-- prepared statement and PL/pgSQL
PREPARE del_ctid(tid) AS DELETE FROM t WHERE ctid = $1;
EXECUTE del_ctid('(0,1)');
DEALLOCATE del_ctid;
CREATE FUNCTION del_by_ctid(tid) RETURNS void LANGUAGE plpgsql AS
  $$ BEGIN DELETE FROM t WHERE ctid = $1; END $$;
SELECT del_by_ctid('(0,1)');
-- K2: another column does not choose the segment.
DELETE FROM t WHERE ctid = '(0,1)' AND v = 0;
-- K3: a list of distribution-key values reaches several segments.
DELETE FROM t WHERE ctid = '(0,1)' AND id IN (1, 2, 5);
-- A distribution key condition that can reach more than one segment: an
-- OR, a range, only one column of a two-column key, a key joined to another
-- table, a randomly distributed table.
DELETE FROM t WHERE ctid = '(0,1)' AND (id = 1 OR id = 2);
DELETE FROM t WHERE ctid = '(0,1)' AND id BETWEEN 1 AND 2;
DELETE FROM m WHERE ctid = '(0,1)' AND a = 1;
DELETE FROM t USING d WHERE t.ctid = '(0,1)' AND t.id = d.id;
DELETE FROM r WHERE ctid = '(0,1)' AND id = 1;
-- another column of the same row is not one fixed value
DELETE FROM t WHERE ctid = '(0,1)' AND id = v;
-- a volatile expression is not one fixed value
DELETE FROM t WHERE ctid = '(0,1)' AND id = (random() * 0 + 1)::int;
-- ctid IS NOT NULL does not pick rows, but a ctid comparison next to it does.
DELETE FROM t WHERE ctid IS NOT NULL AND ctid = '(0,1)';

--
-- gp_segment_id that the planner removes before it checks: rejected.
--
-- T4: "A OR (A AND B)" is simplified to "A".
DELETE FROM t WHERE ctid = '(0,1)' OR (ctid = '(0,1)' AND gp_segment_id = 0);
-- T5: the output list of an EXISTS subquery is discarded, with the qualifier
-- nested in it.
DELETE FROM t a WHERE EXISTS (SELECT (SELECT 1 WHERE b.gp_segment_id = 0) FROM t b WHERE b.id = a.id AND b.ctid = '(0,1)');
-- T7: once z is pulled up, z.k = 1 is true, and so is the OR.
DELETE FROM t a USING (SELECT 1 AS k) z WHERE a.ctid = '(0,1)' AND (a.gp_segment_id = 0 OR z.k = 1);
-- The output of an EXISTS is not used, so this gp_segment_id is not counted.
DELETE FROM t a WHERE EXISTS (SELECT b.gp_segment_id FROM t b WHERE b.id = a.id AND b.ctid = '(0,1)');
-- D1: an unused column of a derived table.
DELETE FROM t a USING (SELECT b.id, b.gp_segment_id AS s FROM t b WHERE b.ctid = '(0,1)') x WHERE a.id = x.id;
-- B1: b's ctid is used through the derived table and b's gp_segment_id is not,
-- so rows of a would be paired with another segment's ctid.
DELETE FROM t a USING (SELECT b.id, b.ctid AS c, b.gp_segment_id AS s FROM t b) x WHERE a.ctid = x.c AND a.gp_segment_id = 1;

--
-- ctid of a table in a subquery that the planner pulls up into the DML:
-- rejected.
--
-- O1: the deleted table's ctid appears only inside a subquery, as an outer
-- reference.  The planner turns the EXISTS into a join on t.ctid.
DELETE FROM t WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid);
-- V2, E1, E2
DELETE FROM t a USING (SELECT b.id FROM t b WHERE b.ctid = '(0,1)') x WHERE a.id = x.id;
DELETE FROM t a WHERE EXISTS (SELECT 1 FROM t b WHERE b.id = a.id AND b.ctid = '(0,1)');
DELETE FROM t WHERE id IN (SELECT id FROM t t2 WHERE t2.ctid = '(0,1)');
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

-- Nothing was changed.
SELECT count(*), sum(id), sum(CASE WHEN note <> 'x' THEN 1 ELSE 0 END) AS changed FROM t;
SELECT count(*) FROM d;
SELECT count(*) FROM pt;

--
-- gp_segment_id mentioned with ctid: allowed, runs as written.  Planned by
-- the planner (GPORCA hands it over), same rows under both optimizers.
--
BEGIN;
-- P1
DELETE FROM t WHERE ctid = '(0,1)' AND gp_segment_id = 0;
SELECT count(*) AS left_rows FROM t;
-- P2
DELETE FROM t USING s WHERE t.ctid = s.tid AND t.gp_segment_id = s.sid;
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
BEGIN;
-- P3: the shape REFRESH MATERIALIZED VIEW CONCURRENTLY uses.
DELETE FROM t WHERE ctid = ANY (SELECT s.tid FROM s WHERE s.tid = t.ctid AND s.sid = t.gp_segment_id);
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
BEGIN;
-- J1: paired in a join qualifier, one query level down.
DELETE FROM t WHERE id IN (SELECT b.id FROM t b JOIN s ON b.ctid = s.tid AND b.gp_segment_id = s.sid);
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
-- An equality on the whole distribution key confines the table to one
-- segment, so the ctid names at most one row, as with gp_segment_id.
BEGIN;
-- K1
DELETE FROM t WHERE ctid = '(0,1)' AND id = 1;
SELECT count(*) AS left_rows, sum(CASE WHEN id = 1 THEN 1 ELSE 0 END) AS id_1 FROM t;
ROLLBACK;
BEGIN;
UPDATE t SET note = 'y' WHERE ctid = '(0,1)' AND 2 = id;
SELECT id, note FROM t WHERE note <> 'x';
ROLLBACK;
BEGIN;
-- both columns of a two-column key
DELETE FROM m WHERE ctid = '(0,1)' AND a = 1 AND b = 1;
SELECT count(*) AS left_rows FROM m;
ROLLBACK;
BEGIN;
-- a parameter, also in a generic plan
SET LOCAL plan_cache_mode = force_generic_plan;
PREPARE del_key(tid, int) AS DELETE FROM t WHERE ctid = $1 AND id = $2;
EXECUTE del_key('(0,1)', 5);
DEALLOCATE del_key;
SELECT count(*) AS left_rows, sum(CASE WHEN id = 5 THEN 1 ELSE 0 END) AS id_5 FROM t;
ROLLBACK;
BEGIN;
-- Y33: d's ctid is a join key and d's gp_segment_id is not mentioned, but d
-- is confined to one segment by its key.
DELETE FROM t USING d WHERE t.ctid = d.ctid AND t.gp_segment_id = 0 AND d.id = 2;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
-- Y35: one table under two aliases, b confined by its key.
DELETE FROM t a USING t b WHERE a.ctid = b.ctid AND a.gp_segment_id = 1 AND b.id = 1;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
-- X27: ctid IS [NOT] NULL is not a row lookup.
DELETE FROM t WHERE ctid IS NOT NULL AND id < 3;
SELECT count(*) AS left_rows FROM t;
DELETE FROM t WHERE ctid IS NULL;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- Mentioning gp_segment_id is the user choosing the segments, even when the
-- condition filters nothing: these delete the row at (0,1) on every segment
-- they allow.
BEGIN;
-- T1
DELETE FROM t WHERE ctid = '(0,1)' AND gp_segment_id = gp_segment_id;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
-- T2
DELETE FROM t WHERE ctid = '(0,1)' AND gp_segment_id IS NOT NULL;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
-- T3
DELETE FROM t WHERE ctid = '(0,1)' AND gp_segment_id >= 0;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
-- T6: gp_segment_id correlated into a subquery that is not pulled up.
DELETE FROM t a WHERE a.ctid = '(0,1)' AND a.id IN (SELECT b.id FROM t b WHERE b.gp_segment_id = a.gp_segment_id UNION ALL SELECT 99);
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
-- T8: the rows at (0,1), or every row of segment 0.
DELETE FROM t WHERE ctid = '(0,1)' OR gp_segment_id = 0;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;

--
-- ctid read, but rows not chosen by it in the DML itself: allowed.
--
BEGIN;
-- N1, N2: ctid only in the SET list or in RETURNING.
UPDATE t SET note = (ctid IS NOT NULL)::text WHERE id = 3;
SELECT note FROM t WHERE id = 3;
DELETE FROM t WHERE id = 4 RETURNING ctid IS NOT NULL AS had_ctid;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- E3, E4: ctid in a subquery the planner plans on its own (a CTE, a scalar
-- subquery with LIMIT).  Planned by the planner, which prints its NOTICE for
-- the subquery.
BEGIN;
WITH x AS (SELECT b.id FROM t b WHERE b.ctid = '(0,1)') DELETE FROM t a USING x WHERE a.id = x.id;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t a WHERE a.id = (SELECT b.id FROM t b WHERE b.ctid = '(0,1)' ORDER BY b.id LIMIT 1);
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE gp_segment_id = (SELECT gp_segment_id FROM t WHERE id = 8)
                AND ctid = (SELECT ctid FROM t WHERE id = 8);
SELECT count(*) AS left_rows, sum(CASE WHEN id = 8 THEN 1 ELSE 0 END) AS id_8 FROM t;
ROLLBACK;
-- CUR: WHERE CURRENT OF does not read ctid.
BEGIN;
DECLARE c CURSOR FOR SELECT id FROM t WHERE id = 1;
FETCH 1 FROM c;
DELETE FROM t WHERE CURRENT OF c;
SELECT count(*) AS left_rows FROM t;
ROLLBACK;

RESET optimizer_trace_fallback;
-- start_ignore
DROP SCHEMA ctid_dml CASCADE;
-- end_ignore
