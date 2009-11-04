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
	struct tc_signal_sub *ed, *es;

	fprintf(stderr, "DRBD reader %d started.\n", my_thread_no);

	ed = tc_signal_subscribe(&the_drbd_signal);
	es = tc_signal_subscribe(&the_signal);

	tc_sleep(CLOCK_MONOTONIC, 0, 10000000);

	fprintf(stderr, "%d: ending DRBD connection\n", my_thread_no);

	tc_signal_unsubscribe(&the_signal, es);
	tc_signal_unsubscribe(&the_drbd_signal, ed);

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
	struct tc_signal_sub *e;

	fprintf(stderr, "starting accepter\n");
	e = tc_signal_subscribe(&the_drbd_signal);

	the_writer = tc_thread_new(writer, NULL, "writer");
	while (tc_thread_wait(the_writer) == RV_INTR) {
		fprintf(stderr, "RV_INTR in tc_thread_wait(%p)\n", the_writer);
	}
	tc_signal_unsubscribe(&the_drbd_signal, e);
	fprintf(stderr, "ending accepter\n");
}


static void starter(void *unused)
{
	struct tc_thread_pool threads;
	struct tc_thread *the_accepter;

	tc_signal_init(&the_drbd_signal);
	tc_signal_init(&the_signal);

	while (1) {
		the_accepter = tc_thread_new(accepter, NULL, "accepter");
		tc_thread_pool_new(&threads, drbd_connection, NULL, "DRBD conn %d");
		fprintf(stderr, "into tc_thread_pool_wait\n");
		tc_thread_pool_wait(&threads);
		fprintf(stderr, "out of tc_thread_pool_wait\n");

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
