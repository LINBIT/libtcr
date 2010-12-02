#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>


static inline int atomic_read(int *v) {
	__sync_synchronize();
	return *v;
}


#define atomic_inc(v) __sync_add_and_fetch(v, 1)
int a_sync;

int socks[2];
int ep;


void thread1(void)
{
	char buff[32768];
	int i, j, wr;
	struct epoll_event ev;


	//	if (socketpair(PF_INET, SOCK_STREAM, IPPROTO_TCP, socks) < 0)
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, socks) < 0)
		perror("socketpair"),exit(1);

	ep = epoll_create1(0);
	if (ep < 0) exit(2);

	ev.data.ptr = NULL;
	ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR;
	epoll_ctl(ep, EPOLL_CTL_ADD, socks[0], &ev);

	/* Fill socket with data */
	wr = 0;
	while ((i = write(socks[0], buff, sizeof(buff))) >= 0) wr += i;
	printf("prepare: after write loop: got errno=%d, expect %d\n", errno, EAGAIN);


	/* drain queue */
	while (epoll_wait(ep, &ev, 1, 0) > 0) ;

	/*** Arrange collision on tc_fd */
	printf("prepare: sync... %d\n", atomic_read(&a_sync));
	alarm(1);

	/* Wait for other thread */
	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) < 2) ;

//	printf("sync... %d\n", atomic_read(&a_sync));
	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) < 4) ;

	/* And re-start the writer */
	{
		/* Keep 1 byte in the socket buffers */
//		char buff[78000];
	read(socks[1], buff, sizeof(buff));
//	while ( read(socks[1], buff, sizeof(buff)) >= 0);
	}

	/* What does the kernel tell us here? */
	i = epoll_wait(ep, &ev, 1, 10);
	printf("thread1: epoll_wait returned %d, ev = %x\n", i, ev.events);

	/* Look what we can do with the socket */
	i = read(socks[0], buff, 1);
	j = write(socks[0], buff, sizeof(buff));
	printf("thread1: could read %d, write %d\n", i, j);
}

void *thread2(void *aa)
{
	int i;
	struct epoll_event ev;

	printf("thread2 ready: %d\n", atomic_read(&a_sync));

	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) < 2) ;
//	printf("thread2 ready: %d\n", atomic_read(&a_sync));

	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) < 4) ;

	write(socks[1], "a", 1);
	i = epoll_wait(ep, &ev, 1, 20);
	printf("thread2: epoll_wait returned %d, ev = %x\n", i, ev.events);
	return NULL;
}

void sig(int s)
{
	printf("SIGNAL: %d\n", atomic_read(&a_sync));
	exit(1);
}

int main()
{
	pthread_t p;

	/* Synchronize */
	a_sync = 0;
	__sync_synchronize();
	signal(SIGALRM, sig);

	pthread_create(&p, NULL, thread2, NULL);
	thread1();
	pthread_join(p, NULL);
	return 0;
}

