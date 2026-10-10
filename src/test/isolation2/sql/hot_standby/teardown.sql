-- reset the setup for hot standby tests
!\retcode gpconfig -r hot_standby;
!\retcode gpconfig -r synchronous_commit;
!\retcode gpconfig -r max_standby_streaming_delay;
-- what an anchor suite that aborted midway can leave behind
1: select gp_inject_fault('all', 'reset', dbid) from gp_segment_configuration where role = 'm';
1q:
!\retcode gpconfig -r whpg_hot_standby_snapshot_mode --skipvalidation;
!\retcode gpconfig -r whpg_hot_standby_anchor_name --skipvalidation;
!\retcode sed -i '/^hot_standby = off$/d; /^whpg_hot_standby_snapshot_mode = unanchored$/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf";
-- a run that died inside an anchor case may have left the standby paused
-- on a restore point; a paused standby would wedge every remote_apply
-- commit and the restart below
!\retcode sed -i '/gp_pause_on_restore_point_replay/d' "$COORDINATOR_DATA_DIRECTORY/../../standby/postgresql.conf"; pg_ctl reload -D "$COORDINATOR_DATA_DIRECTORY/../../standby"; sport=$(psql -d postgres -Atc "select port from gp_segment_configuration where content = -1 and role = 'm'"); PGOPTIONS='-c gp_role=utility' psql -p "$sport" -d postgres -Atc 'select pg_wal_replay_resume()' > /dev/null 2>&1; true;
!\retcode gpstop -ar;
