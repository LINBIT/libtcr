#include "threaded_cr.h"
#include <unistd.h>
#include <sys/epoll.h>

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
	}
}

void starter(void *unused)
{
	struct tc_threads t;
	struct tc_fd *the_tc_fd = tc_register_fd(0);

	tc_mutex_init(&m);
	fprintf(stdout, "beginning starter.\n");
	tc_threads_new(&t, worker, the_tc_fd, "worker");
	tc_threads_wait(&t);
	fprintf(stdout, "ending starter.\n");
}


int main()
{
	tc_run(starter, NULL, "test", 4);
	sleep(60);
	return 0;
}
