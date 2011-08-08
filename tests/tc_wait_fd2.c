/*
 * This program opens a socketpair, and starts two tc_threads that send data in
 * both directions.
 *
 * On STDIN a tc_wait_fd is started; an error would be if that didn't returned.
 * */
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>

#include "tcr/threaded_cr.h"


int flag_exit=0;

int fd = 0;

static struct tc_mutex m;


void stdin_handler(void *_ffd)
{
	struct tc_fd *tfd = _ffd;
	int rv;
	char buff[32];

	while (!flag_exit)
	{
//		tc_mutex_lock(&m);
		rv = tc_wait_fd_prio(EPOLLIN, tfd);
		if (rv) break;

		rv = read(fd, buff, sizeof(buff));
		if (buff[0] == 'q')
		{
			flag_exit = 1;
//			tc_mutex_unlock(&m);
			break;
		}

		printf("%p: got %d bytes, sleeping.\n",
				tc_current(), rv);

		//tc_sleep(CLOCK_MONOTONIC, 1, 0);
		tc_sleep(CLOCK_MONOTONIC, 0, 250e3);
		printf("%p: wake, rearm.\n", tc_current());
//		tc_mutex_unlock(&m);

		rv = tc_rearm(tfd);
		if (rv) printf("rearm: rv=%d\n", rv);
	}

}


void sl(int ms)
{
	long d = ms * (float)(0.4e6 + (float)0.6e6 * rand()/RAND_MAX);
	tc_sleep(CLOCK_MONOTONIC, 0, d);
}

void transfer(void *x)
{
	int s = (long)x & 0xff;
	int r = (long)x >> 8;
	int i;
	char buff[32];

	struct tc_fd *tr = tc_register_fd(r);

	i = 1000*(long)x;
	while (!flag_exit)
	{
		sprintf(buff, "=%d\n", i);
		write(s, buff, strlen(buff));
		/* see nice numbers with 257 - all ending with the same 3 digits. */
		if (! (i % 521))
			printf("%p: wrote(%d => %d) %s",
					tc_current(), r, s, buff);

		tc_wait_fd(EPOLLIN, tr);
		sl(1);

		read(r, buff, sizeof(buff));
		sl(1);
		if (0)
			printf("%p: read(%d => %d) %s",
					tc_current(), r, s, buff);
		sl(2);

		i++;
	}
}

void starter(void *unused)
{
	struct tc_thread *s;
	struct tc_fd *tfd= tc_register_fd(fd);
	int i;
	int socks[2];

	printf("\n\n\n\n\n\nPlease press enter in varying intervals.\nq as first character for quit.\n\n");

	for(i=0; i<9; i++)
	{
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0) return;

		tc_thread_new( transfer, (void*)(long)(256*socks[0] + socks[1]), "send%p");
//		tc_thread_new( transfer, (void*)(long)(256*socks[1] + socks[0]), "send%p");
	}

	s = tc_thread_new( stdin_handler, tfd, "stdin");
	tc_thread_new( stdin_handler, tfd, "stdin2");

	tc_thread_wait(s);

	exit(0);
}


int main()
{
	tc_mutex_init(&m);
	tc_run(starter, NULL, "test", 2);
	return 0;
}
