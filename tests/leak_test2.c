#include <tcr/threaded_cr.h>
#include <tcr/atomic.h>
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
	struct tc_signal_sub *ed, *es;


	ed = tc_signal_subscribe(&the_drbd_signal);
	es = tc_signal_subscribe(&the_signal);

	tc_sleep(CLOCK_MONOTONIC, 0, 10000000);


	tc_signal_unsubscribe(&the_signal, es);
	tc_signal_unsubscribe(&the_drbd_signal, ed);

	tc_signal_fire(&the_drbd_signal);
}


void writer(void *unused)
{
	fprintf(stderr, "starting writer %p\n", tc_current());
	tc_sleep(CLOCK_MONOTONIC, 0, 100000000);
	fprintf(stderr, "Ending writer %p\n", tc_current());
}

void accepter(void *unused)
{
	struct tc_thread *the_writer;
	struct tc_signal_sub *ss;

	ss = tc_signal_subscribe(&the_drbd_signal);

	the_writer = tc_thread_new(writer, NULL, "writer");
	while (tc_thread_wait(the_writer) == RV_INTR) {
		fprintf(stderr, "RV_INTR in tc_thread_wait(%p)\n", the_writer);
	}
	tc_signal_unsubscribe(&the_drbd_signal, ss);
}


static void starter(void *unused)
{
	struct tc_thread_pool threads;
	struct tc_thread *the_accepter;
	int i;

	tc_signal_init(&the_drbd_signal);
	tc_signal_init(&the_signal);


	for(i=0; i<3000; i++)
	{
		fprintf(stderr, "new round\n");
		the_accepter = tc_thread_new(accepter, NULL, "accepter");
		tc_thread_pool_new(&threads, drbd_connection, NULL, "DRBD conn %d", 0);
		tc_thread_pool_wait(&threads);

		tc_thread_wait(the_accepter);

		tc_sleep(CLOCK_MONOTONIC, 0, 10000000);
		fflush(NULL);
	}
}


int main()
{
	tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
