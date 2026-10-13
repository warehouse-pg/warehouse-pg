-- Anchored-mode acceptance cases the feature suites do not carry: the
-- cluster-wide cut under concurrent distributed transactions, visibility
-- within a READ COMMITTED transaction block, the conflict budget (a reader
-- inside it survives, one past it is cancelled while running), a new
-- import while replay waits on a reader, an append-optimized reader
-- cancelled by the recycle rather than reading wrong rows, SERIALIZABLE
-- run as REPEATABLE READ and pinned, anchored and unanchored dispatch
-- sessions side by side, cursors across an invalidation and a
-- publication, the distributed-snapshot case of query_conflict under an
-- anchor, and the way back from anchored mode to a plain hot standby.
--
-- Errors are asserted twice: the expected output carries the message, and
-- the standby coordinator's csv log, whose SQLSTATE sits in its own column,
-- carries the code (script/hs_anchor_log.sh greps what was logged since
-- this test started; an error an executor raised carries the segment
-- suffix there, one the coordinator raised does not).
--
-- Barriers, publication and the direct dispatch to content 0 (a = 7) work
-- as in anchor_conflict: an insert of rows on every segment commits with a
-- two-phase commit whose remote_apply wait covers every mirror; a
-- publication is gpconfig + reload followed by a barrier.
--
-- Session roles: 1, 2: primary QD; -1S: standby QD (dispatch); -1M:
-- standby QD utility; 0M: content-0 mirror utility.

-- start_matchsubs
-- m/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/
-- s/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/(segN IP:PORT pid=PID)/
-- m/xmin \d+/
-- s/xmin \d+/xmin N/
-- end_matchsubs

!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpconfig -c whpg_hot_standby_snapshot_mode -v anchored --skipvalidation;
!\retcode gpstop -ar;
1: select gp_inject_fault('all', 'reset', dbid) from gp_segment_configuration where role = 'm';
!\retcode ./script/hs_anchor_log.sh mark cov -1 0;
1: create table cov_t(a int, b int) distributed by (a);
1: insert into cov_t select i, i from generate_series(1, 300) i;
1: create table cov_barrier(a int) distributed by (a);
1: checkpoint;
1: insert into cov_barrier select generate_series(1, 30);
1: select gp_segment_id from cov_t where a = 7;

----------------------------------------------------------------
-- 1. One cut on every node under concurrent distributed transactions:
-- transfers between accounts on different segments run while restore
-- points are taken and published; every anchor shows the total intact
----------------------------------------------------------------
1: create table cov_acct(id int, bal int) distributed by (id);
1: insert into cov_acct select i, 100 from generate_series(1, 10) i;
1: select count(distinct gp_segment_id) from cov_acct;
!\retcode rm -f /tmp/hs_cov_${PGPORT}_stop /tmp/hs_cov_${PGPORT}_done; nohup bash -c 'i=0; while [ ! -e /tmp/hs_cov_${PGPORT}_stop ]; do i=$((i + 1)); psql -X -q -d isolation2-hot-standby -c "begin; update cov_acct set bal = bal - 1 where id = $((1 + i % 10)); update cov_acct set bal = bal + 1 where id = $((1 + (i * 7 + 3) % 10)); commit;" > /dev/null 2>&1; done; touch /tmp/hs_cov_${PGPORT}_done' > /dev/null 2>&1 & true;
-- transfers are committing
!\retcode b=$(psql -X -d isolation2-hot-standby -Atc "select sum(abs(bal - 100)) from cov_acct"); for i in $(seq 1 100); do n=$(psql -X -d isolation2-hot-standby -Atc "select sum(abs(bal - 100)) from cov_acct"); [ "$n" != "$b" ] && exit 0; sleep 0.1; done; exit 1;
1: select count(*) from gp_create_restore_point('cov_r1');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r1 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select sum(bal), count(*) from cov_acct;
1: select count(*) from gp_create_restore_point('cov_r2');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r2 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select sum(bal), count(*) from cov_acct;
!\retcode touch /tmp/hs_cov_${PGPORT}_stop; for i in $(seq 1 100); do [ -e /tmp/hs_cov_${PGPORT}_done ] && exit 0; sleep 0.1; done; exit 1;
1: select sum(bal), count(*) from cov_acct;

----------------------------------------------------------------
-- 2. READ COMMITTED inside a transaction block: the next statement after
-- a publication reads the new anchor
----------------------------------------------------------------
-1S: begin;
-1S: select count(*) from cov_t;
1: insert into cov_t select i, i from generate_series(301, 320) i;
1: select count(*) from gp_create_restore_point('cov_r3');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r3 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select count(*) from cov_t;
-1S: end;

----------------------------------------------------------------
-- 3a + 4. Inside the budget: with max_standby_streaming_delay = -1 replay
-- waits for the reader, which finishes with the right answer.  The anchor
-- is gone from content 0 for the whole wait: a new anchored import there
-- fails at once (the invalidation runs before the conflict wait)
----------------------------------------------------------------
!\retcode gpconfig -c max_standby_streaming_delay -v -1; gpstop -u;
-1S: begin isolation level repeatable read;
-1S: select count(*) from cov_t where a = 7;
1: delete from cov_t where a = 7;
1: vacuum cov_t;
1&: insert into cov_barrier select generate_series(1, 30);
!\retcode ./script/hs_anchor_log.sh grep cov 0 'invalidated anchor snapshot for restore point ..cov_r3..: replayed cleanup';
-- a second anchored session is refused by content 0 while replay waits
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); PGOPTIONS='-c whpg_hot_standby_snapshot_mode=anchored' psql -X -v VERBOSITY=verbose -p "$sport" -d isolation2-hot-standby -Atc 'select count(*) from cov_t where a = 7' 2>&1 | grep -q '^ERROR:  55000: anchor snapshot "cov_r3" is not registered on this node  (seg0 ';
-- the waiting reader still reads its anchor
-1S: select count(*) from cov_t where a = 7;
-1S: end;
1<:
!\retcode gpconfig -c max_standby_streaming_delay -v 1000; gpstop -u;

----------------------------------------------------------------
-- 3b. Past the budget: a running anchored statement is cancelled
----------------------------------------------------------------
1: insert into cov_t values (7, 7);
1: select count(*) from gp_create_restore_point('cov_r4');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r4 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S&: select count(*) from cov_t where a = 7 and pg_sleep(8) is not null;
!\retcode ./script/hs_anchor_log.sh active - 0 '%pg_sleep(8)%';
1: delete from cov_t where a = 7;
1: vacuum cov_t;
1: insert into cov_barrier select generate_series(1, 30);
-1S<:
!\retcode ./script/hs_anchor_log.sh grep cov -1 '"ERROR","40001","canceling statement due to conflict with recovery  .seg0 ';

----------------------------------------------------------------
-- 5. An append-optimized reader is cancelled by the recycle of the
-- segment file it reads, not left reading it: the conflict probe on the
-- content-0 mirror fires for this table's record, and after the file is
-- refilled the anchor is refused rather than read
----------------------------------------------------------------
1: select gp_inject_fault_infinite('skip_meta_track_update', 'skip', dbid) from gp_segment_configuration where content = -1 and role = 'p';
1: create table cov_ao(a int, b int) with (appendonly = true) distributed by (a);
1: insert into cov_ao select i, i from generate_series(1, 30) i;
1: select count(*) > 0 from cov_ao where gp_segment_id = 0 and a % 2 = 0;
1: select count(*) from gp_create_restore_point('cov_r5');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r5 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S&: select count(*) from cov_ao where a = 7 and pg_sleep(8) is not null;
!\retcode ./script/hs_anchor_log.sh active - 0 '%pg_sleep(8)%';
1: select gp_inject_fault('resolve_snapshot_conflict', 'skip', '', '', (select relfilenode::text from gp_dist_random('pg_class') where relname = 'cov_ao' and gp_segment_id = 0), 1, -1, 0, dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: delete from cov_ao where a % 2 = 0;
1: vacuum cov_ao;
1: insert into cov_barrier select generate_series(1, 30);
-1S<:
1: select gp_wait_until_triggered_fault('resolve_snapshot_conflict', 1, dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: select gp_inject_fault('resolve_snapshot_conflict', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'm';
-- the recycled file takes new rows; the anchor that predates the recycle
-- is refused, never read
1: insert into cov_ao select i, i from generate_series(1, 30) i;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select count(*) from cov_ao;
!\retcode ./script/hs_anchor_log.sh grep cov -1 '"ERROR","55000","anchor snapshot ..cov_r5.. is not registered on this node  .seg[0-9]+ ';
1: select gp_inject_fault('skip_meta_track_update', 'reset', dbid) from gp_segment_configuration where content = -1 and role = 'p';

----------------------------------------------------------------
-- 6. SERIALIZABLE: an explicit request is refused (0A000); the default
-- runs as REPEATABLE READ and pins the anchor of its first statement
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('cov_r6');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r6 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: begin isolation level serializable;
-1S: rollback;
!\retcode ./script/hs_anchor_log.sh grep cov -1 '"ERROR","0A000","cannot use serializable mode in a hot standby"';
-1S: set default_transaction_isolation = serializable;
-1S: begin;
-1S: show transaction_isolation;
-1S: select count(*) from cov_t;
1: insert into cov_t select i, i from generate_series(321, 330) i;
-- still the first statement's anchor
-1S: select count(*) from cov_t;
1: select count(*) from gp_create_restore_point('cov_r7');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r7 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-- the publication retired the pinned anchor: the transaction must end
-1S: select count(*) from cov_t;
-1S: rollback;
!\retcode ./script/hs_anchor_log.sh grep cov -1 '"ERROR","55000","anchor snapshot ..cov_r6.. pinned by this transaction is no longer registered"';
-1S: reset default_transaction_isolation;
-1S: select count(*) from cov_t;

----------------------------------------------------------------
-- 7. Anchored and unanchored dispatch sessions side by side: a cleanup
-- conflict ends the anchored reader; an unanchored reader whose snapshot
-- is newer than the cleanup reads on
----------------------------------------------------------------
1: insert into cov_t values (7, 7);
1: select count(*) from gp_create_restore_point('cov_r8');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r8 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: begin isolation level repeatable read;
-1S: select count(*) from cov_t where a = 7;
1: delete from cov_t where a = 7;
1: insert into cov_barrier select generate_series(1, 30);
!\retcode rm -f /tmp/hs_cov_${PGPORT}_bg; sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); nohup bash -c "PGOPTIONS='-c whpg_hot_standby_snapshot_mode=unanchored' psql -X -q -At -p $sport -d isolation2-hot-standby -c 'begin isolation level repeatable read' -c 'select count(*) from cov_t where a = 7' -c 'select pg_sleep(6)' -c 'select count(*) from cov_t where a = 7' -c 'commit' > /tmp/hs_cov_${PGPORT}_bg.tmp 2>&1; mv /tmp/hs_cov_${PGPORT}_bg.tmp /tmp/hs_cov_${PGPORT}_bg" > /dev/null 2>&1 & true;
!\retcode ./script/hs_anchor_log.sh active - -1 'select pg_sleep(6)';
1: vacuum cov_t;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select count(*) from cov_t where a = 7;
-1S: rollback;
-- the unanchored reader saw the delete twice and no error
!\retcode for i in $(seq 1 150); do [ -e /tmp/hs_cov_${PGPORT}_bg ] && break; sleep 0.1; done; ! grep -q ERROR /tmp/hs_cov_${PGPORT}_bg && test "$(grep -c '^0$' /tmp/hs_cov_${PGPORT}_bg)" = 2;

----------------------------------------------------------------
-- 8. Cursors: one in a REPEATABLE READ transaction reads to its end
-- across a publication that retires its anchor, while the next statement
-- of the transaction fails; one whose anchor is invalidated mid-scan is
-- cancelled
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('cov_r9');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r9 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: begin isolation level repeatable read;
-1S: declare cov_c1 cursor for select a from cov_t where a <= 6 order by a;
-1S: fetch 2 from cov_c1;
1: select count(*) from gp_create_restore_point('cov_r10');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r10 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: fetch 2 from cov_c1;
-1S: fetch all from cov_c1;
-1S: select count(*) from cov_t;
-1S: rollback;
-- the second cursor's executors are still scanning (blocked sending
-- rows) when the cleanup replays on content 0
1: create table cov_big(a int) distributed by (a);
1: insert into cov_big select generate_series(1, 300000);
1: select count(*) from gp_create_restore_point('cov_r10b');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r10b --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
!\retcode ./script/hs_anchor_log.sh mark c8 -1;
-1S: begin;
-1S: declare cov_c2 cursor for select a from cov_big;
-1S: fetch 1 from cov_c2;
!\retcode ./script/hs_anchor_log.sh active - 0 '%cov_big%';
1: delete from cov_big where a = 7;
1: vacuum cov_big;
1: insert into cov_barrier select generate_series(1, 30);
-1S: fetch all from cov_c2;
-1S: rollback;
!\retcode ./script/hs_anchor_log.sh grep c8 -1 '"ERROR","40001","canceling statement due to conflict with recovery  .seg0 ';

----------------------------------------------------------------
-- 9. query_conflict's distributed-snapshot case under an anchor: the
-- first statement runs on the coordinator only, so no executor holds the
-- anchor when the VACUUM replays.  Unanchored that transaction later read
-- the VACUUM's result (0 rows); anchored, the segments no longer hold the
-- anchor and refuse the statement
----------------------------------------------------------------
1: create table cov_qc(a int) distributed by (a);
1: insert into cov_qc select generate_series(1, 10);
1: select count(*) from gp_create_restore_point('cov_r11');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r11 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: begin isolation level repeatable read;
-1S: select relname from pg_class where relname = 'cov_qc';
1: delete from cov_qc;
1: vacuum cov_qc;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select count(*) from cov_qc;
-1S: end;
!\retcode ./script/hs_anchor_log.sh grep cov -1 '"ERROR","55000","anchor snapshot ..cov_r11.. is not registered on this node  .seg[0-9]+ ';

----------------------------------------------------------------
-- 10. Back to a plain hot standby
----------------------------------------------------------------
1: insert into cov_t values (7, 7);
1: select count(*) from gp_create_restore_point('cov_r12');
1: insert into cov_barrier select generate_series(1, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v cov_r12 --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-- a new session takes the configured mode
-1Sq:
-1S: show whpg_hot_standby_snapshot_mode;
-1S: select count(*) from cov_t;
-- 10a. the mode switched off by a reload: sessions that did not SET it
-- read the replay position at their next statement, the registered
-- anchors stay, and a restore point replayed afterwards is not exported
!\retcode gpconfig -c whpg_hot_standby_snapshot_mode -v unanchored --skipvalidation; gpstop -u;
1: insert into cov_t select i, i from generate_series(331, 340) i;
1: insert into cov_barrier select generate_series(1, 30);
-1S: show whpg_hot_standby_snapshot_mode;
-1S: select count(*) from cov_t;
1: select count(*) from gp_create_restore_point('cov_r13');
1: insert into cov_barrier select generate_series(1, 30);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- 10b. the anchor unpublished by a reload: an anchored session is
-- refused, an unanchored one reads on, and nothing is retired
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from cov_t;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation; gpstop -u;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select count(*) from cov_t;
!\retcode ./script/hs_anchor_log.sh grep cov -1 '"ERROR","55000","anchored read requires a published anchor snapshot"';
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from cov_t;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- 10c. the configuration back to the defaults and a restart: no anchor
-- registered or on disk, restore points export nothing, cleanup touches
-- no anchor, and dispatch sessions read as on any hot standby
1q:
-1Sq:
-1Mq:
0Mq:
!\retcode gpconfig -r whpg_hot_standby_snapshot_mode --skipvalidation; gpstop -ar -M fast;
!\retcode ./script/hs_anchor_log.sh mark off -1 0;
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
0M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
!\retcode for c in -1 0 1 2; do d=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = $c and role = 'm'"); test -d "$d/pg_anchor_snapshots" && test -z "$(ls -A "$d/pg_anchor_snapshots")" || exit 1; done;
1: select count(*) from gp_create_restore_point('cov_r14');
1: insert into cov_barrier select generate_series(1, 30);
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
0M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
-1S: show whpg_hot_standby_snapshot_mode;
-1S: select count(*) from cov_t;
1: delete from cov_t where a = 7;
1: vacuum cov_t;
1: insert into cov_barrier select generate_series(1, 30);
-1S: select count(*) from cov_t;
!\retcode ./script/hs_anchor_log.sh absent off 0 'anchor snapshot';
!\retcode ./script/hs_anchor_log.sh absent off -1 'anchor snapshot';

-- cleanup
1: drop table cov_t, cov_barrier, cov_acct, cov_ao, cov_qc, cov_big;
!\retcode rm -f /tmp/hs_cov_${PGPORT}_* /tmp/hs_anchor_log_${PGPORT}_*;
