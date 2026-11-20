-- WHPG (PTT-2078): DROP DATABASE whose COMMIT PREPARED
-- lands on a mirror promoted between PREPARE and COMMIT PREPARED.
--
-- The promoted node never ran dropdb(): it reconstructed the prepared
-- transaction from WAL, so its shared buffers still hold the database's
-- pages from replay (dirty until the next restartpoint) and its checkpointer
-- may hold fsync requests for them. COMMIT PREPARED must discard both before
-- it removes the directories; otherwise every later checkpoint on that node
-- fails with "could not open file" and the segment cannot shut down cleanly.
--
-- Pattern as in twophase_tolerance_with_mirror_promotion: FTS is made to
-- believe the content-1 primary is down while the coordinator is suspended
-- right before it broadcasts COMMIT PREPARED; the DTM then retries the
-- COMMIT PREPARED against the promoted mirror. The old primary is recovered
-- at the end.

-- start_matchsubs
--
-- m/ERROR:  FTS detected connection lost during dispatch to seg1.*/
-- s/seg\d+.*/seg_DUMMY/gm
--
-- end_matchsubs

CREATE EXTENSION IF NOT EXISTS gp_inject_fault;

!\retcode gpconfig -c gp_fts_probe_retries -v 2 --coordinatoronly;
!\retcode gpconfig -c gp_gang_creation_retry_count -v 127 --skipvalidation --coordinatoronly;
!\retcode gpconfig -c gp_gang_creation_retry_timer -v 250 --skipvalidation --coordinatoronly;
!\retcode gpstop -u;
1: SELECT role, preferred_role, content FROM gp_segment_configuration ORDER BY content, role;

-- A database with enough data for its pages to sit in the mirror's shared
-- buffers when the mirror is promoted.
1: CREATE DATABASE dropdb_promo_db;
1: CREATE TABLE dropdb_promo_oid AS SELECT oid AS dboid FROM pg_database WHERE datname = 'dropdb_promo_db' DISTRIBUTED REPLICATED;
2:@db_name dropdb_promo_db: CREATE TABLE t AS SELECT g, repeat('x', 100) AS pad FROM generate_series(1, 100000) g DISTRIBUTED BY (g);
2q:

-- Suspend the coordinator once every segment has prepared, right before it
-- broadcasts COMMIT PREPARED.
1: SELECT gp_inject_fault_infinite('dtm_broadcast_commit_prepared', 'suspend', dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'p';
1&: DROP DATABASE dropdb_promo_db;
3: SELECT gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'p';

-- Promote the content-1 mirror: it has the prepared DROP from WAL and the
-- database's pages in its buffers.
3: SELECT gp_inject_fault_infinite('fts_handle_message', 'error', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
-- do fts probe request twice to guarantee the fault is triggered
3: SELECT gp_request_fts_probe_scan();
3: SELECT gp_request_fts_probe_scan();
3: SELECT role, preferred_role FROM gp_segment_configuration WHERE content = 1;

-- Let the coordinator go on: the DTM retries COMMIT PREPARED on the promoted
-- mirror and the DROP completes.
3: SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'resume', dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'p';
1<:
1: SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'p';
-- the fault lives in the old primary, which is now the (down) mirror
1: SELECT gp_inject_fault('fts_handle_message', 'reset', dbid) FROM gp_segment_configuration WHERE content = 1 AND preferred_role = 'p';

-- The database is gone everywhere ...
4: SELECT count(*) FROM pg_database WHERE datname = 'dropdb_promo_db';
4: SELECT count(*) FROM gp_dist_random('pg_database') WHERE datname = 'dropdb_promo_db';
-- ... including its directory on the promoted node ...
4: SELECT c.content, c.role, coalesce((pg_stat_file(c.datadir || '/base/' || o.dboid, true)).isdir, false) AS has_dir FROM gp_segment_configuration c, dropdb_promo_oid o WHERE c.content = 1 AND c.role = 'p';
-- ... and the promoted node can still complete a checkpoint, i.e. no buffer
-- or fsync request of the dropped database was left behind the unlinked files.
1U: CHECKPOINT;
1U: SELECT count(*) FROM pg_database WHERE datname = 'dropdb_promo_db';
1Uq:

-- Recover the old primary and bring the pair back to its preferred roles.
4: !\retcode gprecoverseg -aF \-\-no-progress;
4: !\retcode gprecoverseg -ar;

!\retcode gpconfig -r gp_fts_probe_retries --coordinatoronly;
!\retcode gpconfig -r gp_gang_creation_retry_count --skipvalidation --coordinatoronly;
!\retcode gpconfig -r gp_gang_creation_retry_timer --skipvalidation --coordinatoronly;
!\retcode gpstop -u;

-- loop while segments come in sync
select wait_until_all_segments_synchronized();

DROP TABLE dropdb_promo_oid;
