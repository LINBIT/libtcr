// #include "drbd-proxy.h"
#include <tc/threaded_cr.h>
#include <tc/atomic.h>
// #include "helpers.h"
// #include "control.h"
#include <sys/epoll.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/queue.h>
#include <semaphore.h>
#include <arpa/inet.h>


struct tc_signal the_drbd_signal;
struct tc_signal the_signal;

void drbd_connection(void *unused)
{
	static int thread_no = 0;
	int my_thread_no = thread_no++;

	fprintf(stderr, "DRBD reader %d started.\n", my_thread_no);

	tc_signal_enable(&the_drbd_signal);
	tc_signal_enable(&the_signal);

	tc_sleep(CLOCK_MONOTONIC, 0, 10000000);

	fprintf(stderr, "%d: ending DRBD connection\n", my_thread_no);

	tc_signal_disable(&the_signal);
	tc_signal_disable(&the_drbd_signal);

	tc_signal_fire(&the_drbd_signal);
	//tc_signal_fire(&the_drbd_signal);
}


void writer(void *unused)
{
	fprintf(stderr, "starting writer\n");
	tc_sleep(CLOCK_MONOTONIC, 0, 100000000);
	fprintf(stderr, "Ending writer\n");
}

void accepter(void *unused)
{
	struct tc_thread *the_writer;

	fprintf(stderr, "starting accepter\n");
	tc_signal_enable(&the_drbd_signal);

	the_writer = tc_thread_new(writer, NULL, "writer");
	while (tc_thread_wait(the_writer) == RV_INTR) {
		fprintf(stderr, "RV_INTR in tc_thread_wait(%p)\n", the_writer);
	}
	tc_signal_disable(&the_drbd_signal);
	fprintf(stderr, "ending accepter\n");
}


static void starter(void *unused)
{
	struct tc_threads threads;
	struct tc_thread *the_accepter;

	tc_signal_init(&the_drbd_signal);
	tc_signal_init(&the_signal);

	while (1) {
		the_accepter = tc_thread_new(accepter, NULL, "accepter");
		tc_threads_new(&threads, drbd_connection, NULL, "DRBD conn %d");
		fprintf(stderr, "into tc_threads_wait\n");
		tc_threads_wait(&threads);
		fprintf(stderr, "out of tc_threads_wait\n");

		fprintf(stderr, "into tc_thread_wait accepter\n");
		tc_thread_wait(the_accepter);
		fprintf(stderr, "out of tc_thread_wait accepter\n");

		tc_sleep(CLOCK_MONOTONIC, 0, 10000000);
	}
}


int main()
{
	tc_run(starter, NULL, "test", 10);
	return 0;
}
