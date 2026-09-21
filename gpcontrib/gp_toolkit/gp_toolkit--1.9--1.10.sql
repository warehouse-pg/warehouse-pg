/* gpcontrib/gp_toolkit/gp_toolkit--1.9--1.10.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION gp_toolkit UPDATE TO '1.10'" to load this file. \quit

--------------------------------------------------------------------------------
-- @function:
--        gp_toolkit.whpg_anchor_snapshots
--
-- @out:
--        text - restore point name
--        xid  - the anchor's xmin
--
-- @doc:
--        The anchor snapshots registered on the node the function runs on:
--        one row per restore point whose snapshot a hot standby exported at
--        replay, oldest registration first.  A row present means the node
--        holds that anchor's snapshot.  Lives here rather than in pg_catalog so
--        that the catalog version of 7.x data directories does not change.
--
--------------------------------------------------------------------------------

CREATE FUNCTION gp_toolkit.whpg_anchor_snapshots(OUT rp_name text, OUT xmin xid)
RETURNS SETOF record
AS 'gp_toolkit.so', 'whpg_anchor_snapshots'
LANGUAGE C VOLATILE;

GRANT EXECUTE ON FUNCTION gp_toolkit.whpg_anchor_snapshots() TO public;
