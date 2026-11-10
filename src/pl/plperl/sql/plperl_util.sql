-- test plperl utility functions (defined in Util.xs)

-- test quote_literal

create or replace function perl_quote_literal() returns setof text language plperl as $$
	return_next "undef: ".quote_literal(undef);
	return_next sprintf"$_: ".quote_literal($_)
		for q{foo}, q{a'b}, q{a"b}, q{c''d}, q{e\f}, q{};
	return undef;
$$;

select perl_quote_literal();

-- test quote_nullable

create or replace function perl_quote_nullable() returns setof text language plperl as $$
	return_next "undef: ".quote_nullable(undef);
	return_next sprintf"$_: ".quote_nullable($_)
		for q{foo}, q{a'b}, q{a"b}, q{c''d}, q{e\f}, q{};
	return undef;
$$;

select perl_quote_nullable();

-- test quote_ident

create or replace function perl_quote_ident() returns setof text language plperl as $$
	return_next "undef: ".quote_ident(undef); # generates undef warning if warnings enabled
	return_next "$_: ".quote_ident($_)
		for q{foo}, q{a'b}, q{a"b}, q{c''d}, q{e\f}, q{g.h}, q{};
	return undef;
$$;

select perl_quote_ident();

-- test decode_bytea

create or replace function perl_decode_bytea() returns setof text language plperl as $$
	return_next "undef: ".decode_bytea(undef); # generates undef warning if warnings enabled
	return_next "$_: ".decode_bytea($_)
		for q{foo}, q{a\047b}, q{};
	return undef;
$$;

select perl_decode_bytea();

-- test encode_bytea

create or replace function perl_encode_bytea() returns setof text language plperl as $$
	return_next encode_bytea(undef); # generates undef warning if warnings enabled
	return_next encode_bytea($_)
		for q{@}, qq{@\x01@}, qq{@\x00@}, q{};
	return undef;
$$;

select perl_encode_bytea();

-- test encode_array_literal

create or replace function perl_encode_array_literal() returns setof text language plperl as $$
	return_next encode_array_literal(undef);
	return_next encode_array_literal(0);
	return_next encode_array_literal(42);
	return_next encode_array_literal($_)
		for [], [0], [1..5], [[]], [[1,2,[3]],4];
	return_next encode_array_literal($_,'|')
		for [], [0], [1..5], [[]], [[1,2,[3]],4];
	return undef;
$$;

select perl_encode_array_literal();

-- test encode_array_constructor

create or replace function perl_encode_array_constructor() returns setof text language plperl as $$
	return_next encode_array_constructor(undef);
	return_next encode_array_constructor(0);
	return_next encode_array_constructor(42);
	return_next encode_array_constructor($_)
		for [], [0], [1..5], [[]], [[1,2,[3]],4];
	return undef;
$$;

select perl_encode_array_constructor();

-- test looks_like_number

create or replace function perl_looks_like_number() returns setof text language plperl as $$
	return_next "undef is undef" if not defined looks_like_number(undef);
	return_next quote_nullable($_).": ". (looks_like_number($_) ? "number" : "not number")
		for 'foo', 0, 1, 1.3, '+3.e-4',
			'42 x', # trailing garbage
			'99  ', # trailing space
			'  99', # leading space
			'    ', # only space
			'';     # empty string
	return undef;
$$;

select perl_looks_like_number();

-- test encode_typed_literal
create type perl_foo as (a integer, b text[]);
create type perl_bar as (c perl_foo[]);
create or replace function perl_encode_typed_literal() returns setof text language plperl as $$
	return_next encode_typed_literal(undef, 'text');
	return_next encode_typed_literal([[1,2,3],[3,2,1],[1,3,2]], 'integer[]');
	return_next encode_typed_literal({a => 1, b => ['PL','/','Perl']}, 'perl_foo');
	return_next encode_typed_literal({c => [{a => 9, b => ['PostgreSQL']}, {b => ['Postgres'], a => 1}]}, 'perl_bar');
$$;

select perl_encode_typed_literal();

-- An error while converting the value must be reported to Perl, not
-- longjmp out of the interpreter; on some platforms this crashed after
-- several iterations.
create or replace function perl_encode_typed_literal_bad() returns setof text language plperl as $$
	return_next encode_typed_literal('abc', 'integer');
$$;
select perl_encode_typed_literal_bad();  -- fail
select perl_encode_typed_literal_bad();  -- fail
select perl_encode_typed_literal_bad();  -- fail
select perl_encode_typed_literal_bad();  -- fail

-- encode_typed_literal() can also be reached when PL/Perl is not inside a
-- SPI procedure, e.g. from a tied element that is fetched while the
-- function result is converted.  Both outcomes must leave SPI usable.
create or replace function perl_encode_typed_literal_late(text) returns text[] language plperl as $$
	our $late_val = $_[0];
	package PLPerlLateLiteral;
	sub TIESCALAR { bless \(my $s), $_[0] }
	sub FETCH { main::encode_typed_literal($main::late_val, 'integer') }
	sub STORE { }
	package main;
	my $r = [ 'x' ];
	tie $r->[0], 'PLPerlLateLiteral';
	return $r;
$$;
select perl_encode_typed_literal_late('42');
select perl_encode_typed_literal_late('abc');  -- fail
select perl_encode_typed_literal_late('42');
create or replace function perl_spi_still_works() returns int language plperl as $$
	return spi_exec_query('select 7 as a')->{rows}[0]{a};
$$;
select perl_spi_still_works();
