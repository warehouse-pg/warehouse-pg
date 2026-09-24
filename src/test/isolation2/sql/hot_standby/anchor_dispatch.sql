-- Anchor snapshots carried to the segments.
--
-- A dispatch session of the standby coordinator in anchored mode ships the
-- name of the anchor its snapshot carries with every statement, and every
-- executor (writer, reader, cursor reader, the single segment of a direct
-- dispatch) installs its own node's anchor of that name.  The restore point
-- is written under the two-phase commit lock, so the anchors of one name
-- describe one cluster instant, and a query anchored on every node reads one
-- cut: a distributed transaction is wholly visible or wholly invisible, a
-- one-phase transaction committed just before the restore point is visible,
-- and the cut moves on every segment at once when the published anchor
-- changes.  Executors read the anchor's xid set alone (they do not consult
-- the distributed log under an anchor).
--
-- Observations use per-segment counts of a table distributed by its column:
-- select gp_segment_id, count(*) ... group by 1.  The other cases of the
-- suite read at the replay position, the server default; anchored sessions
-- here SET the mode.  Publication goes through gpconfig + reload, as in
-- anchor_import; a backend applies the pending reload before it runs the
-- next command it reads.
--
-- Session roles: 1: primary QD; 2: primary QD (transactions left in flight);
-- -1S: standby QD (dispatch); -1M: standby QD utility; 0M/1M: mirror utility.

-- start_matchsubs
-- m/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/
-- s/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/(segN IP:PORT pid=PID)/
-- end_matchsubs

1: create table hs_disp_t(a int) distributed by (a);
1: insert into hs_disp_t select generate_series(1, 30);
-- a running-xacts record on every node, so that restore points export
1: checkpoint;
1: insert into hs_disp_t select generate_series(31, 40);

----------------------------------------------------------------
-- Baseline: the anchored cut equals the primary at the restore point on
-- every segment; rows inserted after it are invisible everywhere
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_disp_a');
1: insert into hs_disp_t select generate_series(41, 60);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_a --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(61, 70);
1: select gp_segment_id, count(*) from hs_disp_t where a <= 40 group by 1 order by 1;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select gp_segment_id, count(*) from hs_disp_t group by 1 order by 1;
-- the cut is the same for a plan with a reader gang: the join key is not
-- the distribution key of one side, so one side moves and a second slice
-- runs on reader QEs (three of them, one per segment)
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1;
-1S: select count(*) from gp_backend_info() where type = 'r';
-- an unanchored statement of the same session reads at the replay position
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select gp_segment_id, count(*) from hs_disp_t group by 1 order by 1;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-- a utility session on the standby coordinator is never anchored
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_disp_a';
-- every mirror registered the anchor
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;

----------------------------------------------------------------
-- A one-phase transaction committed before the restore point is visible
-- at the anchor on its segment: the anchor of that segment holds it as
-- committed, no distributed commit record needed
----------------------------------------------------------------
1: insert into hs_disp_t values (1001);
-- the restore point is created in the shell so that the wait for its
-- replay on the standby coordinator and every mirror needs no barrier
-- (a barrier would be a two-phase commit after the one-phase one)
!\retcode rows=$(psql -d postgres -Atc "select gp_segment_id, restore_lsn from gp_create_restore_point('hs_disp_1pc')"); for c in -1 0 1 2; do lsn=$(echo "$rows" | awk -F'|' -v c=$c '$1==c {print $2}'); host=$(psql -d postgres -Atc "select hostname from gp_segment_configuration where content = $c and role = 'm'"); port=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = $c and role = 'm'"); cur=f; for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -h "$host" -p "$port" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ] || exit 1; done;
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_1pc --skipvalidation;
!\retcode gpstop -u;
-1S: select count(*) from hs_disp_t where a = 1001;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_disp_t where a = 1001;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- A two-phase transaction in flight at the restore point is invisible on
-- every segment at the anchor.  Case A: prepared everywhere, no
-- distributed commit record yet
----------------------------------------------------------------
1: select gp_inject_fault('dtm_broadcast_prepare', 'suspend', 1);
2: begin;
2: insert into hs_disp_t select generate_series(101, 130);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_broadcast_prepare', 1, 1);
1: select count(*) from gp_create_restore_point('hs_disp_b');
1: select gp_inject_fault('dtm_broadcast_prepare', 'reset', 1);
2<:
1: insert into hs_disp_t select generate_series(71, 80);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_b --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(81, 90);
-- none of 101..130 on any segment
-1S: select gp_segment_id, count(*) from hs_disp_t where a between 101 and 130 group by 1 order by 1;
-1S: select count(*) from hs_disp_t where a between 101 and 130;
-- all of them at the replay position
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_disp_t where a between 101 and 130;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- Case B: the distributed commit record precedes the restore point, the
-- segments' COMMIT PREPARED records follow it.  The anchor of every
-- segment still holds the transaction as prepared, so it is invisible
-- everywhere (point-in-time recovery would rebroadcast the commit; an
-- anchored read does not).  Publishing an anchor taken after the commit
-- then makes it visible on every segment at once
----------------------------------------------------------------
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'suspend', 1);
2: begin;
2: insert into hs_disp_t select generate_series(201, 230);
2&: commit;
1: select gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, 1);
1: select count(*) from gp_create_restore_point('hs_disp_c');
1: select gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', 1);
2<:
1: insert into hs_disp_t select generate_series(91, 100);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_c --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(301, 310);
-1S: select gp_segment_id, count(*) from hs_disp_t where a between 201 and 230 group by 1 order by 1;
-1S: select count(*) from hs_disp_t where a between 201 and 230;
1: select count(*) from gp_create_restore_point('hs_disp_d');
1: insert into hs_disp_t select generate_series(311, 320);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_d --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(321, 330);
-- the same session, next statement: the whole transaction, on every segment
-1S: select gp_segment_id, count(*) from hs_disp_t where a between 201 and 230 group by 1 order by 1;
-1S: select count(*) from hs_disp_t where a between 201 and 230;
-- rows after the new anchor are still invisible
-1S: select count(*) from hs_disp_t where a between 311 and 330;

----------------------------------------------------------------
-- REPEATABLE READ with a reader gang pins the anchor: a publication that
-- retires it fails the next statement; the cursor of another transaction
-- declared before the publication keeps its cut
----------------------------------------------------------------
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1;
1: select count(*) from gp_create_restore_point('hs_disp_e');
1: insert into hs_disp_t select generate_series(331, 340);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_e --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(341, 350);
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1;
-1S: rollback;
-1S: begin;
-1S: declare hs_disp_cur cursor for select gp_segment_id, count(*) from hs_disp_t where a between 311 and 360 group by 1 order by 1;
1: select count(*) from gp_create_restore_point('hs_disp_f');
1: insert into hs_disp_t select generate_series(351, 360);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_f --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(361, 370);
-- the cursor reads the cut of hs_disp_e: 311..330 only
-1S: fetch all from hs_disp_cur;
-1S: end;
-- a new statement reads hs_disp_f: 311..350
-1S: select count(*) from hs_disp_t where a between 311 and 360;

----------------------------------------------------------------
-- Direct dispatch: a single-segment plan ships no distributed snapshot,
-- and the segment still installs the anchor
----------------------------------------------------------------
-1S: explain (costs off) select * from hs_disp_t where a = 365;
-1S: select count(*) from hs_disp_t where a = 365;
-1S: select count(*) from hs_disp_t where a = 345;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_disp_t where a = 365;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- A reader gang under a recovery snapshot with more subtransactions than
-- the snapshot can list: the writer's slot and the cursor dump carry the
-- recovery shape, so readers keep the xid list and do not see rows the
-- transaction commits after their snapshot (the same fix the anchored
-- case below relies on).  A REPEATABLE READ transaction reading at the
-- replay position takes its snapshot while 200 subtransactions of one
-- segment are in flight, the transaction commits, the next statement of
-- the reader gang must not see it.  The joins pair each row of the value
-- with the single row of its predecessor, through a motion
----------------------------------------------------------------
2: begin;
2: do $$ begin for i in 1..200 loop begin insert into hs_disp_t values (5); exception when others then raise; end; end loop; end $$;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: begin isolation level repeatable read;
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 5;
2: commit;
1: insert into hs_disp_t select generate_series(371, 380);
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 5;
-1S: end;
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 5;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- An overflowed anchor on a segment: 200 subtransactions of one segment
-- in flight at the restore point.  Under the anchor the rows stay
-- invisible after the commit, through a direct dispatch, a reader gang
-- and a cursor
----------------------------------------------------------------
2: begin;
2: do $$ begin for i in 1..200 loop begin insert into hs_disp_t values (8); exception when others then raise; end; end loop; end $$;
1: select count(*) from gp_create_restore_point('hs_disp_g');
1: insert into hs_disp_t select generate_series(381, 390);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_g --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(391, 400);
-1S: select count(*) from hs_disp_t where a = 8;
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 8;
2: commit;
1: insert into hs_disp_t select generate_series(401, 410);
-1S: select count(*) from hs_disp_t where a = 8;
-1S: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 8;
-1S: begin;
-1S: declare hs_disp_cur2 cursor for select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 8;
-1S: fetch all from hs_disp_cur2;
-1S: end;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_disp_t where a = 8;
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- An anchor missing on one segment (its export failed) refuses the
-- statement with 55000 from that segment; the next publication recovers
----------------------------------------------------------------
1: select gp_inject_fault('anchor_snapshot_export_write', 'skip', dbid) from gp_segment_configuration where content = 1 and role = 'm';
1: select count(*) from gp_create_restore_point('hs_disp_h');
1: insert into hs_disp_t select generate_series(411, 420);
1: select gp_inject_fault('anchor_snapshot_export_write', 'reset', dbid) from gp_segment_configuration where content = 1 and role = 'm';
1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_disp_h';
0M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_disp_h';
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_h --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(421, 430);
-1S: select count(*) from hs_disp_t;
-- a plan that touches only another segment is not affected
-1S: select count(*) from hs_disp_t where a = 5;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) > 0 from hs_disp_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
1: select count(*) from gp_create_restore_point('hs_disp_i');
1: insert into hs_disp_t select generate_series(431, 440);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_disp_i --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_disp_t select generate_series(441, 450);
-1S: select count(*) from hs_disp_t where a between 411 and 450;

----------------------------------------------------------------
-- On the primary the mode GUC is inert: no anchor is ever dispatched
----------------------------------------------------------------
1: set whpg_hot_standby_snapshot_mode = anchored;
1: select count(*) from hs_disp_t x join hs_disp_t y on x.a = y.a + 1 where x.a = 8;
1: reset whpg_hot_standby_snapshot_mode;

----------------------------------------------------------------
-- Cleanup: no published anchor
----------------------------------------------------------------
1: drop table hs_disp_t;
1q:
2q:
-1Sq:
-1Mq:
0Mq:
1Mq:
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpstop -ar;
