\echo Use "CREATE EXTENSION" to load this file. \quit

CREATE FUNCTION no_rescan_test_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER no_rescan_test_fdw
  HANDLER no_rescan_test_fdw_handler;
