use strict;
use warnings;
use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;
use File::Path qw(rmtree);

program_help_ok('pg_waldump');
program_version_ok('pg_waldump');
program_options_handling_ok('pg_waldump');

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->start;
my $pgdata = $node->data_dir;

$node->command_ok(['pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '1', '--last-valid-walname'],
	'pg_waldump emits last valid walsegment');

$node->command_checks_all(['pg_waldump', '-p', "$pgdata/pg_wal/", '--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: error: timeline not specified\nTry "pg_waldump --help" for more information./],
	'pg_waldump fails if timeline not provided');

$node->command_checks_all(['pg_waldump', '-p', "$pgdata/pg_wal/", '-t', 'st', '--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: error: could not parse timeline ".*"\nTry "pg_waldump --help" for more information./],
    'pg_waldump fails with error message when invalid timeline is provided');

$node->command_checks_all(['pg_waldump', '-t', '1', '--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: error: path not specified\nTry "pg_waldump --help" for more information./],
    'pg_waldump fails if pg_wal path not provided using -p');

$node->command_checks_all(['pg_waldump', '-p', "/tmp/pg_wal/", '-t', '1', '--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: error: path ".*" could not be opened: No such file or directory\nTry "pg_waldump --help" for more information./],
    'pg_waldump fails if pg_wal path is invalid');

$node->command_checks_all(['pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '14', '--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: fatal: could not find valid WAL segment for timeline "14"/],
    'no walfile for given timeline exists');

# Generate new walfiles using pg_switch_wal.
$node->safe_psql('postgres', "CREATE TABLE t (); DROP TABLE t; SELECT pg_switch_wal();");
$node->safe_psql('postgres', "CREATE TABLE t (); DROP TABLE t; SELECT pg_switch_wal();");
$node->safe_psql('postgres', "CREATE TABLE t (); DROP TABLE t; SELECT pg_switch_wal();");

# Get the current walfile.
my $current_wal_lsn = $node->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
my $current_walfile = $node->safe_psql('postgres', "SELECT pg_walfile_name('$current_wal_lsn');");

my ($last_valid_walname, $stderr) = run_command(
	[
		'pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '1', '--last-valid-walname'
	]);
ok ($last_valid_walname eq $current_walfile, 'last valid walfile is the most current walfile');

# Assert contents of wal directory
my $expected_wal_directory_content = "000000010000000000000001\n000000010000000000000002\n000000010000000000000003\narchive_status";
my ($wal_directory_content, $err) = run_command(
	[
		'ls', "$pgdata/pg_wal/"
	]);
ok ($wal_directory_content eq $expected_wal_directory_content, 'pg_wal directory contains expected files');

# Create a wal file with zero size
my $corrupt_wal_file = "$pgdata/pg_wal/000000010000000000000004";
open my $file_handle, '>', $corrupt_wal_file or die "Cannot open file $corrupt_wal_file: $!";
close $file_handle;

$node->command_checks_all(['pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '1','--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: fatal: could not read.*000000010000000000000004.*/],
	'pg_waldump fails if any file is of size 0');

# Create a walfile having corrupt header having size of 24 bytes
my $header_size = 24;
open my $fh, '>', $corrupt_wal_file or die "Cannot open WAL file $corrupt_wal_file: $!";

# Simulate corrupt header
my $corrupt_header = "\x00" x $header_size; # Filling the header with null bytes
print $fh $corrupt_header;
close $fh;

$node->command_checks_all(['pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '1','--last-valid-walname'],
    1,
    [qr{^$}],
    [qr/pg_waldump: fatal: could not read.*000000010000000000000004.*/],
	'pg_waldump fails if some files can not be read');

# Create a walfile having corrupt header having 32k size
my $header_size1 = 32*1024;
open my $fh1, '>', $corrupt_wal_file or die "Cannot open WAL file $corrupt_wal_file: $!";

# Simulate corrupt header, but fill xlp_seg_size position with DEFAULT_XLOG_SEG_SIZE.
# Otherwise, the below command may report fatal as above case when set WalSegSz.
my $corrupt_header1 = "\x00" x $header_size1; # Filling the header with null bytes
my $value = 64 * 1024 * 1024;
substr($corrupt_header1, 32, 4) = pack("V", $value);
print $fh1 $corrupt_header1;
close $fh1;

my ($last_valid_walname2, $stderr2) = run_command(
	[
		'pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '1', '--last-valid-walname'
	]);
ok ($last_valid_walname2 eq $current_walfile, 'last valid walfile is not the walfile with corrupt header');

rmtree($corrupt_wal_file);

#  Add some other non-walfiles to ensure that those aren't considered (validating IsXLogFileName)
my $wal_file_name1= "$pgdata/pg_wal/tmp.txt";
open my $file_handle1, '>', $wal_file_name1 or die "Cannot open file $wal_file_name1: $!";
close $file_handle1;

my $wal_file_name2 = "$pgdata/pg_wal/0000000100000000000000ff";
open my $file_handle2, '>', $wal_file_name2 or die "Cannot open file $wal_file_name2: $!";
close $file_handle2;

my ($last_valid_walname3, $stderr3) = run_command(
	[
		'pg_waldump', '-p', "$pgdata/pg_wal/", '-t', '1', '--last-valid-walname'
	]);
ok ($last_valid_walname3 eq $current_walfile, 'invalid wal file names are not considered as last valid wal file');

done_testing();
