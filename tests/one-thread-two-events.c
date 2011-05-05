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
	LL("DEF %p = waker thread", tc_current());
	while (1) {
		tc_sleep(CLOCK_MONOTONIC, 0, 100);
		tc_waitq_wakeup_all(&wq);
	}
}

void unlocker(void *v)
{
	funlocker++;
	LL("unlocker start");
	tc_sleep(CLOCK_MONOTONIC, 0, 10e3);
	LL("unlocker unlock");
	tc_mutex_unlock(&l);
	LL("unlocker end");
	funlocker--;
}


int waiter(void) {
	int rv;
	struct tc_thread *t;

	LL("c is %d", c);
	if (--c >= 0)
		return 0;

	t = tc_thread_new(unlocker, NULL, "unlocker");

	rv = tc_mutex_lock(&l);
	LL("locking: %d", rv);
	assert(!rv);
	tc_mutex_unlock(&l);
	tc_thread_wait(t);
	LL("unlocker %p quit", t);
	return 1;
}

static void starter(void *unused)
{
	int i, rv;

	funlocker = 0;
	srand(22);
	tc_waitq_init(&wq);
	tc_mutex_init(&l);
	LL("DEF %p = looper thread", tc_current());
	tc_thread_new(waker, NULL, "waker_valid");

	for(i=0; i<30000; i++) {
		LL("next loop %d", i);
		c = 3;

		rv = tc_mutex_lock(&l);
		assert(!rv);
		if (!(i & 0x0ff))
			printf("%d\n", i);

		tc_waitq_wait_event(&wq, waiter());
		LL("unlocker = %d", funlocker);
		assert(!funlocker);
	}
}

/* TODO: make threads jump around on the pthreads */


int main(int argc, char *args[])
{
	if (argc == 1)
		tc_run(starter, NULL, "test", 4);
	else
		TCR_DEBUG_PARSE(args[1]);

	return 0;
}

