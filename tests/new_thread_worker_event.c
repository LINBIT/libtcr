/* Triggers "unexpected event" in tc_waitq_finish_wait()
 * Immediately after tc_thread_new() or never. */

#include <sys/epoll.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"

#define LOOPS 20

static struct tc_waitq wq;
static int loops = LOOPS;

static void waker(void *unused)
{
	struct timespec tv;

	LL("DEF %p = waker thread", tc_current());

	tv.tv_sec = 0;
	while (loops > 0) {
		tc_sleep(CLOCK_MONOTONIC, 0, 1 + (rand() & 0xff)); /* 30ms */
		tv.tv_nsec = 1 + (rand() & 0xff);
//		LL("sleeping for %d nsec", tv.tv_nsec);
//		nanosleep(&tv, &tv);
		tc_waitq_wakeup(&wq);
	}
}


static int delay_a_bit(void)
{
	usleep(1);
	return rand() & (1 << 6);
}


static void starter(void *unused)
{
	struct tc_thread *p1,*p2,*p3;

	tc_waitq_init(&wq);

	p1 = tc_thread_new(waker, NULL, "waker_valid");
	p2 = tc_thread_new(waker, NULL, "waker_valid");
	p3 = tc_thread_new(waker, NULL, "waker_valid");

	LL("DEF %p = looper thread", tc_current());
	LL("DEF %p = prg wq", &wq);

	while (loops > 0) {
		/* The event should already be in tc->pending, ie. we should get
		 * signalled between prepare_to_wait and finish_wait. */
		tc_waitq_wait_event(&wq, delay_a_bit());
		loops--;
	}

	/*
	tc_thread_wait(p1);
	tc_thread_wait(p2);
	*/
	tc_thread_wait(p3);
}



int main(int argc, char *args[])
{
	int i, s;
	pid_t pid;

	if (argc == 1) {
		/* Only reproducible on freshly started process/threads. */

		for(i=0; i<200; i++)
		{
			printf("run %d\n", i);
			pid = fork();
			if (pid < 0)
				exit(1);

			if (!pid) {
				execlp(args[0], args[0], "*", NULL);
				exit(2);
			}

			wait(&s);
			if (s)
				exit(3);
		}
	}
	else if (strcmp(args[1], "*") == 0)
		tc_run(starter, NULL, "test", 4);
	else
		TCR_DEBUG_PARSE(args[1]);

	return 0;
}

