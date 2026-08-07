create or replace function test_event_trigger() returns event_trigger as $$
BEGIN
    RAISE NOTICE 'test_event_trigger: % %', tg_event, tg_tag;
END
$$ language plpgsql;

create event trigger regress_event_trigger on ddl_command_start
   execute procedure test_event_trigger();

-- Test event triggers on GPDB specific objects
CREATE EXTERNAL WEB TABLE echotest (x text) EXECUTE 'echo foo;' FORMAT 'text';
DROP EXTERNAL TABLE echotest;

CREATE OR REPLACE FUNCTION write_to_file() RETURNS integer as '$libdir/gpextprotocol.so', 'demoprot_export' LANGUAGE C STABLE NO SQL;
CREATE OR REPLACE FUNCTION read_from_file() RETURNS integer as '$libdir/gpextprotocol.so', 'demoprot_import' LANGUAGE C STABLE NO SQL;

CREATE PROTOCOL demoprot_event_trig_test (readfunc = 'read_from_file', writefunc = 'write_to_file');

CREATE WRITABLE EXTERNAL TABLE demoprot_w(a int) location('demoprot_event_trig_test://demoprotfile.txt') format 'text';

DROP EXTERNAL TABLE demoprot_w CASCADE;

DROP PROTOCOL demoprot_event_trig_test;

drop event trigger regress_event_trigger;

-- Verify that CREATE EXTERNAL TABLE is collected for ddl_command_end triggers.
-- ddl_command_start never touches command collection, so the checks above would
-- pass even if pg_event_trigger_ddl_commands() dropped external tables entirely.
create table regress_ddl_history (id serial, tag text, identity text, objtype text)
    DISTRIBUTED BY (id);

create or replace function test_event_trigger_end() returns event_trigger as $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN SELECT object_identity, object_type
             FROM pg_event_trigger_ddl_commands()
    LOOP
        INSERT INTO regress_ddl_history (tag, identity, objtype)
        VALUES (tg_tag, r.object_identity, r.object_type);
    END LOOP;
END
$$ language plpgsql;

create event trigger regress_event_trigger_end on ddl_command_end
   execute procedure test_event_trigger_end();

CREATE EXTERNAL WEB TABLE endtest_ext (x text) EXECUTE 'echo foo;' FORMAT 'text';
CREATE TABLE endtest_heap (x text) DISTRIBUTED BY (x);

drop event trigger regress_event_trigger_end;

-- both the external and the regular table must be here
SELECT tag, identity, objtype FROM regress_ddl_history ORDER BY id;

DROP EXTERNAL TABLE endtest_ext;
DROP TABLE endtest_heap;
DROP TABLE regress_ddl_history;
DROP FUNCTION test_event_trigger_end();
