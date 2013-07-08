/* Triggers "unexpected event" in tc_waitq_finish_wait() */

#include <sys/epoll.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"


struct tc_waitq wq;
struct tc_mutex l;
int c;
int stop=0;

static void waker(void *v)
{
	LL("DEF %p = waker thread", tc_current());
	while (!stop) {
		tc_mutex_lock(&l);
		tc_sleep(CLOCK_MONOTONIC, 0, 10e3);
		tc_waitq_wakeup_all(&wq);
		tc_sleep(CLOCK_MONOTONIC, 0, 3e3);
		tc_mutex_unlock(&l);
	}
}

int waiter(void) {
	LL("c is %d", c);
	if (--c >= 0)
		return 0;

	LL("locking");
	tc_mutex_lock(&l);
	tc_mutex_unlock(&l);
	return rand() & 1;
}

void unlocker(void *v)
{
	tc_mutex_lock(&l);
	tc_sleep(CLOCK_MONOTONIC, 0, 100e3);
	tc_mutex_unlock(&l);
}

static void starter(void *unused)
{
	struct tc_thread *p1;
	int i;

	tc_waitq_init(&wq);
	tc_mutex_init(&l);
	LL("DEF %p = looper thread", tc_current());
	p1 = tc_thread_new(waker, NULL, "waker_valid");

	for(i=0; i<100000; i++) {
		printf("%d\n", i);
		c = 3;

//		tc_thread_new(unlocker, NULL, "unlocker");
		tc_waitq_wait_event(&wq, waiter());
	}

	stop = 1;

	tc_thread_wait(p1);
	printf("done.\n");
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

