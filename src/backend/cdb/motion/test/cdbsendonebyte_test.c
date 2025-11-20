#include <unistd.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "../../motion/ic_udpifc.c"

/*
 * Use a 10-second timeout to guarantee that the polling path is exercised
 * during testing and to eliminate unexpected race conditions.
 */
#undef RX_THREAD_POLL_TIMEOUT
#define RX_THREAD_POLL_TIMEOUT (10000)

/*
 * PROTOTYPES
 */

extern ssize_t __real_sendto(int sockfd, const void *buf, size_t len, int flags,
							 const struct sockaddr *dest_addr, socklen_t addrlen);
int __wrap_errcode(int sqlerrcode);
int __wrap_errdetail(const char *fmt,...);
int __wrap_errmsg(const char *fmt,...);
ssize_t __wrap_sendto(int sockfd, const void *buf, size_t len, int flags,
					  const struct sockaddr *dest_addr, socklen_t addrlen);
void __wrap_elog_finish(int elevel, const char *fmt,...);
void __wrap_elog_start(const char *filename, int lineno, const char *funcname);
void __wrap_errfinish(int dummy __attribute__((unused)),...);
void __wrap_write_log(const char *fmt,...);
bool __wrap_errstart(int elevel, const char *domain);

/*
 * WRAPPERS
 */

int __wrap_errcode(int sqlerrcode)  {return 0;}
int __wrap_errdetail(const char *fmt,...) { return 0; }
int __wrap_errmsg(const char *fmt,...) { return 0; }
void __wrap_elog_start(const char *filename, int lineno, const char *funcname) {}
void __wrap_errfinish(int dummy __attribute__((unused)),...) {}
bool __wrap_errstart(int elevel, const char *domain){ return false;}

void
__wrap_write_log(const char *fmt,...)
{
	printf("%s\n", fmt);
}

void
__wrap_elog_finish(int elevel, const char *fmt,...)
{
	assert_true(elevel <= LOG);
}

ssize_t
__wrap_sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	assert_true(sockfd != PGINVALID_SOCKET);
#if defined(__darwin__)
	if (udp_dummy_packet_sockaddr.ss_family == AF_INET6)
	{
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) dest_addr;
		char address[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &in6->sin6_addr, address, sizeof(address));
		/* '::' and '::1' should always be '::1' */
		assert_true(strcmp("::1", address) == 0);
	}
#endif

	return	__real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

static void
start_receiver()
{
	pthread_attr_t	t_atts;
	sigset_t		pthread_sigs;
	int				pthread_err;

	pthread_attr_init(&t_atts);
	pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (128 * 1024)));
	ic_set_pthread_sigmasks(&pthread_sigs);
	pthread_err = pthread_create(&ic_control_info.threadHandle, &t_atts, rxThreadFunc, NULL);
	ic_reset_pthread_sigmasks(&pthread_sigs);

	pthread_attr_destroy(&t_atts);
	if (pthread_err != 0)
	{
		ic_control_info.threadCreated = false;
		printf("failed to create thread");
		fail();
	}

	ic_control_info.threadCreated = true;
}


/* START UNIT TEST */
static void
test_run_self_pipe(void **state)
{
	uint16 listenerPort;
	int txFamily;
	setupUDPListeningSocket(&UDP_listenerFd, &listenerPort, &txFamily);

	/*
	 * The receiver thread blocks on the read end of the pipe.
	 * The main thread signals it by writing a byte to the write end.
	 * In rxThreadFunc, poll should report the read descriptor as ready.
	 */
	struct timeval start, end;
	long start_ms, end_ms;

	/* Initialize the self-pipe mechanism used for UDP receiver termination */
	setupUDPReceiverTerminateSelfPipe();

	/* Record start timestamp */
	gettimeofday(&start, NULL);

	start_receiver();
	proc_exit_inprogress = true;
	/* Allow the receiver thread to enter its polling loop */
	sleep(1);

	/* Trigger receiver shutdown and wait for it to exit */
	WaitInterconnectQuitUDPIFC();

	/* Record end timestamp */
	gettimeofday(&end, NULL);

	start_ms = start.tv_sec * 1000L + start.tv_usec / 1000L;
	end_ms =  end.tv_sec * 1000L + end.tv_usec / 1000L;

	if (end_ms - start_ms > RX_THREAD_POLL_TIMEOUT + 1000)
	{
		printf("expect to receive the signal immediately, but took %lu ms\n", end_ms - start_ms);
		fail();
	}
}

int
main(int argc, char* argv[])
{
	cmockery_parse_arguments(argc, argv);

	/* set up debug log level */
	log_min_messages = DEBUG1;
	gp_log_interconnect = GPVARS_VERBOSITY_DEBUG;

	const UnitTest tests[] = {
		unit_test(test_run_self_pipe),
	};
	return run_tests(tests);
}
