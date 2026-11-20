--
-- Re-entrant use of the Tcl interpreter from a domain CHECK constraint
-- while the outer call is still converting the function's Tcl-owned
-- result string.  Regression guard for the hardening added against this
-- pattern (not believed to be exploitable on this branch; see commit
-- message).  This must not crash or corrupt memory.
--
create function pltcl_reenter(text) returns boolean as $$
    # clobber the interpreter result and force allocations
    set junk [string repeat "z" 100000]
    set l [list]
    for {set i 0} {$i < 100} {incr i} { lappend l "x$i$junk" }
    return 1
$$ language pltcl;

create domain reenter_dom as text check (pltcl_reenter(value));

create function reenter_result() returns reenter_dom as $$
    return [string repeat "A" 5000]
$$ language pltcl;

select length(reenter_result());
