--
-- pg_file_write()/pg_file_rename()/pg_file_unlink() must not be executable
-- by an arbitrary role: only superusers and members of the
-- pg_write_server_files role may use them. pg_logdir_ls() has the same
-- defect on the read side, gated by pg_read_server_files instead.
--
create user bfv_genfile_regular_user;
create user bfv_genfile_writer_user in role pg_write_server_files;
create user bfv_genfile_reader_user in role pg_read_server_files;

-- a regular, non-superuser role must be denied
set session authorization bfv_genfile_regular_user;
select pg_file_write('bfv_genfile_test.txt', 'hello', false);
select pg_file_rename('bfv_genfile_test.txt', 'bfv_genfile_test2.txt', null);
select pg_file_unlink('bfv_genfile_test.txt');
reset session authorization;

-- a pg_write_server_files member is allowed
set session authorization bfv_genfile_writer_user;
select pg_file_write('bfv_genfile_test.txt', 'hello', false);
select pg_file_rename('bfv_genfile_test.txt', 'bfv_genfile_test2.txt', null);
select pg_file_unlink('bfv_genfile_test2.txt');
reset session authorization;

-- the superuser is allowed
select pg_file_write('bfv_genfile_test.txt', 'hello', false);
select pg_file_rename('bfv_genfile_test.txt', 'bfv_genfile_test2.txt', null);
select pg_file_unlink('bfv_genfile_test2.txt');

-- pg_logdir_ls(): a regular role is denied, a pg_read_server_files member
-- and the superuser are allowed. Row content is non-deterministic
-- (depends on which log files happen to exist), so only assert it
-- doesn't error.
set session authorization bfv_genfile_regular_user;
select count(*) >= 0 as ok from pg_logdir_ls() as t(starttime timestamp, filename text);
reset session authorization;

set session authorization bfv_genfile_reader_user;
select count(*) >= 0 as ok from pg_logdir_ls() as t(starttime timestamp, filename text);
reset session authorization;

select count(*) >= 0 as ok from pg_logdir_ls() as t(starttime timestamp, filename text);

-- The checks above run on a fresh initdb, where the REVOKE FROM public in
-- system_views.sql already denies bfv_genfile_regular_user at the ACL
-- layer, before the requireWriteServerFilesPrivilege()/
-- requireReadServerFilesPrivilege() checks in genfile.c ever run. On an
-- upgraded cluster proacl stays NULL forever (no initdb), so those C-level
-- checks are the only defense; simulate that here by explicitly granting
-- EXECUTE to a plain role and confirming genfile.c still denies it.
grant execute on function pg_file_write(text,text,boolean) to bfv_genfile_regular_user;
grant execute on function pg_file_rename(text,text,text) to bfv_genfile_regular_user;
grant execute on function pg_file_unlink(text) to bfv_genfile_regular_user;
grant execute on function pg_logdir_ls() to bfv_genfile_regular_user;
set session authorization bfv_genfile_regular_user;
select pg_file_write('bfv_genfile_test.txt', 'hello', false);
select pg_file_rename('bfv_genfile_test.txt', 'bfv_genfile_test2.txt', null);
select pg_file_unlink('bfv_genfile_test.txt');
select count(*) >= 0 as ok from pg_logdir_ls() as t(starttime timestamp, filename text);
reset session authorization;
revoke execute on function pg_file_write(text,text,boolean) from bfv_genfile_regular_user;
revoke execute on function pg_file_rename(text,text,text) from bfv_genfile_regular_user;
revoke execute on function pg_file_unlink(text) from bfv_genfile_regular_user;
revoke execute on function pg_logdir_ls() from bfv_genfile_regular_user;

drop user bfv_genfile_regular_user;
drop user bfv_genfile_writer_user;
drop user bfv_genfile_reader_user;
