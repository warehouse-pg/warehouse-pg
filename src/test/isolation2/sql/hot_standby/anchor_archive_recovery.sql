-- Anchor snapshots on an archive-recovery node: the replay path of a
-- whpg-dr read replica (recovery.signal, WAL fetched by restore_command),
-- next to the streaming standby the rest of the suite uses.
--
-- The node is a pg_basebackup of the primary coordinator fed from that
-- coordinator's WAL archive, held paused at a restore point by
-- gp_pause_on_restore_point_replay.  It carries the source coordinator's
-- dbid (a clone, as a replica is) and starts in dispatch role; its catalog
-- still names this cluster's segments, so it only runs coordinator-local
-- statements (catalog-table scans, the reads anchor_import uses).  A
-- dispatch topology file names the node itself for content -1 (and this
-- cluster's mirrors for the segments, never dispatched to); with it set,
-- once the node is promoted at the end, its FTS and dtx recovery stay
-- dormant instead of acting on this cluster's live segments (see
-- fts_topology_gate).  It is stopped with
-- an immediate shutdown and removed before the test ends.
--
-- Covered: export at restore-point replay, re-registration of the
-- published anchor at restart, publication, an anchored import that reads
-- the restore point's cut while replay has moved on, retirement at the next
-- publication, and promotion under open anchored sessions: a REPEATABLE
-- READ transaction pinned to the anchor fails at its next statement, a
-- READ COMMITTED session is not refused for its anchor.  Its statements
-- after the promotion are refused all the same, by the dispatch topology
-- contract: while the GUC is still set (whpg-dr's promote retires it) a
-- dispatcher out of recovery refuses dispatch sessions (SQLSTATE F0000).
-- Clearing the GUC here would wake the clone's dtx recovery against this
-- cluster's live segments, so the ordinary snapshot after the retirement
-- is left to whpg-dr's live-promote end-to-end test.
--
-- Two more clones of the base backup cover missing WAL: archive recovery
-- held by gp_pause_on_restore_point_replay waits for it, and without the
-- setting still ends at the end of the archive.
--
-- Every statement against the node runs through psql in a shell line; the
-- node is not in gp_segment_configuration, so its log is grepped directly.

!\retcode T=/tmp/hs_arc_${PGPORT}; rm -rf "$T"; mkdir -p "$T/arch"; gpconfig -c archive_mode -v on --coordinatoronly --skipvalidation && gpconfig -c archive_command -v "'cp %p $T/arch/%f'" --coordinatoronly --skipvalidation && gpstop -ar;
1: create table arc_t(a int) distributed by (a);
1: checkpoint;
1: select pg_switch_wal() is not null;
!\retcode T=/tmp/hs_arc_${PGPORT}; pg_basebackup -h localhost -p ${PGPORT} -X stream -D "$T/node" --target-gp-dbid 1;
!\retcode T=/tmp/hs_arc_${PGPORT}; pg_basebackup -h localhost -p ${PGPORT} -X stream -D "$T/wait" --target-gp-dbid 1 && pg_basebackup -h localhost -p ${PGPORT} -X stream -D "$T/nowait" --target-gp-dbid 1;
1: create table arc_before(a int) distributed by (a);
1: select count(*) from gp_create_restore_point('arc_rp1');
1: create table arc_after(a int) distributed by (a);
1: select count(*) from gp_create_restore_point('arc_rp2');
1: create table arc_after2(a int) distributed by (a);
-- switch to a new segment file and wait until the old one is archived
!\retcode T=/tmp/hs_arc_${PGPORT}; w=$(psql -X -d postgres -Atc "select pg_walfile_name(pg_switch_wal())"); for i in $(seq 1 100); do [ -e "$T/arch/$w" ] && exit 0; sleep 0.1; done; exit 1;

----------------------------------------------------------------
-- Start the node paused at arc_rp1, anchored mode on, nothing published:
-- the restore point is exported when replay meets it
----------------------------------------------------------------
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 90)); printf '%s\n' "port = $N" "hot_standby = on" "restore_command = 'cp $T/arch/%f %p'" "gp_pause_on_restore_point_replay = 'arc_rp1'" "whpg_hot_standby_snapshot_mode = anchored" "whpg_hot_standby_anchor_name = ''" "whpg_dispatch_topology_file = 'hs_arc_topology'" "archive_mode = off" "synchronous_standby_names = ''" >> "$T/node/postgresql.conf"; : > "$T/node/postgresql.auto.conf"; { echo "-1 1 localhost localhost $N $T/node"; psql -X -At -d postgres -c "select content, dbid, hostname, address, port, datadir from gp_segment_configuration where role = 'm' and content >= 0 order by content" | tr '|' ' '; } > "$T/node/hs_arc_topology"; touch "$T/node/recovery.signal"; pg_ctl start -D "$T/node" -l "$T/node.log" -o "-c gp_role=dispatch" -W > /dev/null;
!\retcode N=$((PGPORT + 90)); for i in $(seq 1 200); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_wal_replay_paused()' 2>/dev/null)" = t ] && exit 0; sleep 0.1; done; exit 1;
!\retcode N=$((PGPORT + 90)); test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select string_agg(rp_name, $$,$$ order by rp_name) from gp_toolkit.whpg_anchor_snapshots()')" = arc_rp1;
!\retcode T=/tmp/hs_arc_${PGPORT}; grep -q 'exported anchor snapshot for restore point ..arc_rp1..' "$T"/node/log/*.csv;
-- nothing published yet: an anchored statement is refused
!\retcode N=$((PGPORT + 90)); psql -X -v VERBOSITY=verbose -p $N -d isolation2-hot-standby -Atc "select count(*) from pg_class where relname like 'arc\_%'" 2>&1 | grep -q '^ERROR:  55000: anchored read requires a published anchor snapshot$';

----------------------------------------------------------------
-- Publish arc_rp1, restart the node: the published anchor is
-- re-registered from its file, and anchored reads see its cut
----------------------------------------------------------------
!\retcode T=/tmp/hs_arc_${PGPORT}; sed -i "s/^whpg_hot_standby_anchor_name = ''$/whpg_hot_standby_anchor_name = 'arc_rp1'/" "$T/node/postgresql.conf"; pg_ctl restart -D "$T/node" -l "$T/node.log" -m fast -o "-c gp_role=dispatch" -W > /dev/null;
!\retcode N=$((PGPORT + 90)); for i in $(seq 1 200); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_wal_replay_paused()' 2>/dev/null)" = t ] && exit 0; sleep 0.1; done; exit 1;
!\retcode N=$((PGPORT + 90)); test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select string_agg(rp_name, $$,$$ order by rp_name) from gp_toolkit.whpg_anchor_snapshots()')" = arc_rp1;
!\retcode N=$((PGPORT + 90)); test "$(psql -X -p $N -d isolation2-hot-standby -Atc "select string_agg(relname, ',' order by relname) from pg_class where relname like 'arc\_%'")" = arc_before,arc_t;

----------------------------------------------------------------
-- Replay moves on to arc_rp2: anchored reads stay at arc_rp1, unanchored
-- ones read the replay position; publishing arc_rp2 moves the anchored
-- reads and retires arc_rp1
----------------------------------------------------------------
-- The new pause target must be in effect before replay resumes: a new
-- session showing it proves the postmaster re-read the file and signalled
-- the startup process, which applies it before replaying another record.
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 90)); sed -i "s/^gp_pause_on_restore_point_replay = 'arc_rp1'$/gp_pause_on_restore_point_replay = 'arc_rp2'/" "$T/node/postgresql.conf"; pg_ctl reload -D "$T/node" > /dev/null; ok=0; for i in $(seq 1 100); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'show gp_pause_on_restore_point_replay')" = arc_rp2 ] && { ok=1; break; }; sleep 0.1; done; [ $ok = 1 ] || exit 1; PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_wal_replay_resume()' > /dev/null; for i in $(seq 1 200); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_wal_replay_paused()')" = t ] && [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select count(*) from gp_toolkit.whpg_anchor_snapshots()')" = 2 ] && exit 0; sleep 0.1; done; exit 1;
!\retcode N=$((PGPORT + 90)); test "$(psql -X -p $N -d isolation2-hot-standby -Atc "select string_agg(relname, ',' order by relname) from pg_class where relname like 'arc\_%'")" = arc_before,arc_t;
!\retcode N=$((PGPORT + 90)); test "$(PGOPTIONS='-c whpg_hot_standby_snapshot_mode=unanchored' psql -X -p $N -d isolation2-hot-standby -Atc "select string_agg(relname, ',' order by relname) from pg_class where relname like 'arc\_%'")" = arc_after,arc_before,arc_t;
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 90)); sed -i "s/^whpg_hot_standby_anchor_name = 'arc_rp1'$/whpg_hot_standby_anchor_name = 'arc_rp2'/" "$T/node/postgresql.conf"; pg_ctl reload -D "$T/node" > /dev/null; for i in $(seq 1 100); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select string_agg(rp_name, $$,$$ order by rp_name) from gp_toolkit.whpg_anchor_snapshots()')" = arc_rp2 ] && exit 0; sleep 0.1; done; exit 1;
!\retcode N=$((PGPORT + 90)); test "$(psql -X -p $N -d isolation2-hot-standby -Atc "select string_agg(relname, ',' order by relname) from pg_class where relname like 'arc\_%'")" = arc_after,arc_before,arc_t;

----------------------------------------------------------------
-- Promotion under open anchored sessions: a REPEATABLE READ transaction
-- pinned to arc_rp2 fails at its next statement once recovery has ended
-- (55000); a READ COMMITTED session gets no anchor error, only the
-- topology refusal of the promote window (F0000, see above).  Both
-- sessions stay connected across the promotion (one psql each; the
-- promotion and the resume that lets the startup process act on it run
-- from inside the REPEATABLE READ session, the READ COMMITTED one waits
-- for recovery to end).
----------------------------------------------------------------
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 90)); q="select string_agg(relname, ',' order by relname) from pg_class where relname like 'arc\_%';"; printf '%s\n' '\set VERBOSITY verbose' 'select 1 as rc_before;' "$q" "\\! for i in \$(seq 1 300); do [ \"\$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery()')\" = f ] && break; sleep 0.1; done" 'select 2 as rc_after;' "$q" > "$T/rc.sql"; printf '%s\n' '\set VERBOSITY verbose' 'begin isolation level repeatable read;' "$q" "\\! pg_ctl promote -W -D $T/node > /dev/null; PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_wal_replay_resume()' > /dev/null; for i in \$(seq 1 200); do [ \"\$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery()')\" = f ] && break; sleep 0.1; done" "$q" 'rollback;' > "$T/rr.sql"; psql -X -q -p $N -d isolation2-hot-standby -At -f "$T/rc.sql" > "$T/rc.out" 2>&1 & sleep 1; psql -X -q -p $N -d isolation2-hot-standby -At -f "$T/rr.sql" > "$T/rr.out" 2>&1; wait; true;
-- the pinned transaction: its cut, then the refusal
!\retcode T=/tmp/hs_arc_${PGPORT}; { grep -q 'ERROR:  55000: anchor snapshot "arc_rp2" pinned by this transaction is no longer registered$' "$T/rr.out" && test "$(head -1 "$T/rr.out")" = arc_after,arc_before,arc_t && test "$(grep -cE '^arc_' "$T/rr.out")" = 1; } || { cat "$T/rr.out"; false; };
-- the READ COMMITTED session: the anchor's cut before the promotion;
-- after it no anchor error, the topology refusal of the promote window
!\retcode T=/tmp/hs_arc_${PGPORT}; { test "$(sed -n 2p "$T/rc.out")" = arc_after,arc_before,arc_t && ! grep -q '55000' "$T/rc.out" && grep -q 'ERROR:  F0000: whpg_dispatch_topology_file is set, but this dispatcher is not a hot standby$' "$T/rc.out"; } || { cat "$T/rc.out"; false; };
-- the promoted node holds no anchor, and its anchor directory is empty
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 90)); test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery(), count(*) from gp_toolkit.whpg_anchor_snapshots()')" = 'f|0' && test -z "$(ls -A "$T/node/pg_anchor_snapshots")";

----------------------------------------------------------------
-- Missing WAL does not end an archive recovery held at a restore point.
-- Two more clones of the same base backup, started in utility role: with
-- gp_pause_on_restore_point_replay set, a node whose restore_command
-- cannot deliver the next segment keeps waiting in recovery (a WARNING
-- names the position) instead of selecting a new timeline; without the
-- setting, archive recovery still ends at the end of the archive.
----------------------------------------------------------------
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 91)); printf '%s\n' "port = $N" "hot_standby = on" "restore_command = 'cp $T/arch/%f %p'" "gp_pause_on_restore_point_replay = 'arc_rp1'" "archive_mode = off" "synchronous_standby_names = ''" >> "$T/wait/postgresql.conf"; : > "$T/wait/postgresql.auto.conf"; touch "$T/wait/recovery.signal"; pg_ctl start -D "$T/wait" -l "$T/wait.log" -o "-c gp_role=utility" -W > /dev/null;
!\retcode N=$((PGPORT + 91)); for i in $(seq 1 200); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_wal_replay_paused()' 2>/dev/null)" = t ] && exit 0; sleep 0.1; done; exit 1;
-- a pause target the node has already passed: replay runs to the end of
-- the archive and waits there, in recovery, on the original timeline
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 91)); PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_wal_replay_resume()' > /dev/null; for i in $(seq 1 300); do grep -q 'waiting for WAL at ' "$T"/wait/log/*.csv && break; sleep 0.1; done; sleep 2; test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery()')" = t && grep -q 'archive recovery does not end while gp_pause_on_restore_point_replay is set (""arc_rp1"")' "$T"/wait/log/*.csv && ! grep -q 'selected new timeline ID' "$T"/wait/log/*.csv;
-- a fast shutdown while waiting completes
!\retcode T=/tmp/hs_arc_${PGPORT}; pg_ctl stop -D "$T/wait" -m fast -w -t 60 > /dev/null;
-- a pause target not in the archive yet: the node waits, then replays to
-- it and pauses there once its WAL is archived
!\retcode T=/tmp/hs_arc_${PGPORT}; sed -i "s/^gp_pause_on_restore_point_replay = 'arc_rp1'$/gp_pause_on_restore_point_replay = 'arc_rp3'/" "$T/wait/postgresql.conf"; N=$((PGPORT + 91)); pg_ctl start -D "$T/wait" -l "$T/wait.log" -o "-c gp_role=utility" -W > /dev/null; for i in $(seq 1 300); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc "select pg_is_in_recovery(), pg_is_wal_replay_paused(), wait_event from pg_stat_activity where backend_type = 'startup'" 2>/dev/null)" = 't|f|RecoveryWalAll' ] && exit 0; sleep 0.1; done; exit 1;
1: create table arc_after3(a int) distributed by (a);
1: select count(*) from gp_create_restore_point('arc_rp3');
!\retcode T=/tmp/hs_arc_${PGPORT}; w=$(psql -X -d postgres -Atc "select pg_walfile_name(pg_switch_wal())"); for i in $(seq 1 100); do [ -e "$T/arch/$w" ] && exit 0; sleep 0.1; done; exit 1;
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 91)); for i in $(seq 1 300); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_wal_replay_paused()')" = t ] && break; sleep 0.1; done; test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery(), pg_is_wal_replay_paused()')" = 't|t' && grep -q 'setting recovery pause at restore point ..arc_rp3..' "$T"/wait/log/*.csv;
-- a promotion request while waiting ends recovery
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 91)); sed -i "s/^gp_pause_on_restore_point_replay = 'arc_rp3'$/gp_pause_on_restore_point_replay = 'arc_rp4'/" "$T/wait/postgresql.conf"; pg_ctl reload -D "$T/wait" > /dev/null; ok=0; for i in $(seq 1 100); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'show gp_pause_on_restore_point_replay')" = arc_rp4 ] && { ok=1; break; }; sleep 0.1; done; [ $ok = 1 ] || exit 1; PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_wal_replay_resume()' > /dev/null; ok=0; for i in $(seq 1 300); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc "select pg_is_in_recovery(), wait_event from pg_stat_activity where backend_type = 'startup'" 2>/dev/null)" = 't|RecoveryWalAll' ] && { ok=1; break; }; sleep 0.1; done; [ $ok = 1 ] || exit 1; pg_ctl promote -D "$T/wait" -w -t 60 > /dev/null && test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery()')" = f;
-- without the setting, archive recovery ends at the end of the archive
!\retcode T=/tmp/hs_arc_${PGPORT}; N=$((PGPORT + 92)); printf '%s\n' "port = $N" "hot_standby = on" "restore_command = 'cp $T/arch/%f %p'" "archive_mode = off" "synchronous_standby_names = ''" >> "$T/nowait/postgresql.conf"; : > "$T/nowait/postgresql.auto.conf"; touch "$T/nowait/recovery.signal"; pg_ctl start -D "$T/nowait" -l "$T/nowait.log" -o "-c gp_role=utility" -W > /dev/null; for i in $(seq 1 300); do [ "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery()' 2>/dev/null)" = f ] && break; sleep 0.1; done; test "$(PGOPTIONS='-c gp_role=utility' psql -X -p $N -d postgres -Atc 'select pg_is_in_recovery()')" = f && grep -q 'selected new timeline ID' "$T"/nowait/log/*.csv && ! grep -q 'waiting for WAL at' "$T"/nowait/log/*.csv;

-- cleanup: stop the nodes at once, archiving off again
!\retcode T=/tmp/hs_arc_${PGPORT}; pg_ctl stop -D "$T/node" -m immediate -W > /dev/null; pg_ctl stop -D "$T/wait" -m immediate -W > /dev/null; pg_ctl stop -D "$T/nowait" -m immediate -W > /dev/null; rm -rf "$T";
1q:
!\retcode gpconfig -r archive_mode --coordinatoronly --skipvalidation && gpconfig -r archive_command --coordinatoronly --skipvalidation && gpstop -ar;
1: drop table arc_t, arc_before, arc_after, arc_after2, arc_after3;
