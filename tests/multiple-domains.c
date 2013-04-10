#include <sys/epoll.h>
#include <unistd.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

static int go;
static struct tc_waitq wq;

struct per_sched_t {
	atomic_t cnt;
	struct tc_mutex m;
};

struct per_sched_t per_sched[2] = {  };

atomic_t worker_no;


struct tc_domain *sched2;


void worker(void *_a)
{
	struct per_sched_t *ps = _a;

	int this_worker_no = atomic_inc(&worker_no);

	fprintf(stdout, "worker %d started (%p), for %p.\n", this_worker_no, tc_current(), ps);
	tc_waitq_wait_event(&wq, go);

	while (go) {
		tc_mutex_lock(&ps->m);
		atomic_inc(&ps->cnt);
		tc_mutex_unlock(&ps->m);
	}
}


void starter(void *unused)
{
	struct tc_thread_pool t1, t2;

	go = 0;
	tc_waitq_init(&wq);

	fprintf(stdout, "master started (%p)\n", tc_current());
	atomic_set(&worker_no, 0);

	tc_thread_pool_new(&t1, worker, &per_sched[0], "worker1", 0);

	sched2 = tc_new_domain(0);
	tc_thread_pool_new_in_domain(&t2, worker, &per_sched[1], "worker2", 0, sched2);
	tc_renice_domain(sched2, 10);

	tc_sleep(CLOCK_MONOTONIC, 0, 300e9);

	go = 1;
	tc_waitq_wakeup_all(&wq);

	tc_sleep(CLOCK_MONOTONIC, 3, 0);
	go = 0;

	tc_thread_pool_wait(&t1);
	tc_thread_pool_wait(&t2);

	fprintf(stdout, "ending starter.\n");
}


void sig(int s)
{
	printf("\nsignal\n");
	fflush(NULL);
	exit(0);
}


int main(int argc, char *args[])
{
	if (argc == 1) {
		tc_run(starter, NULL, "sched1", 0);
	}
	return 0;
}
