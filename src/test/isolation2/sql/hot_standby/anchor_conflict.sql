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
-- the anchor never saw.  That commit cancels nobody: a reader that took its
-- anchored snapshot and then acquires a relation lock after the commit was
-- replayed re-checks its anchor at the lock and fails (55000), and a
-- session that merely shares the anchor's xmin is left alone.  Database
-- drops touch no anchor.
--
-- Invalidation is per node.  A VACUUM of a user table writes its cleanup
-- records on the segments (the coordinator's copy of the table is empty),
-- so the coordinator keeps the anchor and the next statement is refused by
-- a segment; a catalog VACUUM and a relation-file drop reach every node.
-- Statements that must fail on one segment are direct dispatches to a
-- single row.  The expected output cannot tell which node raised an error
-- (the regression diff strips the segment suffix), so each such case
-- greps the standby coordinator's log, where an error an executor raised
-- carries the suffix and one the coordinator raised does not, reading only
-- what was logged since this test started.
--
-- A VACUUM that only prunes assigns no xid and writes no commit record,
-- so synchronous_commit = remote_apply does not wait for its cleanup
-- records to be applied on the standby, and a one-row insert waits for one
-- mirror only.  Every VACUUM and every publication below is followed by an
-- insert of rows on every segment (hs_conf_barrier), whose two-phase commit
-- returns only once every mirror has applied everything before it, the
-- conflict resolution of the cleanup included.
--
-- Session roles: 1, 2: primary QD; -1S: standby QD (dispatch); -1M: standby
-- QD utility; 0M/1M: mirror utility.  Publication goes through gpconfig +
-- reload, as in anchor_import; a backend applies the pending reload before
-- the next command it reads.

-- anchors are exported only where the configuration sets anchored mode;
-- a stale anchor name and the anchors an aborted earlier run registered
-- are cleared by the restart, and its faults are reset
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpconfig -c whpg_hot_standby_snapshot_mode -v anchored --skipvalidation;
!\retcode gpstop -ar;
1: select gp_inject_fault('all', 'reset', dbid) from gp_segment_configuration where role = 'm';
-- where the log greps below start reading
!\retcode ok=0; for c in -1 0; do d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = $c and role = 'm'"); [ -n "$d" ] || ok=1; cat "$d"/log/*.csv | wc -c > /tmp/hs_conf_${PGPORT}_log_off_$c; done; [ "$ok" = 0 ];
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
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); PGOPTIONS='-c whpg_hot_standby_snapshot_mode=unanchored' psql -X -p "$sport" -d postgres -Atc "select coalesce(sum(confl_snapshot), 0) from gp_stat_database_conflicts" > /tmp/hs_conf_${PGPORT}_confl_before;
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
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_0 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'invalidated anchor snapshot for restore point ..hs_conf_a..: replayed cleanup of relation'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-- a new anchored statement: the coordinator installs the anchor, the
-- segment refuses it
-1S: select count(*) from hs_conf_t where a = 7;
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot ..hs_conf_a.. is not registered on this node  .seg0 '; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_conf_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-- the termination was counted as a snapshot conflict
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); b=$(cat /tmp/hs_conf_${PGPORT}_confl_before); ok=1; for i in $(seq 1 50); do n=$(PGOPTIONS='-c whpg_hot_standby_snapshot_mode=unanchored' psql -X -p "$sport" -d postgres -Atc "select coalesce(sum(confl_snapshot), 0) from gp_stat_database_conflicts"); if [ -n "$b" ] && [ "$n" -gt "$b" ]; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];

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
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'invalidated anchor snapshot for restore point ..hs_conf_b..: replayed cleanup of relation'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot ..hs_conf_b.. is not registered on this node",'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];

----------------------------------------------------------------
-- Invalidation precedes the standard resolution and the record: with the
-- startup process of one mirror suspended right after it invalidated the
-- anchor, the entry is already gone there while a REPEATABLE READ
-- reader's executor on that segment is still alive (the resolution has
-- not run yet); once replay resumes, the resolution terminates it
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_c');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_c --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_conf_t where a = 7;
1: select gp_inject_fault('anchor_snapshot_conflict_invalidated', 'suspend', dbid) from gp_segment_configuration where content = 0 and role = 'm';
-- the VACUUM returns at once (no xid, no commit record to wait for); the
-- suspended mirror is the one that has not applied it yet
1: delete from hs_conf_t where b % 4 = 1;
1: vacuum hs_conf_t;
1: select gp_wait_until_triggered_fault('anchor_snapshot_conflict_invalidated', 1, dbid) from gp_segment_configuration where content = 0 and role = 'm';
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
0M: select count(*) > 0 from pg_stat_activity where state = 'idle in transaction' and backend_xmin is not null and pid <> pg_backend_pid();
1: select gp_inject_fault('anchor_snapshot_conflict_invalidated', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: insert into hs_conf_barrier select generate_series(1, 30);
-- the resolution ran after the invalidation: the reader's executor is gone
-1S: select count(*) from hs_conf_t where a = 7;
-1S: rollback;
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
-- (had a catalog prune invalidated hs_conf_d instead, the log says so)
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; [ -n "$d" ] && ! cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'hs_conf_d..: replayed cleanup';
-1S: select count(*) from hs_conf_t where a = 7;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- a reader that took its anchored snapshot and then waits behind the
-- replayed AccessExclusiveLock (the coordinator plans the statement under
-- its snapshot and locks the table while inlining the STABLE function)
-- fails (55000) when it acquires the lock the replayed commit released,
-- instead of waking into the truncated file with a snapshot that describes
-- the old one.  The barrier insert from a second session waits until every
-- node has applied the lock record; the commit waits until the reader is
-- queued for the lock.
1: create function hs_conf_count() returns bigint language sql stable as 'select count(*) from hs_conf_t';
1: insert into hs_conf_barrier select generate_series(1, 30);
1: begin;
1: truncate hs_conf_t;
2: insert into hs_conf_barrier select generate_series(1, 30);
-1S&: select hs_conf_count();
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); n=0; for i in $(seq 1 100); do n=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select count(*) from pg_locks where locktype = 'relation' and not granted"); [ "$n" -ge 1 ] && break; sleep 0.1; done; [ "$n" -ge 1 ];
1: commit;
-1S<:
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot of this statement was invalidated by a replayed commit that dropped relation files",'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-1S: select count(*) from hs_conf_t where a = 7;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'invalidated anchor snapshot for restore point ..hs_conf_d..: transaction [0-9]* committed dropping'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-- the next anchor reads the truncated, refilled table
1: insert into hs_conf_t select i, i from generate_series(1, 100) i;
1: select count(*) from gp_create_restore_point('hs_conf_e');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_e --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;
-- the same wait inside a REPEATABLE READ transaction, whose first snapshot
-- pinned the anchor (on another table, so the transaction holds no lock on
-- hs_conf_t): the pinned-anchor error, the transaction must roll back
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_conf_ao where a = 7;
1: begin;
1: truncate hs_conf_t;
2: insert into hs_conf_barrier select generate_series(1, 30);
-1S&: select hs_conf_count();
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); n=0; for i in $(seq 1 100); do n=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select count(*) from pg_locks where locktype = 'relation' and not granted"); [ "$n" -ge 1 ] && break; sleep 0.1; done; [ "$n" -ge 1 ];
1: commit;
-1S<:
-1S: rollback;
1: insert into hs_conf_t select i, i from generate_series(1, 100) i;
1: select count(*) from gp_create_restore_point('hs_conf_e2');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_e2 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;

----------------------------------------------------------------
-- A file-dropping commit cancels nobody.  A transaction open on the
-- primary across the restore point gives the anchor and every later
-- snapshot on a segment the same xmin; an unanchored REPEATABLE READ
-- transaction whose executors hold that xmin between statements survives
-- the replayed TRUNCATE commit of another table, where a resolution by
-- xmin would have terminated them.  The anchor is gone all the same.
----------------------------------------------------------------
1: create table hs_conf_long(a int) distributed by (a);
1: create table hs_conf_drop(a int) distributed by (a);
1: insert into hs_conf_drop select generate_series(1, 30);
2: begin;
2: insert into hs_conf_long select generate_series(1, 30);
1: select count(*) from gp_create_restore_point('hs_conf_f');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_f --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_conf_t;
-- the precondition: on content 0 an executor of that transaction holds the
-- anchor's xmin
0M: select count(*) > 0 from pg_stat_activity s, gp_toolkit.whpg_anchor_snapshots() a where a.rp_name = 'hs_conf_f' and s.backend_xmin = a.xmin and s.pid <> pg_backend_pid();
1: truncate hs_conf_drop;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;
-1S: commit;
2: rollback;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_conf_t;

----------------------------------------------------------------
-- An executor that installed its anchor and takes its first relation lock
-- only after its segment replayed a file-dropping commit fails at the
-- lock; the error carries the segment suffix in the coordinator's log.
-- The executor is held between the two by a fault; the commit drops
-- another table's files, so no lock waits anywhere and the primary's
-- commit is not held up.  The coordinator took its locks before the
-- commit was replayed and is not involved.
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_k');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_k --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
1: select gp_inject_fault('anchor_snapshot_installed', 'suspend', dbid) from gp_segment_configuration where content = 0 and role = 'm';
-1S&: select count(*) from hs_conf_t where a = 7;
1: select gp_wait_until_triggered_fault('anchor_snapshot_installed', 1, dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: insert into hs_conf_drop select generate_series(1, 30);
1: truncate hs_conf_drop;
1: insert into hs_conf_barrier select generate_series(1, 30);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1: select gp_inject_fault('anchor_snapshot_installed', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'm';
-1S<:
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot of this statement was invalidated by a replayed commit that dropped relation files  .seg0 '; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- the published anchor is gone on every node; the next publication
-- restores service
1: select count(*) from gp_create_restore_point('hs_conf_l');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_l --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: select count(*) from hs_conf_t;

----------------------------------------------------------------
-- A publication retires the anchor a running statement reads with and
-- destroys nothing: the statement runs on, although it keeps taking new
-- locks (every toast value fetched opens the toast relation).  A cursor
-- over rows whose values live in the toast relation is held between two
-- fetches across the publication; its executors are blocked mid-scan on
-- the interconnect and resume afterwards.  In the same READ COMMITTED
-- transaction a LOCK takes no snapshot and is not checked, and the next
-- statement reads the new anchor.
----------------------------------------------------------------
1: create table hs_conf_toast(a int, v text) distributed by (a);
1: insert into hs_conf_toast select i, (select string_agg(md5(random()::text || j || i), '') from generate_series(1, 3000) j) from generate_series(1, 60) i;
1: select count(*) from gp_create_restore_point('hs_conf_m');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_m --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: begin;
-1S: declare hs_conf_cur cursor for select a, v from hs_conf_toast;
-1S: move 1 in hs_conf_cur;
-- the precondition: content 0's executor is still in the middle of its scan
0M: select count(*) > 0 from pg_stat_activity where query like '%hs_conf_toast%' and state = 'active' and pid <> pg_backend_pid();
1: select count(*) from gp_create_restore_point('hs_conf_n');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_n --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: move all in hs_conf_cur;
-1S: close hs_conf_cur;
-- a file-dropping commit now: LOCK has no snapshot and is not checked, the
-- next statement's import is refused
1: insert into hs_conf_drop select generate_series(1, 30);
1: truncate hs_conf_drop;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: lock table hs_conf_ao in access share mode;
-1S: select count(*) from hs_conf_toast;
-1S: rollback;

----------------------------------------------------------------
-- A file-dropping commit fails a running statement at the next new lock
-- it takes, here the next toast value its executors fetch after the
-- commit was replayed, although the dropped files belong to another table
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_conf_p');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_p --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: begin;
-1S: declare hs_conf_cur2 cursor for select a, v from hs_conf_toast;
-1S: move 1 in hs_conf_cur2;
0M: select count(*) > 0 from pg_stat_activity where query like '%hs_conf_toast%' and state = 'active' and pid <> pg_backend_pid();
1: insert into hs_conf_drop select generate_series(1, 30);
1: truncate hs_conf_drop;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1S: move all in hs_conf_cur2;
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot of this statement was invalidated by a replayed commit that dropped relation files  .seg'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-1S: rollback;
1: select count(*) from gp_create_restore_point('hs_conf_q');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_q --skipvalidation;
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
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_0 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'invalidated anchor snapshot for restore point ..hs_conf_h...*Heap2/CLEANUP_INFO'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-1S: select count(*) from hs_conf_ao where a = 7;
-1S: rollback;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select count(*) from hs_conf_ao where a = 7;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_conf_ao;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- An overflowed anchor is not kept by a start that has hot standby on but
-- does not register it (anchored mode off): that start zeroes pg_subtrans
-- from its checkpoint's oldest active xid on and its restartpoints truncate
-- pg_subtrans without regard to the anchor, so the start that would
-- register it later could read subtransactions without their parents.  A
-- transaction with more than 64 subtransactions, open across the restore
-- point, overflows the anchor (checked on the coordinator's file).
----------------------------------------------------------------
2: begin;
2: do $$ begin for i in 1..70 loop begin execute format('create table hs_conf_sub_%s(a int) distributed by (a)', i); exception when others then raise; end; end loop; end $$;
1: select count(*) from gp_create_restore_point('hs_conf_o');
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_conf_o --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_conf_barrier select generate_series(1, 30);
2: commit;
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_conf_o';
-- a checkpoint replayed past the record: the restarts below begin redo
-- after it, so nothing exports the anchor again
1: checkpoint;
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode grep -q '^sof:1$' "$COORDINATOR_DATA_DIRECTORY/../../standby/pg_anchor_snapshots/hs_conf_o";
-1Sq:
-1Mq:
!\retcode echo "whpg_hot_standby_snapshot_mode = unanchored" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot file for restore point ..hs_conf_o.. swept: it is overflowed'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
!\retcode test ! -e "$COORDINATOR_DATA_DIRECTORY/../../standby/pg_anchor_snapshots/hs_conf_o";
!\retcode sed -i '/^whpg_hot_standby_snapshot_mode = unanchored$/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_conf_o';
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_conf_t;

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
-- a checkpoint replayed past the record: every restart below begins redo
-- after it, so only the file can bring the anchor back
1: checkpoint;
1: insert into hs_conf_barrier select generate_series(1, 30);
-1Sq:
-1Mq:
-- A start whose configuration leaves anchored mode off keeps the published
-- anchor's file the same way, and the next start with the line registers
-- it again from the file (hence the wait)
!\retcode echo "whpg_hot_standby_snapshot_mode = unanchored" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot file for restore point ..hs_conf_i.. kept but not registered: anchored mode is off on this node'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_conf_i';
!\retcode test -e "$COORDINATOR_DATA_DIRECTORY/../../standby/pg_anchor_snapshots/hs_conf_i";
-1Mq:
!\retcode sed -i '/^whpg_hot_standby_snapshot_mode = unanchored$/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); for i in $(seq 1 600); do n=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_conf_i'"); [ "$n" = 1 ] && break; sleep 0.1; done; [ "$n" = 1 ];
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot ..hs_conf_i.. at [0-9A-F/]*; registered from its file\|re-registered anchor snapshot for restore point ..hs_conf_i..'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
-- A start with hot standby disabled keeps it too; here the cleanup that
-- replays meanwhile removes the kept file
!\retcode echo "hot_standby = off" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'anchor snapshot file for restore point ..hs_conf_i.. kept but not registered: hot standby is disabled'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
1: alter table hs_conf_churn2 rename to hs_conf_churn3;
1: vacuum pg_class;
1: insert into hs_conf_barrier select generate_series(1, 30);
-- past the cleanup too: without the removal, the start below would
-- register the file again
1: checkpoint;
1: insert into hs_conf_barrier select generate_series(1, 30);
!\retcode d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); o=$(cat /tmp/hs_conf_${PGPORT}_log_off_-1 2>/dev/null); [ -n "$o" ] || o=999999999999; ok=1; for i in $(seq 1 50); do if cat "$d"/log/*.csv | tail -c +$((o + 1)) | grep -q 'kept anchor snapshot file for restore point ..hs_conf_i.. removed: replayed cleanup of relation'; then ok=0; break; fi; sleep 0.1; done; [ "$ok" = 0 ];
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
1: drop table hs_conf_long;
1: drop table hs_conf_drop;
1: drop table hs_conf_toast;
1: do $$ begin for i in 1..70 loop execute format('drop table hs_conf_sub_%s', i); end loop; end $$;
1: drop table hs_conf_barrier;
1q:
2q:
-1Sq:
-1Mq:
0Mq:
1Mq:
!\retcode gpconfig -r whpg_hot_standby_snapshot_mode --skipvalidation;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpstop -ar;
