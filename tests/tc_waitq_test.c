#include <sys/epoll.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

#define LOOPS 300

static int cond = 0;
static struct tc_waitq wq;
static int loops = LOOPS;

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
	while (loops-- > 0) {
		printf("wakeup %d\n", loops);
		tc_sleep(CLOCK_MONOTONIC, 0, 30e6); /* 30ms */
		cond = 1;
		tc_waitq_wakeup(&wq);
	}
}

static void starter(void *unused)
{
	int c = 0;

	tc_waitq_init(&wq);
	tc_thread_new(waker_valid, NULL, "waker_valid");
	tc_thread_new(waker_superfluous, NULL, "waker_superfluous");

	while (loops > 0) {
		tc_waitq_wait_event(&wq, cond == 1);
		c++;
		printf("condition got true: %d\n", c);
		cond = 0;
	}

	if (c > LOOPS) {
		fprintf(stderr, "superfluous wakeups!\n");
		exit (1);
	}
}


int main()
{
	tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
