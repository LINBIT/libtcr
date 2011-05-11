/* Triggers "unexpected event" in tc_waitq_finish_wait()
 * Immediately after tc_thread_new() or never. */

#include <sys/epoll.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

static struct tc_mutex l;

static void onoff(void *unused)
{
	while (1) {
		tc_mutex_lock(&l);
		tc_sleep(CLOCK_MONOTONIC, 0, 10);
		tc_mutex_unlock(&l);
		tc_sleep(CLOCK_MONOTONIC, 0, 10);
	}
}

static void runner(void *v)
{
	if (v)
		tc_thread_wait(v);

	tc_thread_new(runner, tc_current(), "asdga");
	tc_sleep(CLOCK_MONOTONIC, 0, 1);
}

static void starter(void *unused)
{
	int i;


	tc_mutex_init(&l);
	tc_thread_new(runner, NULL, "runner");
	tc_thread_new(onoff, NULL, "onoff");
	tc_thread_new(runner, NULL, "runner");

	for(i=0; i<100000; i++) {
		tc_mutex_lock(&l);
		usleep(1);
		tc_mutex_unlock(&l);
	}
}



int main(int argc, char *args[])
{
	if (argc == 1)
		tc_run(starter, NULL, "test", 0);
	else
		exit(33);

	return 0;
}

