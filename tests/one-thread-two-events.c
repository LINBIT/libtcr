/* Triggers "unexpected event" in tc_waitq_finish_wait() */

#include <sys/epoll.h>
#include <sys/wait.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"


struct tc_waitq wq;
struct tc_mutex l;
int c;
int funlocker;

static void waker(void *v)
{
	while (1) {
		tc_sleep(CLOCK_MONOTONIC, 0, 100);
		tc_waitq_wakeup_all(&wq);
	}
}

void unlocker(void *v)
{
	funlocker++;
	tc_sleep(CLOCK_MONOTONIC, 0, 10e3);
	tc_mutex_unlock(&l);
	funlocker--;
}


int waiter(void) {
	int rv;
	struct tc_thread *t;

	if (--c >= 0)
		return 0;

	t = tc_thread_new(unlocker, NULL, "unlocker");

	rv = tc_mutex_lock(&l);
	assert(!rv);
	tc_mutex_unlock(&l);
	tc_thread_wait(t);
	return 1;
}

static void starter(void *unused)
{
	int i, rv;

	funlocker = 0;
	srand(22);
	tc_waitq_init(&wq);
	tc_mutex_init(&l);
	tc_thread_new(waker, NULL, "waker_valid");

	for(i=0; i<30000; i++) {
		c = 3;

		rv = tc_mutex_lock(&l);
		assert(!rv);
		if (!(i & 0x0ff))
			printf("%d\n", i);

		tc_waitq_wait_event(&wq, waiter());
		assert(!funlocker);
	}
}

/* TODO: make threads jump around on the pthreads */


int main(int argc, char *args[])
{
	if (argc == 1)
		tc_run(starter, NULL, "test", 4);
	else
		exit(33);

	return 0;
}

