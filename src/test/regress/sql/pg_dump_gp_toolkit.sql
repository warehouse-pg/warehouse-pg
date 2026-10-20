-- pg_dump does not dump the gp_toolkit schema, so the gp_toolkit extension
-- it dumps must restore both into a database created from template0, as
-- pg_dumpall creates them, which has no gp_toolkit, and into one created
-- from template1, which has it already.

-- start_ignore
-- left over from an interrupted run
DROP DATABASE IF EXISTS dump_gp_toolkit_src;
DROP DATABASE IF EXISTS dump_gp_toolkit_t0;
DROP DATABASE IF EXISTS dump_gp_toolkit_t1;
-- end_ignore
CREATE DATABASE dump_gp_toolkit_src TEMPLATE template0;
CREATE DATABASE dump_gp_toolkit_t0 TEMPLATE template0;
CREATE DATABASE dump_gp_toolkit_t1;

\c dump_gp_toolkit_src
CREATE EXTENSION gp_toolkit;
-- A user object that needs gp_toolkit.
CREATE VIEW resgroup_names AS SELECT groupname FROM gp_toolkit.gp_resgroup_config;

\! pg_dump dump_gp_toolkit_src | grep 'CREATE EXTENSION'

-- No output: neither restore has errors.
\! pg_dump dump_gp_toolkit_src | psql -X -q -d dump_gp_toolkit_t0 2>&1 | grep -E 'ERROR|FATAL'
\! pg_dump dump_gp_toolkit_src | psql -X -q -d dump_gp_toolkit_t1 2>&1 | grep -E 'ERROR|FATAL'

\c dump_gp_toolkit_t0
SELECT extname, extnamespace::regnamespace FROM pg_extension WHERE extname = 'gp_toolkit';
SELECT * FROM resgroup_names LIMIT 0;

\c dump_gp_toolkit_t1
SELECT extname, extnamespace::regnamespace FROM pg_extension WHERE extname = 'gp_toolkit';
SELECT * FROM resgroup_names LIMIT 0;

\c regression
DROP DATABASE dump_gp_toolkit_src;
DROP DATABASE dump_gp_toolkit_t0;
DROP DATABASE dump_gp_toolkit_t1;
