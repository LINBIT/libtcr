#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <math.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

struct tc_domain *sched2,*sched3;

struct tc_waitq wq;
struct tc_rw_lock l;

int stop = 0;
atomic_t go, threads;
unsigned long long counter[3]={0};

void runner(void *_x)
{
		unsigned long long *p = _x;

		atomic_inc(&threads);

		while (!stop) {
			tc_rw_r_lock(&l);
			tc_rw_r_unlock(&l);
			tc_waitq_wait_event( &wq, atomic_read(&go));
			(*p) ++;
			tc_sched_yield();
		}
}

void starter(void *unused)
{
	struct tc_thread_pool p1, p2, p3;
	int i;

	sched2 = tc_new_domain(0);
	sched3 = tc_new_domain(0);

	tc_rw_init(&l);
	assert(tc_rw_w_lock(&l) == 0);
	tc_waitq_init(&wq);

	atomic_set(&threads, 0);
	atomic_set(&go, 0);
	tc_thread_pool_new          (&p1, runner, counter+0, "1-%d", 0);
	tc_thread_pool_new_in_domain(&p2, runner, counter+1, "2-%d", 0, sched2);
	tc_thread_pool_new_in_domain(&p3, runner, counter+2, "3-%d", 0, sched3);
	tc_sleep(CLOCK_MONOTONIC, 0, 100e6);

	for(i=0; i<300; i++) {
		/* runners are collected at the rw lock.
		 * then they may proceed once, and are stopped again. */
		printf("%llu %llu %llu\n", counter[0], counter[1], counter[2]);
		atomic_set(&go, 0);
		printf("%d: %d threads; %d\n", atomic_read(&threads), i, atomic_read(&go));
		tc_rw_w_unlock(&l);

		tc_sleep(CLOCK_MONOTONIC, 0, 1e6);
		atomic_set(&go, atomic_read(&threads));
		printf("readers: %d\n", atomic_read(&l.readers));
		tc_rw_w_lock(&l);
		tc_waitq_wakeup_all(&wq);

	}
	printf("\n");
	stop = 1;
	tc_waitq_wakeup_all( &wq);
	tc_sleep(CLOCK_MONOTONIC, 0, 1e6);
	tc_waitq_wakeup_all( &wq);
	tc_sleep(CLOCK_MONOTONIC, 0, 1e6);
	tc_waitq_wakeup_all( &wq);
	tc_thread_pool_wait(&p1);
	tc_thread_pool_wait(&p2);
	tc_thread_pool_wait(&p3);

	printf("%llu %llu %llu\n", counter[0], counter[1], counter[2]);

	exit(0);
}


int main(int argc, char *args[])
{
	if (argc == 1)
	{
		tc_run(starter, NULL, "test", 0);
	}

	return 0;
}

