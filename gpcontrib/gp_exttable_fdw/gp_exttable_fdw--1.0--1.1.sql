/* gpcontrib/gp_exttable_fdw/gp_exttable_fdw--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION gp_exttable_fdw UPDATE TO '1.1'" to load this file. \quit

-- The pg_exttable view replaces what used to be a catalog table readable by
-- everyone, and the pg_exttable() function behind it is executable by PUBLIC,
-- so let every role read the view too.
GRANT SELECT ON pg_catalog.pg_exttable TO PUBLIC;
