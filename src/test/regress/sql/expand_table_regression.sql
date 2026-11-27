-- start_ignore
create extension if not exists gp_debug_numsegments;
-- end_ignore

drop schema if exists test_expand_table_regression cascade;
create schema test_expand_table_regression;
set search_path=test_expand_table_regression,public;

--
-- Rows can end up on the wrong segment (e.g. a load that bypassed the
-- distribution-key check); data expansion should tolerate this.
--
-- WHPG: this used to seed the misplaced rows with a BEFORE ROW
-- trigger that rewrote the distribution key after the row had been routed.
-- WHPG now refuses such a trigger, so the misplaced rows are loaded with
-- COPY ... ON SEGMENT instead, which stores each segment's input where it
-- lands once gp_enable_segment_copy_checking is off.
--

select gp_debug_set_create_table_default_numsegments(2);
create table b(data int);
select gp_debug_reset_create_table_default_numsegments();

-- by default '345' should be inserted on seg0
insert into b values ('345');
select gp_segment_id, * from b;

truncate b;

-- however loaded ON SEGMENT without the check, '345' lands on both segments
-- (ON SEGMENT requires a <SEGID> placeholder in the command; it is unused here)
set gp_enable_segment_copy_checking = off;
copy b from program 'yes 345 | head -n 5 # <SEGID>' on segment;
reset gp_enable_segment_copy_checking;
select gp_segment_id, * from b;

-- data expansion should tolerant it
alter table b expand table;
select gp_segment_id, * from b;

-- start_ignore
-- We need to do a cluster expansion which will check if there are partial
-- tables, we need to drop the partial tables to keep the cluster expansion
-- run correctly.
reset search_path;
drop schema test_expand_table_regression cascade;
-- end_ignore
