#include "../resgroupcmds.c"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

/*
 * getCpuSetByRole() splits CPUSET='<coordinator_cores>;<segment_cores>'
 * by the role of the current node.  These tests pin the direction of the
 * split, which was silently reversed once before (commit 8588bc98803).
 */

static void
test__getCpuSetByRole_coordinator_takes_part_before_semicolon(void **state)
{
    GpIdentity.segindex = COORDINATOR_CONTENT_ID;
    assert_string_equal(getCpuSetByRole("1;4"), "1");
    assert_string_equal(getCpuSetByRole("0-3;4-7"), "0-3");
}

static void
test__getCpuSetByRole_segment_takes_part_after_semicolon(void **state)
{
    GpIdentity.segindex = 0;
    assert_string_equal(getCpuSetByRole("1;4"), "4");
    assert_string_equal(getCpuSetByRole("0-3;4-7"), "4-7");
}

static void
test__getCpuSetByRole_single_part_shared_by_both_roles(void **state)
{
    GpIdentity.segindex = COORDINATOR_CONTENT_ID;
    assert_string_equal(getCpuSetByRole("1"), "1");
    GpIdentity.segindex = 0;
    assert_string_equal(getCpuSetByRole("1"), "1");
}

int
main(int argc, char *argv[])
{
    cmockery_parse_arguments(argc, argv);

    const UnitTest tests[] = {
        unit_test(test__getCpuSetByRole_coordinator_takes_part_before_semicolon),
        unit_test(test__getCpuSetByRole_segment_takes_part_after_semicolon),
        unit_test(test__getCpuSetByRole_single_part_shared_by_both_roles),
    };

    MemoryContextInit();
    return run_tests(tests);
}
