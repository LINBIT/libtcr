#define _GNU_SOURCE
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include "tcr/threaded_cr.h"

static struct tc_signal the_drbd_signal;
struct tc_mutex m;

void sig(int s)
{ 
	printf("signal caught\n"); fflush(NULL); 
	exit(1);
}


void worker(void* vou)
{
	int i;
	struct tc_signal_sub *ss;

	ss = tc_signal_subscribe(&the_drbd_signal);
	printf("worker %p: waiting for lock\n", tc_current());

	i=0;
	TC_NO_INTR(tc_mutex_lock(&m));
	printf("worker %p: lock %d\n", tc_current(), i);
	i = tc_sleep(CLOCK_MONOTONIC, 0, 300.0e6); // needed
	printf("worker %p: sleep %d\n", tc_current(), i);

	// Moving this to the end of the function makes it never work
	tc_signal_unsubscribe(&the_drbd_signal, ss);
	tc_mutex_unlock(&m);

	printf("worker %p: end.\n", tc_current());
}

void starter(void *unused)
{
	struct tc_thread *t[5]; // doesn't trigger bug for <= 3
	int i;

	printf("%p: starter\n", tc_current());
	tc_mutex_init(&m);
	for(i = 0; i<sizeof(t)/sizeof(t[0]); i++)
		t[i] = tc_thread_new(worker, &m, "worker");

	// Making this delay different from the one above lets this test run
	tc_sleep(CLOCK_MONOTONIC, 0, 300e6);
	printf("   %p: fire!\n", tc_current());
	tc_signal_fire(&the_drbd_signal);
	printf("fired\n");

	for(i = 0; i<sizeof(t)/sizeof(t[0]); i++)
		tc_thread_wait(t[i]);
}


int main()
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGALRM, sig);
	signal(SIGINT, sig);

//	alarm(2);
	tc_mutex_init(&m);
	tc_signal_init(&the_drbd_signal);
	tc_run(starter, NULL, "test", 2);
	printf("end.\n");
	return 0;
}
