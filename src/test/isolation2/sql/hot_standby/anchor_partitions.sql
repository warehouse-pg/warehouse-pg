-- Partition and inheritance membership under an anchor.
--
-- An anchored statement reads table data as of the anchor and the catalog
-- as of the replay position.  The planners expand a parent into its
-- children through the current pg_inherits rows and partition bounds, and
-- each child they reach passes the file check when its own file is
-- unchanged, so a partition attached, detached, exchanged or dropped after
-- the anchor, a changed bound, or an inheritance child removed would make
-- the parent read rows it did not have at the anchor, or miss rows it had,
-- without an error.  At executor start, a query that expands a parent
-- (names without ONLY a partitioned table, or a table with inheritance
-- children now or at the anchor) under an anchor therefore reads the
-- parent's pg_inherits rows and its children's bounds with the anchor's
-- snapshot, down through its sub-partitions, and fails (55000) unless the
-- current ones are the same; a plan cached before the anchor applied is
-- checked too.  Data changes in the children leave the parent readable at
-- the anchor, and a partition read by name, a parent read with ONLY, a
-- parent a regclass constant names and EXPLAIN are not refused.
--
-- Every case publishes a fresh anchor, changes the source, and reads the
-- parent on the standby coordinator with both planners; an unanchored
-- session reads the current state.  The barrier inserts work as in
-- anchor_conflict: their two-phase commit returns once every mirror has
-- applied everything before it.
--
-- DDL leaves dead catalog rows, and a prune of them replayed on the
-- standby would invalidate the anchor first (anchor_conflict), so every
-- statement below would be refused as not registered instead of reaching
-- the check.  vacuum_defer_cleanup_age on the primaries keeps those rows,
-- as an operator does for a replica.
--
-- Session roles: 1: primary QD; -1S: standby QD (dispatch); -1M: standby
-- QD utility.

!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpconfig -c whpg_hot_standby_snapshot_mode -v anchored --skipvalidation;
!\retcode gpconfig -c vacuum_defer_cleanup_age -v 1000000 --skipvalidation;
!\retcode gpstop -ar;
1: create table hs_part_t(a int, k int) partition by range (k) distributed by (a);
1: create table hs_part_t_1 partition of hs_part_t for values from (1) to (11);
1: create table hs_part_t_2 partition of hs_part_t for values from (11) to (21);
1: create table hs_part_t_3 partition of hs_part_t for values from (21) to (31);
1: insert into hs_part_t select i, 1 + i % 30 from generate_series(1, 300) i;
1: create table hs_part_stage(a int, k int) distributed by (a);
1: insert into hs_part_stage select i, 1 + i % 10 from generate_series(1001, 1050) i;
1: create table hs_part_extra(a int, k int) distributed by (a);
1: insert into hs_part_extra select i, 31 + i % 10 from generate_series(2001, 2020) i;
1: create table hs_inh_p(a int, k int) distributed by (a);
1: create table hs_inh_c() inherits (hs_inh_p) distributed by (a);
1: insert into hs_inh_p select i, i from generate_series(1, 10) i;
1: insert into hs_inh_c select i, i from generate_series(11, 30) i;
1: create table hs_part_ml(a int, k int, j int) partition by range (k) distributed by (a);
1: create table hs_part_ml_m partition of hs_part_ml for values from (1) to (11) partition by list (j);
1: create table hs_part_ml_l1 partition of hs_part_ml_m for values in (1);
1: create table hs_part_ml_l2 partition of hs_part_ml_m for values in (2);
1: create table hs_part_ml_c partition of hs_part_ml for values from (11) to (21);
1: insert into hs_part_ml select i, 1 + i % 20, 1 + i % 2 from generate_series(1, 200) i;
1: create table hs_part_r(a int, k int) partition by range (k) distributed by (a);
1: create table hs_part_r_1 partition of hs_part_r for values from (1) to (11);
1: create table hs_part_r_2 partition of hs_part_r for values from (11) to (21);
1: insert into hs_part_r select i, 1 + i % 20 from generate_series(1, 100) i;
1: create table hs_inh_q(a int, k int) distributed by (a);
1: create table hs_inh_q_c() inherits (hs_inh_q) distributed by (a);
1: insert into hs_inh_q select i, i from generate_series(1, 10) i;
1: insert into hs_inh_q_c select i, i from generate_series(11, 30) i;
1: create table hs_part_free(a int, k int) distributed by (a);
1: insert into hs_part_free select i, 1 + i % 10 from generate_series(1, 40) i;
1: create function hs_part_srf() returns setof hs_part_r language sql stable as 'select * from hs_part_r';
1: create view hs_part_v as select * from hs_part_r;
1: create table hs_inh_t(a int, k int) distributed by (a);
1: create table hs_inh_t_c() inherits (hs_inh_t) distributed by (a);
1: insert into hs_inh_t select i, 11 + i % 10 from generate_series(1, 10) i;
1: insert into hs_inh_t_c select i, i from generate_series(11, 30) i;
1: create table hs_part_idx(a int, b int) distributed by (a);
1: insert into hs_part_idx select i, i % 10 from generate_series(1, 300) i;
1: create table hs_part_barrier(a int) distributed by (a);
-- a running-xacts record on every node, so that restore points export
1: checkpoint;
1: insert into hs_part_barrier select generate_series(1, 30);

----------------------------------------------------------------
-- Unchanged membership: the parent reads the anchor's rows, a data
-- change in a child included
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a0');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a0 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: insert into hs_part_t_1 select i, 5 from generate_series(3001, 3010) i;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*), sum(a) from hs_part_t;
-1S: select count(*) from hs_part_t where k < 11;
-1S: select count(*) from hs_inh_p;
-1S: set optimizer = on;
-1S: select count(*), sum(a) from hs_part_t;
-1S: select count(*) from hs_part_t where k < 11;
-1S: select count(*) from hs_inh_p;

----------------------------------------------------------------
-- EXCHANGE PARTITION: as many partitions, another relation in place of one
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a1');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a1 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: alter table hs_part_t exchange partition for (5) with table hs_part_stage;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set optimizer = on;
-1S: select count(*), sum(a) from hs_part_t;
-- a partition the change did not touch, read by name: not a parent
-1S: select count(*) from hs_part_t_2;
-- a plan built unanchored after the change and cached: its execution under
-- the anchor is refused (the executor opens only the partitions)
-1S: set optimizer = off;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: prepare hs_part_q as select count(*), sum(a) from hs_part_t;
-1S: execute hs_part_q;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: execute hs_part_q;
-1S: deallocate hs_part_q;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- DETACH PARTITION
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a2');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a2 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*), sum(a) from hs_part_t;
1: alter table hs_part_t detach partition hs_part_t_2;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: select count(*), sum(a) from hs_part_t;
-1S: set optimizer = on;
-1S: select count(*), sum(a) from hs_part_t;
-- the detached table itself is read at the anchor
-1S: select count(*) from hs_part_t_2;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- ATTACH PARTITION of a table that existed at the anchor
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a3');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a3 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: alter table hs_part_t attach partition hs_part_extra for values from (31) to (41);
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set optimizer = on;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- DROP of a partition
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a4');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a4 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: drop table hs_part_extra;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set optimizer = on;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*), sum(a) from hs_part_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- The same partitions, a bound narrowed (detached, rows deleted, attached
-- again): pruning by the current bound would skip rows the anchor's bound
-- held
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a5');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a5 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: alter table hs_part_t detach partition hs_part_t_3;
1: delete from hs_part_t_3 where k >= 26;
1: alter table hs_part_t attach partition hs_part_t_3 for values from (21) to (26);
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*) from hs_part_t where k >= 26;
-1S: set optimizer = on;
-1S: select count(*) from hs_part_t where k >= 26;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_part_t where k >= 26;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- Traditional inheritance: a child removed with NO INHERIT
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a6');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a6 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: alter table hs_inh_c no inherit hs_inh_p;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*) from hs_inh_p;
-1S: set optimizer = on;
-1S: select count(*) from hs_inh_p;
-- ANALYZE clears relhassubclass, so the planners no longer expand the
-- parent: it had a child at the anchor all the same
1: analyze hs_inh_p;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*) from hs_inh_p;
-1S: set optimizer = on;
-1S: select count(*) from hs_inh_p;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_inh_p;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- A multi-level tree: a leaf detached from the middle level, the root's
-- own partitions unchanged; and a partitioned table created after the
-- anchor.  GPORCA falls back to the Postgres planner on a multi-level
-- tree, so its run checks the same plan.
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a7');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a7 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: alter table hs_part_ml_m detach partition hs_part_ml_l2;
1: create table hs_part_new(a int, k int) partition by range (k) distributed by (a);
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*) from hs_part_ml;
-1S: set optimizer = on;
-1S: select count(*) from hs_part_ml;
-1S: select count(*) from hs_part_new;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_part_ml;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- What the check leaves alone, after a partition detached, an inheritance
-- child removed and a table attached to a partitioned table created after
-- the anchor: a regclass constant naming the parent, the parent read with
-- ONLY, the attached table read by name (its open reads the new parent for
-- its partition constraint) and EXPLAIN without ANALYZE.  Reading the
-- parents themselves is refused, the new one as not existing at the
-- anchor, and so is reading a parent through a view or a SQL function (the
-- Postgres planner inlines the function's query while planning).
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a10');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a10 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: alter table hs_part_r detach partition hs_part_r_2;
1: alter table hs_inh_q_c no inherit hs_inh_q;
1: create table hs_part_np(a int, k int) partition by range (k) distributed by (a);
1: alter table hs_part_np attach partition hs_part_free for values from (1) to (11);
1: alter table hs_inh_t_c no inherit hs_inh_t;
1: alter table hs_part_np attach partition hs_inh_t for values from (11) to (21);
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: select count(*) from hs_part_idx where 'hs_part_r'::regclass <> 0;
-1S: select count(*) from only hs_inh_q;
-1S: select count(*) from hs_part_free;
-1S: explain (costs off) select count(*) from hs_part_r;
-1S: select count(*) from hs_part_r;
-1S: select count(*) from hs_inh_q;
-1S: select count(*) from hs_part_np;
-1S: select count(*) from hs_part_srf();
-1S: select count(*) from hs_part_v;
-1S: select count(*) from hs_inh_t;
-1S: set optimizer = on;
-1S: select count(*) from hs_part_srf();
-1S: select count(*) from hs_part_v;
-1S: select count(*) from hs_part_idx where 'hs_part_r'::regclass <> 0;
-1S: select count(*) from only hs_inh_q;
-1S: select count(*) from hs_part_free;
-1S: select count(*) from hs_part_r;
-1S: select count(*) from hs_inh_q;
-1S: select count(*) from hs_part_np;
-1S: select count(*) from hs_inh_t;
-1S: set optimizer = off;

----------------------------------------------------------------
-- The standby coordinator behind its segments: a bound narrowed that the
-- standby segments have replayed and the standby coordinator has not.  The
-- coordinator's check finds the parent unchanged; run-time partition
-- pruning on a segment reads the segment's bounds, so the executors check
-- too.  Replay is held on the standby coordinator only, and the session
-- that changes the source does not wait for the standby to apply it (a
-- remote_apply wait would wait for the held coordinator); the standby
-- segments are polled for the new bound instead.
----------------------------------------------------------------
1: create table hs_part_skew(a int, k int) partition by range (k) distributed by (a);
1: create table hs_part_skew_1 partition of hs_part_skew for values from (1) to (11);
1: create table hs_part_skew_2 partition of hs_part_skew for values from (11) to (21);
1: insert into hs_part_skew select i, 1 + i % 20 from generate_series(1, 200) i;
1: create table hs_part_skew_keys(k int) distributed by (k);
1: insert into hs_part_skew_keys values (17);
1: analyze hs_part_skew;
1: analyze hs_part_skew_keys;
1: select count(*) from gp_create_restore_point('hs_part_a9');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a9 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = off;
-1S: set plan_cache_mode = force_generic_plan;
-1S: prepare hs_part_skew_q(int) as select count(*) from hs_part_skew where k = $1;
-1S: execute hs_part_skew_q(17);
-1S: select count(*) from hs_part_skew join hs_part_skew_keys using (k);
-1M: select pg_wal_replay_pause();
1: set synchronous_commit = local;
1: delete from hs_part_skew where k >= 15;
1: alter table hs_part_skew detach partition hs_part_skew_2;
1: alter table hs_part_skew attach partition hs_part_skew_2 for values from (11) to (15);
1: reset synchronous_commit;
!\retcode for p in $(psql -d postgres -Atc "select port from gp_segment_configuration where role = 'm' and content >= 0"); do n=0; until PGOPTIONS='-c gp_role=utility -c whpg_hot_standby_snapshot_mode=unanchored' psql -p $p -d isolation2-hot-standby -Atc "select pg_get_expr(relpartbound, oid) from pg_class where relname = 'hs_part_skew_2'" | grep -q '(15)'; do n=$((n+1)); [ $n -lt 600 ] || exit 1; sleep 0.1; done; done;
-- the standby coordinator still has the bound the anchor saw
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select pg_get_expr(relpartbound, oid) from pg_class where relname = 'hs_part_skew_2';
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: execute hs_part_skew_q(17);
-1S: select count(*) from hs_part_skew join hs_part_skew_keys using (k);
-1S: set optimizer = on;
-1S: select count(*) from hs_part_skew join hs_part_skew_keys using (k);
-1S: set optimizer = off;
-1S: deallocate hs_part_skew_q;
-1S: reset plan_cache_mode;
-1M: select pg_wal_replay_resume();
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: select count(*) from hs_part_skew where k = 17;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_part_skew where k = 17;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- Not a partition case: the existing refusal of an index created after
-- the anchor, with GPORCA (anchor_conflict covers the Postgres planner)
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_part_a8');
1: insert into hs_part_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_part_a8 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_part_barrier select generate_series(1, 30);
1: create index hs_part_idx_b on hs_part_idx(b);
1: insert into hs_part_barrier select generate_series(1, 30);
-1S: set optimizer = on;
-1S: set optimizer_enable_tablescan = off;
-1S: select count(*) from hs_part_idx where b = 7;
-1S: reset optimizer_enable_tablescan;
-1S: select count(*) from hs_part_idx;

----------------------------------------------------------------
-- Cleanup: no published anchor
----------------------------------------------------------------
-1S: reset optimizer;
1: drop table hs_part_t;
1: drop table hs_part_t_2;
1: drop table hs_part_stage;
1: drop table hs_inh_c;
1: drop table hs_inh_p;
1: drop table hs_part_idx;
1: drop table hs_part_ml;
1: drop table hs_part_ml_l2;
1: drop table hs_part_new;
1: drop function hs_part_srf();
1: drop view hs_part_v;
1: drop table hs_part_r;
1: drop table hs_part_r_2;
1: drop table hs_inh_q_c;
1: drop table hs_inh_q;
1: drop table hs_part_np;
1: drop table hs_inh_t_c;
1: drop table hs_part_skew;
1: drop table hs_part_skew_keys;
1: drop table hs_part_barrier;
1q:
-1Sq:
-1Mq:
!\retcode gpconfig -r whpg_hot_standby_snapshot_mode --skipvalidation;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpconfig -r vacuum_defer_cleanup_age --skipvalidation;
!\retcode gpstop -ar;
