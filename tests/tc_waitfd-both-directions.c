/*
 * This program opens a few socketpairs, and starts tc_threads that relay data
 * in both directions.
 *
 * On STDIN a tc_wait_fd is started; an error would be if that didn't returned.
 * */
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

atomic_t a_sync;

int socks[2];
struct tc_fd *tc_fds[2];

void reader(void *g)
{
	char buff[128];
	int t, rr, w;

	printf("%p reader waiting\n", tc_current());

	t = tc_wait_fd(EPOLLIN, tc_fds[0]);
	rr=read(socks[0], buff, sizeof(buff));
	/* Look whether we can write */
	w = write(socks[0], buff, 1);
	printf("%p reader wait_fd: %d read: %d write: %d\n", tc_current(), t, rr, w);
}

void writer(void *g)
{
	int rr;

	printf("%p writer going\n", tc_current());
	/* Let the reader put its event on the tc_fd */
	tc_sleep(CLOCK_MONOTONIC, 0, 30e6);

	printf("%p writer waiting\n", tc_current());
	atomic_inc(&a_sync);

	rr = tc_wait_fd(EPOLLOUT | EPOLLERR, tc_fds[0]);
	printf("%p writer wait_fd: %d\n", tc_current(), rr);
	if (rr) return;

	printf("%p writer could write\n", tc_current());
}

void StartReader(void*aa)
{
	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) != 3) ;
	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) != 5) ;

	write(socks[1], "a", 1);
	tc_sleep(CLOCK_MONOTONIC, 0, 10e6);
}

void starter(void *unused)
{
	struct tc_thread *r, *w, *s;
	char buff[2048];


	//	if (socketpair(PF_INET, SOCK_STREAM, IPPROTO_TCP, socks) < 0)
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0)
		perror("socketpair"),exit(1);

	/* Sets them to NOHANG */
	tc_fds[0] = tc_register_fd(socks[0]);
	tc_fds[1] = tc_register_fd(socks[1]);

	/* Repeat until reproduced */
	while (1)
	{
		/* Fill socket with data */
		while (write(socks[0], buff, sizeof(buff)) >= 0) ;
		printf("prepare: after write loop: got errno=%d, expect %d\n", errno, EAGAIN);

		/* Synchronize */
		atomic_set(&a_sync, 0);


		/* Start a reader and a writer */
		w=tc_thread_new( writer, NULL, "writer");
		r=tc_thread_new( reader, NULL, "reader");
		tc_sleep(CLOCK_MONOTONIC, 0, 90e6);

		/*** Arrange collision on tc_fd */

		printf("prepare: sync...\n");
		alarm(1);

		s=tc_thread_new( StartReader, NULL, "writer");

		/* Wait for other thread */
		atomic_inc(&a_sync);
		while (atomic_read(&a_sync) != 3) ;
		atomic_inc(&a_sync);
		while (atomic_read(&a_sync) != 5) ;

		/* And re-start the writer */
		read(socks[1], buff, sizeof(buff));

		tc_sleep(CLOCK_MONOTONIC, 0, 10e6);

		printf("prepare: waiting for threads\n");
		/* wait for threads */
		tc_thread_wait(s);
		printf("prepare: .\n");
		tc_thread_wait(r);
		printf("prepare: .\n");
		tc_thread_wait(w);

		printf("prepare: retrying.\n");
		alarm(0);
		/* Try again. */
		tc_sleep(CLOCK_MONOTONIC, 0, 500e6);
	}

	exit(0);
}

void stop(int sig)
{
	printf("WRITER NOT RESTARTED\n");
	exit(0);
}

int main()
{
	signal(SIGALRM, stop);
	tc_run(starter, NULL, "test", 2);

	return 0;
}
