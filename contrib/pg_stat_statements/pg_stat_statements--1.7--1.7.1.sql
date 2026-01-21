\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.7.1'" to load this file. \quit

-- WHPG-specific views for distributed statistics

-- View for coordinator and segment statistics
-- Shows stats from coordinator (gp_segment_id = -1) and all segments
-- Segment rows get query text from coordinator via queryid join
CREATE VIEW whpg_stat_statements AS
  SELECT -1 AS gp_segment_id, s.*
  FROM pg_stat_statements s
  UNION ALL
  SELECT
    gp_execution_segment() AS gp_segment_id,
    seg.userid,
    seg.dbid,
    seg.queryid,
    coord.query,
    seg.calls,
    seg.total_time,
    seg.min_time,
    seg.max_time,
    seg.mean_time,
    seg.stddev_time,
    seg.rows,
    seg.shared_blks_hit,
    seg.shared_blks_read,
    seg.shared_blks_dirtied,
    seg.shared_blks_written,
    seg.local_blks_hit,
    seg.local_blks_read,
    seg.local_blks_dirtied,
    seg.local_blks_written,
    seg.temp_blks_read,
    seg.temp_blks_written,
    seg.blk_read_time,
    seg.blk_write_time
  FROM gp_dist_random('pg_stat_statements') seg
  LEFT JOIN pg_stat_statements coord ON seg.queryid = coord.queryid
    AND seg.userid = coord.userid AND seg.dbid = coord.dbid;

GRANT SELECT ON whpg_stat_statements TO PUBLIC;

-- Aggregated view: combines coordinator and segment statistics
CREATE VIEW whpg_stat_statements_aggregated AS
  SELECT
    c.userid,
    c.dbid,
    c.queryid,
    c.query,
    c.calls,
    c.total_time,
    c.min_time,
    c.max_time,
    c.mean_time,
    c.stddev_time,
    c.rows + COALESCE(s.segment_rows, 0) AS rows,
    c.shared_blks_hit + COALESCE(s.segment_shared_blks_hit, 0) AS shared_blks_hit,
    c.shared_blks_read + COALESCE(s.segment_shared_blks_read, 0) AS shared_blks_read,
    c.shared_blks_dirtied + COALESCE(s.segment_shared_blks_dirtied, 0) AS shared_blks_dirtied,
    c.shared_blks_written + COALESCE(s.segment_shared_blks_written, 0) AS shared_blks_written,
    c.local_blks_hit + COALESCE(s.segment_local_blks_hit, 0) AS local_blks_hit,
    c.local_blks_read + COALESCE(s.segment_local_blks_read, 0) AS local_blks_read,
    c.local_blks_dirtied + COALESCE(s.segment_local_blks_dirtied, 0) AS local_blks_dirtied,
    c.local_blks_written + COALESCE(s.segment_local_blks_written, 0) AS local_blks_written,
    c.temp_blks_read + COALESCE(s.segment_temp_blks_read, 0) AS temp_blks_read,
    c.temp_blks_written + COALESCE(s.segment_temp_blks_written, 0) AS temp_blks_written,
    c.blk_read_time + COALESCE(s.segment_blk_read_time, 0) AS blk_read_time,
    c.blk_write_time + COALESCE(s.segment_blk_write_time, 0) AS blk_write_time
  FROM pg_stat_statements c
  LEFT JOIN (
    SELECT
      userid,
      dbid,
      queryid,
      SUM(rows) AS segment_rows,
      SUM(shared_blks_hit) AS segment_shared_blks_hit,
      SUM(shared_blks_read) AS segment_shared_blks_read,
      SUM(shared_blks_dirtied) AS segment_shared_blks_dirtied,
      SUM(shared_blks_written) AS segment_shared_blks_written,
      SUM(local_blks_hit) AS segment_local_blks_hit,
      SUM(local_blks_read) AS segment_local_blks_read,
      SUM(local_blks_dirtied) AS segment_local_blks_dirtied,
      SUM(local_blks_written) AS segment_local_blks_written,
      SUM(temp_blks_read) AS segment_temp_blks_read,
      SUM(temp_blks_written) AS segment_temp_blks_written,
      SUM(blk_read_time) AS segment_blk_read_time,
      SUM(blk_write_time) AS segment_blk_write_time
    FROM gp_dist_random('pg_stat_statements')
    GROUP BY userid, dbid, queryid
  ) s ON c.queryid = s.queryid AND c.userid = s.userid AND c.dbid = s.dbid;

GRANT SELECT ON whpg_stat_statements_aggregated TO PUBLIC;

-- Function to reset pg_stat_statements on coordinator and all segments
CREATE FUNCTION whpg_stat_statements_reset(IN p_userid Oid DEFAULT 0,
    IN p_dbid Oid DEFAULT 0,
    IN p_queryid bigint DEFAULT 0
)
RETURNS void
AS $$
BEGIN
    -- Reset on coordinator
    PERFORM pg_stat_statements_reset(p_userid, p_dbid, p_queryid);
    -- Reset on all segments
    PERFORM pg_stat_statements_reset(p_userid, p_dbid, p_queryid)
    FROM gp_dist_random('gp_id');
END;
$$ LANGUAGE plpgsql;

-- Don't want this to be available to non-superusers.
REVOKE ALL ON FUNCTION whpg_stat_statements_reset(Oid, Oid, bigint) FROM PUBLIC;
