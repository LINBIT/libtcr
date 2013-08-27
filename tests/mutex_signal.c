/* send a signal while waiting for a mutex.
 * If that fails, re-enabling the signal event after delivery
 * is botched. */
#include <sys/epoll.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

static struct tc_mutex m;
static struct tc_signal the_drbd_signal;

int stop=0;
atomic_t signalled= { 0 };
atomic_t number;

#define SIGNALS_TO_SEND 1000

void worker(void *ttf_vp)
{
	int i;
	struct tc_signal_sub *ss;
	int me = 1 << (atomic_inc(&number) -1);

	ss = tc_signal_subscribe(&the_drbd_signal);
	printf("%p is %x\n", tc_current(), me);

	while (!stop) {
		i = tc_mutex_lock(&m);
		assert(i != RV_OK);
		atomic_add_return(me, &signalled);
//		printf("%p got signal\n", tc_current());
//		tc_sleep(CLOCK_MONOTONIC, 0, 3e3);
	}

	tc_signal_unsubscribe(&the_drbd_signal, ss);
}

void starter(void *unused)
{
	struct tc_thread_pool t;
	struct tc_fd *the_tc_fd = tc_register_fd(0);
	int i, expect, got, d;

	tc_signal_init(&the_drbd_signal);
	tc_mutex_init(&m);
	assert( tc_mutex_lock(&m) == RV_OK);
	tc_thread_pool_new(&t, worker, the_tc_fd, "worker", 0);

	tc_sleep(CLOCK_MONOTONIC, 0, 20e6);
	expect = (1 << tc_thread_count()) -1;

	for(i=0; i<SIGNALS_TO_SEND; i++) {
		/* Send a signal, then check that it arrived. */

		tc_signal_fire(&the_drbd_signal);
		tc_sched_yield();

		for(d=0; d<8; d++)
		{
			got = atomic_read(&signalled);
			if (got == expect)
				break;

			/* 0.1msec, 0.2msec, 0.4msec, ... 12.8msec */
			tc_sleep(CLOCK_MONOTONIC, 0, 0.1e6 * (1 << d));
		}

		if (got != expect) {
			printf("signalled - expected %x, got %x; missing %x\n", expect, got, expect ^ got);
			assert(0);
		}
		atomic_set(&signalled, 0);
	}

	stop = 1;
	tc_signal_fire(&the_drbd_signal);

	tc_thread_pool_wait(&t);

	fprintf(stdout, "success, %d rounds, ending starter.\n", SIGNALS_TO_SEND);
}


int main(int argc, char *args[])
{
	if (argc == 1)
		tc_run(starter, NULL, "test", 0);

	return 0;
}
