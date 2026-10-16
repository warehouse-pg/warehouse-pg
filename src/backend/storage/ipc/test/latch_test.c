#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"
#include "postgres.h"

#include <sys/socket.h>

#include "utils/memutils.h"

#include "../latch.c"

/*
 * Tests for RemoveWaitEvent().
 *
 * The dispatcher takes a QE's socket out of its wait set when the QE has
 * finished, so that a socket which later becomes readable on its own -- a
 * crashing segment leaves a WARNING and the EOF unread in it -- cannot keep
 * WaitEventSetWait() returning for an event nobody consumes.  Positions have
 * to survive the removal, because callers hand the returned position back and
 * store their own index in the event's user_data.
 */

#define MAX_TEST_EVENTS 8

static int	pairs[MAX_TEST_EVENTS][2];
static int	npairs;

static WaitEventSet *
setup_set(int nevents)
{
	npairs = 0;
	return CreateWaitEventSet(CurrentMemoryContext, nevents);
}

/* A readable/writable socket to register; returns the read end. */
static int
new_socket(void)
{
	int			sv[2];

	assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	pairs[npairs][0] = sv[0];
	pairs[npairs][1] = sv[1];
	npairs++;
	return sv[0];
}

static void
make_readable(int idx)
{
	assert_true(write(pairs[idx][1], "x", 1) == 1);
}

static void
close_sockets(void)
{
	for (int i = 0; i < npairs; i++)
	{
		close(pairs[i][0]);
		close(pairs[i][1]);
	}
	npairs = 0;
}

/*
 * The removed slot is made inert and every other event keeps its position and
 * its user_data.
 */
static void
test__RemoveWaitEvent__keeps_the_positions_of_the_other_events(void **state)
{
	WaitEventSet *set = setup_set(MAX_TEST_EVENTS);
	int			pos[3];

	for (int i = 0; i < 3; i++)
		pos[i] = AddWaitEventToSet(set, WL_SOCKET_READABLE, new_socket(),
								   NULL, (void *) (intptr_t) (100 + i));

	assert_int_equal(pos[0], 0);
	assert_int_equal(pos[1], 1);
	assert_int_equal(pos[2], 2);
	assert_int_equal(set->nevents, 3);

	RemoveWaitEvent(set, pos[1]);

	/* the slot is still there, but inert */
	assert_int_equal(set->nevents, 3);
	assert_int_equal(set->events[1].fd, PGINVALID_SOCKET);
	assert_int_equal(set->events[1].events, 0);

	/* the neighbours are untouched */
	assert_int_equal(set->events[0].pos, 0);
	assert_int_equal(set->events[2].pos, 2);
	assert_true(set->events[0].fd != PGINVALID_SOCKET);
	assert_true(set->events[2].fd != PGINVALID_SOCKET);
	assert_int_equal((int) (intptr_t) set->events[0].user_data, 100);
	assert_int_equal((int) (intptr_t) set->events[2].user_data, 102);

	FreeWaitEventSet(set);
	close_sockets();
}

/*
 * A socket that has been removed is not reported again, even when it is
 * readable -- which is the whole point: a finished QE's socket becomes
 * readable when its segment dies.
 */
static void
test__RemoveWaitEvent__stops_reporting_the_removed_socket(void **state)
{
	WaitEventSet *set = setup_set(MAX_TEST_EVENTS);
	WaitEvent	occurred[MAX_TEST_EVENTS];
	int			pos[3];
	int			nready;

	for (int i = 0; i < 3; i++)
		pos[i] = AddWaitEventToSet(set, WL_SOCKET_READABLE, new_socket(),
								   NULL, (void *) (intptr_t) (100 + i));

	/* every socket has something to read */
	for (int i = 0; i < 3; i++)
		make_readable(i);

	nready = WaitEventSetWait(set, 0, occurred, MAX_TEST_EVENTS, 0);
	assert_int_equal(nready, 3);

	RemoveWaitEvent(set, pos[1]);

	/*
	 * Without the removal this would still return 3 and the caller would go
	 * on skipping the finished QE for ever.
	 */
	nready = WaitEventSetWait(set, 0, occurred, MAX_TEST_EVENTS, 0);
	assert_int_equal(nready, 2);
	for (int i = 0; i < nready; i++)
	{
		assert_true(occurred[i].pos != pos[1]);
		assert_true((int) (intptr_t) occurred[i].user_data == 100 ||
					(int) (intptr_t) occurred[i].user_data == 102);
	}

	FreeWaitEventSet(set);
	close_sockets();
}

/*
 * Removing twice is a no-op, and a socket whose owner has already closed it
 * can still be removed: libpq drops the socket of a broken connection before
 * the dispatcher gets to it.
 */
static void
test__RemoveWaitEvent__is_idempotent_and_survives_a_closed_socket(void **state)
{
	WaitEventSet *set = setup_set(MAX_TEST_EVENTS);
	int			pos;
	int			fd;

	fd = new_socket();
	pos = AddWaitEventToSet(set, WL_SOCKET_READABLE, fd, NULL, NULL);

	/* the owner closed it before we removed the event */
	close(pairs[0][0]);
	close(pairs[0][1]);
	npairs = 0;

	RemoveWaitEvent(set, pos);
	assert_int_equal(set->events[pos].fd, PGINVALID_SOCKET);

	/* second removal does nothing and must not raise */
	RemoveWaitEvent(set, pos);
	assert_int_equal(set->events[pos].fd, PGINVALID_SOCKET);

	/* and the set is still usable for the remaining sockets */
	assert_true(FlushWaitEventSet(set));

	FreeWaitEventSet(set);
	close_sockets();
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const		UnitTest tests[] = {
		unit_test(test__RemoveWaitEvent__keeps_the_positions_of_the_other_events),
		unit_test(test__RemoveWaitEvent__stops_reporting_the_removed_socket),
		unit_test(test__RemoveWaitEvent__is_idempotent_and_survives_a_closed_socket)
	};

	MemoryContextInit();

	return run_tests(tests);
}
