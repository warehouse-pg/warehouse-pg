@gpcheckcat
Feature: gpcheckcat tests

    Scenario: gpcheckcat should log into gpAdminLogs
        When the user runs "gpcheckcat -l"
        Then verify that the utility gpcheckcat ever does logging into the user's "gpAdminLogs" directory

    Scenario: run all the checks in gpcheckcat
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -A"
        Then gpcheckcat should return a return code of 0
        When the user runs "gpcheckcat -C pg_class"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "Execution error:" to stdout
        And the user runs "dropdb all_good"

    Scenario: gpcheckcat should drop leaked schemas
        Given database "leak_db" is dropped and recreated
        And the user runs the command "psql leak_db -f 'test/behave/mgmt_utils/steps/data/gpcheckcat/create_temp_schema_leak.sql'" in the background without sleep
        And waiting "1" seconds
        Then read pid from file "test/behave/mgmt_utils/steps/data/gpcheckcat/pid_leak" and kill the process
        And the temporary file "test/behave/mgmt_utils/steps/data/gpcheckcat/pid_leak" is removed
        And waiting "2" seconds
        When the user runs "gpstop -ar"
        Then gpstart should return a return code of 0
        When the user runs "psql leak_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/leaked_schema.sql"
        Then psql should return a return code of 0
        And psql should print "pg_temp_" to stdout
        And psql should print "(1 row)" to stdout
        When the user runs "gpcheckcat leak_db"
        Then gpchekcat should return a return code of 0
        Then gpcheckcat should print "Found and dropped 2 unbound temporary schemas" to stdout
        And the user runs "psql leak_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/leaked_schema.sql"
        Then psql should return a return code of 0
        And psql should print "(0 rows)" to stdout
        And verify that the schema "good_schema" exists in "leak_db"
        And the user runs "dropdb leak_db"

    Scenario: gpcheckcat should report unique index violations
        Given database "unique_index_db" is dropped and recreated
        And the user runs "psql unique_index_db -f 'test/behave/mgmt_utils/steps/data/gpcheckcat/create_unique_index_violation.sql'"
        Then psql should return a return code of 0
        And psql should not print "(0 rows)" to stdout
        When the user runs "gpcheckcat unique_index_db"
        Then gpcheckcat should return a return code of 3
        And gpcheckcat should print "Table pg_compression has a violated unique index: pg_compression_compname_index" to stdout
        And the user runs "dropdb unique_index_db"

    Scenario Outline: gpcheckcat should discover missing attributes for tables
        Given database "miss_attr_db1" is dropped and recreated
        And there is a "heap" table "public.heap_table" in "miss_attr_db1" with data
        And there is a "heap" partition table "public.heap_part_table" in "miss_attr_db1" with data
        And there is a "ao" table "public.ao_table" in "miss_attr_db1" with data
        And there is a "ao" partition table "public.ao_part_table" in "miss_attr_db1" with data
        And the user runs "psql miss_attr_db1 -c "ALTER TABLE heap_table ALTER COLUMN column1 SET DEFAULT 1;""
        And the user runs "psql miss_attr_db1 -c "ALTER TABLE heap_part_table ALTER COLUMN column1 SET DEFAULT 1;""
        And the user runs "psql miss_attr_db1 -c "ALTER TABLE ao_table ALTER COLUMN column1 SET DEFAULT 1;""
        And the user runs "psql miss_attr_db1 -c "ALTER TABLE ao_part_table ALTER COLUMN column1 SET DEFAULT 1;""
        And the user runs "psql miss_attr_db1 -c "CREATE RULE notify_me AS ON UPDATE TO heap_table DO ALSO NOTIFY ao_table;""
        And the user runs "psql miss_attr_db1 -c "CREATE RULE notify_me AS ON UPDATE TO heap_part_table DO ALSO NOTIFY ao_part_table;""
        And the user runs "psql miss_attr_db1 -c "CREATE RULE notify_me AS ON UPDATE TO ao_table DO ALSO NOTIFY heap_table;""
        And the user runs "psql miss_attr_db1 -c "CREATE RULE notify_me AS ON UPDATE TO ao_part_table DO ALSO NOTIFY heap_part_table;""
        When the user runs "gpcheckcat miss_attr_db1"
        And gpcheckcat should return a return code of 0
        Then gpcheckcat should not print "Missing" to stdout
        And the user runs "psql miss_attr_db1 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='heap_table'::regclass::oid;""
        And the user runs "psql miss_attr_db1 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='heap_part_table'::regclass::oid;""
        And the user runs "psql miss_attr_db1 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='ao_table'::regclass::oid;""
        And the user runs "psql miss_attr_db1 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='ao_part_table'::regclass::oid;""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db1"
        Then gpcheckcat should print "Missing" to stdout
        And gpcheckcat should print "Table miss_attr_db1.public.heap_table.-1" to stdout
        And gpcheckcat should print "Table miss_attr_db1.public.heap_part_table.-1" to stdout
        And gpcheckcat should print "Table miss_attr_db1.public.ao_table.-1" to stdout
        And gpcheckcat should print "Table miss_attr_db1.public.ao_part_table.-1" to stdout
        And gpcheckcat should print "on content -1" to stdout
        Examples:
          | attrname   | tablename     |
          | attrelid   | pg_attribute  |
          | adrelid    | pg_attrdef    |
          | typrelid   | pg_type       |
          | ev_class   | pg_rewrite    |

    Scenario Outline: gpcheckcat should discover missing attributes for indexes
        Given database "miss_attr_db2" is dropped and recreated
        And there is a "heap" table "public.heap_table" in "miss_attr_db2" with data
        And there is a "heap" partition table "public.heap_part_table" in "miss_attr_db2" with data
        And there is a "ao" table "public.ao_table" in "miss_attr_db2" with data
        And there is a "ao" partition table "public.ao_part_table" in "miss_attr_db2" with data
        And the user runs "psql miss_attr_db2 -c "CREATE INDEX heap_table_idx on heap_table (column1);""
        And the user runs "psql miss_attr_db2 -c "CREATE INDEX heap_part_table_idx on heap_part_table (column1);""
        And the user runs "psql miss_attr_db2 -c "CREATE INDEX ao_table_idx on ao_table (column1);""
        And the user runs "psql miss_attr_db2 -c "CREATE INDEX ao_part_table_idx on ao_part_table (column1);""
        When the user runs "gpcheckcat miss_attr_db2"
        And gpcheckcat should return a return code of 0
        Then gpcheckcat should not print "Missing" to stdout
        And the user runs "psql miss_attr_db2 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='heap_table_idx'::regclass::oid;""
        And the user runs "psql miss_attr_db2 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='heap_part_table_idx'::regclass::oid;""
        And the user runs "psql miss_attr_db2 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='ao_table_idx'::regclass::oid;""
        And the user runs "psql miss_attr_db2 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='ao_part_table_idx'::regclass::oid;""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db2"
        Then gpcheckcat should print "Missing" to stdout
        And gpcheckcat should print "Table miss_attr_db2.public.heap_table_idx.-1" to stdout
        And gpcheckcat should print "Table miss_attr_db2.public.heap_part_table_idx.-1" to stdout
        And gpcheckcat should print "Table miss_attr_db2.public.ao_table_idx.-1" to stdout
        And gpcheckcat should print "Table miss_attr_db2.public.ao_part_table_idx.-1" to stdout
        Examples:
          | attrname   | tablename    |
          | indexrelid | pg_index     |

    Scenario: gpcheckcat should print out tables with missing and extraneous attributes in a readable format
        Given database "miss_attr_db4" is dropped and recreated
        And there is a "heap" table "public.heap_table" in "miss_attr_db4" with data
        And there is a "ao" table "public.ao_table" in "miss_attr_db4" with data
        When the user runs "gpcheckcat miss_attr_db4"
        And gpcheckcat should return a return code of 0
        Then gpcheckcat should not print "Missing" to stdout
        And an attribute of table "heap_table" in database "miss_attr_db4" is deleted on segment with content id "0"
        And psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db4"
        Then gpcheckcat should print "Missing" to stdout
        And gpcheckcat should print "Table miss_attr_db4.public.heap_table.0" to stdout
        And the user runs "psql miss_attr_db4 -c "SET allow_system_table_mods=true; DELETE FROM pg_attribute where attrelid='heap_table'::regclass::oid;""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db4"
        Then gpcheckcat should print "Extra" to stdout
        And gpcheckcat should print "Table miss_attr_db4.public.heap_table.1" to stdout

    Scenario: gpcheckcat should report inconsistent pg_fastsequence.lastrownums values with gp_fastsequence for AO tables
        Given database "errorneous_lastrownums" is dropped and recreated
        And the user runs "psql errorneous_lastrownums -c "create table errlastrownum(a int) using ao_row; insert into errlastrownum select * from generate_series(1,100);""
        And the user runs "psql errorneous_lastrownums -c "alter table errlastrownum add column newcol int;""
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 0
        When the user runs sql "set allow_system_table_mods=on; update gp_fastsequence set last_sequence = 0 where last_sequence > 0;" in "errorneous_lastrownums" on first primary segment
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 3
        And gpcheckcat should print "Failed test\(s\) that are not reported here: ao_lastrownums" to stdout
        Given database "errorneous_lastrownums" is dropped and recreated
        And the user runs "psql errorneous_lastrownums -c "create table errlastrownum(a int) using ao_row; insert into errlastrownum select * from generate_series(1,10);""
        And the user runs "psql errorneous_lastrownums -c "alter table errlastrownum add column newcol int;""
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 0
        Then the user runs sql "set allow_system_table_mods=on; delete from gp_fastsequence where last_sequence > 0;" in "errorneous_lastrownums" on first primary segment
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 3
        And gpcheckcat should print "Failed test\(s\) that are not reported here: ao_lastrownums" to stdout

    Scenario: gpcheckcat should report inconsistent pg_fastsequence.lastrownums values with gp_fastsequence for AOCO tables
        Given database "errorneous_lastrownums" is dropped and recreated
        And the user runs "psql errorneous_lastrownums -c "create table errlastrownum(a int) using ao_column; insert into errlastrownum select * from generate_series(1,100);""
        And the user runs "psql errorneous_lastrownums -c "alter table errlastrownum add column newcol int;""
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 0
        When the user runs sql "set allow_system_table_mods=on; update gp_fastsequence set last_sequence = 0 where last_sequence > 0;" in "errorneous_lastrownums" on first primary segment
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 3
        And gpcheckcat should print "Failed test\(s\) that are not reported here: ao_lastrownums" to stdout
        Given database "errorneous_lastrownums" is dropped and recreated
        And the user runs "psql errorneous_lastrownums -c "create table errlastrownum(a int) using ao_column; insert into errlastrownum select * from generate_series(1,10);""
        And the user runs "psql errorneous_lastrownums -c "alter table errlastrownum add column newcol int;""
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 0
        Then the user runs sql "set allow_system_table_mods=on; delete from gp_fastsequence where last_sequence > 0;" in "errorneous_lastrownums" on first primary segment
        When the user runs "gpcheckcat -R ao_lastrownums errorneous_lastrownums"
        Then gpcheckcat should return a return code of 3
        And gpcheckcat should print "Failed test\(s\) that are not reported here: ao_lastrownums" to stdout

    Scenario: gpcheckcat should report and repair owner errors and produce timestamped repair scripts
        Given database "owner_db1" is dropped and recreated
        And database "owner_db2" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And there is a "heap" table "gpadmin_tbl" in "owner_db1" with data
        And there is a "heap" table "gpadmin_tbl" in "owner_db2" with data
        And the user runs "psql owner_db1 -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_user_wolf.sql"
        Then psql should return a return code of 0
        Given the user runs sql "alter table gpadmin_tbl OWNER TO wolf" in "owner_db1" on first primary segment
        When the user runs "gpcheckcat -R owner owner_db1"
        Then gpcheckcat should return a return code of 3
        Then the path "gpcheckcat.repair.*" is found in cwd "1" times
        Then gpcheckcat should print "reported here: owner" to stdout
        And waiting "1" seconds
        When the user runs "gpcheckcat -R owner owner_db1"
        Then gpcheckcat should return a return code of 3
        Then the path "gpcheckcat.repair.*" is found in cwd "2" times
        Then gpcheckcat should print "reported here: owner" to stdout
        Then run all the repair scripts in the dir "gpcheckcat.repair.*"
        And the path "gpcheckcat.repair.*" is removed from current working directory
        When the user runs "gpcheckcat -R owner owner_db1"
        Then gpcheckcat should return a return code of 0
        Then the path "gpcheckcat.repair.*" is found in cwd "0" times
        When the user runs "gpcheckcat -R owner owner_db2"
        Then gpcheckcat should return a return code of 0
        Then the path "gpcheckcat.repair.*" is found in cwd "0" times
        And the user runs "dropdb owner_db1"
        And the user runs "dropdb owner_db2"
        And the path "gpcheckcat.repair.*" is removed from current working directory

    Scenario: gpcheckcat should report and repair owner errors on appendonly tables and its indexes
        Given database "owner_db" is dropped and recreated
          And the path "gpcheckcat.repair.*" is removed from current working directory
          And there is a "ao" table "public.gpadmin_ao_tbl" in "owner_db" with data
          And the user runs "psql owner_db -c "CREATE INDEX gpadmin_ao_tbl_idx on gpadmin_ao_tbl (column1);""
          And the user runs sql "alter table gpadmin_ao_tbl OWNER TO wolf" in "owner_db" on first primary segment
         Then psql should return a return code of 0

        When the user runs "gpcheckcat -R owner owner_db"
         Then gpcheckcat should return a return code of 3
         Then the path "gpcheckcat.repair.*" is found in cwd "1" times

        When the user runs all the repair scripts in the dir "gpcheckcat.repair.*"
          And the path "gpcheckcat.repair.*" is removed from current working directory
          And the user runs "gpcheckcat -R owner owner_db"
         Then Then gpcheckcat should return a return code of 0
         Then the path "gpcheckcat.repair.*" is found in cwd "0" times

        And the user runs "dropdb owner_db"
        And the path "gpcheckcat.repair.*" is removed from current working directory
        
    Scenario: gpcheckcat should report and repair invalid constraints
        Given database "constraint_db" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user runs "psql constraint_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_invalid_constraint.sql"
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R distribution_policy constraint_db"
        Then gpcheckcat should return a return code of 1
        Then validate and run gpcheckcat repair
        When the user runs "gpcheckcat -R distribution_policy constraint_db"
        Then gpcheckcat should return a return code of 0
        And the user runs "dropdb constraint_db"

    Scenario: gpcheckcat should report, but not repair, invalid policy issues
        Given database "policy_db" is dropped and recreated
          And the path "gpcheckcat.repair.*" is removed from current working directory
          And the user runs "psql policy_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_inconsistent_policy.sql"
         Then psql should return a return code of 0

         When the user runs "gpcheckcat -R part_integrity -g repair_dir policy_db"
         Then gpcheckcat should return a return code of 1
          And gpcheckcat should print "child partition\(s\) are distributed differently from the root partition, and must be manually redistributed, for some tables" to stdout
          And gpcheckcat should print "Failed test\(s\) that are not reported here: part_integrity" to stdout

         When the user runs all the repair scripts in the dir "repair_dir"
          And the user runs "gpcheckcat -R part_integrity -g repair_dir policy_db"
         Then gpcheckcat should return a return code of 1
          And gpcheckcat should print "child partition\(s\) are distributed differently from the root partition, and must be manually redistributed, for some tables" to stdout
          And gpcheckcat should print "Failed test\(s\) that are not reported here: part_integrity" to stdout

         Then the user runs "dropdb policy_db"
          And the path "gpcheckcat.repair.*" is removed from current working directory

    Scenario: gpcheckcat should not report when parent is hash distributed and child is randomly distributed and child is a leaf level partition
        Given database "policy_db" is dropped and recreated
        And the user runs "psql policy_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_multilevel_partition.sql"
        And the user runs sql "set allow_system_table_mods=true; update gp_distribution_policy set distkey = '', distclass='' where localoid='sales_1_prt_2_2_prt_asia'::regclass::oid;" in "policy_db" on all the segments
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R part_integrity policy_db"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "child partition\(s\) are distributed differently from the root partition, and must be manually redistributed, for some tables" to stdout
        And gpcheckcat should not print "Failed test\(s\) that are not reported here: part_integrity" to stdout
        And the user runs "dropdb policy_db"

    Scenario: gpcheckcat should report when parent is hash distributed and child is randomly distributed and child is a middle level partition
        Given database "policy_db" is dropped and recreated
        And the user runs "psql policy_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_multilevel_partition.sql"
        And the user runs sql "set allow_system_table_mods=true; update gp_distribution_policy set distkey = '', distclass='' where localoid='sales_1_prt_2'::regclass::oid;" in "policy_db" on all the segments
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R part_integrity policy_db"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "child partition\(s\) are distributed differently from the root partition, and must be manually redistributed, for some tables" to stdout
        And gpcheckcat should print "Failed test\(s\) that are not reported here: part_integrity" to stdout
        And the user runs "dropdb policy_db"

    Scenario: gpcheckcat should report when parent is randomly distributed and child is hash distributed
        Given database "policy_db" is dropped and recreated
        And the user runs "psql policy_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_multilevel_partition.sql"
        And the user runs sql "set allow_system_table_mods=true; update gp_distribution_policy set distkey = '', distclass='' where localoid='sales'::regclass::oid;" in "policy_db" on all the segments
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R part_integrity policy_db"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "child partition\(s\) are distributed differently from the root partition, and must be manually redistributed, for some tables" to stdout
        And gpcheckcat should print "Failed test\(s\) that are not reported here: part_integrity" to stdout
        And the user runs "dropdb policy_db"

    Scenario: gpcheckcat should not report part_integrity errors from readable external partitions
        Given database "policy_db" is dropped and recreated
        And the user runs "psql policy_db -c "create table part(a int) partition by list(a); create table p1(a int); create external web table p2_ext (like p1) EXECUTE 'cat something.txt' FORMAT 'TEXT';""
        And the user runs "psql policy_db -c "alter table part attach partition p1 for values in (1); alter table part attach partition p2_ext for values in (2);""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R part_integrity policy_db"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "child partition\(s\) have different numsegments value from the root partition" to stdout
        And gpcheckcat should not print "child partition\(s\) are distributed differently from the root partition, and must be manually redistributed, for some tables" to stdout
        And gpcheckcat should not print "Failed test\(s\) that are not reported here: part_integrity" to stdout
        And the user runs "dropdb policy_db"

    Scenario: gpcheckcat should report when parent and child partitions have different numsegments value
        Given database "policy_db" is dropped and recreated
        And the user runs "psql policy_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_multilevel_partition.sql"
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R part_integrity policy_db"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "child partition\(s\) have different numsegments value from the root partition" to stdout
        And gpcheckcat should not print "Failed test\(s\) that are not reported here: part_integrity" to stdout

        And the user runs sql "set allow_system_table_mods=true; update gp_distribution_policy set numsegments = '1' where localoid='sales_1_prt_2'::regclass::oid;" in "policy_db" on all the segments
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R part_integrity policy_db"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "child partition\(s\) have different numsegments value from the root partition" to stdout
        And gpcheckcat should print "Failed test\(s\) that are not reported here: part_integrity" to stdout
        And the user runs "dropdb policy_db"

    Scenario: gpcheckcat foreign key check should report missing catalog entries. Also test missing_extraneous for the same case.
        Given database "fkey_db" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And there is a "heap" table "gpadmin_tbl" in "fkey_db" with data
        And there is a view without columns in "fkey_db"
        When the entry for the table "gpadmin_tbl" is removed from "pg_catalog.pg_class" with key "oid" in the database "fkey_db"
        Then the user runs "gpcheckcat -E -R missing_extraneous fkey_db"
        And gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_class" to stdout
        Then gpcheckcat should return a return code of 1
        Then validate and run gpcheckcat repair
        Then the user runs "gpcheckcat -E -R foreign_key fkey_db"
        Then gpcheckcat should print "No pg_class {.*} entry for pg_attribute {.*}" to stdout
        Then gpcheckcat should print "No pg_class {.*} entry for pg_type {.*}" to stdout
        Then gpcheckcat should print "No pg_class {.*} entry for gp_distribution_policy {.*}" to stdout
        Then gpcheckcat should return a return code of 3
        Then the user runs "gpcheckcat -E -R missing_extraneous fkey_db"
        Then gpcheckcat should return a return code of 0
        Then the path "gpcheckcat.repair.*" is found in cwd "0" times
        And the user runs "dropdb fkey_db"

    Scenario Outline: gpcheckcat foreign key check should report missing catalog entries for segments. Also test missing_extraneous for the same case.
        Given database "fkey_ta" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user creates an index for table "index_table" in database "fkey_ta"
        And there is a "ao" table "ao_table" in "fkey_ta" with data
        When the entry for the table "<table_name>" is removed from "pg_catalog.<catalog_name>" with key "<catalog_oid_key>" in the database "fkey_ta" on the first primary segment
        Then the user runs "gpcheckcat -E -R foreign_key fkey_ta"
        Then gpcheckcat should print "No <catalog_name> {.*} entry for pg_class {.*}" to stdout
        Then gpcheckcat should return a return code of 3
        And the user runs "dropdb fkey_ta"
        Examples:
          | catalog_name                | catalog_oid_key | table_name |
          | pg_attribute                | attrelid        | index_table |
          | pg_index                    | indrelid        | index_table |
          | pg_appendonly               | relid           | ao_table   |

    Scenario Outline: gpcheckcat foreign key check should report missing catalog entries. Also test missing_extraneous for the same case.
        Given database "fkey_ta" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user creates an index for table "index_table" in database "fkey_ta"
        And there is a "ao" table "ao_table" in "fkey_ta" with data
        When the entry for the table "<table_name>" is removed from "pg_catalog.<catalog_name>" with key "<catalog_oid_key>" in the database "fkey_ta"
        Then the user runs "gpcheckcat -E -R foreign_key fkey_ta"
        Then gpcheckcat should print "No <catalog_name> {.*} entry for pg_class {.*}" to stdout
        Then gpcheckcat should return a return code of 3
        And the user runs "dropdb fkey_ta"
        Examples:
          | catalog_name                | catalog_oid_key | table_name |
          | pg_attribute                | attrelid        | index_table |
          | pg_index                    | indrelid        | index_table |
          | pg_appendonly               | relid           | ao_table   |

    Scenario: gpcheckcat foreign key check should report missing catalog entries. Also test missing_extraneous for the same case.
        Given database "fkey_ta" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And there is a "heap" table "gpadmin_tbl" in "fkey_ta" with data
        When the entry for the table "gpadmin_tbl" is removed from "pg_catalog.pg_type" with key "typrelid" in the database "fkey_ta"
        Then the user runs "gpcheckcat -E -R foreign_key fkey_ta"
        Then gpcheckcat should print "No pg_type {.*} entry for pg_class {.*}" to stdout
        Then gpcheckcat should return a return code of 3
        And the user runs "dropdb fkey_ta"

    Scenario: gpcheckcat should report and repair extra entries with non-oid primary keys
        Given database "extra_pk_db" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user runs "psql extra_pk_db -c 'CREATE SCHEMA my_pk_schema' "
        And the user runs "psql extra_pk_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/add_operator.sql "
        Then psql should return a return code of 0
        And the user runs "psql extra_pk_db -c "set allow_system_table_mods=true;DELETE FROM pg_catalog.pg_operator where oprname='!#'" "
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R missing_extraneous extra_pk_db"
        Then gpcheckcat should return a return code of 3
        And the path "gpcheckcat.repair.*" is found in cwd "0" times
        When the user runs "gpcheckcat -R missing_extraneous -E extra_pk_db"
        Then gpcheckcat should return a return code of 1
        And validate and run gpcheckcat repair
        When the user runs "gpcheckcat -R missing_extraneous -E extra_pk_db"
        Then gpcheckcat should return a return code of 0
        And the user runs "dropdb extra_pk_db"
        And the path "gpcheckcat.repair.*" is removed from current working directory

    Scenario: gpcheckcat should report and repair extra entries in coordinator as well as all the segments
        Given database "extra_db" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user runs "psql extra_db -c "CREATE TABLE foo(i int)""
        Then The user runs sql "set allow_system_table_mods=true;delete from pg_class where relname='foo'" in "extra_db" on first primary segment
        And the user runs "psql extra_db -c "drop table if exists foo""
        Then the user runs "gpcheckcat -R missing_extraneous extra_db"
        Then gpcheckcat should return a return code of 3
        Then the path "gpcheckcat.repair.*" is found in cwd "0" times
        Then the user runs "gpcheckcat -R missing_extraneous -E extra_db"
        Then gpcheckcat should return a return code of 1
        Then validate and run gpcheckcat repair
        When the user runs "gpcheckcat -R missing_extraneous -E extra_db"
        Then gpcheckcat should return a return code of 0
        Then the path "gpcheckcat.repair.*" is found in cwd "0" times
        And the user runs "dropdb extra_db"
        And the path "gpcheckcat.repair.*" is removed from current working directory

    Scenario: gpcheckcat should report inconsistency between gp_fastsequence and pg_class
        Given database "fkey2_db" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user runs "psql fkey2_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_aoco_table.sql"
        And the user runs sql file "test/behave/mgmt_utils/steps/data/gpcheckcat/create_inconsistent_gpfastsequence.sql" in "fkey2_db" on all the segments
        Then the user runs "gpcheckcat fkey2_db"
        Then gpcheckcat should return a return code of 3
        Then gpcheckcat should print "No pg_class {.*} entry for gp_fastsequence {.*}" to stdout
        Then validate and run gpcheckcat repair
        Then the user runs "gpcheckcat -R foreign_key fkey2_db"
        Then gpcheckcat should not print "No pg_class {.*} entry for gp_fastsequence {.*}" to stdout
        Then gpcheckcat should return a return code of 3
        And the user runs "dropdb fkey2_db"
        And the path "gpcheckcat.repair.*" is removed from current working directory

    Scenario: gpcheckcat should generate repair scripts when -g, -R, and -E options are provided
        Given database "extra_gr_db" is dropped and recreated
        And the path "repair_dir" is removed from current working directory
        And the user runs "psql extra_gr_db -c "CREATE TABLE foo(i int)""
        Then The user runs sql "set allow_system_table_mods=true;delete from pg_class where relname='foo'" in "extra_gr_db" on first primary segment
        And the user runs "psql extra_gr_db -c "drop table if exists foo""
        Then the user runs "gpcheckcat -R missing_extraneous -E -g repair_dir extra_gr_db"
        Then gpcheckcat should return a return code of 1
        Then gpcheckcat should print "repair script\(s\) generated in dir repair_dir" to stdout
        Then the path "repair_dir" is found in cwd "1" times
        Then run all the repair scripts in the dir "repair_dir"
        And the path "repair_dir" is removed from current working directory
        When the user runs "gpcheckcat -R missing_extraneous -E -g repair_dir extra_gr_db"
        Then gpcheckcat should return a return code of 0
        Then the path "repair_dir" is found in cwd "0" times
        And the user runs "dropdb extra_gr_db"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should generate repair scripts when only -g option is provided
        Given database "constraint_g_db" is dropped and recreated
        And the user runs "psql constraint_g_db -c "create table foo(i int primary key);""
        And the user runs sql "set allow_system_table_mods=true; update gp_distribution_policy  set distkey='', distclass='' where localoid='foo'::regclass::oid;" in "constraint_g_db" on all the segments
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -g repair_dir constraint_g_db"
        Then gpcheckcat should return a return code of 1
        Then gpcheckcat should print "repair script\(s\) generated in dir repair_dir" to stdout
        Then the path "repair_dir" is found in cwd "1" times
        Then run all the repair scripts in the dir "repair_dir"
        And the path "repair_dir" is removed from current working directory
        When the user runs "gpcheckcat -g repair_dir constraint_g_db"
        Then gpcheckcat should return a return code of 0
        Then the path "repair_dir" is found in cwd "0" times
        And the user runs "dropdb constraint_g_db"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should use the same timestamp for creating repair dir and scripts
        Given database "timestamp_db" is dropped and recreated
        And the path "gpcheckcat.repair.*" is removed from current working directory
        And the user runs "psql timestamp_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_aoco_table.sql"
        And the user runs sql file "test/behave/mgmt_utils/steps/data/gpcheckcat/create_inconsistent_gpfastsequence.sql" in "timestamp_db" on all the segments
        And the user runs "psql timestamp_db -c "CREATE TABLE foo(i int)""
        Then The user runs sql "set allow_system_table_mods=true;delete from pg_class where relname='foo'" in "timestamp_db" on first primary segment
        And the user runs "psql timestamp_db -c "drop table if exists foo""
        Then the user runs "gpcheckcat timestamp_db"
        Then gpcheckcat should return a return code of 3
        Then the timestamps in the repair dir are consistent
        And the user runs "dropdb timestamp_db"
        And the path "gpcheckcat.repair.*" is removed from current working directory

    Scenario: gpcheckcat missing_extraneous and dependency tests detects pg_depend issues
        Given database "gpcheckcat_dependency" is dropped and recreated
        And there is a "heap" table "heap_table1" in "gpcheckcat_dependency" with data
        And there is a "heap" table "heap_table2" in "gpcheckcat_dependency" with data
        And there is a "heap" table "heap_table3" in "gpcheckcat_dependency" with data
        And the entry for the table "heap_table1" is removed from "pg_catalog.pg_depend" with key "objid" in the database "gpcheckcat_dependency" on the first primary segment
        And the entry for the table "heap_table1" is removed from "pg_catalog.pg_depend" with key "refobjid" in the database "gpcheckcat_dependency" on the first primary segment
        And the entry for the table "heap_table2" is removed from "pg_catalog.pg_type" with key "typrelid" in the database "gpcheckcat_dependency" on the first primary segment
        And the entry for the table "heap_table3" is removed from "pg_catalog.pg_depend" with key "refobjid" in the database "gpcheckcat_dependency" on the first primary segment
        And table "heap_table3" is dropped in "gpcheckcat_dependency"
        When the user runs "gpcheckcat gpcheckcat_dependency"
        Then gpcheckcat should return a return code of 3
        Then gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_type" to stdout
        Then gpcheckcat should print "Extra type metadata of {.*} on content 0" to stdout
        Then gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_depend" to stdout
        Then gpcheckcat should print "Extra depend metadata of {.*} on content 0" to stdout
        Then gpcheckcat should print "Missing depend metadata of {.*} on content 0" to stdout
        Then gpcheckcat should print "Name of test which found this issue: dependency_pg_class" to stdout
        Then gpcheckcat should print "Table pg_class has a dependency issue on oid .* at content 0" to stdout
        Then gpcheckcat should print "Name of test which found this issue: dependency_pg_type" to stdout
        Then gpcheckcat should print "Table pg_type has a dependency issue on oid .* at content 0" to stdout
        And the user runs "dropdb gpcheckcat_dependency"

    Scenario: gpcheckcat should report no inconsistency of pg_extension between Coordinator and Segements
        Given database "pgextension_db" is dropped and recreated
        And the user runs sql "set allow_system_table_mods=true;update pg_extension set extconfig='{2130}', extcondition='{2130}';" in "pgextension_db" on first primary segment
        Then the user runs "gpcheckcat -R inconsistent pgextension_db"
        Then gpcheckcat should return a return code of 0
        And the user runs "dropdb gpextension_db"

    Scenario: gpcheckcat orphaned_toast_tables test should pass when there is valid temp toast table exists
        Given database "temp_toast" is dropped and recreated
        And the user connects to "temp_toast" with named connection "default"
        And the user executes "CREATE TEMP TABLE temp_t1 (c1 text)" with named connection "default"
        Then the user runs "gpcheckcat -R orphaned_toast_tables temp_toast"
        And gpcheckcat should return a return code of 0
        And the user drops the named connection "default"
        And the user runs "dropdb temp_toast"

    Scenario: gpcheckcat should repair "bad reference" orphaned toast tables (caused by missing reltoastrelid)
        Given the database "gpcheckcat_orphans" is broken with "bad reference" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "To fix, run the generated repair script which updates a pg_class entry using the correct dependent table OID for reltoastrelid" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "Found no catalog issue" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should repair "bad dependency" orphaned toast tables (caused by missing pg_depend entry)
        Given the database "gpcheckcat_orphans" is broken with "bad dependency" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "To fix, run the generated repair script which inserts a pg_depend entry using the correct dependent table OID for refobjid" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "Found no catalog issue" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should log and not attempt to repair "double orphan - no parent" orphaned toast tables (caused by both missing reltoastrelid and missing pg_depend entry)
        Given the database "gpcheckcat_orphans" is broken with "double orphan - no parent" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "The parent table does not exist. Therefore, the toast table" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should log and not attempt to repair "double orphan - valid parent" orphaned toast tables (caused by both missing reltoastrelid and missing pg_depend entry)
        Given the database "gpcheckcat_orphans" is broken with "double orphan - valid parent" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "The parent table already references a valid toast table" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should log and not attempt to repair "double orphan - invalid parent" orphaned toast tables (caused by both missing reltoastrelid and missing pg_depend entry)
        Given the database "gpcheckcat_orphans" is broken with "double orphan - invalid parent" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "Verify that the parent table requires a toast table." to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should log and not repair "mismatched non-cyclic" orphaned toast tables (caused by non-matching reltoastrelid)
        Given the database "gpcheckcat_orphans" is broken with "mismatched non-cyclic" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "A manual catalog change is needed to fix by updating the pg_depend TOAST table entry and setting the refobjid field to the correct dependent table" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "A manual catalog change is needed" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should log and not attempt to repair "mismatched cyclic" orphaned toast tables
        Given the database "gpcheckcat_orphans" is broken with "mismatched cyclic" orphaned toast tables
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "A manual catalog change is needed to fix by updating the pg_depend TOAST table entry and setting the refobjid field to the correct dependent table" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And gpcheckcat should print "A manual catalog change is needed" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should repair orphaned toast tables that are only orphaned on some segments
        Given the database "gpcheckcat_orphans" is broken with "bad reference" orphaned toast tables only on segments with content IDs "0, 1"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "On segment\(s\) 0, 1 table" to stdout
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "Found no catalog issue" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should repair orphaned toast tables that are only orphaned on the coordinator
		# TODO: should we just combine this into the test above?
        Given the database "gpcheckcat_orphans" is broken with "bad reference" orphaned toast tables only on segments with content IDs "-1"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print "On segment\(s\) -1 table" to stdout
        And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
        And run all the repair scripts in the dir "repair_dir"
        When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
        And gpcheckcat should print "Found no catalog issue" to stdout
        And the user runs "dropdb gpcheckcat_orphans"
        And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should repair tables that are orphaned in different ways per segment
        Given the database "gpcheckcat_orphans" has a table that is orphaned in multiple ways
         When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
         Then gpcheckcat should return a return code of 1
          And gpcheckcat should print "Found a \"bad reference\" orphaned TOAST table caused by missing a reltoastrelid in pg_class." to stdout
          And gpcheckcat should print "Found a \"bad dependency\" orphaned TOAST table caused by missing a pg_depend entry." to stdout
          And gpcheckcat should print "catalog issue\(s\) found , repair script\(s\) generated" to stdout
          And run all the repair scripts in the dir "repair_dir"
         When the user runs "gpcheckcat -R orphaned_toast_tables -g repair_dir gpcheckcat_orphans"
         Then gpcheckcat should print "Found no catalog issue" to stdout
          And the user runs "dropdb gpcheckcat_orphans"
          And the path "repair_dir" is removed from current working directory

    Scenario: gpcheckcat should report vpinfo inconsistent error
        Given database "vpinfo_inconsistent_db" is dropped and recreated
          And there is a "co" table "public.co_vpinfo" in "vpinfo_inconsistent_db" with data
         When the user runs "gpcheckcat vpinfo_inconsistent_db"
         Then gpcheckcat should return a return code of 0
         When a table "co_vpinfo" in database "vpinfo_inconsistent_db" has its relnatts inflated on segment with content id "0"
         Then psql should return a return code of 0
         When the user runs "gpcheckcat -R aoseg_table vpinfo_inconsistent_db"
         Then gpcheckcat should print "Failed test\(s\) that are not reported here: aoseg_table" to stdout
          And the user runs "dropdb vpinfo_inconsistent_db"

    Scenario: gpcheckcat should not print error when vpinfo for RESERVED_SEGNO is of different length than relnatts
        Given database "vpinfo_reserved_segno" is dropped and recreated
        And the user runs "psql vpinfo_reserved_segno -c "CREATE TABLE co_table(a int, b int) using ao_column; INSERT INTO co_table values (1,1);""
        And the user runs "psql vpinfo_reserved_segno -c "BEGIN; ALTER TABLE co_table ADD COLUMN newcol int; INSERT INTO co_table VALUES (1,1,1); ABORT;""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat vpinfo_reserved_segno"
        And gpcheckcat should return a return code of 0
        Then gpcheckcat should not print "[FAIL] inconsistent vpinfo" to stdout

    Scenario: skip one check in gpcheckcat
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -s owner"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "owner" to stdout
        And the user runs "dropdb all_good"

    Scenario: skip multiple checks in gpcheckcat
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -s 'owner, acl'"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "owner" to stdout
        And gpcheckcat should not print "acl" to stdout
        And the user runs "dropdb all_good"

    Scenario: run multiple checks in gpcheckcat
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -R 'foreign_key, distribution_policy'"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should print "foreign_key" to stdout
        And gpcheckcat should print "distribution_policy" to stdout
        And the user runs "dropdb all_good"

    Scenario: run all the checks in gpcheckcat and default skips acl, owner tests  a
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -v"
        Then gpcheckcat should return a return code of 0
        And validate gpcheckcat logs contain skipping ACL and Owner tests
        And the user runs "dropdb all_good"

    Scenario: gpcheckcat should return 3 if catalog issue is found on one database but the next database in the list has no catalog issue
        Given database "mis_attr_db" is dropped and recreated
        And the user runs "psql -d mis_attr_db -c "set allow_system_table_mods=true;DELETE FROM pg_class WHERE relname='gp_fastsequence';""
        Then psql should return a return code of 0
        Then the user runs "gpcheckcat -A"
        Then gpcheckcat should return a return code of 3
        And the user runs "dropdb mis_attr_db"

    Scenario: gpcheckcat should not report dependency error from pg_default_acl, pg_subscription and pg_transform
        Given database "check_dependency_error" is dropped and recreated
        And the user runs "psql -d check_dependency_error -c "CREATE ROLE foo; ALTER DEFAULT PRIVILEGES FOR ROLE foo REVOKE EXECUTE ON FUNCTIONS FROM PUBLIC;""
        Then psql should return a return code of 0
        And the user runs "psql -d check_dependency_error -c "CREATE SUBSCRIPTION foo CONNECTION '' PUBLICATION bar WITH (connect = false, slot_name = NONE);""
        Then psql should return a return code of 0
        And the user runs "psql -d check_dependency_error -c "CREATE TRANSFORM FOR int LANGUAGE SQL (FROM SQL WITH FUNCTION prsd_lextype(internal), TO SQL WITH FUNCTION int4recv(internal));""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R dependency check_dependency_error"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "SUMMARY REPORT: FAILED" to stdout
        And gpcheckcat should not print "has a dependency issue on oid" to stdout
        And gpcheckcat should print "Found no catalog issue" to stdout
        And the user runs "psql -d check_dependency_error -c "DROP SUBSCRIPTION foo""
        And the user runs "psql -d check_dependency_error -c "DROP TRANSFORM FOR int LANGUAGE SQL""
        And the user runs "dropdb check_dependency_error"
        And the user runs "psql -d postgres -c "DROP ROLE foo""

    Scenario Outline: gpcheckcat should discover missing attributes for external tables
        Given database "miss_attr_db3" is dropped and recreated
        And the user runs "echo > /tmp/backup_gpfdist_dummy"
        And the user runs "gpfdist -p 8098 -d /tmp &"
        And there is a partition table "part_external" has external partitions of gpfdist with file "backup_gpfdist_dummy" on port "8098" in "miss_attr_db3" with data
        Then data for partition table "part_external" with leaf partition distributed across all segments on "miss_attr_db3"
        When the user runs "gpcheckcat miss_attr_db3"
        And gpcheckcat should return a return code of 0
        Then gpcheckcat should not print "Missing" to stdout
        And the user runs "psql miss_attr_db3 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='part_external_1_prt_p_2'::regclass::oid;""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db3"
        Then gpcheckcat should print "Missing" to stdout
        And gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_foreign_table" to stdout
        And gpcheckcat should print "Table miss_attr_db3.public.part_external_1_prt_p_2.-1" to stdout
        Examples:
            | attrname   | tablename          |
            | ftrelid    | pg_foreign_table   |

    Scenario Outline: gpcheckcat should discover missing attributes for external tables
        Given database "miss_attr_db3" is dropped and recreated
        And the user runs "echo > /tmp/backup_gpfdist_dummy"
        And the user runs "gpfdist -p 8098 -d /tmp &"
        And there is a partition table "part_external" has external partitions of gpfdist with file "backup_gpfdist_dummy" on port "8098" in "miss_attr_db3" with data
        Then data for partition table "part_external" with leaf partition distributed across all segments on "miss_attr_db3"
        When the user runs "gpcheckcat miss_attr_db3"
        And gpcheckcat should return a return code of 0
        Then gpcheckcat should not print "Missing" to stdout
        And the user runs "psql miss_attr_db3 -c "SET allow_system_table_mods=true; DELETE FROM <tablename> where <attrname>='part_external_1_prt_p_2';""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db3"
        Then gpcheckcat should print "Missing" to stdout
        And gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_class" to stdout
        And gpcheckcat should print "Relation name: part_external_1_prt_p_2" to stdout
        Examples:
            | attrname   | tablename          |
            | relname    | pg_class           |

    Scenario: gpcheckcat should discover missing attributes of pg_description and pg_shdescription catalogue table without errors
        Given database "miss_attr_db5" is dropped and recreated
        And there is a "heap" table "public.heap_table" in "miss_attr_db5" with data and description
        And a tablespace is created with data and description
        When the user runs "gpcheckcat miss_attr_db5"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should not print "Missing" to stdout
        When the user runs "psql miss_attr_db5 -c "SET allow_system_table_mods=true; DELETE FROM pg_description where objoid='heap_table'::regclass::oid;""
        Then psql should return a return code of 0
        When the user runs "psql miss_attr_db5 -c "SET allow_system_table_mods=true; DELETE FROM pg_shdescription where objoid=(SELECT oid from pg_tablespace where spcname='outerspace');""
        Then psql should return a return code of 0
        When the user runs "gpcheckcat miss_attr_db5"
        Then gpcheckcat should print "Missing description metadata of {.*} on content -1" to stdout
        And gpcheckcat should not print "Execution error:" to stdout
        And gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_description" to stdout
        Then gpcheckcat should print "Missing shdescription metadata of {.*} on content -1" to stdout
        And gpcheckcat should print "Name of test which found this issue: missing_extraneous_pg_shdescription" to stdout

    Scenario: set multiple GUC at session level in gpcheckcat
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -x disable_cost=3e15 -x log_min_messages=debug5 -R foreign_key"
        Then gpcheckcat should return a return code of 0
        And gpcheckcat should print "foreign_key" to stdout
        And the user runs "dropdb all_good"


    Scenario: set GUC with invalid value at session level in gpcheckcat
        Given database "all_good" is dropped and recreated
        Then the user runs "gpcheckcat -x disable_cost=invalid -R foreign_key"
        Then gpcheckcat should return a return code of 1
        And gpcheckcat should print ".* invalid value for parameter "disable_cost": "invalid"" to stdout
        And the user runs "dropdb all_good"


    Scenario: validate session GUC passed with -x is set
        Given the database is not running
          And the user runs "gpstart -ma"
          And "gpstart -ma" should return a return code of 0
         Then the user runs "gpcheckcat -R foreign_key"
         Then gpcheckcat should return a return code of 1
          And gpcheckcat should print ".* System was started in single node mode - only utility mode connections are allowed" to stdout
         Then the user runs "gpcheckcat -x gp_role=utility -R foreign_key"
         Then gpcheckcat should return a return code of 0
          And the user runs "gpstop -ma"
          And "gpstop -m" should return a return code of 0
          And the user runs "gpstart -a"

    Scenario: Validate if gpecheckcat throws error when there are tables created using mix distribution policy
        Given database "hashops_db" is dropped and recreated
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_legacy_hash_ops_tables.sql"
        Then psql should return a return code of 0
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_non_legacy_hashops_tables.sql"
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -R mix_distribution_policy hashops_db "
        And gpcheckcat should print "Found two types of tables within one database, some use legacy hashops, and others use modern hashops as their distribution policies." to stdout
        And gpcheckcat should print "Please run the gpcheckcat.distpolicy.sql file to list the tables." to stdout
        And gpcheckcat should return a return code of 3
        And the user runs "dropdb hashops_db"

    Scenario: Validate if gpcheckcat succeeds and there are no tables
        Given database "hashops_db" is dropped and recreated
        When the user runs "gpcheckcat -R mix_distribution_policy hashops_db"
        And gpcheckcat should print "PASSED" to stdout
        And the user runs "dropdb hashops_db"

    Scenario: Validate if gpcheckcat throws error when GUC gp_use_legacy_hashops is on and there are non legacy tables
        Given database "hashops_db" is dropped and recreated
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_non_legacy_hashops_tables.sql"
        Then psql should return a return code of 0
        And the user runs "gpconfig -c gp_use_legacy_hashops -v on --skipvalidation"
        Then gpconfig should return a return code of 0
        And the user runs "gpstop -a"
        Then gpstop should return a return code of 0
        And the user runs "gpstart -a"
        When the user runs "gpcheckcat -R mix_distribution_policy hashops_db"
        And gpcheckcat should print "GUC gp_use_legacy_hashops is on." to stdout
        And gpcheckcat should print "all newly created tables will use legacy hash ops by default for hash distributed table," to stdout
        And gpcheckcat should print "but there are tables using non-legacy hash ops in the cluster." to stdout
        And gpcheckcat should print "Please run the gpcheckcat.distpolicy.sql file to list the tables." to stdout
        And gpcheckcat should return a return code of 3
        And the user runs "dropdb hashops_db"

      Scenario: Validate if gpcheckcat succeeds when GUC gp_use_legacy_hashops is on and there are legacy tables
        Given database "hashops_db" is dropped and recreated
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_legacy_hash_ops_tables.sql"
        Then psql should return a return code of 0
        And the user runs "gpconfig -c gp_use_legacy_hashops -v on --skipvalidation"
        Then gpconfig should return a return code of 0
        And the user runs "gpstop -a"
        Then gpstop should return a return code of 0
        And the user runs "gpstart -a"
        When the user runs "gpcheckcat -R mix_distribution_policy hashops_db"
         And gpcheckcat should print "PASSED" to stdout
         And gpcheckcat should return a return code of 0
        And the user runs "dropdb hashops_db"

    Scenario: Validate if gpcheckcat throws error when GUC gp_use_legacy_hashops is off and there are legacy tables
        Given database "hashops_db" is dropped and recreated
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_legacy_hash_ops_tables.sql"
        And the user runs "gpconfig -c gp_use_legacy_hashops -v off --skipvalidation"
        Then gpconfig should return a return code of 0
        And the user runs "gpstop -a"
        Then gpstop should return a return code of 0
        And the user runs "gpstart -a"
        When the user runs "gpcheckcat -R mix_distribution_policy hashops_db"
        And gpcheckcat should print "GUC gp_use_legacy_hashops is off." to stdout
        And gpcheckcat should print "all newly created tables will use non legacy hash ops by default for hash distributed table," to stdout
        And gpcheckcat should print "but there are tables using legacy hash ops in the cluster." to stdout
        And gpcheckcat should print "Please run the gpcheckcat.distpolicy.sql file to list the tables." to stdout
        And gpcheckcat should return a return code of 3
        And the user runs "dropdb hashops_db"

    Scenario: Validate if gpcheckcat succeeds when GUC gp_use_legacy_hashops is off and there are non legacy tables 
        Given database "hashops_db" is dropped and recreated
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_non_legacy_hashops_tables.sql"
        And the user runs "gpconfig -c gp_use_legacy_hashops -v off --skipvalidation"
        Then gpconfig should return a return code of 0
        And the user runs "gpstop -a"
        Then gpstop should return a return code of 0
        And the user runs "gpstart -a"
        When the user runs "gpcheckcat -R mix_distribution_policy hashops_db"
        And gpcheckcat should print "PASSED" to stdout
        And the user runs "dropdb hashops_db"

    Scenario: gpcheckcat -l should report mix_distribution_policy to stdout
        When the user runs "gpcheckcat -l "
        And gpcheckcat should print "mix_distribution_policy" to stdout

    Scenario: gpcheckcat report all tables created using legacy opclass on multiple database
        Given database "hashops_db" is dropped and recreated
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_legacy_hash_ops_tables.sql"
        And the user runs "psql hashops_db -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_non_legacy_hashops_tables.sql"
        Then psql should return a return code of 0
        Given database "hashops_db2" is dropped and recreated
        And the user runs "psql hashops_db2 -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_legacy_hash_ops_tables.sql"
        And the user runs "psql hashops_db2 -f test/behave/mgmt_utils/steps/data/gpcheckcat/create_non_legacy_hashops_tables.sql"
        Then psql should return a return code of 0
        When the user runs "gpcheckcat -A -R mix_distribution_policy"
        And gpcheckcat should print "Found two types of tables within one database, some use legacy hashops, and others use modern hashops as their distribution policies." to stdout
        And gpcheckcat should print "Please run the gpcheckcat.distpolicy.sql file to list the tables." to stdout
        And gpcheckcat should return a return code of 3
        Then gpcheckcat should print "Completed 1 test(s) on database 'hashops_db'" to logfile with latest timestamp
        Then gpcheckcat should print "Completed 1 test(s) on database 'hashops_db2'" to logfile with latest timestamp
        And the user runs "dropdb hashops_db"
        And the user runs "dropdb hashops_db2"
