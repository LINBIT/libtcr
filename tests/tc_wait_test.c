#include <tc/threaded_cr.h>
#include <stdio.h>
#include <sys/epoll.h>

/* This is to show a little bug that has been introduced to 
 * the tc library recently. It seems that the EPOLL_ONESHOT
 * flag is missing in the call to epoll_wait(). The (errnous)
 * behaviour is that the read_stdin tc thread is created
 * twice (should be only once).
 */

spinlock_t s;

static void read_stdin(void *unused)
{
	char c;
	static int thread_no = 0;
	int my_thread_no;

	spin_lock(&s);
	thread_no++;
	my_thread_no = thread_no;
	spin_unlock(&s);

	fprintf(stderr, "[%d]: read_stdin thread: into getchar().\n", my_thread_no);
	while ((c=getchar()) != EOF) 
		printf("[%d]: read %c\n", my_thread_no, c);
	fprintf(stderr, "[%d]: read_stdin thread: got eof, exiting.\n", my_thread_no);
}


static void read_stdin_starter(void *data)
{
	struct tc_fd *t;

	t = tc_register_fd(0);
	while (1) {
		fprintf(stderr, "into tc_wait_fd.\n");
		tc_wait_fd(EPOLLIN, t);
		fprintf(stderr, "starting new stdin reader thread.\n");
		tc_thread_new(read_stdin, NULL, "ctl_connection");
	}
}


int main(int argc, char ** argv)
{
	spin_lock_init(&s);

	fprintf(stderr, "tc_wait_test: only one thread should be started.\n");
	fprintf(stderr, "type something ...\n");

	tc_run(read_stdin_starter, NULL, "stdin_reader", 10);
	return 0;
}

