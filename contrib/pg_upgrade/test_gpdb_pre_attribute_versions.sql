-- Composite values and attribute versions for the upgrade test: the objects
-- whose attribute versions and stored values have to survive pg_upgrade.
-- test_gpdb.sh compares the versions before and after the upgrade and runs
-- test_gpdb_post_attribute_versions.sql against the upgraded cluster.
--
-- Run in the regression database: make_tuple_unversioned() and
-- clear_attribute_versions() of the regress library are created there by
-- create_function_1.

DROP SCHEMA IF EXISTS upgrade_attver CASCADE;
CREATE SCHEMA upgrade_attver;
SET search_path = upgrade_attver, public;

-- A stored composite datum with fewer attributes than the type, due to a
-- subsequent ADD ATTRIBUTE.
CREATE TYPE rowver_comp AS (a int);
CREATE TABLE rowver_tbl (i int, c rowver_comp) DISTRIBUTED BY (i);
INSERT INTO rowver_tbl VALUES (1, '(1)');
ALTER TYPE rowver_comp ADD ATTRIBUTE b int;
INSERT INTO rowver_tbl VALUES (2, '(2,22)');

-- The same in append-optimized tables.
CREATE TABLE rowver_ao (i int, c rowver_comp) WITH (appendonly=true) DISTRIBUTED BY (i);
CREATE TABLE rowver_aoco (i int, c rowver_comp) WITH (appendonly=true, orientation=column) DISTRIBUTED BY (i);
INSERT INTO rowver_ao SELECT * FROM rowver_tbl;
INSERT INTO rowver_aoco SELECT * FROM rowver_tbl;

-- A stored composite datum with attributes that are later dropped.
CREATE TABLE rowdrop_src (a int) DISTRIBUTED BY (a);
ALTER TABLE rowdrop_src ADD COLUMN b int;
CREATE TABLE rowdrop_tbl (i int, r rowdrop_src) DISTRIBUTED BY (i);
INSERT INTO rowdrop_tbl VALUES (1, ROW(1,2));
ALTER TABLE rowdrop_src DROP COLUMN b;

-- The same for a composite type, whose versions pg_dump collects separately.
CREATE TYPE rowdropt_comp AS (a int);
ALTER TYPE rowdropt_comp ADD ATTRIBUTE b int;
CREATE TABLE rowdropt_tbl (i int, c rowdropt_comp) DISTRIBUTED BY (i);
INSERT INTO rowdropt_tbl VALUES (1, ROW(1,2));
ALTER TYPE rowdropt_comp DROP ATTRIBUTE b;

-- Restore the attribute versions of a view with a stored datum made narrower
-- than the view by a subsequent CREATE OR REPLACE VIEW.
CREATE VIEW rowview AS SELECT 1 AS a;
CREATE TABLE rowview_tbl (i int, r rowview) DISTRIBUTED BY (i);
INSERT INTO rowview_tbl SELECT 1, v FROM rowview v;
CREATE OR REPLACE VIEW rowview AS SELECT 1 AS a, 2 AS b;

-- A self-referencing view needs its versions restored before its final
-- definition is parsed.  Field selection deforms the constant when the view
-- is planned.
CREATE VIEW rowselfv AS SELECT 'firstver'::text AS a;
CREATE OR REPLACE VIEW rowselfv AS
  SELECT ('(secondver)'::rowselfv).a;

-- One CREATE TABLE creates a partitioned table with all its partitions; each
-- of them has versions of its own.
CREATE TABLE rowpart (a int, b int, c text) DISTRIBUTED BY (a)
  PARTITION BY RANGE (b)
  SUBPARTITION BY LIST (a) SUBPARTITION TEMPLATE
    (SUBPARTITION s1 VALUES (1), SUBPARTITION s2 VALUES (2))
  (START (0) END (2) EVERY (1));
ALTER TABLE rowpart ADD COLUMN d int;
ALTER TABLE rowpart ALTER COLUMN c TYPE varchar(40);
CREATE TABLE rowpart_tbl (i int, r rowpart, leaf rowpart_1_prt_1_2_prt_s1) DISTRIBUTED BY (i);
INSERT INTO rowpart_tbl VALUES (1, ROW(1,0,'c',4), ROW(1,0,'leaf',4));
CREATE TABLE rowpart_aoco (a int, b int, c text)
  WITH (appendonly=true, orientation=column) DISTRIBUTED BY (a)
  PARTITION BY RANGE (b) (START (0) END (4) EVERY (2), DEFAULT PARTITION dflt);
ALTER TABLE rowpart_aoco ADD COLUMN e int;

CREATE TYPE rowunver_comp AS (a int, b int);

-- A table whose column versions are cleared below, to simulate columns
-- created before updating to a release with attribute versioning.
CREATE TABLE rowunvdrop_src (a int, b int, c int) DISTRIBUTED BY (a);
CREATE TABLE rowunvdrop_tbl (i int, r rowunvdrop_src) DISTRIBUTED BY (i);

-- Further relations with a row type, with differing attribute versions, to
-- verify pg_upgrade preserves their versions.
CREATE TABLE rowalt (a int, b int, c int) DISTRIBUTED BY (a);
ALTER TABLE rowalt ALTER COLUMN b TYPE bigint;
CREATE TABLE rowinh_parent (a int) DISTRIBUTED BY (a);
CREATE TABLE rowinh_child (b int) INHERITS (rowinh_parent) DISTRIBUTED BY (a);
ALTER TABLE rowinh_parent ADD COLUMN c int;
CREATE TYPE rowtyped_comp AS (a int, b int);
CREATE TABLE rowtyped_tbl OF rowtyped_comp DISTRIBUTED BY (a);
ALTER TYPE rowtyped_comp ADD ATTRIBUTE c int CASCADE;
CREATE MATERIALIZED VIEW rowmatview AS SELECT 1 AS a, 2 AS b DISTRIBUTED BY (a);
CREATE EXTERNAL WEB TABLE rowext (a int, b text) EXECUTE 'echo 1' ON MASTER FORMAT 'text';
CREATE TYPE rowzero_comp AS ();

-- Clearing the type's versions produces a versioned datum with version zero.
-- This is distinct from an unversioned datum.  A catalog update only reaches
-- the node it is issued on, so the versions are cleared on every node.
SELECT clear_attribute_versions('rowunver_comp'::regclass);
SELECT count(clear_attribute_versions('rowunver_comp'::regclass)) > 0 FROM gp_dist_random('gp_id');
SELECT clear_attribute_versions('rowunvdrop_src'::regclass);
SELECT count(clear_attribute_versions('rowunvdrop_src'::regclass)) > 0 FROM gp_dist_random('gp_id');

CREATE TABLE rowunver_tbl (i int, c rowunver_comp) DISTRIBUTED BY (i);
INSERT INTO rowunver_tbl VALUES (1, '(3,4)');

-- Other attribute options and dropping a column must cope with columns
-- lacking a version.
ALTER TABLE rowunvdrop_src ALTER COLUMN c SET (n_distinct = 5);
INSERT INTO rowunvdrop_tbl VALUES (1, ROW(1, 2, 3));
ALTER TABLE rowunvdrop_src DROP COLUMN b;

-- Values as binaries without attribute versioning left them: without a
-- version.  The values of INSERT ... VALUES are put out as text on their way
-- to the segments, which rejects a datum without a version for a type that
-- has one, so the check is switched off while they are stored.
CREATE TYPE rowold_comp AS (a int);
CREATE TABLE rowold_tbl (key text, c rowold_comp) DISTRIBUTED BY (key);
SET composite_type_mismatch = disabled;
INSERT INTO rowold_tbl
  VALUES ('prefix', make_tuple_unversioned(ROW(0)::rowold_comp));
RESET composite_type_mismatch;
ALTER TYPE rowold_comp ADD ATTRIBUTE b int;
SET composite_type_mismatch = disabled;
INSERT INTO rowold_tbl
  VALUES ('before-alter', make_tuple_unversioned(ROW(1,2)::rowold_comp));
RESET composite_type_mismatch;
SELECT clear_attribute_versions('rowold_comp'::regclass);
SELECT count(clear_attribute_versions('rowold_comp'::regclass)) > 0 FROM gp_dist_random('gp_id');
ALTER TYPE rowold_comp ADD ATTRIBUTE c int;
