#define _GNU_SOURCE
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "tcr/threaded_cr.h"

static struct tc_signal the_drbd_signal;
struct tc_mutex m;
atomic_t c;
int max_sleepers;

void sig(int s)
{
	printf("signal caught\n"); fflush(NULL);
	exit(1);
}


void worker(void* vou)
{
	int i;
	struct tc_signal_sub *ss = NULL;
	struct timespec a,b;
	float exp;

	/* can you please explain what "vou" is supposed to stand for,
	 * when it would be "true", and what this is supposed to do?
	 */
	if (vou) {
		ss = tc_signal_subscribe(&the_drbd_signal);
		printf("worker %p: waiting for lock\n", tc_current());

		TC_NO_INTR(tc_mutex_lock(&m));
		i = 0;
		printf("worker %p: lock %d\n", tc_current(), i);
	}

	if (atomic_read(&c) == -1) {
		exp = 0.3;
		i = 0;
	} else {
		i = atomic_dec(&c);
		exp = 0.13 + 0.001 * i;
	}


	clock_gettime(CLOCK_MONOTONIC, &a);
	i = tc_sleep(CLOCK_MONOTONIC, (int)exp, (long long)(exp * 1e9) % (long)1e9); // needed
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("worker %p: sleep %d, delta %.2f exp %.2f msec (%zu.%09lu - %zu.%09lu)\n", tc_current(), i,
			(b.tv_sec-a.tv_sec)*1000.0 +
			(b.tv_nsec - a.tv_nsec)*1e-6,
			exp * 1000.0,
			b.tv_sec, b.tv_nsec,
			a.tv_sec, a.tv_nsec);

	if (vou) {
		// Moving this to the end of the function makes it never work
		tc_signal_unsubscribe(&the_drbd_signal, ss);
		tc_mutex_unlock(&m);
	}

//	printf("worker %p: end.\n", tc_current());
}

void starter(void *unused)
{
	struct tc_thread *t[5]; // doesn't trigger bug for <= 3
	unsigned i;
	struct rlimit rlim;
	struct tc_thread_pool tp;


	printf("%p: starter\n", tc_current());
	atomic_set(&c, -1);
	tc_signal_init(&the_drbd_signal);
	tc_mutex_init(&m);
	for(i = 0; i<sizeof(t)/sizeof(t[0]); i++)
		t[i] = tc_thread_new(worker, &m, "worker");

	// Making this delay different from the one above lets this test run
	tc_sleep(CLOCK_MONOTONIC, 0, 700e6);
	printf("   %p: fire!\n", tc_current());
	tc_signal_fire(&the_drbd_signal);
	printf("fired\n");

	for(i = 0; i<sizeof(t)/sizeof(t[0]); i++)
		tc_thread_wait(t[i]);



	getrlimit(RLIMIT_NOFILE, &rlim);
	max_sleepers = rlim.rlim_max + 20;
	printf("%d sleepers\n", max_sleepers);
	atomic_set(&c, max_sleepers + sysconf(_SC_NPROCESSORS_ONLN) + 10);

	tc_thread_pool_new(&tp, worker, NULL, "sleeper",  max_sleepers);
	i = tc_thread_pool_wait(&tp);
	printf("returned %d\n", i);

	tc_dump_threads(NULL);
}


int main(int argc, char *args[])
{
	if (argc == 1) {
		signal(SIGPIPE, SIG_IGN);
		signal(SIGALRM, sig);
		signal(SIGINT, sig);

		//	alarm(2);
		tc_run(starter, NULL, "test", 0);
		printf("end.\n");
	}
	return 0;
}
