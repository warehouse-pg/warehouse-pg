-- Anchor snapshots and distributed transactions committed on the
-- coordinator but not yet on the segments.
--
-- The coordinator writes its distributed commit record before it
-- broadcasts COMMIT PREPARED, and gp_create_restore_point's two-phase
-- commit lock blocks only the broadcast.  A restore point can land between
-- the two: the segments' anchors then hold the transaction as in progress
-- (prepared), and the coordinator's anchor must too, or an anchored read
-- would see its rows in coordinator-only tables but not its rows on the
-- segments.  The primary shows neither until COMMIT PREPARED.  The
-- transaction is held in that window with the fault
-- dtm_broadcast_commit_prepared (before the broadcast takes the lock).
--
-- When the standby coordinator did not replay the commit record itself (it
-- started from a restartpoint past it, and took the gxid from the
-- checkpoint), it cannot tell the transaction's local xids: it exports no
-- anchor for such a restore point and says why.  After the transaction's
-- FORGET it still reads as in progress there (that start's
-- latestCompletedXid predates it), so the next restore points export only
-- once the coordinator has replayed a later local commit.  A checkpoint
-- that lists a transaction whose FORGET lies before its redo point leaves
-- that gxid in the committed array for good after such a start; a later
-- checkpoint that no longer lists it releases the exports.
--
-- The WARNING checks read only the standby log lines written after a
-- mark taken just before the restore point, so that a line left by an
-- earlier run with the same restore point name cannot satisfy them.
--
-- Session roles: 1: primary QD; 2: primary QD (the held transaction);
-- 3: primary QD (a transaction whose xid falls between the held one's);
-- -1S: standby QD (dispatch); -1M: standby QD utility; 0M: mirror utility.

-- start_matchsubs
-- m/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/
-- s/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/(segN IP:PORT pid=PID)/
-- end_matchsubs

!\retcode gpconfig -c whpg_hot_standby_snapshot_mode -v anchored --skipvalidation;
!\retcode gpstop -u;
1: create table hs_co_h(a int) distributed coordinator only;
1: create table hs_co_ao(a int) with (appendonly = true) distributed coordinator only;
1: create table hs_co_d(a int) distributed by (a);
-- a running-xacts record on every node, so that restore points export
1: checkpoint;
1: insert into hs_co_d select generate_series(1, 30);

----------------------------------------------------------------
-- A restore point inside the window: the anchor holds the transaction as
-- in progress on the coordinator (heap and append-optimized rows, and a
-- subtransaction's row) as on the segments
----------------------------------------------------------------
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'suspend', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2: begin;
2: insert into hs_co_h values (1);
2: insert into hs_co_ao values (1);
2: savepoint s;
2: insert into hs_co_h values (2);
2: release savepoint s;
2: insert into hs_co_d select generate_series(101, 130);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, dbid) from gp_segment_configuration where content = -1 and role = 'p';
-- the primary shows none of it yet
1: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_ao) ao, (select count(*) from hs_co_d where a > 100) d;
-- the restore point is created in the shell, which then waits until the
-- standby coordinator and every mirror have replayed it
!\retcode rows=$(psql -d postgres -Atc "select gp_segment_id, restore_lsn from gp_create_restore_point('hs_co_a')"); for c in -1 0 1 2; do lsn=$(echo "$rows" | awk -F'|' -v c=$c '$1==c {print $2}'); host=$(psql -d postgres -Atc "select hostname from gp_segment_configuration where content = $c and role = 'm'"); port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = $c and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -h "$host" -p "$port" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ] || exit 1; done;
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'resume', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2<:
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', dbid) from gp_segment_configuration where content = -1 and role = 'p';
1: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_ao) ao, (select count(*) from hs_co_d where a > 100) d;
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_co_a --skipvalidation;
!\retcode gpstop -u;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_ao) ao, (select count(*) from hs_co_d where a > 100) d;
-1S: select count(*) from hs_co_d;

-- the next restore point shows the whole transaction
1: select count(*) from gp_create_restore_point('hs_co_b');
1: insert into hs_co_d select generate_series(201, 210);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_co_b --skipvalidation;
!\retcode gpstop -u;
-1S: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_ao) ao, (select count(*) from hs_co_d where a > 100 and a < 200) d;

----------------------------------------------------------------
-- The standby coordinator restarted past the commit record: it has the
-- gxid from the checkpoint but not its local xids, so it exports no anchor
-- for a restore point inside the window, while the mirrors do; after the
-- FORGET it exports none until a later local commit is replayed.  The
-- held transaction's subtransaction takes an xid above another
-- transaction's, which commits first: the horizon must cover the
-- subtransaction, not only the FORGET record's top xid
----------------------------------------------------------------
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'suspend', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2: begin;
2: insert into hs_co_h values (3);
3: begin;
3: insert into hs_co_h values (31);
2: savepoint s;
2: insert into hs_co_h values (32);
2: release savepoint s;
2: insert into hs_co_d select generate_series(301, 330);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, dbid) from gp_segment_configuration where content = -1 and role = 'p';
-- a checkpoint that lists the held transaction; wait until the standby
-- coordinator has replayed it
1: checkpoint;
!\retcode clsn=$(psql -d postgres -Atc "select pg_current_wal_lsn()"); sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$clsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ];
-- a restartpoint there, so that the restart's redo starts past the commit record
-1M: checkpoint;
-1Mq:
-1Sq:
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; cat "$sdir"/log/*.csv | wc -l > "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
!\retcode rows=$(psql -d postgres -Atc "select gp_segment_id, restore_lsn from gp_create_restore_point('hs_co_c')"); for c in -1 0 1 2; do lsn=$(echo "$rows" | awk -F'|' -v c=$c '$1==c {print $2}'); host=$(psql -d postgres -Atc "select hostname from gp_segment_configuration where content = $c and role = 'm'"); port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = $c and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -h "$host" -p "$port" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ] || exit 1; done;
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_c';
0M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_c';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_co_c" && cat "$sdir"/log/*.csv | tail -n +$(( $(cat "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines") + 1 )) | grep -q 'anchor snapshot for restore point ..hs_co_c.. not exported: distributed transaction [0-9]* committed on the coordinator before the restore point and is not known to be committed on every segment: its commit record lies before this start.s redo point';
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'resume', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2<:
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', dbid) from gp_segment_configuration where content = -1 and role = 'p';
-- the held transaction has committed everywhere, but the standby
-- coordinator still counts it as in progress: no export, also once the
-- in-between transaction's commit has moved xmax past the top xid
3: commit;
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; cat "$sdir"/log/*.csv | wc -l > "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
1: select count(*) from gp_create_restore_point('hs_co_e');
1: insert into hs_co_d select generate_series(401, 410);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_e';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_co_e" && cat "$sdir"/log/*.csv | tail -n +$(( $(cat "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines") + 1 )) | grep -q 'anchor snapshot for restore point ..hs_co_e.. not exported: transaction [0-9]* had committed when this standby.s snapshot was initialized';
-- a later local commit on the coordinator; the next restore point exports
-- and shows every row
1: insert into hs_co_h values (4);
1: select count(*) from gp_create_restore_point('hs_co_f');
1: insert into hs_co_d select generate_series(501, 510);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_f';
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_co_f --skipvalidation;
!\retcode gpstop -u;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_d where a > 300 and a < 400) d;

----------------------------------------------------------------
-- A checkpoint collected while a transaction has written its FORGET but
-- not yet cleared includeInCkpt lists it although the FORGET precedes the
-- checkpoint's redo point.  A start from that checkpoint keeps the gxid in
-- the committed array for good: a later local commit does not release the
-- exports, a later checkpoint that no longer lists it does
----------------------------------------------------------------
1: select gp_inject_fault('dtm_after_insert_forget_committed', 'suspend', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2: begin;
2: insert into hs_co_h values (5);
2: insert into hs_co_d select generate_series(601, 630);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_after_insert_forget_committed', 1, dbid) from gp_segment_configuration where content = -1 and role = 'p';
1: checkpoint;
!\retcode clsn=$(psql -d postgres -Atc "select pg_current_wal_lsn()"); sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$clsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ];
-1M: checkpoint;
-1Mq:
-1Sq:
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
1: select gp_inject_fault('dtm_after_insert_forget_committed', 'resume', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2<:
1: select gp_inject_fault('dtm_after_insert_forget_committed', 'reset', dbid) from gp_segment_configuration where content = -1 and role = 'p';
-- a later local commit is not enough
1: insert into hs_co_h values (6);
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; cat "$sdir"/log/*.csv | wc -l > "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
1: select count(*) from gp_create_restore_point('hs_co_g');
1: insert into hs_co_d select generate_series(701, 710);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_g';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_co_g" && cat "$sdir"/log/*.csv | tail -n +$(( $(cat "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines") + 1 )) | grep -q 'anchor snapshot for restore point ..hs_co_g.. not exported: distributed transaction [0-9]* committed on the coordinator before the restore point';
-- a checkpoint that no longer lists it, then a local commit past the
-- horizon: the next restore point exports and shows every row
1: checkpoint;
1: insert into hs_co_h values (7);
1: select count(*) from gp_create_restore_point('hs_co_h');
1: insert into hs_co_d select generate_series(801, 810);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_h';
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_co_h --skipvalidation;
!\retcode gpstop -u;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_d where a > 600 and a < 700) d;

----------------------------------------------------------------
-- A transaction that has written its FORGET and cleared includeInCkpt but
-- not yet left the proc array (with remote_apply it waits for the standby
-- there) is not in a checkpoint's list but is in its running-xacts record.
-- A start from that checkpoint reads it as in progress, although it has
-- committed everywhere, until a later local commit; no export meanwhile
----------------------------------------------------------------
1: select gp_inject_fault('dtm_after_include_in_ckpt_cleared', 'suspend', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2: begin;
2: insert into hs_co_h values (8);
2: insert into hs_co_d select generate_series(901, 930);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_after_include_in_ckpt_cleared', 1, dbid) from gp_segment_configuration where content = -1 and role = 'p';
1: checkpoint;
!\retcode clsn=$(psql -d postgres -Atc "select pg_current_wal_lsn()"); sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$clsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ];
-1M: checkpoint;
-1Mq:
-1Sq:
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
1: select gp_inject_fault('dtm_after_include_in_ckpt_cleared', 'resume', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2<:
1: select gp_inject_fault('dtm_after_include_in_ckpt_cleared', 'reset', dbid) from gp_segment_configuration where content = -1 and role = 'p';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; cat "$sdir"/log/*.csv | wc -l > "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
1: select count(*) from gp_create_restore_point('hs_co_i');
1: insert into hs_co_d select generate_series(1001, 1010);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_i';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_co_i" && cat "$sdir"/log/*.csv | tail -n +$(( $(cat "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines") + 1 )) | grep -q 'anchor snapshot for restore point ..hs_co_i.. not exported: transaction [0-9]* had committed when this standby.s snapshot was initialized'; rm -f "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
-- a later local commit; the next restore point exports and shows every row
1: insert into hs_co_h values (9);
1: select count(*) from gp_create_restore_point('hs_co_j');
1: insert into hs_co_d select generate_series(1101, 1110);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_j';
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_co_j --skipvalidation;
!\retcode gpstop -u;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select (select count(*) from hs_co_h) h, (select count(*) from hs_co_d where a > 900 and a < 1000) d;

----------------------------------------------------------------
-- The same with an overflowed running-xacts record (more than 64
-- subtransactions): it lists no subxids, which can be above both the top
-- xid and latestCompletedXid, and a later overflowed record makes the
-- standby ready without initializing it again.  The horizon must cover
-- every xid the record covers
----------------------------------------------------------------
1: select gp_inject_fault('dtm_after_include_in_ckpt_cleared', 'suspend', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2: begin;
2: insert into hs_co_h values (50);
3: insert into hs_co_h values (51);
2: do $$ begin for i in 1..70 loop begin insert into hs_co_h values (5000 + i); exception when others then raise; end; end loop; end $$;
2: insert into hs_co_d select generate_series(1201, 1230);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_after_include_in_ckpt_cleared', 1, dbid) from gp_segment_configuration where content = -1 and role = 'p';
1: checkpoint;
!\retcode clsn=$(psql -d postgres -Atc "select pg_current_wal_lsn()"); sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -p "$sport" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$clsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ];
-1M: checkpoint;
-1Mq:
-1Sq:
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
1: select gp_inject_fault('dtm_after_include_in_ckpt_cleared', 'resume', dbid) from gp_segment_configuration where content = -1 and role = 'p';
2<:
1: select gp_inject_fault('dtm_after_include_in_ckpt_cleared', 'reset', dbid) from gp_segment_configuration where content = -1 and role = 'p';
-- an overflowed transaction started after the restart keeps the next
-- record overflowed with an oldest running xid past the first one's xids
3: begin;
3: do $$ begin for i in 1..70 loop begin insert into hs_co_h values (6000 + i); exception when others then raise; end; end loop; end $$;
1: checkpoint;
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; cat "$sdir"/log/*.csv | wc -l > "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
1: select count(*) from gp_create_restore_point('hs_co_k');
1: insert into hs_co_d select generate_series(1301, 1310);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_k';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_co_k" && cat "$sdir"/log/*.csv | tail -n +$(( $(cat "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines") + 1 )) | grep -q 'anchor snapshot for restore point ..hs_co_k.. not exported: transaction [0-9]* had committed when this standby.s snapshot was initialized'; rm -f "$COORDINATOR_DATA_DIRECTORY/hs_co_log.lines";
-- once the overflowed transaction commits, the next restore point exports
-- and shows the whole held transaction, subtransactions included
3: commit;
1: select count(*) from gp_create_restore_point('hs_co_l');
1: insert into hs_co_d select generate_series(1401, 1410);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_co_l';
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_co_l --skipvalidation;
!\retcode gpstop -u;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select (select count(*) from hs_co_h where a = 50 or a between 5001 and 5070) t, (select count(*) from hs_co_d where a between 1201 and 1230) d;

----------------------------------------------------------------
-- Cleanup: no published anchor
----------------------------------------------------------------
1: drop table hs_co_h;
1: drop table hs_co_ao;
1: drop table hs_co_d;
1q:
2q:
3q:
-1Sq:
-1Mq:
0Mq:
!\retcode gpconfig -r whpg_hot_standby_snapshot_mode --skipvalidation;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpstop -ar;
