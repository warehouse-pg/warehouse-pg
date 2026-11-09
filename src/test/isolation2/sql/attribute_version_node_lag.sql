-- A node can see a committed change of a row type later than another node:
-- invalidations are only processed when a lock is taken.  Here a segment
-- starts its part of a query after ALTER TYPE ... ADD ATTRIBUTE has
-- committed and forms composite values with the new attribute, while the
-- coordinator, which planned the query before, has taken no lock since and
-- would still use the shorter definition.  It catches up when it receives
-- the segment's rows.
create type avl_t as (a int, b text);
create table avl_src (id int, b text) distributed by (id);
insert into avl_src select g, 'r' || g from generate_series(1, 6) g;

-- session 1 caches the type on the coordinator
1: begin;
1: select row(1, 'x')::avl_t;

-- its next query is held on content 0 before the segment looks at the type
2: select gp_inject_fault('exec_mpp_query_start', 'suspend', dbid) from gp_segment_configuration where content = 0 and role = 'p';
1&: select id, row(id, b)::avl_t from avl_src order by id;
2: select gp_wait_until_triggered_fault('exec_mpp_query_start', 1, dbid) from gp_segment_configuration where content = 0 and role = 'p';

-- the type gets another attribute, then the segment continues
2: alter type avl_t add attribute z int;
2: select gp_inject_fault('exec_mpp_query_start', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'p';
1<:
1: commit;

-- The same when the coordinator reads a field of the value.  The executor
-- looks the type up before it looks at the value, and does not pin the
-- descriptor it gets.
alter type avl_t drop attribute z;
1: begin;
1: select row(1, 'x')::avl_t;
2: select gp_inject_fault('exec_mpp_query_start', 'suspend', dbid) from gp_segment_configuration where content = 0 and role = 'p';
1&: select id, (r).a, (r).b from (select id, row(id, b)::avl_t as r from avl_src order by id limit 10) s order by id;
2: select gp_wait_until_triggered_fault('exec_mpp_query_start', 1, dbid) from gp_segment_configuration where content = 0 and role = 'p';
2: alter type avl_t add attribute z int;
2: select gp_inject_fault('exec_mpp_query_start', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'p';
1<:
1: commit;

-- The same with a value that the coordinator copies and relabels: the cast
-- from a child table's row type to its parent's needs no conversion, so the
-- value a segment formed after ADD COLUMN on the parent is copied with the
-- child's and labelled with the parent's definition.
create table avl_parent (a int, b text) distributed by (a);
create table avl_child () inherits (avl_parent);

1: begin;
1: select row(1, 'x')::avl_child::avl_parent;
2: select gp_inject_fault('exec_mpp_query_start', 'suspend', dbid) from gp_segment_configuration where content = 0 and role = 'p';
1&: select id, r::avl_parent from (select id, row(id, b)::avl_child as r from avl_src order by id limit 10) s order by id;
2: select gp_wait_until_triggered_fault('exec_mpp_query_start', 1, dbid) from gp_segment_configuration where content = 0 and role = 'p';
2: alter table avl_parent add column z int;
2: select gp_inject_fault('exec_mpp_query_start', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'p';
1<:
1: commit;

drop table avl_child;
drop table avl_parent;

drop table avl_src;
drop type avl_t;
