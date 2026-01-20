-- =======================================================================
-- Test Scenario 1: Basic sub-transaction visibility with shared snapshot
-- =======================================================================

1: CREATE TABLE test_sharedsnapshot_subxips_1(c1 int, c2 int);
1: CREATE TABLE test_sharedsnapshot_subxips_2(c1 int, c2 int);

-- Force the use of DSM (Dynamic Shared Memory) for sub-transaction XID arrays
1: SELECT gp_inject_fault('force_sharedsnapshot_subxip_dsm', 'skip', dbid) FROM gp_segment_configuration WHERE content > -1 AND role = 'p';

1: INSERT INTO test_sharedsnapshot_subxips_1 VALUES (1,2),(3,4);

-- Session 2: Start a transaction with a sub-transaction
2: BEGIN;
2: INSERT INTO test_sharedsnapshot_subxips_2 VALUES (1,2);
-- Start a sub-transaction using SAVEPOINT
2: SAVEPOINT p1;
2: INSERT INTO test_sharedsnapshot_subxips_2 VALUES (3,4);

-- Session 3: Advance ShmemVariableCache->latestCompletedXid
-- This ensures session 2's transaction ID can be placed in the xip array
-- of the snapshot, making the test scenario more realistic.
3: CREATE TABLE test_sharedsnapshot_subxips_3(c1 int, c2 int);
3: BEGIN;
3: DROP TABLE test_sharedsnapshot_subxips_3;

-- Verify that session 2 can see both inserts within its own transaction
2: SELECT * FROM test_sharedsnapshot_subxips_2;

-- Critical test: Issue a query that creates a reader gang
-- The reader gang should use a snapshot that correctly reflects session 2's
-- ongoing transaction, including its sub-transaction state.
1: SELECT * FROM test_sharedsnapshot_subxips_1 AS t1 LEFT JOIN test_sharedsnapshot_subxips_2 AS t2 ON t1.c1=t2.c2;

2: COMMIT;

-- Ensure tuple (3,4) inserted in the sub-transaction is visible.
2: SELECT * FROM test_sharedsnapshot_subxips_2;

3: COMMIT;

1: SELECT gp_inject_fault('force_sharedsnapshot_subxip_dsm', 'reset', dbid) FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
1: DROP TABLE test_sharedsnapshot_subxips_1;
1: DROP TABLE test_sharedsnapshot_subxips_2;

-- ===================================================================
-- Test Scenario 2: Complex sub-transaction snapshot synchronization
-- ===================================================================

CREATE TABLE test_sharedsnapshot_subxips_3(a int);
CREATE TABLE test_sharedsnapshot_subxips_4(a int);

1: BEGIN;
1: SELECT txid_current() IS NOT NULL;
1: END;

2: BEGIN;
2: SELECT txid_current() IS NOT NULL;

3: BEGIN;
3: SAVEPOINT s1;
3: TRUNCATE test_sharedsnapshot_subxips_4;

4: BEGIN;
4: SELECT txid_current() IS NOT NULL;
4: END;

5: BEGIN;
5: SELECT txid_current() IS NOT NULL;

6: SELECT * FROM test_sharedsnapshot_subxips_3 JOIN (SELECT oid FROM pg_class) x(a) ON x.a = test_sharedsnapshot_subxips_3.a;

3: SAVEPOINT s2;
3: TRUNCATE test_sharedsnapshot_subxips_4;

1q:
2q:
3q:
4q:
5q:
6q:

DROP TABLE test_sharedsnapshot_subxips_3;
DROP TABLE test_sharedsnapshot_subxips_4;
