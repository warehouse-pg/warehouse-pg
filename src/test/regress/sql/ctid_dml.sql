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
CREATE TABLE ih (id int, v int) DISTRIBUTED BY (id);
CREATE TABLE ih_c () INHERITS (ih) DISTRIBUTED BY (id);
INSERT INTO ih VALUES (1, 1);
INSERT INTO ih_c VALUES (1, 2);
-- More partitioned tables: the distribution key includes the partition key;
-- two levels, one subpartition attached with its columns in another order;
-- hash partitions; a partition key expression.
CREATE TABLE pk2 (id int, k int) DISTRIBUTED BY (id, k)
  PARTITION BY RANGE (k) (START (0) END (4) EVERY (1));
INSERT INTO pk2 SELECT 1, g FROM generate_series(0, 3) g;
CREATE TABLE p2 (id int, k int, v int, w int) DISTRIBUTED BY (id) PARTITION BY RANGE (k);
CREATE TABLE p2_a (w int, id int, k int, v int) DISTRIBUTED BY (id) PARTITION BY RANGE (v);
CREATE TABLE p2_a0 PARTITION OF p2_a FOR VALUES FROM (0) TO (2);
CREATE TABLE p2_a1 PARTITION OF p2_a FOR VALUES FROM (2) TO (4);
ALTER TABLE p2 ATTACH PARTITION p2_a FOR VALUES FROM (0) TO (1);
INSERT INTO p2 SELECT 1, 0, g, 0 FROM generate_series(0, 3, 2) g;
CREATE TABLE ph (id int, k int) DISTRIBUTED BY (id) PARTITION BY HASH (k);
CREATE TABLE ph_0 PARTITION OF ph FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE ph_1 PARTITION OF ph FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO ph SELECT 1, g FROM generate_series(0, 3) g;
CREATE TABLE pe (id int, k int) DISTRIBUTED BY (id) PARTITION BY RANGE ((k + 0));
CREATE TABLE pe_a PARTITION OF pe FOR VALUES FROM (0) TO (4);
INSERT INTO pe SELECT 1, g FROM generate_series(0, 3) g;
-- More fixtures: views that expose ctid (one with a DELETE rule that deletes
-- by it), a SQL function that deletes by ctid, LIST partitions,
-- append-only partitions, two levels with a different sub-key per partition.
CREATE VIEW tv AS SELECT ctid AS c, id, v FROM t;
CREATE VIEW tr AS SELECT ctid AS c, id, v FROM t;
CREATE RULE tr_del AS ON DELETE TO tr DO INSTEAD DELETE FROM t WHERE ctid = OLD.c;
CREATE FUNCTION del_sql(tid) RETURNS void LANGUAGE sql AS $$ DELETE FROM t WHERE ctid = $1 $$;
CREATE TABLE lp (id int, region text) DISTRIBUTED BY (id) PARTITION BY LIST (region);
CREATE TABLE lp_e PARTITION OF lp FOR VALUES IN ('e');
CREATE TABLE lp_w PARTITION OF lp FOR VALUES IN ('w');
CREATE TABLE lp_d PARTITION OF lp DEFAULT;
INSERT INTO lp VALUES (1, 'e'), (1, 'w'), (1, 'x');
CREATE TABLE pao (id int, k int) WITH (appendonly = true) DISTRIBUTED BY (id)
  PARTITION BY RANGE (k) (START (0) END (4) EVERY (1));
INSERT INTO pao SELECT 1, g FROM generate_series(0, 3) g;
CREATE TABLE hz (id int, k int, v int, w int) DISTRIBUTED BY (id) PARTITION BY RANGE (k);
CREATE TABLE hz_a PARTITION OF hz FOR VALUES FROM (0) TO (1) PARTITION BY RANGE (v);
CREATE TABLE hz_a0 PARTITION OF hz_a FOR VALUES FROM (0) TO (2);
CREATE TABLE hz_a1 PARTITION OF hz_a FOR VALUES FROM (2) TO (4);
CREATE TABLE hz_b PARTITION OF hz FOR VALUES FROM (1) TO (2) PARTITION BY RANGE (w);
CREATE TABLE hz_b0 PARTITION OF hz_b FOR VALUES FROM (0) TO (2);
CREATE TABLE hz_b1 PARTITION OF hz_b FOR VALUES FROM (2) TO (4);
INSERT INTO hz VALUES (1, 0, 0, 0), (1, 0, 2, 0), (1, 1, 0, 0), (1, 1, 0, 2);

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
-- A distribution key condition that does not fix the key to values: a
-- range, only one column of a two-column key, a key joined to another table,
-- a list read by a sub-select, an OR that mixes the key with another column
-- or a range, a NOT IN list, a randomly distributed table.
DELETE FROM t WHERE ctid = '(0,1)' AND id BETWEEN 1 AND 2;
DELETE FROM m WHERE ctid = '(0,1)' AND a = 1;
DELETE FROM m WHERE ctid = '(0,1)' AND a IN (1, 4);
DELETE FROM t USING d WHERE t.ctid = '(0,1)' AND t.id = d.id;
DELETE FROM t WHERE ctid = '(0,1)' AND id IN (SELECT id FROM d);
DELETE FROM t WHERE ctid = '(0,1)' AND id = ANY (ARRAY(SELECT id FROM d));
DELETE FROM t WHERE ctid = '(0,1)' AND (id = 1 OR v = 2);
DELETE FROM t WHERE ctid = '(0,1)' AND (id = 1 OR id > 5);
DELETE FROM t WHERE ctid = '(0,1)' AND id NOT IN (1, 2);
DELETE FROM t WHERE ctid = '(0,1)' AND id = ALL ('{1,2}'::int[]);
DELETE FROM r WHERE ctid = '(0,1)' AND id = 1;
-- A table with partitions or inheritance children: each member has its own
-- ctids, so one key value can reach a row in every member, unless the
-- statement names the member as well.  Not named: no condition, a range on
-- the partition key, only the top level of two, tableoid from another
-- table, a partition key expression (not analysed).  As the target, joined
-- in, or two subquery levels down.
DELETE FROM pt WHERE ctid = '(0,1)' AND id = 1;
UPDATE pt SET k = k WHERE ctid = '(0,1)' AND id = 1;
DELETE FROM ih WHERE ctid = '(0,1)' AND id = 1;
DELETE FROM t USING pt WHERE pt.ctid = '(0,1)' AND pt.id = 1 AND t.id = pt.k;
DELETE FROM pt WHERE k IN (SELECT v FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = pt.ctid AND pt.id = 7));
DELETE FROM pt WHERE ctid = '(0,1)' AND id = 1 AND k > 1;
DELETE FROM p2 WHERE ctid = '(0,1)' AND id = 1 AND k = 0;
DELETE FROM pt USING d WHERE pt.ctid = '(0,1)' AND pt.id = 1 AND pt.tableoid = d.v;
DELETE FROM pe WHERE ctid = '(0,1)' AND id = 1 AND k = 2;
-- another column of the same row is not a fixed value
DELETE FROM t WHERE ctid = '(0,1)' AND id = v;
DELETE FROM t WHERE ctid = '(0,1)' AND id IN (1, v);
-- a volatile expression is not a fixed value
DELETE FROM t WHERE ctid = '(0,1)' AND id = (random() * 0 + 1)::int;
-- A non-strict operator, which constant folding can turn into true.
CREATE FUNCTION ctid_dml_any(int, bigint) RETURNS bool LANGUAGE sql IMMUTABLE AS 'SELECT true';
CREATE OPERATOR >>> (LEFTARG = int, RIGHTARG = bigint, FUNCTION = ctid_dml_any);
-- Skip-level, not proven, left to the planner, which cannot plan skip-level
-- correlations: no gp_segment_id at all (Y11); a segment condition in an OR
-- that is always true; in the output list of a sibling sub-select; in a
-- scalar sub-select inside CASE or a non-strict operator, which constant
-- folding can remove.
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid));
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND (t.gp_segment_id = 0 OR 1 = 1)));
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND EXISTS (SELECT t.gp_segment_id FROM s s2));
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND CASE WHEN false THEN (SELECT count(*) FROM s s2 WHERE s2.sid = t.gp_segment_id) > 0 ELSE true END);
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND 0 >>> (SELECT count(*) FROM s s2 WHERE s2.sid = t.gp_segment_id));
-- One level down, kept as a SubPlan (LIMIT): the planner folds the operator
-- to true before it checks, and rejects the statement.
DELETE FROM t WHERE ctid = '(0,1)' AND 0 >>> (SELECT count(*) FROM s s2 WHERE s2.sid = t.gp_segment_id LIMIT 1);
DROP OPERATOR >>> (int, bigint);
DROP FUNCTION ctid_dml_any(int, bigint);
-- ctid IS NOT NULL does not pick rows, but a ctid comparison next to it does.
DELETE FROM t WHERE ctid IS NOT NULL AND ctid = '(0,1)';

--
-- gp_segment_id that the planner removes before it checks: rejected.
--
-- T4: "A OR (A AND B)" is simplified to "A".
DELETE FROM t WHERE ctid = '(0,1)' OR (ctid = '(0,1)' AND gp_segment_id = 0);
-- The same after constant folding, which the planner does first.
DELETE FROM t WHERE ctid = '(0,1)' OR (ctid = ('(0,' || '1)')::tid AND gp_segment_id = 0);
-- With a parameter bound to the same ctid (a custom plan folds it).
PREPARE del_or(tid) AS DELETE FROM t WHERE ctid = $1 OR (ctid = '(0,1)' AND gp_segment_id = 0);
EXECUTE del_or('(0,1)');
DEALLOCATE del_or;
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

-- More statements that do not pin the row.  Except where noted, each changed
-- rows on other segments (or partitions) under GPORCA in 7.6.0 with this
-- data; all are rejected now.  Other SQL forms:
UPDATE t SET note = 'y' FROM (VALUES ('(0,1)'::tid), ('(0,2)'::tid)) v(c) WHERE t.ctid = v.c;
DELETE FROM t WHERE ctid IS NOT DISTINCT FROM '(0,1)';
DELETE FROM t WHERE (ctid = '(0,1)') IS TRUE;
DELETE FROM t WHERE CASE WHEN ctid = '(0,1)' THEN true ELSE false END;
DELETE FROM t WHERE ctid > ALL (SELECT tid FROM s);
DELETE FROM t WHERE ctid IN (SELECT tid FROM s UNION SELECT '(0,2)'::tid);
DELETE FROM t USING s LEFT JOIN d ON d.id = 7 WHERE t.ctid = s.tid;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM t t2 WHERE t2.id = d.id AND t2.ctid = '(0,1)'));
DELETE FROM tv WHERE c = '(0,1)';
DELETE FROM tr WHERE id = 7;
SELECT del_sql('(0,1)');
DO $$ BEGIN EXECUTE format('DELETE FROM ctid_dml.t WHERE ctid = %L', '(0,1)'); END $$;
CREATE TEMP TABLE tt AS SELECT * FROM t DISTRIBUTED BY (id);
DELETE FROM tt WHERE ctid = '(0,1)';
DROP TABLE tt;
-- A condition that does not pin the row: ctid IS NOT NULL in an OR, and
-- another table's ctid matched through a third table.
DELETE FROM t WHERE ctid IS NOT NULL OR ctid = '(0,1)';
CREATE TABLE s1 (tid tid) DISTRIBUTED RANDOMLY;
INSERT INTO s1 VALUES ('(0,1)');
DELETE FROM t USING d, s1 WHERE d.ctid = s1.tid AND t.id = d.id;
DROP TABLE s1;
-- The rule counts a distribution key fixed with =, IN, = ANY or an OR of =
-- only, so these are rejected although each can reach a single segment only
-- (a cast of the key; IS NULL, all NULL keys being on one segment).
DELETE FROM t WHERE ctid = '(0,1)' AND id::text = '1';
DELETE FROM t WHERE ctid = '(0,1)' AND id IS NULL;
-- A gp_segment_id condition that filters nothing after simplification, or
-- does not reach the statement's rows:
DELETE FROM t WHERE ctid = '(0,1)' AND CASE WHEN true THEN true ELSE gp_segment_id = 0 END;
DELETE FROM t WHERE ctid = '(0,1)' AND COALESCE(true, gp_segment_id = 0);
WITH x AS (SELECT 1 FROM t b WHERE b.ctid = '(0,1)' AND b.gp_segment_id = 0) DELETE FROM t a WHERE a.ctid = '(0,1)';
DELETE FROM t a WHERE a.ctid = '(0,1)' AND a.id IN (SELECT b.id FROM t b ORDER BY a.gp_segment_id);
-- Partitioned tables, the row not named: a partition alone by ctid, tableoid
-- or the partition key without the distribution key, a range on the
-- partition key, LIST and append-only partitions, a prepared statement.
DELETE FROM pt_1_prt_2 WHERE ctid = '(0,1)';
DELETE FROM pt WHERE ctid = '(0,1)' AND tableoid = 'pt_1_prt_2'::regclass;
DELETE FROM pt WHERE ctid = '(0,1)' AND k = 1;
UPDATE pt SET k = 3 WHERE ctid = '(0,1)' AND k < 2;
DELETE FROM lp WHERE ctid = '(0,1)' AND id = 1;
SELECT ctid AS pao_ctid FROM pao WHERE id = 1 AND k = 0 \gset
DELETE FROM pao WHERE ctid = :'pao_ctid' AND id = 1;
SET plan_cache_mode = force_generic_plan;
PREPARE del_lp(int, tid) AS DELETE FROM lp WHERE id = $1 AND ctid = $2;
EXECUTE del_lp(1, '(0,1)');
DEALLOCATE del_lp;
RESET plan_cache_mode;

-- Nothing was changed.
SELECT count(*), sum(id), sum(CASE WHEN note <> 'x' THEN 1 ELSE 0 END) AS changed FROM t;
SELECT count(*) FROM d;
SELECT count(*) FROM pt;
SELECT count(*) FROM ih;
SELECT count(*) FROM pk2;
SELECT count(*) FROM p2;
SELECT count(*) FROM ph;
SELECT count(*) FROM pe;
SELECT count(*) FROM lp;
SELECT count(*) FROM pao;
SELECT count(*) FROM hz;

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
-- A distribution key fixed to values names each row's segment, so the ctid
-- locates at most one row per key value, as with gp_segment_id.
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
-- K3: a list of key values; ids 1, 2 and 5 are the rows at (0,1)
DELETE FROM t WHERE ctid = '(0,1)' AND id IN (1, 2, 5);
SELECT count(*) AS left_rows FROM t WHERE id IN (1, 2, 5);
ROLLBACK;
BEGIN;
DELETE FROM t WHERE ctid = '(0,1)' AND (id = 1 OR id = 2);
SELECT count(*) AS left_rows FROM t WHERE id IN (1, 2);
ROLLBACK;
BEGIN;
-- a list as an array parameter, generic plan
SET LOCAL plan_cache_mode = force_generic_plan;
PREPARE del_keys(tid, int[]) AS DELETE FROM t WHERE ctid = $1 AND id = ANY ($2);
EXECUTE del_keys('(0,1)', '{1,5}');
DEALLOCATE del_keys;
SELECT count(*) AS left_rows FROM t WHERE id IN (1, 2, 5);
ROLLBACK;
BEGIN;
-- a two-column key: a list on one column, a value on the other
DELETE FROM m WHERE ctid = '(0,1)' AND a IN (1, 4) AND b = 1;
SELECT count(*) AS left_rows FROM m;
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
-- A ctid referenced two subquery levels down.  The Postgres planner cannot
-- plan such a skip-level correlation at all; GPORCA plans it when the
-- statement itself proves the row's segment, with a condition on
-- gp_segment_id in an AND around the ctid (Y12, its UPDATE form, the
-- condition one level up, IS NOT NULL).
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND s.sid = t.gp_segment_id));
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
BEGIN;
UPDATE t SET note = 'y' WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND s.sid = t.gp_segment_id));
SELECT id, note FROM t WHERE note <> 'x';
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT d.id FROM d WHERE t.gp_segment_id = (SELECT sid FROM s) AND EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND t.gp_segment_id IS NOT NULL));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- The same with the segment given in an OR the planner keeps (R2), in a
-- sibling EXISTS (R3), NOT EXISTS, scalar or ALL sub-select, or with the
-- distribution key fixed inside the subquery (R4).
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND (s.sid = t.gp_segment_id OR s.sid IS NULL)));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND EXISTS (SELECT 1 FROM s s2 WHERE s2.sid = t.gp_segment_id));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id + 1 FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND NOT EXISTS (SELECT 1 FROM s s2 WHERE s2.sid = t.gp_segment_id));
SELECT count(*) AS left_rows, sum(CASE WHEN id = 10 THEN 1 ELSE 0 END) AS id_10 FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND (SELECT count(*) FROM s s2 WHERE s2.sid = t.gp_segment_id) > 0);
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND t.v <> ALL (SELECT s2.sid + 10 FROM s s2 WHERE s2.sid = t.gp_segment_id));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND t.id = 7));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid AND (s.sid = t.gp_segment_id OR 1 = 0)));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
BEGIN;
DELETE FROM t WHERE id IN (SELECT id FROM d WHERE EXISTS (SELECT 1 FROM s WHERE s.tid = t.ctid) AND EXISTS (SELECT 1 FROM d d2 WHERE EXISTS (SELECT 1 FROM s s2 WHERE s2.sid = t.gp_segment_id)));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- With a strict operator the sub-select stays, and so does gp_segment_id.
BEGIN;
DELETE FROM t WHERE ctid = '(0,1)' AND 0 < (SELECT count(*) FROM s s2 WHERE s2.sid = t.gp_segment_id LIMIT 1);
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- One partition named directly, or the inheritance parent alone (ONLY), is
-- an ordinary table: the key locates the row.
BEGIN;
DELETE FROM pt_1_prt_2 WHERE ctid = '(0,1)' AND id = 5;
DELETE FROM ONLY ih WHERE ctid = '(0,1)' AND id = 1;
SELECT (SELECT count(*) FROM pt_1_prt_2) AS leaf_rows, (SELECT count(*) FROM ih) AS ih_rows, (SELECT count(*) FROM ih_c) AS ih_c_rows;
ROLLBACK;
-- The member named too: the partition key fixed to a value or a list (one
-- row per listed partition), tableoid fixed, a distribution key that
-- includes the partition key, both levels of two, hash partitions.
BEGIN;
DELETE FROM pt WHERE ctid = '(0,1)' AND id = 5 AND k = 1;
DELETE FROM pt WHERE ctid = '(0,1)' AND id = 2 AND k IN (1, 2);
DELETE FROM pt WHERE ctid = '(0,1)' AND id = 3 AND tableoid = 'pt_1_prt_4'::regclass;
DELETE FROM ih WHERE ctid = '(0,1)' AND id = 1 AND tableoid = 'ih_c'::regclass;
DELETE FROM pk2 WHERE ctid = '(0,1)' AND id = 1 AND k = 2;
DELETE FROM p2 WHERE ctid = '(0,1)' AND id = 1 AND k = 0 AND v = 2;
DELETE FROM ph WHERE ctid = '(0,1)' AND id = 1 AND k = 3;
SELECT (SELECT count(*) FROM pt) AS pt, (SELECT count(*) FROM ih) AS ih, (SELECT count(*) FROM pk2) AS pk2, (SELECT count(*) FROM p2) AS p2, (SELECT count(*) FROM ph) AS ph;
ROLLBACK;
-- The partition key compared under another collation, and two levels with a
-- different sub-key per partition when every partition key is fixed.
BEGIN;
DELETE FROM lp WHERE ctid = '(0,1)' AND id = 1 AND region = 'e' COLLATE "C";
DELETE FROM hz WHERE ctid = '(0,1)' AND id = 1 AND k = 0 AND v = 2 AND w = 0;
SELECT (SELECT count(*) FROM lp) AS lp, (SELECT count(*) FROM hz) AS hz;
ROLLBACK;
-- A key condition inside a sub-select the planner keeps as a SubPlan still
-- fixes the segment: both optimizers accept it.
BEGIN;
DELETE FROM t WHERE 0 < (SELECT count(*) FROM s WHERE s.tid = t.ctid AND t.id = 7);
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
-- The same with a key value from an immutable function that the planner
-- runs while it plans the statement, and that plans (and catches an error
-- in) a statement of its own.
CREATE FUNCTION ctid_dml_seven() RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
  PERFORM 1 / 0;
  RETURN 7;
EXCEPTION WHEN division_by_zero THEN
  RETURN 7;
END $$;
BEGIN;
DELETE FROM t WHERE 0 < (SELECT count(*) FROM s WHERE s.tid = t.ctid AND t.id = ctid_dml_seven());
SELECT count(*) AS left_rows, sum(CASE WHEN id = 7 THEN 1 ELSE 0 END) AS id_7 FROM t;
ROLLBACK;
DROP FUNCTION ctid_dml_seven();
-- An OR whose arms share the gp_segment_id condition: the planner reduces it
-- to that condition, and GPORCA plans it.
BEGIN;
DELETE FROM t WHERE ctid = '(0,1)' AND ((gp_segment_id = 0 AND v = 1) OR gp_segment_id = 0);
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- An OR with a sub-select in an arm is left to the planner, which plans it.
BEGIN;
DELETE FROM t WHERE ctid = '(0,1)' AND (gp_segment_id = 0 OR EXISTS (SELECT 1 FROM s WHERE s.sid = t.v));
SELECT count(*) AS left_rows FROM t;
ROLLBACK;
-- A ctid reached through a select list is not proven, so GPORCA leaves the
-- statement to the planner, which plans this one.
BEGIN;
DELETE FROM t WHERE id IN (SELECT (SELECT max(sid) FROM s WHERE s.tid = t.ctid AND s.sid = t.gp_segment_id) FROM d);
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
