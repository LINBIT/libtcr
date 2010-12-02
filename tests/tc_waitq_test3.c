#include <sys/syscall.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

#define WAITERS 3

static int cond = 0;
static struct tc_waitq wq;
int seen[WAITERS] = {0};

pid_t gettid(void)
{
	return syscall(__NR_gettid);
}

static void waiter(void *arg)
{
	int nr = (long)arg;

	while (1) {
		tc_waitq_wait_event(&wq, cond != seen[nr]);
		printf("%d: condition got %d (on %d) \n", nr, cond, gettid());
		seen[nr] = cond;
	}
}

static void starter(void *unused)
{
	int i;
	tc_waitq_init(&wq);

	for(i=0; i<WAITERS; i++)
		tc_thread_new(waiter, (void *)(long)i, "waiter%d");

	while (cond < 1000) {
		tc_sleep(CLOCK_MONOTONIC, 0, 20e6); // 20ms
		//tc_sleep(CLOCK_MONOTONIC, 3, 0);
		cond++;
		tc_waitq_wakeup(&wq);
	}

	tc_sleep(CLOCK_MONOTONIC, 0, 30e6);
	for(i=0; i<WAITERS; i++)
		if (seen[i] != cond)
			exit(1);
	exit(0);
}


int main()
{
	tc_run(starter, NULL, "starter", 4);
	return 0;
}
