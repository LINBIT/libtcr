#include <sys/epoll.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

static struct tc_mutex m;
static struct tc_signal the_drbd_signal;

int worker_no=0;

void sig(int s)
{
	static int c=0;

	printf("signal caught\n");
	tc_signal_fire(&the_drbd_signal);
	c++;
	if (c> 2) kill(getpid(), SIGKILL);
}

void worker(void *ttf_vp)
{
	int i;
	struct tc_signal_sub *ss;
	int me = worker_no++;

	ss = tc_signal_subscribe(&the_drbd_signal);

	for(i=0; i<1280; i++) {
		TC_NO_INTR(tc_mutex_lock(&m));
		usleep(1e3);
		printf("worker %2d=%p with %d\n", me, tc_current(), i);
		fflush(stdout);
		tc_mutex_unlock(&m);
		tc_sleep(CLOCK_MONOTONIC, 0, 3e3);
	}

	tc_signal_unsubscribe(&the_drbd_signal, ss);
}

void starter(void *unused)
{
	struct tc_thread_pool t;
	struct tc_fd *the_tc_fd = tc_register_fd(0);

	tc_signal_init(&the_drbd_signal);
	signal(SIGINT, sig);
	tc_mutex_init(&m);
	tc_thread_pool_new(&t, worker, the_tc_fd, "worker", 0);
	tc_thread_pool_wait(&t);
	fprintf(stdout, "ending starter.\n");
}


int main(int argc, char *args[])
{
	if (argc == 1)
		tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN)*1.5+1);
	else
		exit(33);

	return 0;
}
