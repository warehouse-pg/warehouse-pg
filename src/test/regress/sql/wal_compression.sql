-- start_ignore
DROP TABLE IF EXISTS wal_compression_fail;
-- end_ignore
SET wal_compression = on;
SELECT gp_inject_fault('xlog_compression_fail', 'skip', dbid) FROM gp_segment_configuration WHERE role = 'p';
CREATE TABLE wal_compression_fail(c1 int);
INSERT INTO wal_compression_fail SELECT generate_series(1,100);
DROP TABLE IF EXISTS wal_compression_fail;
SELECT gp_inject_fault('xlog_compression_fail', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p';
RESET wal_compression;
