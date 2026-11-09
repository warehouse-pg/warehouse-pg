# Test that pg_restore rejects corrupt archives instead of acting on them.
#
# Each case takes a valid archive written by pg_dump, damages one thing in
# it, and checks that pg_restore stops with the matching error.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
my $backupdir = $node->backup_dir;
mkdir($backupdir) unless -d $backupdir;

$node->safe_psql('postgres', 'CREATE DATABASE regression_corrupt');
$node->safe_psql(
	'regression_corrupt', q{
	CREATE TABLE t (id int, c text);
	INSERT INTO t VALUES (1, 'one'), (2, 'two');
	CREATE SCHEMA s;
	CREATE FUNCTION s.f() RETURNS int LANGUAGE sql AS 'SELECT 1';});

sub read_binary
{
	my ($path) = @_;
	open(my $fh, '<:raw', $path) || die "could not open $path: $!";
	local $/;
	my $contents = <$fh>;
	close($fh);
	return $contents;
}

sub write_binary
{
	my ($path, $contents) = @_;
	open(my $fh, '>:raw', $path) || die "could not open $path: $!";
	print $fh $contents;
	close($fh);
	return;
}

# Make a fresh directory-format dump and return its path.
sub dump_directory
{
	my ($name) = @_;
	my $dump = "$backupdir/$name";
	$node->command_ok(
		[
			'pg_dump', '--format=directory', '--compress=0', '--no-sync',
			"--file=$dump", $node->connstr('regression_corrupt'),
		],
		"$name: dump in directory format");
	return $dump;
}

# Apply a substitution to a directory-format archive's toc.dat.  $edit gets
# the TOC in $_ and must change it without changing its length.
sub edit_toc
{
	my ($name, $dump, $edit) = @_;
	local $_ = read_binary("$dump/toc.dat");
	my $old_length = length($_);
	ok($edit->(), "$name: found the TOC bytes to corrupt");
	is(length($_), $old_length, "$name: TOC length is unchanged");
	write_binary("$dump/toc.dat", $_);
	return;
}

sub restore_fails_like
{
	my ($name, $dump, $expected) = @_;
	command_fails_like(
		[ 'pg_restore', "--file=$backupdir/$name.sql", $dump ],
		$expected, "$name: pg_restore rejects the archive");
	return;
}

# The name of the table's data file, as recorded in the TOC.
sub data_file_name
{
	my ($dump) = @_;
	opendir(my $dh, $dump) || die "could not open $dump: $!";
	my @files = grep { /^\d+\.dat$/ } readdir($dh);
	closedir($dh);
	is(scalar(@files), 1, 'dump has one data file');
	return $files[0];
}

# A data file that is a symbolic link.
my $dump = dump_directory('symlink');
my $datafile = data_file_name($dump);
write_binary("$backupdir/outside.dat", "3\tthree\n\\.\n");
unlink("$dump/$datafile") || die "could not remove $dump/$datafile: $!";
symlink("$backupdir/outside.dat", "$dump/$datafile")
  || die "could not create symbolic link: $!";
restore_fails_like('symlink', $dump,
	qr/\Qpg_restore: error: "$dump\/$datafile" is not a regular file\E/);

# A data file name that points outside the archive directory.
$dump = dump_directory('filename');
$datafile = data_file_name($dump);
my $badname = '../' . ('x' x (length($datafile) - 3));
edit_toc('filename', $dump, sub { s/\Q$datafile\E/$badname/ });
restore_fails_like('filename', $dump,
	qr/\Qpg_restore: error: invalid file name in archive: "$badname"\E/);

# A dependency ID that is not a valid dump ID.  In the TOC each dependency
# is a string holding a decimal number, the list ends with a NULL string,
# and the data file name follows.  Make the last dependency of the TABLE
# DATA entry negative.
$dump = dump_directory('dependency');
$datafile = data_file_name($dump);
my $baddep;
edit_toc(
	'dependency',
	$dump,
	sub {
		return 0
		  unless
		  s/(\x00.\x00\x00\x00)\d(\d+)(\x01\x01\x00\x00\x00\x00.\x00\x00\x00\Q$datafile\E)/$1-$2$3/s;
		$baddep = "-$2";
		return 1;
	});
restore_fails_like('dependency', $dump,
	qr/\Qpg_restore: error: dependency ID "$baddep" out of range -- perhaps a corrupt TOC\E/
);

# A dependency ID of zero is accepted.  WHPG 6 writes one for every schema,
# and those archives must stay restorable.  Zero out the last dependency of
# an entry that has no data file.
$dump = dump_directory('dependency_zero');
edit_toc(
	'dependency_zero',
	$dump,
	sub {
		s/(\x00.\x00\x00\x00)(\d+)(\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00)/$1 . ('0' x length($2)) . $3/se;
	});
$node->command_ok(
	[ 'pg_restore', "--file=$backupdir/dependency_zero.sql", $dump ],
	'dependency_zero: pg_restore accepts the archive');

# A DROP statement that does not start with "DROP ".
$dump = dump_directory('dropstmt');
edit_toc('dropstmt', $dump,
	sub { s/DROP FUNCTION s\.f\(\)/XXXX FUNCTION s.f()/ });
restore_fails_like('dropstmt', $dump,
	qr/\Qpg_restore: error: unexpected DROP statement for object type "FUNCTION" -- perhaps a corrupt TOC\E/
);

# Build a tar header for a member with the given name and raw size field.
sub tar_header
{
	my ($name, $size_field) = @_;
	my $header = pack(
		'a100 a8 a8 a8 a12 a12 a8 a1 a100 a6 a2 a32 a32 a8 a8 a155 a12',
		$name, "0000600\0", "0004000\0", "0002000\0",
		$size_field, "00000000000\0", ' ' x 8, '0',
		'', "ustar\0", '00', 'postgres',
		'postgres', "0000000\0", "0000000\0", '',
		'');
	my $checksum = 0;
	$checksum += $_ foreach unpack('C*', $header);
	substr($header, 148, 8) = sprintf("%06o\0 ", $checksum);
	return $header;
}

my $tardump = "$backupdir/valid.tar";
$node->command_ok(
	[
		'pg_dump', '--format=tar', '--no-sync',
		"--file=$tardump", $node->connstr('regression_corrupt'),
	],
	'tar: dump in tar format');
my $tar = read_binary($tardump);

# A tar archive whose first member is not the TOC.
write_binary("$backupdir/member_first.tar",
	tar_header('1.dat', "00000000000\0") . $tar);
restore_fails_like(
	'member_first',
	"$backupdir/member_first.tar",
	qr/\Qpg_restore: error: tar member "1.dat" precedes "toc.dat" -- perhaps a corrupt archive\E/
);

# A tar member whose length does not fit in a pgoff_t.
write_binary("$backupdir/member_length.tar",
	tar_header('toc.dat', "\x80" . ("\xff" x 11)) . $tar);
restore_fails_like(
	'member_length',
	"$backupdir/member_length.tar",
	qr/\Qpg_restore: error: corrupt tar header found in toc.dat (invalid file length)\E/
);

# The valid archives still restore.
$dump = dump_directory('valid');
$node->command_ok([ 'pg_restore', "--file=$backupdir/valid.sql", $dump ],
	'valid directory archive restores');
$node->command_ok(
	[ 'pg_restore', "--file=$backupdir/valid_tar.sql", $tardump ],
	'valid tar archive restores');

done_testing();
