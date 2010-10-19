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


void func(struct tc_fd *fd)
{
	int rr;

	IF printf("*** %p func waiting fd=%p started.\n", tc_current(), fd);
	rr = tc_wait_fd(EPOLLOUT |
			(rand() & 1 ? EPOLLERR : 0) |
			(rand() & 1 ? EPOLLHUP : 0), fd);
	IF printf("*** %p wait_fd: %d\n", tc_current(), rr);
}


void starter(void *unused)
{
char buff[1024];
	struct tc_thread_pool p;

	//	if (socketpair(PF_INET, SOCK_STREAM, IPPROTO_TCP, socks) < 0)
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0)
		perror("socketpair"),exit(1);
	tc_fds[0] = tc_register_fd(socks[0]);
	tc_fds[1] = tc_register_fd(socks[1]);

	/* Make FD unwriteable. */
	while (write(socks[0], buff, sizeof(buff)) >0) ;

	tc_thread_pool_new( &p, (void*)(void *)func, tc_fds[0], "r", 2);
	IF printf("pool created.\n");

	tc_sleep(CLOCK_MONOTONIC, 0, 200e6);
	IF printf("closing.\n");
	fflush(NULL);
	close(socks[1]);
	IF printf("closed.\n");

	tc_thread_pool_wait(&p);
	IF printf("returned\n");

	exit(0);
}


void s(int sig)
{
	fflush(NULL);
	exit(1);
}
int main()
{
//	alarm(2);
	signal(SIGALRM, s);
	tc_run(starter, NULL, "test", 1);

	return 0;
}
