#include <sys/epoll.h>
#include <unistd.h>

#include "tc/threaded_cr.h"

static int cond = 0;
static struct tc_waitq wq;

static void waker_superfluous(void *unused)
{
	while (1) {
		tc_sleep(CLOCK_MONOTONIC, 0, 1000); /* 1us, as sched yield() .. */
		// printf("waking\n");
		tc_waitq_wakeup(&wq);
	}
}

static void waker_valid(void *unused)
{
	while (1) {
		tc_sleep(CLOCK_MONOTONIC, 0, 100000000); /* 100ms */
		cond = 1;
		tc_waitq_wakeup(&wq);
	}
}

static void starter(void *unused)
{
	tc_waitq_init(&wq);
	tc_thread_new(waker_valid, NULL, "waker_valid");
	tc_thread_new(waker_superfluous, NULL, "waker_superfluous");

	while (1) {
		tc_waitq_wait_event(&wq, cond == 1);
		printf("condition got true\n");
		cond = 0;
	}
}


int main()
{
	tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN));
	sleep(60);
	return 0;
}
