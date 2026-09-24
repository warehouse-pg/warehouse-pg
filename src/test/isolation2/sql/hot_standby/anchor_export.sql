-- Anchor snapshot export at restore-point replay on a hot standby.
--
-- The startup process of every hot-standby node exports its xid snapshot
-- when it replays a restore-point record: a file under
-- pg_anchor_snapshots/ named after the restore point, and a {name, xmin}
-- entry in the node's shared-memory registry, which
-- gp_toolkit.whpg_anchor_snapshots() exposes.  This test drives the export through gp_create_restore_point on
-- the primary and reads the mirrors (0M:, 1M:) and the standby coordinator
-- (-1M:) in utility mode.
--
-- gp_create_restore_point does not wait for the standbys to replay the
-- record (its transaction has no xid, so synchronous commit does not
-- apply); the multi-row insert after every restore point is the barrier:
-- with synchronous_commit = remote_apply its commit returns only once each
-- mirror and the standby coordinator have applied everything before it.
--
-- Session roles: 1: primary QD; -1S: standby QD (dispatch); -1M: standby
-- QD utility; 0M:/1M: mirror of content 0/1 in utility mode; 1U: the
-- primary of content 1 in utility mode (after the failover below, that is
-- the promoted mirror).  Log assertions grep the csv server logs, where
-- embedded double quotes are doubled; the patterns match them with dots.
--
-- The registry capacity is lowered to 3 so that the eviction and
-- publication cases fill it quickly; that needs a cluster restart at the
-- start and at the end.  The clean-restart case takes a checkpoint on the
-- primary first so that the standbys' shutdown restartpoints lie past every
-- restore point created before it: the anchor the GUC names is then
-- registered at start, and the other records are not replayed again.  The
-- crash-restart case does the opposite on purpose.

-- start_matchsubs
-- m/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/
-- s/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/(segN IP:PORT pid=PID)/
-- end_matchsubs

-- a stale anchor line from an aborted earlier run would be re-registered
-- by the restart below and skew the counts
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpconfig -c whpg_max_anchor_snapshots -v 3 --skipvalidation;
!\retcode gpconfig -c gp_fts_probe_interval -v 10 --coordinatoronly;
!\retcode gpstop -ar;

1: create table hs_anchor_t(a int) distributed by (a);

----------------------------------------------------------------
-- Export at restore-point replay
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_anchor_rp1');
1: insert into hs_anchor_t select generate_series(1, 30);

0M: select rp_name, xmin::text::bigint > 2 as xmin_ok from gp_toolkit.whpg_anchor_snapshots() order by 1;
1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- the SRF executes locally on the standby QD as well
-1S: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;

-- the file exists on the mirror, opens with the format and rp_name lines,
-- carries the record's timeline and LSN, exactly the fixed fields of the
-- grammar (the sxp lines vary), and closes with the checksum line
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); f="$mdir/pg_anchor_snapshots/hs_anchor_rp1"; test -s "$f" && head -2 "$f" | tr '\n' ' ' | grep -q '^fmt:2 rp_name:hs_anchor_rp1 $' && grep -q -E '^tli:[0-9]+$' "$f" && grep -q -E '^lsn:[0-9A-F]+/[0-9A-F]+$' "$f" && test "$(grep -E -c '^(fmt|rp_name|tli|lsn|xmin|xmax|xcnt|sof|rec|crc):' "$f")" = 10 && tail -1 "$f" | grep -q -E '^crc:[0-9A-F]{8}$';
-- and the mirror logged the export
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); grep -q 'exported anchor snapshot for restore point ..hs_anchor_rp1.. (xmin' "$mdir"/log/*.csv;

----------------------------------------------------------------
-- A duplicate name is skipped; the existing entry and file are kept
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_anchor_rp1');
1: insert into hs_anchor_t select generate_series(31, 60);

0M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_anchor_rp1';
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); grep -q 'anchor snapshot for restore point ..hs_anchor_rp1.. not exported: name already registered' "$mdir"/log/*.csv;

----------------------------------------------------------------
-- An invalid name is rejected before anything is written
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('../hs_anchor_bad');
1: insert into hs_anchor_t select generate_series(61, 90);

0M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name like '%hs_anchor_bad';
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$mdir/hs_anchor_bad" && test ! -e "$mdir/pg_anchor_snapshots/../hs_anchor_bad.tmp" && grep -q 'anchor snapshot for restore point ..\.\./hs_anchor_bad.. not exported: invalid name' "$mdir"/log/*.csv;

----------------------------------------------------------------
-- Registry full (capacity 3): the fourth export evicts the oldest
-- anchor.  No anchor is published yet, so the oldest of all goes.
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_anchor_rp2');
1: insert into hs_anchor_t select generate_series(91, 120);
1: select count(*) from gp_create_restore_point('hs_anchor_rp3');
1: insert into hs_anchor_t select generate_series(121, 150);
1: select count(*) from gp_create_restore_point('hs_anchor_rp4');
1: insert into hs_anchor_t select generate_series(151, 180);

0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
0M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rp1" && test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp4" && grep -q 'evicted anchor snapshot for restore point ..hs_anchor_rp1.. to make room for ..hs_anchor_rp4..: registry full' "$mdir"/log/*.csv;

-- A failed write on a full registry evicts nothing: the file is written
-- before a slot is taken, so rp2 (the would-be victim), rp3 and rp4 all
-- survive on content 0; the other nodes, where the write succeeds, evict
1: select gp_inject_fault('anchor_snapshot_export_write', 'skip', dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: select count(*) from gp_create_restore_point('hs_anchor_rpfail');
1: insert into hs_anchor_t select generate_series(1001, 1030);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp2" && test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp3" && test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp4" && test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rpfail" && test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rpfail.tmp" && ! grep -q 'to make room for ..hs_anchor_rpfail' "$mdir"/log/*.csv && grep -q 'could not write anchor snapshot file ..pg_anchor_snapshots/hs_anchor_rpfail.tmp..: fault injected' "$mdir"/log/*.csv;
1: select gp_inject_fault('anchor_snapshot_export_write', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'm';

----------------------------------------------------------------
-- Publication retires the anchors registered before the published one
----------------------------------------------------------------
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_anchor_rp3 --skipvalidation;
!\retcode gpstop -u;
-- the startup process picks the reload up at its next interrupt check;
-- a restore point gives it one, and the freed slot takes the new export
1: select count(*) from gp_create_restore_point('hs_anchor_rp5');
1: insert into hs_anchor_t select generate_series(181, 210);

0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rp2" && test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp3" && test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp5" && grep -q 'retired anchor snapshot for restore point ..hs_anchor_rp2..: superseded by ..hs_anchor_rp3' "$mdir"/log/*.csv;

-- a name the registry does not know retires nothing; and with the GUC
-- naming no registered anchor, the next eviction takes the oldest entry,
-- the formerly published rp3 included
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_anchor_none --skipvalidation;
!\retcode gpstop -u;
1: select count(*) from gp_create_restore_point('hs_anchor_rp6');
1: insert into hs_anchor_t select generate_series(211, 240);

0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); grep -q 'anchor ..hs_anchor_none.. named by whpg_hot_standby_anchor_name is not registered on this node; no anchor retired' "$mdir"/log/*.csv && grep -q 'evicted anchor snapshot for restore point ..hs_anchor_rp3.. to make room for ..hs_anchor_rp6' "$mdir"/log/*.csv;

----------------------------------------------------------------
-- A publication that arrives before its restore point completes when
-- the record is exported: the reload alone retires nothing, the export
-- of the named anchor retires everything before it
----------------------------------------------------------------
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_anchor_rp7 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_anchor_t select generate_series(241, 270);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); grep -q 'anchor ..hs_anchor_rp7.. named by whpg_hot_standby_anchor_name is not registered on this node; no anchor retired' "$mdir"/log/*.csv;

-- the full registry evicts rp4 for rp7, then rp7's publication retires
-- rp5 and rp6
1: select count(*) from gp_create_restore_point('hs_anchor_rp7');
1: insert into hs_anchor_t select generate_series(271, 300);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rp4" && test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rp5" && test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rp6" && test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp7" && test "$(ls -A "$mdir/pg_anchor_snapshots" | wc -l)" = 1 && grep -q 'retired anchor snapshot for restore point ..hs_anchor_rp5..: superseded by ..hs_anchor_rp7' "$mdir"/log/*.csv;

----------------------------------------------------------------
-- Clean restart: the anchor the GUC names is registered at start (its
-- record lies before the redo start point), everything else is swept
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_anchor_rp8');
1: insert into hs_anchor_t select generate_series(301, 330);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1: checkpoint;
1: insert into hs_anchor_t select generate_series(331, 360);
-- every session must be gone: gpstop's smart shutdown waits for them
1q:
0Mq:
1Mq:
-1Mq:
-1Sq:
!\retcode gpstop -ar;

0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test -s "$mdir/pg_anchor_snapshots/hs_anchor_rp7" && test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_rp8" && test "$(ls -A "$mdir/pg_anchor_snapshots" | wc -l)" = 1 && grep -q 're-registered anchor snapshot for restore point ..hs_anchor_rp7.. (xmin' "$mdir"/log/*.csv;

-- exports keep working after the rebuild
1: select count(*) from gp_create_restore_point('hs_anchor_rp9');
1: insert into hs_anchor_t select generate_series(361, 390);
0M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;

----------------------------------------------------------------
-- Crash restart: the standby coordinator is stopped immediately after
-- several restore points with no checkpoint in between, so its redo
-- restarts before them.  The anchor the GUC names must not be registered
-- until replay meets its record again: replay is made to pause at the
-- earliest restore point, where the later one's anchor is still pending.
-- Only restore points and inserts happen between the clean restart above
-- and the crash, so no page of the standby coordinator is written back
-- and hot standby opens (minRecoveryPoint) before the paused position.
--
-- The published name, pend_y, is exported TWICE before the crash (names
-- are not unique on the primary; the publication of pend_z retires the
-- first instance and frees the name): redo meets the earlier pend_y first,
-- while the later one's file is pending.  That record must not be
-- exported over the pending file.
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_anchor_pend_x');
1: insert into hs_anchor_t select generate_series(391, 420);
1: select count(*) from gp_create_restore_point('hs_anchor_pend_y');
1: insert into hs_anchor_t select generate_series(421, 450);
1: select count(*) from gp_create_restore_point('hs_anchor_pend_z');
1: insert into hs_anchor_t select generate_series(451, 480);
-- publishing pend_z retires pend_x and the first pend_y, entry and file
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_anchor_pend_z --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_anchor_t select generate_series(481, 510);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-- the second pend_y: its record LSN on the standby coordinator is kept to
-- check later that the pending file survived with this instance's snapshot
!\retcode psql -d postgres -Atc "select restore_lsn from gp_create_restore_point('hs_anchor_pend_y') where gp_segment_id = -1" > "$COORDINATOR_DATA_DIRECTORY/hs_anchor_pend_y.lsn"; test -s "$COORDINATOR_DATA_DIRECTORY/hs_anchor_pend_y.lsn";
1: insert into hs_anchor_t select generate_series(511, 540);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_anchor_pend_y --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_anchor_t select generate_series(541, 570);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1Mq:
!\retcode echo "gp_pause_on_restore_point_replay = 'hs_anchor_pend_x'" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
-- restart (not stop + start) keeps the postmaster options gpstart gave it.
-- -W: for a standby coordinator pg_ctl would wait until the walreceiver
-- streams, which a startup process paused at pend_x never asks for; hot
-- standby itself opens at once, so wait for a utility connection instead
!\retcode pg_ctl restart -W -m immediate -D "$COORDINATOR_DATA_DIRECTORY/../../standby"; sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); for i in $(seq 1 60); do PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_is_wal_replay_paused()' 2> /dev/null | grep -q t && break; sleep 1; done; PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_is_wal_replay_paused()';

-- paused at pend_x (hot standby opens before the pause lands, so the loop
-- above waited for the pause itself): pend_y is pending, its file kept,
-- the records before it exported again on the way
-1M: select pg_is_wal_replay_paused();
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test -s "$sdir/pg_anchor_snapshots/hs_anchor_pend_y" && grep -q 'anchor snapshot for restore point ..hs_anchor_pend_y.. held pending until replay reaches' "$sdir"/log/*.csv;

-- resume: replay meets the earlier pend_y first and skips it (a pending
-- name counts as registered), then pend_z, then the pending record: the
-- anchor is registered from its file, whose lsn line is the second
-- instance's, and its publication retires the re-exported older anchors.
-- pend_y was exported exactly twice in this run, both before the crash
!\retcode sed -i '/hs_anchor_pend_x/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
!\retcode pg_ctl reload -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1M: select pg_wal_replay_resume();
1: insert into hs_anchor_t select generate_series(571, 600);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; grep -q 'replay reached the restore-point record of pending anchor snapshot ..hs_anchor_pend_y.. at [0-9A-F/]*; registered from its file' "$sdir"/log/*.csv && test "$(ls -A "$sdir/pg_anchor_snapshots" | wc -l)" = 1 && grep -q 'anchor snapshot for restore point ..hs_anchor_pend_y.. not exported: a snapshot file of that name from an earlier run is pending registration' "$sdir"/log/*.csv && test "$(cat "$sdir"/log/*.csv | grep -c 'exported anchor snapshot for restore point ..hs_anchor_pend_y.. (xmin')" = 2 && grep -q "^lsn:$(cat "$COORDINATOR_DATA_DIRECTORY/hs_anchor_pend_y.lsn")$" "$sdir/pg_anchor_snapshots/hs_anchor_pend_y"; rm -f "$COORDINATOR_DATA_DIRECTORY/hs_anchor_pend_y.lsn";

----------------------------------------------------------------
-- The export gate: a node whose standby snapshot is not ready exports
-- nothing.  hot_standby = off on the standby coordinator keeps its
-- standbyState at STANDBY_DISABLED; the mirrors still export.
----------------------------------------------------------------
-1Mq:
!\retcode echo "hot_standby = off" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
1: select count(*) from gp_create_restore_point('hs_anchor_gate');
1: insert into hs_anchor_t select generate_series(511, 540);

0M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_anchor_gate';
-- the published anchor's file survives a start that could not examine it
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_anchor_gate" && test -s "$sdir/pg_anchor_snapshots/hs_anchor_pend_y" && grep -q 'anchor snapshot file for restore point ..hs_anchor_pend_y.. kept but not registered: hot standby is disabled' "$sdir"/log/*.csv;
!\retcode sed -i '/^hot_standby = off$/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1M: show hot_standby;
-- and is registered again once hot standby is back
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;

----------------------------------------------------------------
-- A failed write leaves the standby serving and exports nothing
----------------------------------------------------------------
-- the mirrors hold pend_y and gate (capacity 3): a slot is free
1: select gp_inject_fault('anchor_snapshot_export_write', 'skip', dbid) from gp_segment_configuration where content = 0 and role = 'm';
1: select count(*) from gp_create_restore_point('hs_anchor_fault');
1: insert into hs_anchor_t select generate_series(541, 570);

0M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_anchor_fault';
1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_anchor_fault';
-1S: select count(*) from hs_anchor_t;
!\retcode mdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 0 and role = 'm'"); test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_fault" && test ! -e "$mdir/pg_anchor_snapshots/hs_anchor_fault.tmp" && grep -q 'could not write anchor snapshot file ..pg_anchor_snapshots/hs_anchor_fault.tmp..: fault injected' "$mdir"/log/*.csv;
1: select gp_inject_fault('anchor_snapshot_export_write', 'reset', dbid) from gp_segment_configuration where content = 0 and role = 'm';

----------------------------------------------------------------
-- A damaged file is refused at start: the checksum catches a changed
-- byte, the anchor is not registered and the file is swept
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_anchor_crc');
1: insert into hs_anchor_t select generate_series(571, 600);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_anchor_crc --skipvalidation;
!\retcode gpstop -u;
1: checkpoint;
1: insert into hs_anchor_t select generate_series(601, 630);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1Mq:
!\retcode sed -i 's/^xcnt:0$/xcnt:1/' "$COORDINATOR_DATA_DIRECTORY/../../standby/pg_anchor_snapshots/hs_anchor_crc";
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_anchor_crc" && grep -q 'anchor snapshot file ..pg_anchor_snapshots/hs_anchor_crc.. refused: checksum mismatch' "$sdir"/log/*.csv;

----------------------------------------------------------------
-- Promotion clears the registry and the directory before recovery ends
----------------------------------------------------------------
0Mq:
1Mq:
-1Sq:
1: select gp_inject_fault('out_of_recovery_in_startupxlog', 'skip', dbid) from gp_segment_configuration where content = 1 and role = 'm';
1: select pg_ctl(datadir, 'stop', 'immediate') from gp_segment_configuration where content = 1 and role = 'p';
1: select gp_request_fts_probe_scan();
1: select dbid, content, role, preferred_role, mode, status from gp_segment_configuration where content = 1;
1: select gp_wait_until_triggered_fault('out_of_recovery_in_startupxlog', 1, dbid) from gp_segment_configuration where content = 1 and role = 'p';
1: select gp_inject_fault('out_of_recovery_in_startupxlog', 'reset', dbid) from gp_segment_configuration where content = 1 and role = 'p';

-- the promoted node holds no anchors and its directory is empty
1U: select count(*) from gp_toolkit.whpg_anchor_snapshots();
!\retcode pdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = 1 and role = 'p'"); test -z "$(ls -A "$pdir/pg_anchor_snapshots")" && grep -q 'anchor snapshot(s) at the end of recovery' "$pdir"/log/*.csv;

-- bring the downed mirror back and rebalance
!\retcode gprecoverseg -aF;
1: select wait_until_all_segments_synchronized();
!\retcode gprecoverseg -ar;
1: select wait_until_all_segments_synchronized();
1: select dbid, content, role, preferred_role, mode, status from gp_segment_configuration where content = 1;

----------------------------------------------------------------
-- The anchor is registered before the node publishes the record's
-- replay position: a caller that waits for pg_last_wal_replay_lsn() to
-- reach what gp_create_restore_point returned finds the anchor, with no
-- further barrier
----------------------------------------------------------------
!\retcode mhost=$(psql -d postgres -Atc "select hostname from gp_segment_configuration where content = 0 and role = 'm'"); mport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = 0 and role = 'm'"); lsn=$(psql -d postgres -Atc "select restore_lsn from gp_create_restore_point('hs_anchor_lsn') where gp_segment_id = 0"); for i in $(seq 1 600); do cur=$(PGOPTIONS='-c gp_role=utility' psql -X -h "$mhost" -p "$mport" -d postgres -Atc "select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn"); [ "$cur" = t ] && break; sleep 0.1; done; [ "$cur" = t ] && test "$(PGOPTIONS='-c gp_role=utility' psql -X -h "$mhost" -p "$mport" -d postgres -Atc "select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_anchor_lsn'")" = 1;

----------------------------------------------------------------
-- Cleanup: default capacity, no published anchor
----------------------------------------------------------------
1: drop table hs_anchor_t;
1q:
1Uq:
-1Mq:
!\retcode gpconfig -r whpg_max_anchor_snapshots --skipvalidation;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpstop -ar;
