#include <sys/epoll.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <tcr/threaded_cr.h>

static void stdin_reader(void *data)
{
	struct tc_fd *tcfd = (struct tc_fd *)data;

	tc_wait_fd(EPOLLIN, tcfd);
	tc_rearm(tcfd); /* The others of the pool should terminate as well */
}

static void busy(void *arg)
{
	volatile int *f = (volatile int *)arg;
	while (*f == 0)
		;
}

static void starter(void *arg)
{
	struct tc_thread *a;
	struct tc_thread_pool tp;
	struct tc_fd *tcfd;
	int f = 0;

	a = tc_thread_new(busy, &f, "busy_endless_loop");

	tcfd = tc_register_fd(fileno(stdin));
	tc_thread_pool_new(&tp, stdin_reader, tcfd, "stdin_reader_%d", 0);

#ifdef WAIT_DEBUG
	tc_sleep(CLOCK_MONOTONIC, 0, 1000000); /* 1ms, allow the stdin_reader to reach tc_wait_fd() */
	tc_dump_threads();

	printf("\nSend a SIGUSR1 ... any input causes the program to terminate\n");
	printf("kill -USR1 %d\n", getpid());
#else
	printf("To use this example properly you need to configure the library\n"
	       "with --enable-wait-debug\n\n"
		"any input causes the program to terminate\n");
#endif

	tc_thread_pool_wait(&tp);
	f = 1;
	tc_thread_wait(a);
}

int main()
{
#ifdef WAIT_DEBUG
	signal(SIGUSR1, (__sighandler_t) &tc_dump_threads);
#endif

	tc_run(starter, NULL, "starter", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
