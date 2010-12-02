#include <sys/epoll.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

static struct tc_rw_lock m;
static struct tc_signal the_drbd_signal;


void sig(int s)
{
	static int c=0;

	printf("signal caught\n");
	tc_signal_fire(&the_drbd_signal);
	c++;
	if (c> 10) exit(0);
	if (c> 14) kill(getpid(), SIGKILL);
}

void worker(void *ttf_vp)
{
	int i;
	struct tc_signal_sub *ss;

	ss = tc_signal_subscribe(&the_drbd_signal);

	for(i=0; i< 128; i++) {
		TC_NO_INTR(tc_rw_w_lock(&m));
		usleep(10e3);
		printf("worker %p with %d - Press Ctrl+C, perhaps multiple times\n", tc_current(), i);
		fflush(stdout);
		tc_rw_w_unlock(&m);
		TC_NO_INTR(tc_rw_r_lock(&m));
		tc_sleep(CLOCK_MONOTONIC, 0, 10e6);
		tc_rw_r_unlock(&m);
	}

	tc_signal_unsubscribe(&the_drbd_signal, ss);
}

void starter(void *unused)
{
	struct tc_thread_pool t;
	struct tc_fd *the_tc_fd = tc_register_fd(0);

	tc_rw_init(&m);
	tc_thread_pool_new(&t, worker, the_tc_fd, "worker", 0);
	tc_thread_pool_wait(&t);
	fprintf(stdout, "ending starter.\n");
}


int main()
{
	tc_signal_init(&the_drbd_signal);
	signal(SIGINT, sig);
	tc_run(starter, NULL, "test", 16);
	return 0;
}
