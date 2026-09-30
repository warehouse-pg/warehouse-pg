-- Conflict linkage: replayed cleanup invalidates anchor snapshots ahead of
-- the standard recovery-conflict resolution.
--
-- An anchored import gives a session an xmin older than the replay position,
-- which ordinary hot standby never does.  Replay keeps right of way: when it
-- meets a record that removes or hides row versions up to some xid (a heap
-- cleanup, prune, freeze or all-visible record, an index vacuum delete or
-- page reuse), the startup process first invalidates every registered
-- anchor whose xmin is at or below that xid -- entry and file, the published
-- anchor included, no grace -- then the standard delay-then-cancel cancels
-- the readers that hold such an xmin, then the record is applied.  A new
-- import of the anchor fails (SQLSTATE 55000) from the invalidation on.  A
-- commit that drops relation files (TRUNCATE, a rewrite, DROP, REINDEX)
-- invalidates every registered anchor as well: the catalog is read at the
-- replay position, so an anchored read would follow the relation to a file
-- the anchor never saw.  Database drops touch no anchor.
--
-- Invalidation is per node.  A VACUUM of a user table writes its cleanup
-- records on the segments (the coordinator's copy of the table is empty),
-- so the coordinator keeps the anchor and the next statement is refused by
-- a segment, with the segment suffix; a catalog VACUUM and a relation-file
-- drop reach every node.  Statements that must fail on one segment are
-- direct dispatches to a single row, so that the suffix is deterministic.
--
-- A VACUUM that only prunes assigns no xid and writes no commit record,
-- so synchronous_commit = remote_apply does not wait for its cleanup
-- records to be applied on the standby, and a one-row insert waits for one
-- mirror only.  Every VACUUM and every publication below is followed by an
-- insert of rows on every segment (hs_conf_barrier), whose two-phase commit
-- returns only once every mirror has applied everything before it, the
-- conflict resolution of the cleanup included.
--
-- Session roles: 1: primary QD; -1S: standby QD (dispatch); -1M: standby QD
-- utility; 0M/1M: mirror utility.  Publication goes through gpconfig +
-- reload, as in anchor_import; a backend applies the pending reload before
-- the next command it reads.

-- start_matchsubs
-- m/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/
-- s/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/(segN IP:PORT pid=PID)/
-- end_matchsubs

1: create table hs_conf_t(a int, b int) distributed by (a);
1: insert into hs_conf_t select i, i from generate_series(1, 300) i;
1: create table hs_conf_ao(a int, b int) with (appendonly = true) distributed by (a);
1: insert into hs_conf_ao select i, i from generate_series(1, 300) i;
1: create table hs_conf_churn(a int) distributed by (a);
-- the barrier table: an insert of rows on every segment commits with a
-- two-phase commit whose remote_apply wait covers every mirror
1: create table hs_conf_barrier(a int) distributed by (a);
-- a running-xacts record on every node, so that restore points export
1: checkpoint;
1: insert into hs_conf_barrier select generate_series(1, 30);
-- the segment of the direct dispatches below
1: select gp_segment_id from hs_conf_t where a = 7;

----------------------------------------------------------------
-- Backlogged cleanup on the segments: a VACUUM after the restore point
-- invalidates the published anchor on every segment, the transaction that
-- was reading it is cancelled by the standard resolution, a new anchored
-- statement is refused by the segment, and the coordinator still lists
-- the anchor
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_a');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_a --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_conf_t where a = 7;
1: delete from hs_conf_t where b % 2 = 0;
1: vacuum hs_conf_t;
1: insert into hs_conf_barrier select generate_series(1, 30);
-- the reader's executor held the anchor's xmin and was terminated by the
-- standard resolution while it waited for the next statement
-1S: select count(*) from hs_conf_t where a = 7;
-1S: rollback;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode dir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); grep -q 'invalidated anchor snapshot for restore point ..hs_conf_a..: replayed cleanup of relation' "$dir"/log/*.csv;
-- a new anchored statement: the coordinator installs the anchor, the
-- segment refuses it
-1S: select count(*) from hs_conf_t where a = 7;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_conf_t;
-- the termination was counted as a snapshot conflict
-1S: select max(confl_snapshot) > 0 from gp_stat_database_conflicts where datname = current_database();
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- A catalog VACUUM reaches every node: the coordinator invalidates its
-- anchor too and refuses the statement itself
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_b');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_b --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t where a = 7;
-- a catalog row version dies on every node, and the VACUUM removes it
1: alter table hs_conf_churn rename to hs_conf_churn2;
1: vacuum pg_class;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select count(*) from hs_conf_t where a = 7;
!\retcode grep -q 'invalidated anchor snapshot for restore point ..hs_conf_b..: replayed cleanup of relation' "$COORDINATOR_DATA_DIRECTORY"/../../standby/log/*.csv;

----------------------------------------------------------------
-- Invalidation precedes the record: with the startup process of one
-- mirror suspended right after it invalidated the anchor, before the
-- cleanup record is applied, a new import already fails on that segment
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_c');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_c --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t where a = 7;
1: select gp_inject_fault('anchor_snapshot_conflict_invalidated', 'suspend', dbid) from gp_segment_configuration where content = 0 and role = 'm';
-- the VACUUM returns at once (no xid, no commit record to wait for); the
-- suspended mirror is the one that has not applied it yet
1: delete from hs_conf_t where b % 4 = 1;
1: vacuum hs_conf_t;
1: select gp_wait_until_triggered_fault('anchor_snapshot_conflict_invalidated', 1, dbid) from gp_segment_configuration where content = 0 and role = 'm';
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select count(*) from hs_conf_t where a = 7;
1: select gp_inject_fault('anchor_snapshot_conflict_invalidated', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_conf_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- A commit that drops relation files invalidates every anchor on every
-- node; a temporary table's files do not count
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_d');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_d --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t where a = 7;
1: create temp table hs_conf_tmp(a int) distributed by (a);
1: insert into hs_conf_tmp values (1);
1: drop table hs_conf_tmp;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t where a = 7;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- a reader that took its anchored snapshot and then waits behind the
-- replayed AccessExclusiveLock (a STABLE function locks the table only when
-- it first runs, after the statement's snapshot) is cancelled when the
-- commit is replayed, before the lock is released: it must not wake into
-- the truncated file with a snapshot that describes the old one.  The
-- barrier insert from a second session waits until every node has applied
-- the lock record.
1: create function hs_conf_count() returns bigint language sql stable as 'select count(*) from hs_conf_t';
1: insert into hs_conf_barrier select generate_series(1, 30);
1: begin;
1: truncate hs_conf_t;
2: insert into hs_conf_barrier select generate_series(1, 30);
-1S&: select hs_conf_count();
1: commit;
-1S<:
-1S: select count(*) from hs_conf_t where a = 7;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode grep -q 'invalidated anchor snapshot for restore point ..hs_conf_d..: transaction [0-9]* committed dropping' "$COORDINATOR_DATA_DIRECTORY"/../../standby/log/*.csv;
-- the next anchor reads the truncated, refilled table
1: insert into hs_conf_t select i, i from generate_series(1, 100) i;
1: select count(*) from gp_create_restore_point('hs_conf_e');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_e --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;

----------------------------------------------------------------
-- A database drop touches no anchor
----------------------------------------------------------------
1: create database hs_conf_db;
1: drop database hs_conf_db;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select count(*) from hs_conf_t;

----------------------------------------------------------------
-- VACUUM FREEZE after the restore point: the freeze and all-visible
-- records of rows the anchor does not see invalidate it on the segments
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_g');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_g --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t where a = 7;
1: insert into hs_conf_t select i, i from generate_series(101, 200) i;
1: vacuum freeze hs_conf_t;
1: insert into hs_conf_barrier select generate_series(1, 30);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select count(*) from hs_conf_t where a = 7;

----------------------------------------------------------------
-- An append-optimized table: the cleanup-info record its VACUUM writes
-- before recycling a segment file cancels the anchored reader and
-- invalidates the anchor, instead of the reader finding unrelated rows
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_h');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_h --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_conf_ao where a = 7;
1: delete from hs_conf_ao where b % 2 = 0;
1: vacuum hs_conf_ao;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_ao where a = 7;
-1S: rollback;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select count(*) from hs_conf_ao where a = 7;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_conf_ao;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- A start that cannot register the published anchor (hot standby
-- disabled) keeps its file; cleanup replayed meanwhile removes the file,
-- so the start that re-enables hot standby does not register an anchor
-- whose rows are gone
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_i');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_i --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;
-1Sq:
-1Mq:
!\retcode echo "hot_standby = off" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
!\retcode grep -q 'anchor snapshot file for restore point ..hs_conf_i.. kept but not registered: hot standby is disabled' "$COORDINATOR_DATA_DIRECTORY"/../../standby/log/*.csv;
1: alter table hs_conf_churn2 rename to hs_conf_churn3;
1: vacuum pg_class;
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode grep -q 'kept anchor snapshot file for restore point ..hs_conf_i.. removed: replayed cleanup of relation' "$COORDINATOR_DATA_DIRECTORY"/../../standby/log/*.csv;
!\retcode test ! -e "$COORDINATOR_DATA_DIRECTORY/../../standby/pg_anchor_snapshots/hs_conf_i";
!\retcode sed -i '/^hot_standby = off$/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_conf_i';
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_conf_t;
-- the next publication restores service
1: select count(*) from gp_create_restore_point('hs_conf_j');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_j --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;

----------------------------------------------------------------
-- Cleanup: no published anchor
----------------------------------------------------------------
1: drop function hs_conf_count();
1: drop table hs_conf_t;
1: drop table hs_conf_ao;
1: drop table hs_conf_churn3;
1: drop table hs_conf_barrier;
1q:
2q:
-1Sq:
-1Mq:
0Mq:
1Mq:
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpstop -ar;
