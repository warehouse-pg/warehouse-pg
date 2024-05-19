/* gpcontrib/gp_toolkit/gp_toolkit--1.6--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION gp_toolkit UPDATE TO '1.8'" to load this file. \quit

-- the fixed version for view pg_catalog.gp_distributed_xacts
CREATE OR REPLACE FUNCTION gp_toolkit.gp_get_distributed_xacts()
    RETURNS setof record
AS 'gp_toolkit.so', 'gp_distributed_xacts_v2'
    LANGUAGE C VOLATILE;
GRANT EXECUTE ON FUNCTION gp_toolkit.gp_get_distributed_xacts() TO public;

-- the fixed version for function pg_catalog.gp_distributed_xid()
CREATE OR REPLACE FUNCTION gp_toolkit.gp_distributed_xid()
    RETURNS int8
AS 'gp_toolkit.so', 'gp_distributed_xid_v2'
    LANGUAGE C VOLATILE;
GRANT EXECUTE ON FUNCTION gp_toolkit.gp_distributed_xid() TO public;

-- the fixed version for view pg_catalog.gp_distributed_log
CREATE OR REPLACE FUNCTION gp_toolkit.gp_get_distributed_log()
    RETURNS setof record
AS 'gp_toolkit.so', 'gp_distributed_log_v2'
    LANGUAGE C VOLATILE;
GRANT EXECUTE ON FUNCTION gp_toolkit.gp_get_distributed_log() TO public;

CREATE OR REPLACE VIEW gp_toolkit.gp_distributed_xacts AS
    SELECT * FROM gp_toolkit.gp_get_distributed_xacts() AS
    L(distributed_xid int8, state text, gp_session_id int, xmin_distributed_snapshot int8);
GRANT SELECT ON gp_toolkit.gp_distributed_xacts TO PUBLIC;

CREATE OR REPLACE VIEW gp_toolkit.gp_distributed_log AS
    SELECT * FROM gp_toolkit.gp_get_distributed_log() AS
    L(segment_id smallint, dbid smallint, distributed_xid int8, status text, local_transaction xid);
GRANT SELECT ON gp_toolkit.gp_distributed_log TO PUBLIC;
