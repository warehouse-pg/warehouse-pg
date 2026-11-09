# Test the restore.sql script that pg_dump --format=tar puts in the archive.
#
# restore.sql is meant to be edited and then fed to psql, so it needs the
# same \restrict protection as a plain-format dump (CVE-2026-116330).
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tar = $ENV{TAR};
if (!defined $tar || $tar eq '')
{
	plan skip_all => 'no tar program available';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
my $backupdir = $node->backup_dir;
mkdir($backupdir) unless -d $backupdir;

$node->safe_psql('postgres', 'CREATE DATABASE regression_restore');
$node->safe_psql(
	'regression_restore', q{
	CREATE TABLE t (id int, c text);
	INSERT INTO t VALUES (1, 'one'), (2, E'\n'), (3, '\N');});

# Dump in tar format and return the contents of the archive's restore.sql,
# with $$PATH$$ replaced by the directory holding the extracted data files.
sub dump_and_fetch_restore_sql
{
	my ($name, @extra_dump) = @_;
	my $dump_file = "$backupdir/$name.tar";
	my $unpacked = "$backupdir/$name.unpacked";

	$node->command_ok(
		[
			'pg_dump', '--format=tar', @extra_dump, '--no-sync',
			"--file=$dump_file", $node->connstr('regression_restore'),
		],
		"$name: dump in tar format");

	mkdir($unpacked) || die "mkdir $unpacked: $!";
	system_or_bail($tar, 'xf', $dump_file, '-C', $unpacked);
	my $restore_sql = slurp_file("$unpacked/restore.sql");
	$restore_sql =~ s/'\$\$PATH\$\$/'$unpacked/g;
	return $restore_sql;
}

# The first psql meta-command must be \restrict, the last one \unrestrict
# with the same key, and no command may carry a "(null)" key.
sub check_restrict_commands
{
	my ($name, $restore_sql, $key_re) = @_;
	my @metas = grep { /^\\(?:un)?restrict\b/ } split(/\n/, $restore_sql);

	like($metas[0], qr/^\\restrict ($key_re)$/,
		"$name: restore.sql starts in restricted mode");
	my ($key) = $metas[0] =~ /^\\restrict (.*)$/;
	is($metas[-1], "\\unrestrict $key",
		"$name: restore.sql leaves restricted mode with the same key");
	unlike($restore_sql, qr/\(null\)/,
		"$name: restore.sql has no \"(null)\" restrict key");
	return $key;
}

# Explicit key.
my $restore_sql = dump_and_fetch_restore_sql('explicit', '--restrict-key=test');
check_restrict_commands('explicit', $restore_sql, 'test');

# restore.sql leaves restricted mode only around its \connect.
like(
	$restore_sql,
	qr/^\\unrestrict test\n\\connect [^\n]*\n\\restrict test$/m,
	'explicit: restore.sql re-enters restricted mode after \connect');

# restore.sql must still work when run through psql.  It drops and
# recreates the database, like "pg_dump --clean --create", so connect to
# some other database.
my ($result, $stdout, $stderr) = $node->psql('postgres', $restore_sql);
is($result, 0, 'explicit: restore.sql succeeds');
is($stderr, '', 'explicit: restore.sql has no stderr');
is( $node->safe_psql(
		'regression_restore', 'SELECT count(*), count(c) FROM t'),
	'3|3',
	'explicit: restore.sql restored the table data');

# Generated key.
$restore_sql = dump_and_fetch_restore_sql('generated');
check_restrict_commands('generated', $restore_sql, '[a-zA-Z0-9]+');

# Formats without a psql script still reject the option.
command_fails_like(
	[ 'pg_dump', '--format=custom', '--restrict-key=test', 'postgres' ],
	qr/\Qpg_dump: error: option --restrict-key can only be used with --format=plain or --format=tar\E/,
	'pg_dump: --restrict-key rejected for custom format');

done_testing();
