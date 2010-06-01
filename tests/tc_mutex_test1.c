#include <sys/epoll.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

static struct tc_mutex m;
static int in_cr = 0;
static int worker_no = 0;

void worker(void *ttf_vp)
{
	struct tc_fd *the_tc_fd = (struct tc_fd*) ttf_vp;
	int this_worker_no = ++worker_no;
	int old_cr;

	fprintf(stdout, "worker %d started.\n", this_worker_no);
	while (1) {
		tc_mutex_lock(&m);
		old_cr = in_cr;
		in_cr = 1;
		tc_wait_fd(EPOLLIN, the_tc_fd);
		if (old_cr) {
			exit(5);
		}
		/* usleep(1); */
		in_cr = 0;
		tc_mutex_unlock(&m);
		printf("progress on worker %d\n", this_worker_no);
	}
}

void starter(void *unused)
{
	struct tc_thread_pool t;
	struct tc_fd *the_tc_fd = tc_register_fd(0);

	tc_mutex_init(&m);
	fprintf(stdout, "beginning starter.\n");
	tc_thread_pool_new(&t, worker, the_tc_fd, "worker", 0);
	tc_thread_pool_wait(&t);
	fprintf(stdout, "ending starter.\n");
}


int main()
{
	tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN));
	sleep(60);
	return 0;
}
