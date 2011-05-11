/* Triggers "unexpected event" in tc_waitq_finish_wait() */

#include <sys/epoll.h>
#include <sys/wait.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

#define DEPTH 8

struct tc_waitq wq[DEPTH];
struct tc_mutex l;

static void waker(void *v)
{
    int i, rv;
    i = 0;
    while (1) {
	rv = tc_sleep(CLOCK_MONOTONIC, 0, 100e3);
	assert(!rv);

//	i = rand() % DEPTH;
	i = (i+1) % DEPTH;

	tc_waitq_wakeup_all(wq + i);
    }
}

void unlocker(void *v)
{
    int i;
    i = tc_sleep(CLOCK_MONOTONIC, 0, 100e3);
    assert(!i);
    tc_mutex_unlock(&l);
}

int waiter(long num) {
    int rv;

    if (num < DEPTH) {
	/* tc_waitq_wait_event does an additional check, which we don't
	 * want here.*/
	__tc_wait_event(wq+num, waiter(num+1), rv);

	return 1;
    }

    tc_thread_new(unlocker, NULL, "unlock");
    rv = tc_mutex_lock(&l);
    assert(!rv);
    return 1;
}



static void starter(void *unused)
{
    struct tc_thread *p1;
    int i, rv;

    srand(20123);

    for(i=0; i<DEPTH; i++)
	tc_waitq_init(wq+i);

    tc_thread_new(waker, NULL, "waker_valid");
    tc_mutex_init(&l);

    for(i=0; i<100000; i++) {
	printf("loop %d\n", i);

	rv = tc_mutex_lock(&l);
	assert(!rv);
	p1 =  tc_thread_new((void(*)(void*))waiter, 0, "waiter");
	tc_thread_wait(p1);
	tc_mutex_unlock(&l);
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

