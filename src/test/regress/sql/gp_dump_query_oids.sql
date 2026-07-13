-- gp_dump_query_oids() doesn't list built-in functions, so we need a UDF to test with.
CREATE FUNCTION dumptestfunc(t text) RETURNS integer AS $$ SELECT 123 $$ LANGUAGE SQL;
CREATE FUNCTION dumptestfunc2(t text) RETURNS integer AS $$ SELECT 123 $$ LANGUAGE SQL;
create table base(a int);
create materialized view base_mv as select a from base;
create view base_v as select a from base;
create materialized view base_mv2 as select a from base_mv;
create view base_v_over_mv as select a from base_mv;
create table rule_tgt(a int);
create table rule_log(a int);
create rule tgt_log_rule as on insert to rule_tgt do also insert into rule_log select a from base_mv;
CREATE SEQUENCE minirepro_test_b;
CREATE TABLE minirepro_test(a serial, b int default nextval('minirepro_test_b'));

-- The function returns OIDs. We need to replace them with something reproducable.
CREATE FUNCTION sanitize_output(t text) RETURNS text AS $$
declare
  dumptestfunc_oid oid;
  dumptestfunc2_oid oid;
  base_oid oid;
  base_mv_oid oid;
  base_v_oid oid;
  base_mv2_oid oid;
  base_v_over_mv_oid oid;
  rule_tgt_oid oid;
  rule_log_oid oid;
  minirepro_test_oid oid;
  minirepro_test_b_oid oid;
  minirepro_test_a_seq_oid oid;
begin
    dumptestfunc_oid = 'dumptestfunc'::regproc::oid;
    dumptestfunc2_oid = 'dumptestfunc2'::regproc::oid;
    base_oid = 'base'::regclass::oid;
    base_mv_oid = 'base_mv'::regclass::oid;
    base_v_oid = 'base_v'::regclass::oid;
    base_mv2_oid = 'base_mv2'::regclass::oid;
    base_v_over_mv_oid = 'base_v_over_mv'::regclass::oid;
    rule_tgt_oid = 'rule_tgt'::regclass::oid;
    rule_log_oid = 'rule_log'::regclass::oid;
    minirepro_test_oid = 'minirepro_test'::regclass::oid;
    minirepro_test_b_oid = 'minirepro_test_b'::regclass::oid;
    minirepro_test_a_seq_oid = 'minirepro_test_a_seq'::regclass::oid;

    t := replace(t, dumptestfunc_oid::text, '<dumptestfunc>');
    t := replace(t, dumptestfunc2_oid::text, '<dumptestfunc2>');
    t := replace(t, base_oid::text, '<base_table>');
    t := replace(t, base_mv_oid::text, '<base_mv>');
    t := replace(t, base_v_oid::text, '<base_v>');
    t := replace(t, base_mv2_oid::text, '<base_mv2>');
    t := replace(t, base_v_over_mv_oid::text, '<base_v_over_mv>');
    t := replace(t, rule_tgt_oid::text, '<rule_tgt>');
    t := replace(t, rule_log_oid::text, '<rule_log>');
    t := replace(t, minirepro_test_oid::text, '<minirepro_test>');
    t := replace(t, minirepro_test_b_oid::text, '<minirepro_test_b>');
    t := replace(t, minirepro_test_a_seq_oid::text, '<minirepro_test_a_seq>');

    RETURN t;
end;
$$ LANGUAGE plpgsql;

-- Test the built-in gp_dump_query function.
SELECT sanitize_output(gp_dump_query_oids('SELECT 123'));
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM pg_proc'));
SELECT sanitize_output(gp_dump_query_oids('SELECT length(proname) FROM pg_proc'));
SELECT sanitize_output(gp_dump_query_oids('SELECT dumptestfunc(proname) FROM pg_proc'));

-- with EXPLAIN
SELECT sanitize_output(gp_dump_query_oids('explain SELECT dumptestfunc(proname) FROM pg_proc'));

-- with a multi-query statement
SELECT sanitize_output(gp_dump_query_oids('SELECT dumptestfunc(proname) FROM pg_proc; SELECT dumptestfunc2(relname) FROM pg_class'));

-- Test error reporting on an invalid query.
SELECT gp_dump_query_oids('SELECT * FROM nonexistent_table');
SELECT gp_dump_query_oids('SELECT with syntax error');

-- Test partition and inherited tables
CREATE TABLE minirepro_partition_test (id int, info json);
CREATE TABLE foo (id int, year int, a int, b int, c int, d int, region text)
 DISTRIBUTED BY (id)
 PARTITION BY RANGE (year)
     SUBPARTITION BY RANGE (a)
        SUBPARTITION TEMPLATE (
         START (1) END (2) EVERY (1),
         DEFAULT SUBPARTITION other_a )
            SUBPARTITION BY RANGE (b)
               SUBPARTITION TEMPLATE (
               START (1) END (2) EVERY (1),
               DEFAULT SUBPARTITION other_b )
( START (2002) END (2003) EVERY (1),
   DEFAULT PARTITION outlying_years );
CREATE TABLE ptable (c1 text, c2 float);
CREATE TABLE ctable (c3 char(2)) INHERITS (ptable);
CREATE TABLE cctable (c4 char(2)) INHERITS (ctable);
INSERT INTO minirepro_partition_test VALUES (1, (select gp_dump_query_oids('SELECT * FROM foo') ) :: json);
INSERT INTO minirepro_partition_test VALUES (2, (select gp_dump_query_oids('SELECT * FROM ptable') ) :: json);
INSERT INTO minirepro_partition_test VALUES (3, (select gp_dump_query_oids('SELECT * FROM pg_class') ) :: json);
SELECT array['foo'::regclass::oid::text,
             'foo_1_prt_outlying_years'::regclass::oid::text,
             'foo_1_prt_outlying_years_2_prt_other_a'::regclass::oid::text,
             'foo_1_prt_outlying_years_2_prt_other_a_3_prt_other_b'::regclass::oid::text,
             'foo_1_prt_outlying_years_2_prt_other_a_3_prt_2'::regclass::oid::text,
             'foo_1_prt_outlying_years_2_prt_2'::regclass::oid::text,
             'foo_1_prt_outlying_years_2_prt_2_3_prt_other_b'::regclass::oid::text,
             'foo_1_prt_outlying_years_2_prt_2_3_prt_2'::regclass::oid::text,
             'foo_1_prt_2'::regclass::oid::text,
             'foo_1_prt_2_2_prt_other_a'::regclass::oid::text,
             'foo_1_prt_2_2_prt_other_a_3_prt_other_b'::regclass::oid::text,
             'foo_1_prt_2_2_prt_other_a_3_prt_2'::regclass::oid::text,
             'foo_1_prt_2_2_prt_2'::regclass::oid::text,
             'foo_1_prt_2_2_prt_2_3_prt_other_b'::regclass::oid::text,
             'foo_1_prt_2_2_prt_2_3_prt_2'::regclass::oid::text] <@  (string_to_array((SELECT info->>'relids' FROM minirepro_partition_test WHERE id = 1),','));
SELECT array['ptable'::regclass::oid::text,
             'ctable'::regclass::oid::text,
             'cctable'::regclass::oid::text] <@  (string_to_array((SELECT info->>'relids' FROM minirepro_partition_test WHERE id = 2),','));
SELECT array['pg_class'::regclass::oid::text] <@  (string_to_array((SELECT info->>'relids' FROM minirepro_partition_test WHERE id = 3),','));
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM base_mv'));
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM base_v'));
-- gp_dump_query_oids should output relids of view/materialized view and used/accessed objects when query contains explain command
SELECT sanitize_output(gp_dump_query_oids('EXPLAIN SELECT * FROM base_mv'));
SELECT sanitize_output(gp_dump_query_oids('EXPLAIN SELECT * FROM base_v'));
-- gp_dump_query_oids should output relids of view/materialized view and used/accessed objects when query contains explain analyze command
SELECT sanitize_output(gp_dump_query_oids('EXPLAIN ANALYZE SELECT * FROM base_mv'));
SELECT sanitize_output(gp_dump_query_oids('EXPLAIN ANALYZE SELECT * FROM base_v'));
-- materialized views must be expanded at any nesting depth, so that objects
-- referenced transitively (here the base table) are always collected
-- materialized view on top of another materialized view
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM base_mv2'));
SELECT sanitize_output(gp_dump_query_oids('EXPLAIN SELECT * FROM base_mv2'));
-- materialized view inside a subquery
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM (SELECT * FROM base_mv) x'));
-- materialized view inside a sublink
SELECT sanitize_output(gp_dump_query_oids('SELECT 1 WHERE EXISTS (SELECT 1 FROM base_mv)'));
-- materialized view behind a regular view
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM base_v_over_mv'));
-- materialized view inside a CTE
SELECT sanitize_output(gp_dump_query_oids('WITH c AS (SELECT * FROM base_mv) SELECT * FROM c'));
-- materialized view inside a DO ALSO rule action fired by a DML statement
SELECT sanitize_output(gp_dump_query_oids('INSERT INTO rule_tgt VALUES (1)'));
-- gp_dump_query_oids should output relids of referenced sequences
SELECT sanitize_output(gp_dump_query_oids('SELECT * FROM minirepro_test'));

DROP TABLE minirepro_test;
DROP SEQUENCE minirepro_test_b;
DROP TABLE foo;
DROP TABLE cctable;
DROP TABLE ctable;
DROP TABLE ptable;
DROP TABLE minirepro_partition_test;
DROP TABLE rule_tgt;
DROP TABLE rule_log;
DROP VIEW base_v_over_mv;
DROP MATERIALIZED VIEW base_mv2;
DROP MATERIALIZED VIEW base_mv;
DROP VIEW base_v;
DROP TABLE base;
DROP FUNCTION dumptestfunc(text);
DROP FUNCTION dumptestfunc2(text);
