--
-- Errors raised on a segment keep the fields that name the database objects
-- (schema, table, column, datatype, constraint), as errors raised on the
-- coordinator do.  The coordinator rebuilds a segment's error in
-- cdbdisp_get_PQerror(); these fields must be carried over.
--
-- start_matchsubs
-- m/^LOCATION:.*:\d+/
-- s/:\d+/:/
-- end_matchsubs
-- start_ignore
DROP SCHEMA IF EXISTS segment_error_fields CASCADE;
-- end_ignore
CREATE SCHEMA segment_error_fields;
SET search_path TO segment_error_fields;

CREATE DOMAIN testdom AS int4 CHECK (value < 10);
CREATE TABLE testerr (id int NOT NULL, val testdom NOT NULL, c int CHECK (c > 0),
                      PRIMARY KEY (id)) DISTRIBUTED BY (id);
INSERT INTO testerr VALUES (1, 1, 1);

\set VERBOSITY verbose
-- raised on the coordinator
SELECT 20::testdom;
-- raised on a segment
INSERT INTO testerr (id, val) VALUES (2, 20);
INSERT INTO testerr (val) VALUES (1);
INSERT INTO testerr VALUES (3, 1, -1);
INSERT INTO testerr VALUES (1, 1, 1);
UPDATE testerr SET c = -1 WHERE id = 1;
COPY testerr FROM STDIN;
4	1	-5
\.
COPY testerr FROM STDIN;
1	1	1
\.
-- an error without object names gets none
SELECT 1 / (c - 1) FROM testerr;
\set VERBOSITY default

RESET search_path;
-- start_ignore
DROP SCHEMA segment_error_fields CASCADE;
-- end_ignore
