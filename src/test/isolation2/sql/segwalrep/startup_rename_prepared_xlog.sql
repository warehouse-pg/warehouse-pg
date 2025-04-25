-- ## Bug Description ##
-- This test targets a bug where a primary segment, during crash recovery, renames the last
-- WAL (XLOG) segment to end with a '.partial' suffix *before* recovering the prepared
-- transactions contained within it. This premature renaming can lead to a segfault when the
-- recovery process (specifically, `RecoverPreparedTransactions`) attempts to read from the
-- no-longer-existing WAL file. This scenario is described in detail on the PostgreSQL hackers
-- mailing list: https://www.postgresql.org/message-id/743b9b45a2d4013bd90b6a5cba8d6faeb717ee34.camel%40cybertec.at
--
-- ## Greenplum/WarehousePG-Specific Challenge ##
-- In the version of PostgreSQL that WarehousePG is based on, this bug is masked by an internal
-- caching mechanism for the last opened WAL segment. To reliably trigger the bug, we must
-- create a more complex scenario: creating unfinished prepared transactions across TWO
-- different WAL segments. The final prepared transaction must reside in the very last
-- WAL segment that will be renamed to '.partial' upon failover.
--
-- ## Test Strategy ##
-- 1. **Setup**: Configure the cluster to enable `archive_mode`, which is a prerequisite for
--    the WAL segment renaming behavior. Also, increase gang creation retry settings to make
--    the test more robust against timing issues during segment recovery.
-- 2. **Create First Prepared TX**: Inject a fault to suspend a 2PC transaction after the PREPARE
--    phase, creating an "in-doubt" prepared transaction.
-- 3. **Switch WAL**: Manually switch the WAL file. This isolates the first transaction's
--    PREPARE record in a now-closed WAL segment.
-- 4. **Create Second Prepared TX**: Create another "in-doubt" prepared transaction. Its
--    PREPARE record will be in the new, current, and *last* WAL segment.
-- 5. **Trigger Failover**: Crash the primary segment to initiate a failover. The mirror will be
--    promoted and begin recovery. This is the critical moment where the bug could manifest.
-- 6. **Verification**: After the new primary is up, verify that it did not crash and that both
--    prepared transactions were successfully recovered and can be committed.
-- 7. **Cleanup**: Restore the cluster to its original state.


-- =================================================================
-- STEP 1: Test Environment Setup
-- =================================================================

-- Increase retry counts for gang creation. This prevents test failures on busy systems
-- where mirror promotion might take longer, by allowing the coordinator to wait longer for
-- segments to become fully ready after recovery.
!\retcode gpconfig -c gp_gang_creation_retry_count -v 120 --skipvalidation --masteronly;
!\retcode gpconfig -c gp_gang_creation_retry_timer -v 1000 --skipvalidation --masteronly;

-- The '.partial' renaming of the last WAL segment only occurs when archive_mode is enabled.
!\retcode gpconfig -c archive_mode -v on;
!\retcode gpconfig -c archive_command -v '/bin/true';

-- Restart the cluster to apply the configuration changes.
!\retcode gpstop -rai;

1: CREATE EXTENSION IF NOT EXISTS gp_inject_fault;
1: CREATE TABLE t_rename1 (a INT);
1: CREATE TABLE t_rename2 (a INT);


-- =================================================================
-- STEP 2: Create Two In-Doubt Transactions in Separate WAL Segments
-- =================================================================

-- Create the first orphaned prepared transaction in the CURRENT WAL segment.
-- We suspend the transaction right after the coordinator broadcasts PREPARE.
1: SELECT gp_inject_fault('dtm_broadcast_prepare', 'suspend', dbid)
   FROM gp_segment_configuration WHERE role = 'p' AND content = -1;

-- Assume (2), (1) are on different segments and one tuple is on the first segment.
-- The test will double-check that.
2&: INSERT INTO t_rename1 VALUES (2), (1);

-- Wait for the fault to be triggered on the coordinator, ensuring the transaction is now in the
-- "prepared" state on the segments.
1: SELECT gp_wait_until_triggered_fault('dtm_broadcast_prepare', 1, dbid)
   FROM gp_segment_configuration WHERE role = 'p' AND content = -1;

-- This is the CRITICAL step to expose the bug. We force a WAL switch.
-- Now, the PREPARE record for the first transaction is in a completed, non-last WAL segment.
-- The next transaction's PREPARE record will be in a new WAL segment.
-- start_ignore
0U: SELECT pg_switch_wal();
-- end_ignore

-- Now, create a second orphaned prepared transaction in the NEW (and currently LAST) WAL segment.
1: SELECT gp_inject_fault('dtm_broadcast_prepare', 'reset', dbid)
   FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1: SELECT gp_inject_fault('dtm_broadcast_prepare', 'suspend', dbid)
   FROM gp_segment_configuration WHERE role = 'p' AND content = -1;

-- This INSERT will also be suspended, with its PREPARE record in the last WAL segment.
3&: INSERT INTO t_rename2 VALUES (2), (1);

-- Wait for the second fault to be triggered.
1: SELECT gp_wait_until_triggered_fault('dtm_broadcast_prepare', 1, dbid)
   FROM gp_segment_configuration WHERE role = 'p' AND content = -1;


-- =================================================================
-- STEP 3: Trigger Segment Failover and Recovery
-- =================================================================

-- Shutdown the primary segment for content 0 using an immediate shutdown.
-- This forces the mirror to perform crash recovery from WAL.
-1U: SELECT pg_ctl((SELECT datadir FROM gp_segment_configuration c
    WHERE c.role='p' AND c.content=0), 'stop', 'immediate');

-- Request the Fault Tolerance Server (FTS) to scan for the downed segment.
1: SELECT gp_request_fts_probe_scan();

-- Verify that the mirror has been promoted to the new primary.
-- The new primary will now be in recovery mode, reading the WAL files. If the bug exists,
-- it will crash at this stage while trying to recover the second prepared transaction.
1: SELECT role, preferred_role FROM gp_segment_configuration WHERE content = 0 ORDER BY role;


-- =================================================================
-- STEP 4: Verification
-- =================================================================

-- Double-check that the original transaction was indeed a 2PC transaction by verifying
-- that its data is distributed across at least two segments.
-- This also implicitly confirms that the new primary is responsive, and the
-- shutdown then recovered segment 0 has data.
1: INSERT INTO t_rename1 VALUES (2), (1);
1: SELECT gp_segment_id, a FROM t_rename1 ORDER BY a;

-- Reset the fault, allowing the coordinator to send the COMMIT PREPARED command for the
-- two orphaned transactions.
1: SELECT gp_inject_fault('dtm_broadcast_prepare', 'reset', dbid)
   FROM gp_segment_configuration WHERE role = 'p' AND content = -1;

-- Wait for the background sessions to complete their commits.
2<:
3<:

-- Confirm that the data from both transactions is now visible, meaning the prepared
-- transactions were successfully recovered and subsequently committed.
1: SELECT * FROM t_rename1 ORDER BY a;
1: SELECT * FROM t_rename2 ORDER BY a;


-- =================================================================
-- STEP 5: Cluster Restoration and Cleanup
-- =================================================================

-- Recover the failed segment (which will become the new mirror).
!\retcode gprecoverseg -a;
1: SELECT wait_until_segment_synchronized(0);

-- Rebalance the cluster to return the original primary to its preferred role.
!\retcode gprecoverseg -ar;
1: SELECT wait_until_segment_synchronized(0);

-- Verify the segment roles are back to their original state.
1: SELECT role, preferred_role FROM gp_segment_configuration WHERE content = 0 ORDER BY role;

-- Final cleanup.
1: DROP TABLE t_rename1;
1: DROP TABLE t_rename2;
!\retcode gpconfig -r gp_gang_creation_retry_count --skipvalidation;
!\retcode gpconfig -r gp_gang_creation_retry_timer --skipvalidation;
!\retcode gpconfig -r archive_mode --skipvalidation;
!\retcode gpconfig -r archive_command --skipvalidation;
!\retcode gpstop -rai;
