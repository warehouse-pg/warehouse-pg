-- WHPG (PTT-2078): DROP DATABASE that fails on one segment
-- after the other segments have already run dropdb().
--
-- The distributed transaction aborts, and the database must come back intact
-- and usable on every node: base/<oid> is still there on every primary and
-- mirror, and the data can be read and written again, including through
-- utility-mode connections to the segments that had run dropdb().
-- A DROP that commits removes base/<oid> on every primary, and on every
-- mirror once the commit record is replayed.

CREATE EXTENSION IF NOT EXISTS gp_inject_fault;
CREATE DATABASE dropdb_abort_a;
CREATE DATABASE dropdb_abort_b;
1:@db_name dropdb_abort_a: CREATE TABLE t AS SELECT g FROM generate_series(1, 100) g DISTRIBUTED BY (g);
1q:
1:@db_name dropdb_abort_b: CREATE TABLE t AS SELECT g FROM generate_series(1, 100) g DISTRIBUTED BY (g);
1q:

-- Remember the OIDs: the directory checks below must work after a DROP has
-- removed the pg_database row.
CREATE TABLE dropdb_abort_oids AS SELECT datname, oid AS dboid FROM pg_database WHERE datname LIKE 'dropdb_abort_%' DISTRIBUTED REPLICATED;

-- Does base/<oid> exist in the data directory of the coordinator and of
-- every primary and mirror segment? (All instances run on this host.)
CREATE VIEW dropdb_abort_dirs AS SELECT o.datname, c.content, c.role, coalesce((pg_stat_file(c.datadir || '/base/' || o.dboid, true)).isdir, false) AS has_dir FROM gp_segment_configuration c, dropdb_abort_oids o WHERE c.content >= 0 OR c.role = 'p';

-- Case 1: a utility-mode connection on segment 0 is connected to the
-- database. Segment 0 refuses the DROP because the database is in use; the
-- other segments have run dropdb() by then.
0U:@db_name dropdb_abort_a: SELECT 1;
2: DROP DATABASE dropdb_abort_a;
0Uq:

-- The database is intact everywhere.
2: SELECT gp_segment_id, count(*) FROM gp_dist_random('pg_database') WHERE datname = 'dropdb_abort_a' GROUP BY 1 ORDER BY 1;
2: SELECT content, role, has_dir FROM dropdb_abort_dirs WHERE datname = 'dropdb_abort_a' ORDER BY content, role;

-- ... and usable: read and write it through the coordinator, and connect to
-- each segment directly, which reads its catalogs from disk.
1:@db_name dropdb_abort_a: SELECT count(*) FROM t;
1: INSERT INTO t SELECT g FROM generate_series(101, 200) g;
1: SELECT count(*) FROM t;
1q:
0U:@db_name dropdb_abort_a: SELECT count(*) FROM pg_class WHERE relname = 't';
0Uq:
1U:@db_name dropdb_abort_a: SELECT count(*) FROM pg_class WHERE relname = 't';
1Uq:
2U:@db_name dropdb_abort_a: SELECT count(*) FROM pg_class WHERE relname = 't';
2Uq:

-- Case 2: every segment runs dropdb() to completion and segment 1 then fails
-- to PREPARE, so the trigger does not depend on an administrative connection.
2: SELECT gp_inject_fault('start_prepare', 'error', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
2: DROP DATABASE dropdb_abort_b;
2: SELECT gp_inject_fault('start_prepare', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;

2: SELECT gp_segment_id, count(*) FROM gp_dist_random('pg_database') WHERE datname = 'dropdb_abort_b' GROUP BY 1 ORDER BY 1;
2: SELECT content, role, has_dir FROM dropdb_abort_dirs WHERE datname = 'dropdb_abort_b' ORDER BY content, role;
1:@db_name dropdb_abort_b: SELECT count(*) FROM t;
1q:
1U:@db_name dropdb_abort_b: SELECT count(*) FROM pg_class WHERE relname = 't';
1Uq:

-- Case 3: a DROP that commits removes the directories on the coordinator and
-- on every primary, and the mirrors remove theirs when they replay the
-- commit record.
2: DROP DATABASE dropdb_abort_a;
2: DROP DATABASE dropdb_abort_b;
2: SELECT count(*) FROM pg_database WHERE datname LIKE 'dropdb_abort_%';
2: SELECT count(*) FROM gp_dist_random('pg_database') WHERE datname LIKE 'dropdb_abort_%';
2: SELECT content, wait_for_replication_replay(content, 5000) FROM gp_segment_configuration WHERE role = 'p' AND content >= 0 ORDER BY content;
2: SELECT datname, content, role, has_dir FROM dropdb_abort_dirs ORDER BY datname, content, role;

DROP VIEW dropdb_abort_dirs;
DROP TABLE dropdb_abort_oids;
