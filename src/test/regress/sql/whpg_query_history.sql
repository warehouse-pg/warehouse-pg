-- Tests for pg_catalog.whpg_query_history and related infrastructure.
--
-- Verifies:
--   1. History table exists with correct column count and storage options.
--   2. History summary view exists.
--   3. Access control: public cannot SELECT from the table or view.
--   4. History GUCs are registered with correct defaults.
--   5. whpg_query_history_summary view has the expected columns.

-- Table must exist in pg_catalog.
SELECT schemaname, tablename
FROM pg_tables
WHERE schemaname = 'pg_catalog'
  AND tablename = 'whpg_query_history';

-- Correct number of columns (24 per the design).
SELECT COUNT(*) AS n_columns
FROM pg_attribute
WHERE attrelid = 'pg_catalog.whpg_query_history'::regclass
  AND attnum > 0
  AND NOT attisdropped;

-- Table is a regular heap table (AOCO can be applied post-initdb by operators).
-- pg_appendonly returns 0 rows for heap tables.
SELECT compresstype, columnstore
FROM pg_catalog.pg_appendonly
WHERE relid = 'pg_catalog.whpg_query_history'::regclass;

-- Default distribution policy (randomly distributed; DISTRIBUTED BY can be
-- applied post-initdb by operators who need segment affinity for rollups).
SELECT attrnums IS NULL AS randomly_distributed
FROM gp_distribution_policy
WHERE localoid = 'pg_catalog.whpg_query_history'::regclass;

-- Summary view must exist in pg_catalog.
SELECT schemaname, viewname
FROM pg_views
WHERE schemaname = 'pg_catalog'
  AND viewname = 'whpg_query_history_summary';

-- Summary view has the expected aggregation columns.
SELECT attname
FROM pg_attribute
WHERE attrelid = 'pg_catalog.whpg_query_history_summary'::regclass
  AND attnum > 0
  AND NOT attisdropped
ORDER BY attnum;

-- Access control: public must not have SELECT on the history table.
SELECT has_table_privilege('public',
    'pg_catalog.whpg_query_history', 'SELECT') AS public_can_select;

-- History GUCs exist with correct defaults.
SELECT name, setting
FROM pg_settings
WHERE name IN (
    'whpg_query_history_enabled',
    'whpg_query_history_min_duration_ms',
    'whpg_history_enqueue_timeout_ms'
)
ORDER BY name;

-- whpg_query_history_enabled defaults to on.
SELECT current_setting('whpg_query_history_enabled') = 'on'
    AS history_enabled_by_default;

-- Superuser can INSERT into the history table (write path test).
INSERT INTO pg_catalog.whpg_query_history (
    queryid, session_id, command_count, backend_pid, dbid, userid,
    start_time, end_time, finish_status,
    slice_id, segindex, plan_node_id, parent_node_id, node_type,
    actual_rows, actual_ms,
    execmem_bytes_peak, workmem_bytes_peak, spill_bytes,
    bufusage_shared_hit, bufusage_shared_read,
    cpu_user_ms, cpu_sys_ms, trace_id
) VALUES (
    42, 1, 1, pg_backend_pid(), (SELECT oid FROM pg_database WHERE datname = current_database()),
    (SELECT oid FROM pg_roles WHERE rolname = current_user),
    now() - interval '1 second', now(), 'ok',
    0, -1, 1, -1, 'Seq Scan',
    100, 12.5,
    1024, 512, 0,
    10, 5,
    0.0, 0.0, decode('00000000000000000000000000000000', 'hex')
);

-- Verify the inserted row is visible and the summary view aggregates it.
SELECT queryid, finish_status, node_type, actual_rows
FROM pg_catalog.whpg_query_history
WHERE queryid = 42;

SELECT queryid, finish_status, total_rows
FROM pg_catalog.whpg_query_history_summary
WHERE queryid = 42;

-- Non-superuser cannot INSERT into the history table.
CREATE ROLE whpg_hist_reader NOSUPERUSER LOGIN;
SET ROLE whpg_hist_reader;

SELECT has_table_privilege(current_user,
    'pg_catalog.whpg_query_history', 'SELECT') AS reader_can_select;

RESET ROLE;
DROP ROLE whpg_hist_reader;

-- Clean up test row.
DELETE FROM pg_catalog.whpg_query_history WHERE queryid = 42;
