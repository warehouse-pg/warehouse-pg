/* gpcontrib/gp_toolkit/gp_toolkit--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION gp_toolkit UPDATE TO '1.9'" to load this file. \quit

--
-- Fix gp_toolkit.gp_workfile_entries inflating workfile metrics: the view
-- used to join the workfile entries to gp_stat_activity on (sess_id, segid)
-- only, so a single spilling workfile set was repeated for every process of
-- the session's gang on that segment, inflating SUM(size)/SUM(numfiles) in
-- the downstream gp_workfile_usage_per_* views.
--
-- The workfile manager now records the pid of the process that created each
-- workfile set, exposed via the new *_v2 UDFs, and the view joins on it.
-- We have to drop the view and its dependents first, and then recreate them.
--

--------------------------------------------------------------------------------
-- @function:
--        gp_toolkit.__gp_workfile_entries_f_v2
--
-- @in:
--
-- @out:
--        int - segment id
--        text - path to workfile set,
--        bigint - size in bytes,
--        text - type of the spilling operation,
--        int - containing slice,
--        int - sessionid,
--        int - command_cnt,
--        int - number of files,
--        int - pid of the process that created the workfile set
--
-- @doc:
--        UDF to retrieve workfile sets currently present on disk on one segment
--
--------------------------------------------------------------------------------

CREATE FUNCTION gp_toolkit.__gp_workfile_entries_f_v2_on_coordinator()
RETURNS SETOF record
AS '$libdir/gp_workfile_mgr', 'gp_workfile_mgr_cache_entries_v2'
LANGUAGE C VOLATILE EXECUTE ON COORDINATOR;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_entries_f_v2_on_coordinator() TO public;

CREATE FUNCTION gp_toolkit.__gp_workfile_entries_f_v2_on_segments()
RETURNS SETOF record
AS '$libdir/gp_workfile_mgr', 'gp_workfile_mgr_cache_entries_v2'
LANGUAGE C VOLATILE EXECUTE ON ALL SEGMENTS;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_entries_f_v2_on_segments() TO public;

ALTER EXTENSION gp_toolkit DROP VIEW gp_toolkit.gp_workfile_entries;
ALTER EXTENSION gp_toolkit DROP VIEW gp_toolkit.gp_workfile_usage_per_segment;
ALTER EXTENSION gp_toolkit DROP VIEW gp_toolkit.gp_workfile_usage_per_query;
DROP VIEW gp_toolkit.gp_workfile_entries CASCADE;

--------------------------------------------------------------------------------
-- @view:
--        gp_toolkit.gp_workfile_entries
--
-- @doc:
--        List of all the workfile sets currently present on disk
--
--------------------------------------------------------------------------------

CREATE VIEW gp_toolkit.gp_workfile_entries AS
WITH all_entries AS (
    SELECT C.*
        FROM gp_toolkit.__gp_workfile_entries_f_v2_on_coordinator() AS C (
           segid int,
           prefix text,
           size bigint,
           optype text,
           slice int,
           sessionid int,
           commandid int,
           numfiles int,
           pid int
        )
    UNION ALL
    SELECT C.*
        FROM gp_toolkit.__gp_workfile_entries_f_v2_on_segments() AS C (
            segid int,
            prefix text,
            size bigint,
            optype text,
            slice int,
            sessionid int,
            commandid int,
            numfiles int,
            pid int
        ))
SELECT S.datname,
       C.pid,
       C.sessionid as sess_id,
       C.commandid as command_cnt,
       S.usename,
       S.query,
       C.segid,
       C.slice,
       C.optype,
       C.size,
       C.numfiles,
       C.prefix
FROM all_entries C LEFT OUTER JOIN gp_stat_activity S
ON C.sessionid = S.sess_id and C.segid = S.gp_segment_id and C.pid = S.pid;

GRANT SELECT ON gp_toolkit.gp_workfile_entries TO public;

--------------------------------------------------------------------------------
-- @view:
--        gp_toolkit.gp_workfile_usage_per_segment
--
-- @doc:
--        Amount of disk space used for workfiles at each segment
--
--------------------------------------------------------------------------------

CREATE VIEW gp_toolkit.gp_workfile_usage_per_segment AS
SELECT gpseg.content AS segid, COALESCE(SUM(wfe.size),0) AS size,
       SUM(wfe.numfiles) AS numfiles
FROM (
         SELECT content
         FROM gp_segment_configuration
         WHERE role = 'p') gpseg
         LEFT JOIN gp_toolkit.gp_workfile_entries wfe
                   ON (gpseg.content = wfe.segid)
GROUP BY gpseg.content;

GRANT SELECT ON gp_toolkit.gp_workfile_usage_per_segment TO public;

--------------------------------------------------------------------------------
-- @view:
--        gp_toolkit.gp_workfile_usage_per_query
--
-- @doc:
--        Amount of disk space used for workfiles by each query
--
--------------------------------------------------------------------------------

CREATE VIEW gp_toolkit.gp_workfile_usage_per_query AS
SELECT datname, pid, sess_id, command_cnt, usename, query, segid,
       SUM(size) AS size, SUM(numfiles) AS numfiles
FROM gp_toolkit.gp_workfile_entries
GROUP BY datname, pid, sess_id, command_cnt, usename, query, segid;

GRANT SELECT ON gp_toolkit.gp_workfile_usage_per_query TO public;
