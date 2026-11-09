#!/usr/bin/env perl
#
# Checks the dumps of the schema made by test_gpdb_pre_attribute_versions.sql:
# what pg_dump emits for attribute versions and dropped columns in binary
# upgrade mode, and that a plain dump has none of it.
#
# usage: test_gpdb_attribute_versions_dump.pl <binary-upgrade dump> <plain dump>

use strict;
use warnings;

my ($bu_file, $plain_file) = @ARGV;
my $failed = 0;

sub slurp
{
	my ($file) = @_;
	local $/;
	open(my $fh, '<', $file) or die "could not open $file: $!";
	my $content = <$fh>;
	close($fh);
	return $content;
}

sub check
{
	my ($ok, $name) = @_;
	if ($ok)
	{
		print "ok - $name\n";
	}
	else
	{
		print "not ok - $name\n";
		$failed = 1;
	}
}

my $bu = slurp($bu_file);
my $plain = slurp($plain_file);

# Iff in binary upgrade mode, dropped attributes and columns must be included
# in the CREATE TABLE/TYPE.
check( $bu =~ /^
		\QCREATE TYPE upgrade_attver.rowdropt_comp AS (\E
		[^;]*
		\"\.+pg\.dropped\.\d\.+\"\Q INTEGER \/* dummy *\/\E
		/xms,
	'dropped attribute, binary upgrade only');
check( $bu =~ /^
		\QCREATE TABLE upgrade_attver.rowdrop_src (\E
		[^;]*
		\"\.+pg\.dropped\.\d\.+\"\Q INTEGER \/* dummy *\/\E
		/xms,
	'dropped column, binary upgrade only');
check($plain !~ /pg\.dropped/, 'no dropped column in a plain dump');

# Attribute and column versions are supplied ahead of the CREATE that consumes
# them iff in binary upgrade mode.
check( $bu =~ /^
		\QSELECT binary_upgrade.set_next_attribute_versions(\E [^;]* \Q);\E
		(?:(?!^CREATE\b).)*?
		^CREATE\sTYPE\supgrade_attver\.rowver_comp\b
		/xms,
	'attribute version, binary upgrade only');
check( $bu =~ /^
		\QSELECT binary_upgrade.set_next_attribute_versions(\E [^;]* \Q);\E
		(?:(?!^CREATE\b).)*?
		^CREATE\sTABLE\supgrade_attver\.rowalt\b
		/xms,
	'column version, binary upgrade only');

# One CREATE TABLE creates a partitioned table with all its partitions, so
# the versions of each of them are supplied ahead of it: the root, two
# partitions and four subpartitions.
my ($before_rowpart) =
  $bu =~ /^((?:(?!^CREATE\b).)*)^CREATE\sTABLE\supgrade_attver\.rowpart\s\(/ms;
$before_rowpart = '' unless defined $before_rowpart;
$before_rowpart =~ s/.*^CREATE\b//ms;
my $nversions = () =
  $before_rowpart =~ /binary_upgrade\.set_next_attribute_versions\(/g;
check($nversions == 7,
	"versions of a partitioned table and its 6 partitions (found $nversions)");

check($plain !~ /set_next_attribute_versions/,
	'no attribute versions in a plain dump');
check($plain !~ /pg_attver/, 'no reserved attribute option in a plain dump');

exit $failed;
