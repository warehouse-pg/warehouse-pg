\echo Use "CREATE EXTENSION" to load this file. \quit

CREATE FUNCTION extended_protocol_commit_test_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER extended_protocol_commit_test_fdw
  HANDLER extended_protocol_commit_test_fdw_handler;
