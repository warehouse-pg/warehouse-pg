-- Anchor snapshot import on the standby coordinator.
--
-- A dispatch session on a hot standby in anchored mode
-- (whpg_hot_standby_snapshot_mode = anchored; the server default is
-- unanchored, a read replica sets anchored in its configuration file)
-- installs, for every statement, the anchor
-- snapshot named by whpg_hot_standby_anchor_name: its transaction snapshot
-- is the node's xid snapshot at that restore point and its xmin is the
-- anchor's.  This test drives publication through gpconfig + reload, as
-- anchor_export does, and observes the installed snapshot on the
-- coordinator itself: txid_current_snapshot() shows the snapshot's xmin,
-- pg_stat_activity.backend_xmin the published xmin, and a scan of pg_class
-- (executed on the coordinator with the transaction snapshot) shows which
-- catalog rows the anchor sees while the catalog snapshot, unanchored, still
-- resolves every name.  Carrying the anchor to the segments is a later
-- change; nothing here depends on segment data.
--
-- The other cases of the suite read at the replay position, the server
-- default; anchored sessions here SET the mode.  A backend applies a
-- pending reload before it runs the next command it reads, so the first
-- statement a session runs after gpstop -u already sees the new anchor
-- name; the startup process applies it at its next replayed record, which
-- the barrier insert after every publication provides.
--
-- Session roles: 1: primary QD; -1S: standby QD (dispatch); -1M: standby
-- QD utility.

-- start_matchsubs
-- m/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/
-- s/\(seg\d+ [0-9.]+:\d+ pid=\d+\)/(segN IP:PORT pid=PID)/
-- end_matchsubs

1: create table hs_imp_t(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(1, 10);

----------------------------------------------------------------
-- No published anchor: anchored statements fail, unanchored ones work
----------------------------------------------------------------
-1S: show whpg_hot_standby_snapshot_mode;
-1S: select count(*) from hs_imp_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_imp_t;
-- SET and SHOW take no snapshot, so a session can always switch mode
-1S: show whpg_hot_standby_snapshot_mode;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from hs_imp_t;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-- the refusal carries SQLSTATE 55000 (the mode set through PGOPTIONS); a
-- utility-mode session is not anchored
!\retcode sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); PGOPTIONS='-c whpg_hot_standby_snapshot_mode=anchored' psql -X -v VERBOSITY=verbose -p "$sport" -d postgres -Atc 'select 1' 2>&1 | grep -q '^ERROR:  55000: anchored read requires a published anchor snapshot$' && PGOPTIONS='-c gp_role=utility -c whpg_hot_standby_snapshot_mode=anchored' psql -X -p "$sport" -d postgres -Atc 'select 1' | grep -q '^1$';

----------------------------------------------------------------
-- A published anchor is installed: xmin, published xmin, and the cut
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_imp_a');
1: insert into hs_imp_t select generate_series(11, 20);
1: create table hs_imp_after_a(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(21, 30);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_a --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(31, 40);
-1Sq:
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_a') as snapshot_at_anchor;
-1S: select backend_xmin::text::bigint = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_a') as xmin_published from pg_stat_activity where pid = pg_backend_pid();
-- table data as of the anchor: the pg_class row created after it is not
-- visible to the transaction snapshot ...
-1S: select count(*) from pg_class where relname = 'hs_imp_after_a';
-- ... while the catalog snapshot, taken at the replay position, resolves it
-1S: select 'hs_imp_after_a'::regclass::text;
-- an unanchored statement of the same session reads at the replay position
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from pg_class where relname = 'hs_imp_after_a';
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-- a utility-mode session is never anchored, whatever the mode says
-1M: set whpg_hot_standby_snapshot_mode = anchored;
-1M: select count(*) from pg_class where relname = 'hs_imp_after_a';
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;

----------------------------------------------------------------
-- READ COMMITTED follows the publication at its next statement
----------------------------------------------------------------
1: select count(*) from gp_create_restore_point('hs_imp_b');
1: insert into hs_imp_t select generate_series(41, 50);
1: create table hs_imp_after_b(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(51, 60);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_b --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(61, 70);
-- the publication retired hs_imp_a; the session now reads at hs_imp_b
-1S: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select relname from pg_class where relname like 'hs_imp_after_%' order by 1;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_b') as snapshot_at_anchor;

----------------------------------------------------------------
-- REPEATABLE READ pins the anchor of its first snapshot
----------------------------------------------------------------
-1S: begin isolation level repeatable read;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_b') as snapshot_at_anchor;
1: select count(*) from gp_create_restore_point('hs_imp_c');
1: insert into hs_imp_t select generate_series(71, 80);
1: create table hs_imp_after_c(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(81, 90);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_c --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(91, 100);
-- publishing hs_imp_c retired hs_imp_b, which this transaction pinned
-1S: select count(*) from hs_imp_t;
-1S: rollback;
-1S: begin isolation level repeatable read;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_c') as snapshot_at_anchor;
-1S: select relname from pg_class where relname like 'hs_imp_after_%' order by 1;
-1S: commit;

-- the pin does not follow the GUC: while the transaction runs, the name is
-- pointed at an anchor this node never registered; the pinned anchor keeps
-- serving, and only the next transaction fails
1: select count(*) from gp_create_restore_point('hs_imp_d');
1: insert into hs_imp_t select generate_series(101, 110);
1: create table hs_imp_after_d(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(111, 120);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_d --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(121, 130);
-1S: begin isolation level repeatable read;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_d') as snapshot_at_anchor;
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_unknown --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(131, 140);
-1S: select relname from pg_class where relname like 'hs_imp_after_%' order by 1;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_d') as snapshot_at_anchor;
-1S: commit;
-1S: select count(*) from hs_imp_t;
-- an unknown name retired nothing: hs_imp_d is still registered
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_d --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(141, 150);
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_d') as snapshot_at_anchor;

----------------------------------------------------------------
-- A cursor keeps the anchor it was opened on across a publication
----------------------------------------------------------------
-1S: begin;
-1S: declare hs_imp_cur cursor for select relname from pg_class where relname like 'hs_imp_after_%' order by 1;
1: select count(*) from gp_create_restore_point('hs_imp_e');
1: insert into hs_imp_t select generate_series(151, 160);
1: create table hs_imp_after_e(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(161, 170);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_e --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(171, 180);
-1S: fetch all from hs_imp_cur;
-1S: commit;
-1S: select relname from pg_class where relname like 'hs_imp_after_%' order by 1;

----------------------------------------------------------------
-- Isolation levels: an explicit SERIALIZABLE request is refused on a hot
-- standby (upstream behaviour); the serializable default falls back to
-- REPEATABLE READ and is anchored like any other transaction
----------------------------------------------------------------
-1S: begin isolation level serializable;
-1S: rollback;
-1S: set default_transaction_isolation = serializable;
-1S: begin;
-1S: show transaction_isolation;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_e') as snapshot_at_anchor;
-1S: commit;
-1S: reset default_transaction_isolation;

-- an anchored transaction cannot import another snapshot
-1S: begin isolation level repeatable read;
-1S: set transaction snapshot '00000003-0000000A-1';
-1S: rollback;

----------------------------------------------------------------
-- A registered anchor whose file is gone is refused, not read around
----------------------------------------------------------------
!\retcode sdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); rm "$sdir/pg_anchor_snapshots/hs_imp_e";
-- a new session reads the file (a session that already installed this
-- anchor keeps its copy)
-1Sq:
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_imp_t;
-- the registry still lists it: the refusal is the reader's, not a retirement
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
1: select count(*) from gp_create_restore_point('hs_imp_f');
1: insert into hs_imp_t select generate_series(181, 190);
1: create table hs_imp_after_f(a int) distributed by (a);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_f --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(191, 200);
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_f') as snapshot_at_anchor;
-1S: select relname from pg_class where relname like 'hs_imp_after_%' order by 1;

----------------------------------------------------------------
-- An overflowed anchor keeps its xid list: a transaction with more than 64
-- subtransactions in flight at the restore point is invisible to the
-- anchor after it commits (its top-level xid sits in the anchor's list and
-- the overflow flag only routes its subtransactions through pg_subtrans)
----------------------------------------------------------------
-- 70 catalog rows written by 70 subtransactions of one open transaction:
-- past 64 the primary logs an xid-assignment record, which makes the
-- standby drop those subtransactions from its known-xid set (pg_subtrans
-- keeps their parent) and mark the set as overflowed.  A catalog-writing
-- transaction that commits afterwards (a coordinator xid: plain inserts
-- assign none there) moves the standby's xmax past the open one, so the
-- anchor exported next has the open transaction below its xmax: its
-- top-level xid and the unreported tail in the list, sof:1
2: begin;
2: do $$ begin for i in 1..70 loop begin execute format('create table hs_imp_sub_%s(a int) distributed by (a)', i); exception when others then raise; end; end loop; end $$;
1: create table hs_imp_sof_mover(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(241, 250);
1: select count(*) from gp_create_restore_point('hs_imp_sof');
1: insert into hs_imp_t select generate_series(251, 260);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_sof --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(261, 270);
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; f="$sdir/pg_anchor_snapshots/hs_imp_sof"; grep -q '^sof:1$' "$f" && test "$(grep -c '^sxp:' "$f")" -ge 1 && test "$(grep -c '^sxp:' "$f")" -lt 64;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_sof') as snapshot_at_anchor;
-- in flight at the anchor: not visible
-1S: select count(*) from pg_class where relname like 'hs_imp_sub_%';
2: commit;
1: insert into hs_imp_t select generate_series(271, 280);
-- committed after the anchor: still not visible to the anchor, visible at
-- the replay position
-1S: select count(*) from pg_class where relname like 'hs_imp_sub_%';
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from pg_class where relname like 'hs_imp_sub_%';
-1S: set whpg_hot_standby_snapshot_mode = anchored;
2q:

----------------------------------------------------------------
-- An overflowed anchor is not re-registered after a restart when its
-- transactions were still running at the start checkpoint: the start
-- zeroes the pg_subtrans pages from that checkpoint's oldest active xid on,
-- and the assignment records that held the subtransactions' parents lie
-- before the redo start point.  Registering it would show a committed
-- transaction's subtransaction rows while hiding its top-level rows.
----------------------------------------------------------------
2: begin;
2: do $$ begin for i in 1..70 loop begin execute format('create table hs_imp_sub2_%s(a int) distributed by (a)', i); exception when others then raise; end; end loop; end $$;
1: create table hs_imp_sof2_mover(a int) distributed by (a);
1: insert into hs_imp_t select generate_series(201, 210);
1: select count(*) from gp_create_restore_point('hs_imp_sof2');
1: insert into hs_imp_t select generate_series(211, 220);
-- a checkpoint on the primary while the transaction runs records it as the
-- oldest active transaction; a checkpoint on the standby performs the
-- restartpoint at that record, so the next start's redo begins there
1: checkpoint;
1: insert into hs_imp_t select generate_series(221, 230);
-1M: checkpoint;
2: commit;
1: insert into hs_imp_t select generate_series(231, 240);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_sof2 --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(241, 250);
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_sof2') as snapshot_at_anchor;
-1S: select count(*) from pg_class where relname like 'hs_imp_sub2_%';
2q:
-1Sq:
-1Mq:
!\retcode pg_ctl restart -W -m immediate -D "$COORDINATOR_DATA_DIRECTORY/../../standby"; sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); for i in $(seq 1 120); do PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_is_in_recovery()' 2> /dev/null | grep -q t && break; sleep 1; done; PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_is_in_recovery()';
-- the anchor was refused at start and its file swept; anchored statements
-- fail closed until the next publication
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots();
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test ! -e "$sdir/pg_anchor_snapshots/hs_imp_sof2" && grep -q 'anchor snapshot for restore point ..hs_imp_sof2.. not re-registered: its overflowed transactions reach past the start checkpoint' "$sdir"/log/*.csv;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_imp_t;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select count(*) from pg_class where relname like 'hs_imp_sub2_%';
-1S: set whpg_hot_standby_snapshot_mode = anchored;

----------------------------------------------------------------
-- After a crash the published anchor is pending until replay confirms its
-- record; anchored statements fail meanwhile and never read a data state
-- older than the anchor
----------------------------------------------------------------
-- The pause at hs_imp_pause lands only if the standby has not reached
-- consistency past it when the record is met again: a clean restart first
-- (its shutdown restartpoint writes every dirty page back, so no later
-- page write can move minRecoveryPoint), then only restore points, inserts
-- and reads until the crash.  The xid-assigning txid_current() moves the
-- standby's xmax past hs_imp_g without dirtying a page, so an unanchored
-- snapshot differs from the anchor afterwards.
-1Sq:
-1Mq:
-- The restarted standby exports nothing until it has replayed a running-xacts
-- record (its snapshot is incomplete before that, and a utility connection is
-- admitted as soon as the walreceiver runs, so a connection proves nothing):
-- a checkpoint on the primary writes that record, the barrier waits for its
-- replay, and the standby's own checkpoint places the restartpoint there,
-- before the two restore points below
!\retcode pg_ctl restart -w -m fast -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
1: checkpoint;
1: insert into hs_imp_t select generate_series(301, 310);
-1M: checkpoint;
1: select count(*) from gp_create_restore_point('hs_imp_pause');
1: insert into hs_imp_t select generate_series(251, 260);
1: select count(*) from gp_create_restore_point('hs_imp_g');
1: insert into hs_imp_t select generate_series(261, 270);
1: select txid_current() is not null;
1: insert into hs_imp_t select generate_series(271, 280);
!\retcode gpconfig -c whpg_hot_standby_anchor_name -v hs_imp_g --skipvalidation;
!\retcode gpstop -u;
1: insert into hs_imp_t select generate_series(281, 290);
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_g') as snapshot_at_anchor;
-1S: set whpg_hot_standby_snapshot_mode = unanchored;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) > (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_g') as unanchored_past_anchor;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1Sq:
-1Mq:
!\retcode echo "gp_pause_on_restore_point_replay = 'hs_imp_pause'" >> "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
-- restart (not stop + start) keeps the postmaster options gpstart gave it;
-- -W because a paused startup process never asks for the walreceiver pg_ctl
-- would wait for; hot standby opens at once, so wait for the pause itself
!\retcode pg_ctl restart -W -m immediate -D "$COORDINATOR_DATA_DIRECTORY/../../standby"; sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); for i in $(seq 1 60); do PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_is_wal_replay_paused()' 2> /dev/null | grep -q t && break; sleep 1; done; PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_is_wal_replay_paused()';
-- paused before hs_imp_g: the anchor is pending, not registered
-1M: select pg_is_wal_replay_paused();
-1M: select count(*) from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_g';
!\retcode sdir="$COORDINATOR_DATA_DIRECTORY/../../standby"; test -s "$sdir/pg_anchor_snapshots/hs_imp_g" && grep -q 'anchor snapshot for restore point ..hs_imp_g.. held pending until replay reaches' "$sdir"/log/*.csv;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from hs_imp_t;
-- resume: replay meets the record, the anchor is registered from its file,
-- and the same session's next statement reads it
!\retcode sed -i '/hs_imp_pause/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
!\retcode pg_ctl reload -D "$COORDINATOR_DATA_DIRECTORY/../../standby";
-1M: select pg_wal_replay_resume();
1: insert into hs_imp_t select generate_series(291, 300);
-1M: select rp_name from gp_toolkit.whpg_anchor_snapshots() order by 1;
-1S: select txid_snapshot_xmin(txid_current_snapshot()) = (select xmin::text::bigint from gp_toolkit.whpg_anchor_snapshots() where rp_name = 'hs_imp_g') as snapshot_at_anchor;
-1S: select relname from pg_class where relname like 'hs_imp_after_%' order by 1;

----------------------------------------------------------------
-- Outside recovery the anchored GUCs are inert: the primary, which carries
-- the same anchor name in its configuration, takes ordinary snapshots
----------------------------------------------------------------
1: show whpg_hot_standby_anchor_name;
1: set whpg_hot_standby_snapshot_mode = anchored;
1: select count(*) from hs_imp_t;
1: select count(*) from pg_class where relname like 'hs_imp_after_%';
1: reset whpg_hot_standby_snapshot_mode;

-- the GUC is known to a stopped data directory's binary (the capability
-- probe of the tooling); the server default is unanchored
!\retcode sdir=$(psql -d postgres -Atc "select datadir from gp_segment_configuration where content = -1 and role = 'm'"); test "$(postgres -C whpg_hot_standby_snapshot_mode -D "$sdir")" = unanchored;

----------------------------------------------------------------
-- With the subsystem off (whpg_max_anchor_snapshots = 0) anchored mode is
-- inert as well: today's snapshots, no error
----------------------------------------------------------------
1q:
-1Sq:
-1Mq:
!\retcode gpconfig -c whpg_max_anchor_snapshots -v 0 --skipvalidation;
!\retcode gpstop -ar;
-1S: set whpg_hot_standby_snapshot_mode = anchored;
-1S: select count(*) from gp_toolkit.whpg_anchor_snapshots();
-1S: select count(*) from pg_class where relname like 'hs_imp_after_%';
-1S: show whpg_hot_standby_anchor_name;

----------------------------------------------------------------
-- Cleanup: default capacity, no published anchor
----------------------------------------------------------------
1: drop table hs_imp_t, hs_imp_after_a, hs_imp_after_b, hs_imp_after_c, hs_imp_after_d, hs_imp_after_e, hs_imp_after_f, hs_imp_sof_mover, hs_imp_sof2_mover;
1: do $$ begin for i in 1..70 loop execute format('drop table hs_imp_sub_%s', i); execute format('drop table hs_imp_sub2_%s', i); end loop; end $$;
1q:
-1Sq:
!\retcode gpconfig -r whpg_max_anchor_snapshots --skipvalidation;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode gpstop -ar;
