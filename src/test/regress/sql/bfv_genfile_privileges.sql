--
-- pg_file_write()/pg_file_rename()/pg_file_unlink() must not be executable
-- by an arbitrary role: only superusers and members of the
-- pg_write_server_files role may use them.
--
create user bfv_genfile_regular_user;
create user bfv_genfile_writer_user in role pg_write_server_files;

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

drop user bfv_genfile_regular_user;
drop user bfv_genfile_writer_user;
