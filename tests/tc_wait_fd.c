/*
 * This program opens a few socketpairs, and starts tc_threads that relay data
 * in both directions.
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


struct tc_signal sig;

int flag_exit=0;

int fd = 0;

volatile int reporter;

static atomic_t tc_count;


#define TC_C 3
int socks[TC_C][2];
/*
 * socketpairs; what goes into one socket gets out on the vertical neighbor
 * =====================
 * |    |    |    |    |
 * |  a |  c |  e |  g |
 * |----|----|----|----|
 * |  b |  d |  f |  h |
 * |    |    |    |    |
 * =====================
 *
 * Relays: b => c (gets out in d)
 *         d => e (gets out in f)
 *         e => d (gets out in c)
 *         c => b (gets out in a)
 * */
struct tc_signal socks_dead;

static struct tc_mutex m;
char newdata[128]="";

void stdin_handler(void *_ffd)
{
	struct tc_fd *tfd = _ffd;
	int rv;

	while (!flag_exit)
	{
		rv = tc_wait_fd_prio(EPOLLIN, tfd);
		if (rv) break;

		tc_mutex_lock(&m);
		rv = read(fd, newdata, sizeof(newdata));
		if (newdata[0] == 'q')
		{
			flag_exit = 1;
			printf("\n\nEXIT\n\n");
			tc_mutex_unlock(&m);
			break;
		}

		if (strchr(newdata, '\n'))
			*strchr(newdata, '\n') = 0;

		printf("%p: got %d bytes, relaying.\n",
				tc_current(), rv);
		tc_mutex_unlock(&m);
		if (newdata[0] == 'r')
			tc_signal_fire(&sig);

		/*
		//tc_sleep(CLOCK_MONOTONIC, 1, 0);
		tc_sleep(CLOCK_MONOTONIC, 0, 250e3);
		printf("%p: wake, rearm.\n", tc_current());

		rv = tc_rearm(tfd);
		if (rv) printf("rearm: rv=%d\n", rv);
		*/
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
	int i, rr;
	char buff[128];
	int me = atomic_inc(&tc_count);
	struct tc_fd *tr = tc_register_fd(r);
	struct tc_signal_sub *ss;


	printf("%p(%d) started.\n", tc_current(), me);
	ss = tc_signal_subscribe(&socks_dead);

	i = 1000*(long)x;
	while (!flag_exit)
	{
		rr = tc_wait_fd(EPOLLIN, tr);
		if (rr) break;
		//		sl(1);

		rr=read(r, buff, sizeof(buff));
		if (rr <=1) break;
		//		sl(1);
		if (0)
			printf("%p: read(%d => %d) %s",
					tc_current(), r, s, buff);
		//		sl(2);

		i++;

		buff[4]=0;
		sprintf(buff, "%04X", (int)(strtoul(buff, NULL, 16)+1 ) & 0xffff);
		buff[4]='-';

		tc_mutex_lock(&m);
		if (newdata[0])
		{
			strcpy(buff+10, newdata);
			newdata[0]=0;
			rr=strlen(buff);
		}
		tc_mutex_unlock(&m);

		if (! (i % 21487))
		{
#if 0
		} {
#endif
			if (me == reporter)
			{
				reporter--;
				if (reporter == 0)
					reporter=atomic_read(&tc_count)-1;

				buff[rr]=0;
				printf("%p(%02x, next %02x) @ %08x: wrote(%2d => %2d) %s(%d)\n",
						tc_current(), me, reporter, i, r, s, buff, rr);
			}
		}

		//		sl(800);
		// usleep(800e3);

		if (write(s, buff, rr) == -1)
			break;
	}

	tc_unregister_fd(tr);
	tc_signal_unsubscribe(&socks_dead, ss);
	printf("%p(%d) stopped.\n", tc_current(), me);
}

void new_sock(void)
{
	int i,j;

	for(i=0; i<TC_C; i++)
		socketpair(AF_UNIX, SOCK_DGRAM, 0, socks[i]);

	atomic_set(&tc_count, 0);
	for(i=0; i<TC_C; i++)
	{
		j = (i+1) % TC_C;
		tc_thread_new( transfer, (void*)(long)(256*socks[i][1] + socks[j][0]), "01-%p");
		tc_thread_new( transfer, (void*)(long)(256*socks[j][0] + socks[i][1]), "10-%p");
	}
	tc_signal_fire(&socks_dead);

	write(socks[0][0], "AAAAAAAAAAAAAAAA", 16);
	write(socks[0][1], "bbbbbbbbbbbbbbbb", 16);
}

void signal_catcher(void *un)
{
	struct tc_signal_sub *ss;
	int i;
	int s2[TC_C][2];

	ss = tc_signal_subscribe(&sig);
	while (!flag_exit)
	{
		i=tc_sleep(CLOCK_MONOTONIC, 30,0);
		printf("\n%p catcher: sleep said %d, dropping sockets.\n", tc_current(), i);
		tc_dump_threads();
		printf("\n\n");

		memcpy(s2, socks, sizeof(s2));
		new_sock();
		for(i=0; i<TC_C; i++)
		{
			close(s2[i][0]);
			close(s2[i][1]);
		}
	}
	tc_signal_unsubscribe(&sig, ss);
}

void starter(void *unused)
{
	struct tc_thread *s;
	struct tc_fd *tfd= tc_register_fd(fd);

	tc_signal_init(&sig);
	tc_signal_init(&socks_dead);
	tc_mutex_init(&m);

	printf("\n\n\n\n\n\nPlease press enter a few characters in varying intervals.\nq as first character for quit.\n\n");

	reporter=5;
	new_sock();


	tc_thread_new( signal_catcher, NULL, "catch");

	tc_thread_new( stdin_handler, tfd, "stdin2");
	s = tc_thread_new( stdin_handler, tfd, "stdin");
	tc_thread_wait(s);

	exit(0);
}


int main()
{
	tc_run(starter, NULL, "test", 0);

	return 0;
}
