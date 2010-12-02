#include <sys/epoll.h>
#include <unistd.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

static struct tc_waitq wq;
static int wakeups = 0;

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
		wakeups ++;
		printf("wakeup %d\n", wakeups);
	}
}

void sig(int s)
{
	printf("%d wakeups.\n", wakeups);
	exit(wakeups < 30000 ? 1 : 0);
}

int main()
{
	signal(SIGALRM, sig);
	alarm(10);
	tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
