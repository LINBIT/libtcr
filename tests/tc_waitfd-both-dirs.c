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
#include <string.h>
#include <signal.h>

#include "tcr/threaded_cr.h"


/* On socks[0] a thread wants to receive, while another uses the same socket
 * for sending.
 * The other end gets a sink, and an occasional sender. */
int socks[2];
struct tc_fd *tc_fds[2];

#define IF if (1)


void receiver(struct tc_fd *fd)
{
	char buff[128];
	int rr;

	 IF printf("%p receiver fd=%p, tc=%p, started.\n", tc_current(), fd, fd);
	rr = tc_wait_fd(EPOLLIN, fd);
	 IF printf("%p wait_fd: %d\n", tc_current(), rr);
	if (rr) return;

	rr=read(tc_fd(fd), buff, sizeof(buff));
	if (rr <=1) return;

	 IF printf("%p: read(%d) => %d\n", tc_current(), tc_fd(fd), rr);
	 //tc_sleep(CLOCK_MONOTONIC, 1, 0);
}


void writer(struct tc_fd *fd)
{
	char buff[128];
	int rr;

	 IF printf("%p writer fd=%p, tc=%p, started.\n", tc_current(), fd, fd);
	rr = tc_wait_fd(EPOLLOUT, fd);
	 IF printf("%p wait_fd: %d\n", tc_current(), rr);
	if (rr) return;

	rr=write(tc_fd(fd), buff, sizeof(buff));
	if (rr <=1) return;

	 IF printf("%p: write(%d) => %d\n", tc_current(), tc_fd(fd), rr);
//	 tc_sleep(CLOCK_MONOTONIC, 1, 0);
}


char buff[128*1024];
void starter(void *unused)
{
	struct tc_thread *r, *w;
	int nw, wrr;

	//	if (socketpair(PF_INET, SOCK_STREAM, IPPROTO_TCP, socks) < 0)
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0)
		perror("socketpair"),exit(1);
	tc_fds[0] = tc_register_fd(socks[0]);
	tc_fds[1] = tc_register_fd(socks[1]);

	wrr = 0;
	while ((nw = write(socks[0], buff, sizeof(buff))) > 0) wrr+=nw;
	 IF printf("wrote %u bytes\n", wrr);

	r=tc_thread_new((void (*)(void *))receiver, tc_fds[0], "recv");
	w=tc_thread_new((void (*)(void *))writer, tc_fds[0], "sender");

	tc_sleep(CLOCK_MONOTONIC, 0, 200e6);
	 IF printf("both blocked currently.\n");

	nw = write(socks[1], buff, sizeof(buff));
	while ((nw = read(socks[1], buff, sizeof(buff))) > 0) ;

	 IF printf("both unblocked.\n");
	tc_thread_wait(r);
	 IF printf("r returned\n");
	tc_thread_wait(w);
	 IF printf("w returned\n");

	exit(0);
}


int main()
{
	tc_run(starter, NULL, "test", 1);

	return 0;
}
