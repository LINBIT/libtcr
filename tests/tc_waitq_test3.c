#include <sys/syscall.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "tc/threaded_cr.h"

static int cond = 0;
static struct tc_waitq wq;

pid_t gettid(void)
{
	return syscall(__NR_gettid);
}

static void waiter(void *arg)
{
	int nr = (int)arg;
	int seen = 0;

	while (1) {
		tc_waitq_wait_event(&wq, cond != seen);
		printf("%d: condition got %d (on %d) \n", nr, cond, gettid());
		seen = cond;
	}
}

static void starter(void *unused)
{
	tc_waitq_init(&wq);

	tc_thread_new(waiter, (void *)1, "waiter1");
	tc_thread_new(waiter, (void *)2, "waiter2");
	tc_thread_new(waiter, (void *)3, "waiter3");

	while (1) {
		tc_sleep(CLOCK_MONOTONIC, 0, 100000000); // 100ms
		//tc_sleep(CLOCK_MONOTONIC, 3, 0);
		cond++;
		tc_waitq_wakeup(&wq);
	}
}


int main()
{
	tc_run(starter, NULL, "starter", 4);
	sleep(60);
	return 0;
}
