-- Test tied Perl values via handmade classes, so as not to require
-- Tie::Hash and Tie::Array; some popular platforms don't have those
-- in their base Perl installations.

-- WHPG: a trigger runs on the segment that owns the row, and that
-- segment's interpreter never ran a coordinator-only DO block.  Define
-- the classes in a function and run it on the coordinator and on every
-- segment.
CREATE FUNCTION tied_classes() RETURNS int AS $$
{
	package PLPerlTiedHash;
	sub TIEHASH  { bless {}, $_[0] }
	sub STORE    { $_[0]{$_[1]} = $_[2] }
	sub FETCH    { $_[0]{$_[1]} }
	sub FIRSTKEY { my $a = keys %{$_[0]}; each %{$_[0]} }
	sub NEXTKEY  { each %{$_[0]} }

	package PLPerlTiedArray;
	sub TIEARRAY  { bless [], $_[0] }
	sub STORE     { $_[0][$_[1]] = $_[2] }
	sub FETCH     { $_[0][$_[1]] }
	sub FETCHSIZE { scalar @{$_[0]} }

	package PLPerlTiedScalar;
	sub TIESCALAR { my $s; bless \$s, $_[0] }
	sub STORE     { ${$_[0]} = $_[1] }
	sub FETCH     { ${$_[0]} }

	package PLPerlTiedReturnNext;
	sub TIESCALAR { my $n = 0; bless \$n, $_[0] }
	sub FETCH     { main::return_next({f1 => 2, f2 => 'inner'})
	                  unless ${$_[0]}++; 'outer' }
}
return 1;
$$ LANGUAGE plperl;
SELECT tied_classes();
SELECT count(*) > 0 FROM (SELECT tied_classes() FROM gp_dist_random('gp_id')) s;

CREATE FUNCTION tied_setof() RETURNS SETOF text AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 'a'; $a[1] = 'b'; $a[2] = 'c';
	return \@a;
$$ LANGUAGE plperl;

SELECT * FROM tied_setof();

CREATE FUNCTION tied_scalar() RETURNS text AS $$
	my $s;
	tie $s, 'PLPerlTiedScalar';
	$s = 'hello';
	return $s;
$$ LANGUAGE plperl;

SELECT tied_scalar();

CREATE TYPE tiedrowperl AS (f1 integer, f2 text, f3 text);

CREATE FUNCTION tied_composite() RETURNS tiedrowperl AS $$
	my %h;
	tie %h, 'PLPerlTiedHash';
	$h{f1} = 1; $h{f2} = 'hello'; $h{f3} = 'world';
	return \%h;
$$ LANGUAGE plperl;

SELECT tied_composite();

CREATE FUNCTION tied_int_array() RETURNS integer[] AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 1; $a[1] = 2; $a[2] = 3;
	return \@a;
$$ LANGUAGE plperl;

SELECT tied_int_array();

-- Tied scalar whose FETCH returns an arrayref.
CREATE FUNCTION tied_scalar_setof() RETURNS SETOF text AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 'x'; $a[1] = 'y';
	my $s;
	tie $s, 'PLPerlTiedScalar';
	$s = \@a;
	return $s;
$$ LANGUAGE plperl;

SELECT * FROM tied_scalar_setof();

CREATE FUNCTION tied_scalar_int_array() RETURNS integer[] AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 4; $a[1] = 5;
	my $s;
	tie $s, 'PLPerlTiedScalar';
	$s = \@a;
	return $s;
$$ LANGUAGE plperl;

SELECT tied_scalar_int_array();

-- Artificial: tied scalar whose FETCH recursively calls return_next
-- while the outer return_next is still converting the row containing it.
CREATE FUNCTION tied_reentrant_setof() RETURNS SETOF tiedrowperl AS $$
	my %h;
	$h{f1} = 1;
	tie $h{f2}, 'PLPerlTiedReturnNext';
	return_next(\%h);
	return undef;
$$ LANGUAGE plperl;

SELECT * FROM tied_reentrant_setof();

-- Artificial: $_TD->{new} and the trigger return value are tied scalars.
CREATE TABLE tied_trigger_test (i int, v text);

CREATE FUNCTION tied_modify() RETURNS trigger AS $$
	my %h;
	tie %h, 'PLPerlTiedHash';
	$h{i} = $_TD->{new}{i};
	$h{v} = 'from_tie';
	my $new;
	tie $new, 'PLPerlTiedScalar';
	$new = \%h;
	$_TD->{new} = $new;
	my $ret;
	tie $ret, 'PLPerlTiedScalar';
	$ret = 'MODIFY';
	return $ret;
$$ LANGUAGE plperl;

CREATE TRIGGER tied_modify_trig BEFORE INSERT ON tied_trigger_test
FOR EACH ROW EXECUTE PROCEDURE tied_modify();

INSERT INTO tied_trigger_test (i, v) VALUES (1, 'orig');

SELECT i, v FROM tied_trigger_test;

-- Verify that we can cope with errors thrown within get magic.
CREATE OR REPLACE FUNCTION tied_classes() RETURNS int AS $$
{
	package PLPerlTiedHash;
	sub TIEHASH  { bless {}, $_[0] }
	sub STORE    { $_[0]{$_[1]} = $_[2] }
	sub FETCH    { die("tied hash fetch failure"); $_[0]{$_[1]} }
	sub FIRSTKEY { my $a = keys %{$_[0]}; each %{$_[0]} }
	sub NEXTKEY  { each %{$_[0]} }

	package PLPerlTiedArray;
	sub TIEARRAY  { bless [], $_[0] }
	sub STORE     { $_[0][$_[1]] = $_[2] }
	sub FETCH     { die("tied array fetch failure"); $_[0][$_[1]] }
	sub FETCHSIZE { scalar @{$_[0]} }

	package PLPerlTiedScalar;
	sub TIESCALAR { my $s; bless \$s, $_[0] }
	sub STORE     { ${$_[0]} = $_[1] }
	sub FETCH     { die("tied scalar fetch failure"); ${$_[0]} }
}
return 1;
$$ LANGUAGE plperl;
SELECT tied_classes();
SELECT count(*) > 0 FROM (SELECT tied_classes() FROM gp_dist_random('gp_id')) s;

SELECT * FROM tied_setof();
SELECT tied_scalar();
SELECT tied_composite();
SELECT tied_int_array();
SELECT * FROM tied_scalar_setof();
SELECT tied_scalar_int_array();
INSERT INTO tied_trigger_test (i, v) VALUES (2, 'orig');

-- SPI functions called when PL/Perl is not inside a SPI procedure: here,
-- from a tied element that is fetched while the function result is
-- converted, after the handler has disconnected from SPI.  The calls
-- must fail cleanly and leave SPI usable, also for an outer procedure.
CREATE FUNCTION tied_late_spi(text) RETURNS text[] AS $$
	our $late_what = $_[0];
	package PLPerlLateSPI;
	sub TIESCALAR { bless \(my $s), $_[0] }
	sub FETCH
	{
		return main::spi_exec_query('select 1 as a')->{status}
			if $main::late_what eq 'exec';
		main::spi_prepare('select 1 as a')
			if $main::late_what eq 'prepare';
		main::spi_query('select 1 as a')
			if $main::late_what eq 'query';
		return defined main::spi_fetchrow('nosuchcursor') ? 'row' : 'no row';
	}
	sub STORE { }
	package main;
	my $r = [ 'x' ];
	tie $r->[0], 'PLPerlLateSPI';
	return $r;
$$ LANGUAGE plperl;

SELECT tied_late_spi('exec');
SELECT tied_late_spi('prepare');
SELECT tied_late_spi('query');
SELECT tied_late_spi('fetchrow');

CREATE FUNCTION tied_late_spi_outer(text) RETURNS text AS $$
DECLARE
	a text[];
	n int;
BEGIN
	BEGIN
		a := tied_late_spi($1);
	EXCEPTION WHEN OTHERS THEN
		a := ARRAY['caught'];
	END;
	SELECT count(*) INTO n FROM pg_class WHERE relname = 'pg_class';
	RETURN a[1] || ', outer SPI sees ' || n || ' row';
END
$$ LANGUAGE plpgsql;

SELECT tied_late_spi_outer('exec');
SELECT tied_late_spi_outer('prepare');

CREATE FUNCTION tied_spi_still_works() RETURNS int AS $$
	return spi_exec_query('select 7 as a')->{rows}[0]{a};
$$ LANGUAGE plperl;

SELECT tied_spi_still_works();
