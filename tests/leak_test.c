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


	tc_signal_unsubscribe(&the_signal, ed);
	tc_signal_unsubscribe(&the_drbd_signal, es);

	tc_signal_fire(&the_drbd_signal);
}


static void starter(void *unused)
{
	int i;
	struct tc_thread_pool threads;

	tc_signal_init(&the_drbd_signal);
	tc_signal_init(&the_signal);

	for(i=0; i<1000; i++)
	{
		putchar(i & 0x3f ? '.' : '\n');
		tc_thread_pool_new(&threads, drbd_connection, NULL, "DRBD conn %d", 0);
		tc_thread_pool_wait(&threads);

		tc_sleep(CLOCK_MONOTONIC, 0, 10000000);
	}
}


int main()
{
	tc_run(starter, NULL, "test", 0);
	return 0;
}
