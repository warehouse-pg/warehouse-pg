--
-- Re-entrant use of the Tcl interpreter from a domain CHECK constraint
-- while the outer call is still converting a Tcl-owned string
-- (CVE-2026-116323).  These must not crash or corrupt memory.
--
create function pltcl_reenter(text) returns boolean as $$
    # clobber the interpreter result and force allocations
    set junk [string repeat "z" 100000]
    set l [list]
    for {set i 0} {$i < 100} {incr i} { lappend l "x$i$junk" }
    return 1
$$ language pltcl;

create domain reenter_dom as text check (pltcl_reenter(value));
create type reenter_row as (a reenter_dom, b int);

-- function result
create function reenter_result() returns reenter_dom as $$
    return [string repeat "A" 5000]
$$ language pltcl;
select length(reenter_result());

-- return_next
create function reenter_next() returns setof reenter_dom as $$
    return_next [string repeat "B" 5000]
    return_next [string repeat "C" 5000]
$$ language pltcl;
select length(x) from reenter_next() x;

-- composite result
create function reenter_tuple() returns reenter_row as $$
    return [list a [string repeat "D" 5000] b 1]
$$ language pltcl;
select length((reenter_tuple()).a);

-- spi_execplan arguments
create function reenter_plan() returns int as $$
    set p [spi_prepare "select length(\$1)" reenter_dom]
    spi_execp $p [list [string repeat "E" 5000]]
    spi_execp $p [list [string repeat "F" 5000]]
    return 1
$$ language pltcl;
select reenter_plan();
