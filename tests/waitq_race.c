#include <sys/epoll.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

#define LOOPS 300e3

static struct tc_waitq wq;
static int loops = LOOPS;

static void waker(void *unused)
{
	struct timespec tv;

	tv.tv_sec = 0;
	while (loops > 0) {
//		tc_sleep(CLOCK_MONOTONIC, 0, 30); /* 30ms */
		tv.tv_nsec = 1 + (rand() & 0xff);
//		LL("sleeping for %d nsec", tv.tv_nsec);
		nanosleep(&tv, &tv);
		tc_waitq_wakeup(&wq);
	}
}


static void starter(void *unused)
{
	int c;

	tc_waitq_init(&wq);
	tc_thread_new(waker, NULL, "waker_valid");
	tc_thread_new(waker, NULL, "waker_valid");
	tc_thread_new(waker, NULL, "waker_valid");

	while (loops > 0) {
		c = 1 + (rand() & 0x7);
		tc_sleep(CLOCK_MONOTONIC, 0, 1 + (rand() & 0xff));
		/* The event should already be in tc->pending, ie. we should get 
		 * signalled between prepare_to_wait and finish_wait. */
		tc_waitq_wait_event(&wq, --c <= 0);
		if (!(loops & 0xff))
			printf("condition got true: %d %d\n", loops, c);
		loops--;
	}
}



int main(int argc, char *args[])
{
	if (argc == 1)
		tc_run(starter, NULL, "test", 4);
	else
		exit(33);

	return 0;
}

