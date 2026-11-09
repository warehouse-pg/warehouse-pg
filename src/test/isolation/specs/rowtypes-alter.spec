#
# Test deforming composite datums with a descriptor they were not formed. This
# file tests scenarios involving multiple sessions.
#
# The altered column changes from bytea to text, for the reasons explained in
# src/test/regress/sql/rowtypes_alter.sql.

setup
{
    CREATE TABLE rta_tab (a int, b bytea);
    CREATE TYPE rta_type AS (a int, b bytea);
    CREATE TABLE rta_lockme ();

    -- Blocks until s2 releases its advisory lock, then processes the
    -- invalidations s2 sent meanwhile.
    CREATE FUNCTION rta_wait() RETURNS void LANGUAGE plpgsql AS $$
    BEGIN
        PERFORM pg_advisory_xact_lock(1);
        LOCK rta_lockme;
    END;
    $$;

}

teardown
{
    DROP FUNCTION rta_wait();
    DROP TABLE rta_tab, rta_lockme;
    DROP TYPE rta_type;
}

session "s1"
# The cached plans need to survive until they are used.
step "s1_prepare"
{
    PREPARE rta_tab_const AS SELECT '(1,\\x4649584d45)'::rta_tab;
    PREPARE rta_type_const AS SELECT '(1,\\x4649584d45)'::rta_type;
}
step "s1_exec_tab" { EXECUTE rta_tab_const; }
step "s1_exec_type" { EXECUTE rta_type_const; }
step "s1_begin" { BEGIN; }
step "s1_declare"
{
    DECLARE rta_cur CURSOR FOR
    SELECT '(1,\\x4649584d45)'::rta_tab FROM generate_series(1, 3);
}
step "s1_fetch" { FETCH NEXT FROM rta_cur; }
# process the invalidations sent by s2
step "s1_inval" { LOCK rta_lockme; }
step "s1_commit" { COMMIT; }
step "s1_declare_hold"
{
    DECLARE rta_hold CURSOR WITH HOLD FOR
    SELECT '(1,\\x4649584d45)'::rta_tab FROM generate_series(1, 3);
}
step "s1_fetch_hold" { FETCH NEXT FROM rta_hold; }
step "s1_query"
{
    SELECT x::text, rta_wait(), x::text
    FROM (SELECT '(1,\\x4649584d45)'::rta_tab AS x OFFSET 0) s;
}
teardown { DEALLOCATE ALL; CLOSE ALL; }

session "s2"
step "s2_alter_tab" { ALTER TABLE rta_tab ALTER COLUMN b TYPE text; }
step "s2_alter_type" { ALTER TYPE rta_type ALTER ATTRIBUTE b TYPE text; }
step "s2_alter_tab_same" { ALTER TABLE rta_tab ALTER COLUMN b TYPE bytea; }
step "s2_lock" { SELECT pg_advisory_lock(1); }
step "s2_unlock" { SELECT pg_advisory_unlock(1); }

# constants in prepared statements, of a table's and a composite type's row type
permutation "s1_prepare" "s1_exec_tab" "s1_exec_type" "s2_alter_tab" "s2_alter_type" "s1_exec_tab" "s1_exec_type"
# a cursor that already returned a row
permutation "s1_begin" "s1_declare" "s1_fetch" "s2_alter_tab" "s1_inval" "s1_fetch" "s1_commit"
# a holdable cursor, materialized before the ALTER
permutation "s1_declare_hold" "s1_fetch_hold" "s2_alter_tab" "s1_fetch_hold"
# a constant read before and after waiting for s2, in one query
permutation "s2_lock" "s1_query" "s2_alter_tab" "s2_unlock"
# altering the column to the type it already has, which keeps the layout
permutation "s1_prepare" "s1_exec_tab" "s2_alter_tab_same" "s1_exec_tab"
