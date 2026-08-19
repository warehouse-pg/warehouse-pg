--
-- Tests for ALTER TABLE ... SPLIT DEFAULT PARTITION:
--  1) the new (non-default) partition keeps its literal name instead of
--     falling back to the legacy "<root>_1_prt_<name>" pattern
--  2) the new (non-default) partition inherits storage from the root
--     table, not from the default partition being split
--  3) regular (non-default) SPLIT PARTITION is unaffected by (1)/(2):
--     it keeps the legacy naming and keeps inheriting storage from the
--     partition being split
--  4) SPLIT DEFAULT PARTITION on a default partition with explicit
--     storage is safely observable via a ddl_command_end event trigger
--

-- 1 & 2: naming and storage inheritance for the new (non-default) half
CREATE TABLE split_default_test (id int, d date, val text)
  USING ao_row WITH (compresstype=zstd, compresslevel=1)
  DISTRIBUTED BY (id)
  PARTITION BY RANGE (d);

CREATE TABLE split_default_test_dflt PARTITION OF split_default_test DEFAULT
  USING ao_column WITH (compresstype=zstd, compresslevel=2);

ALTER TABLE split_default_test
  SPLIT DEFAULT PARTITION START ('2020-03-01') END ('2020-04-01')
  INTO (PARTITION split_default_new, DEFAULT PARTITION);

-- the new partition must be named exactly as given, not "..._1_prt_..."
SELECT relname FROM pg_class WHERE relname = 'split_default_new';
SELECT relname FROM pg_class WHERE relname LIKE 'split_default_test_1_prt%';

-- the new partition must inherit the ROOT's storage (ao_row/level 1), not
-- the old default partition's (ao_column/level 2); the recreated default
-- keeps its own original storage.
SELECT c.oid::regclass, amname, reloptions
  FROM pg_class c LEFT JOIN pg_am am ON am.oid = c.relam
  WHERE c.oid IN (SELECT oid FROM pg_class
                  WHERE relname IN ('split_default_new', 'split_default_test_dflt'))
  ORDER BY 1::text;

DROP TABLE split_default_test;

-- 3: regular (non-default) SPLIT PARTITION must be unaffected
CREATE TABLE split_regular_test (id int, d date, val text)
  USING ao_row WITH (compresstype=zstd, compresslevel=1)
  DISTRIBUTED BY (id)
  PARTITION BY RANGE (d);

CREATE TABLE split_regular_test_p1 PARTITION OF split_regular_test
  FOR VALUES FROM ('2020-01-01') TO ('2020-07-01')
  USING ao_column WITH (compresstype=zstd, compresslevel=5);

ALTER TABLE split_regular_test
  SPLIT PARTITION FOR ('2020-02-01') AT ('2020-04-01')
  INTO (PARTITION jan_mar, PARTITION apr_jun);

-- both halves keep the legacy naming convention
SELECT relname FROM pg_class
  WHERE relname LIKE 'split_regular_test_1_prt%' ORDER BY 1;

-- both halves keep inheriting storage from the partition being split
-- (ao_column/level 5), not the root's (ao_row/level 1)
SELECT c.oid::regclass, amname, reloptions
  FROM pg_class c LEFT JOIN pg_am am ON am.oid = c.relam
  WHERE c.oid IN (SELECT oid FROM pg_class
                  WHERE relname LIKE 'split_regular_test_1_prt%')
  ORDER BY 1::text;

DROP TABLE split_regular_test;

-- 4: SPLIT DEFAULT PARTITION on a default partition with its own explicit
-- storage must be safely observable via a ddl_command_end event trigger.
-- Previously, the internal rename of the old default partition (later
-- dropped within the same statement) could crash
-- pg_event_trigger_ddl_commands() with "cache lookup failed" once the
-- trigger inspected the collected commands.
CREATE OR REPLACE FUNCTION split_default_test_evttrig() RETURNS event_trigger
LANGUAGE plpgsql AS $$
DECLARE
  r RECORD;
BEGIN
  FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP
    RAISE NOTICE 'evttrig: tag=% type=% valid_object=%',
      r.command_tag, r.object_type, (r.objid IS NOT NULL AND r.objid != 0);
  END LOOP;
END;
$$;

CREATE EVENT TRIGGER split_default_test_evttrig
  ON ddl_command_end EXECUTE FUNCTION split_default_test_evttrig();

CREATE TABLE split_default_evt_test (id int, d date, val text)
  USING ao_row WITH (compresstype=zstd, compresslevel=1)
  DISTRIBUTED BY (id)
  PARTITION BY RANGE (d);

CREATE TABLE split_default_evt_test_dflt PARTITION OF split_default_evt_test DEFAULT
  USING ao_column WITH (compresstype=zstd, compresslevel=2);

ALTER TABLE split_default_evt_test
  SPLIT DEFAULT PARTITION START ('2020-03-01') END ('2020-04-01')
  INTO (PARTITION split_default_evt_new, DEFAULT PARTITION);

DROP EVENT TRIGGER split_default_test_evttrig;
DROP FUNCTION split_default_test_evttrig();
DROP TABLE split_default_evt_test;
