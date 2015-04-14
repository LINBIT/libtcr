#include <sys/epoll.h>
#include <unistd.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

static volatile int go;
static struct tc_waitq wq;

struct per_sched_t {
	atomic_t cnt;
	struct tc_mutex m;
	const unsigned char pad[128 - sizeof(atomic_t) - sizeof(struct tc_mutex)];
};

struct per_sched_t per_sched[2] = {  };

atomic_t worker_no;


struct tc_domain *sched2;


void worker(void *_a)
{
	struct per_sched_t *ps = _a;

	int this_worker_no = atomic_inc(&worker_no);
	int64_t a;

	fprintf(stdout, "worker %d started (%p), for %p.\n", this_worker_no, tc_current(), ps);
	tc_waitq_wait_event(&wq, go);

	while (go) {
		tc_mutex_lock(&ps->m);
		atomic_inc(&ps->cnt);
		a = atomic_read(&ps->cnt);
		tc_mutex_unlock(&ps->m);
		if ((a & 0xfffffL) == 0) {
			fprintf(stdout, "a: %lu\n", a);
		}
	}
}


void starter(void *unused)
{
	struct tc_thread_pool t1, t2;

	go = 0;
	tc_waitq_init(&wq);

	fprintf(stdout, "master started (%p)\n", tc_current());
	atomic_set(&worker_no, 0);

	tc_mutex_init(&per_sched[0].m);
	tc_mutex_init(&per_sched[1].m);

	tc_thread_pool_new(&t1, worker, &per_sched[0], "worker1", 0);

	sched2 = tc_new_domain(0);
	tc_thread_pool_new_in_domain(&t2, worker, &per_sched[1], "worker2", 0, sched2);
	tc_renice_domain(sched2, 10);

	tc_sleep(CLOCK_MONOTONIC, 1, 0);
	go = 1;
	tc_waitq_wakeup_all(&wq);

	tc_sleep(CLOCK_MONOTONIC, 10, 0);
	go = 0;

	tc_thread_pool_wait(&t1);
	tc_thread_pool_wait(&t2);

	fprintf(stdout, "ending starter.\n");

	printf("m[0]: %lu\n", atomic_read(&per_sched[0].cnt));
	printf("m[1]: %lu\n", atomic_read(&per_sched[1].cnt));
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
