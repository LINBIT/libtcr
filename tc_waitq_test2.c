#include "threaded_cr.h"
#include <unistd.h>
#include <sys/epoll.h>

static int cond = 0;
static struct tc_waitq wq;

static void waker_superfluous(void *unused)
{
	tc_sleep(CLOCK_MONOTONIC, 0, 1000); /* 1us, as sched yield() .. */
	while (1) {
		tc_waitq_wakeup(&wq);
	}
}

static void starter(void *unused)
{
	int i;

	tc_waitq_init(&wq);
	tc_thread_new(waker_superfluous, NULL, "waker_superfluous");

	while (1) {
		i = 1;
		tc_waitq_wait_event(&wq, i-- == 0);
		tc_sleep(CLOCK_MONOTONIC, 0, 1000); /* 1us, as sched yield() .. */
	}
}


int main()
{
	tc_run(starter, NULL, "test", 4);
	sleep(60);
	return 0;
}
