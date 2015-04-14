/*
 * This program opens a socketpair, keeps the CPUs busy with a few low-priority 
 * TCR threads, and measures latency for high-priority TCR threads.
 *
 * */
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <math.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <signal.h>

#include "tcr/threaded_cr.h"

struct tc_domain *sched2;

int fd[2];

/* 0 for fast, 1 for slow */
struct tc_fd *reader[2], *writer[2];

atomic_t counter[128];

#define MEASUREMENTS (150)

void sleepers(void *_)
{
	tc_sleep(CLOCK_MONOTONIC, 3600, 0);
}


void busy(void *_);
void timestamp_receiver(void *_is_slow)
{
	int is_slow = (long)_is_slow;
	int i, rv, h;
	unsigned long td, c;
	struct timeval now, sent;
	struct tc_thread_pool sleeper;
	struct tc_thread_pool busyp;
        double sum = 0, sumsq = 0, avg;
        char *type = is_slow ? "slow" : "fast";


	if (is_slow)
		tc_thread_pool_new_in_domain(&busyp, busy, NULL, "busy %d", 0, sched2);
	else
		for(i=0; i<20; i++)
			tc_thread_new(sleepers, NULL, "sleeperX");

	tc_thread_pool_new(&sleeper, sleepers, NULL, "sleeper %d", 0);

	/* FD has to be registered in correct domain */
        if (is_slow) {
          reader[1] = tc_register_fd(fd[0]);
          writer[1] = tc_register_fd(fd[1]);
        }


	for(i=0; i<MEASUREMENTS; i++)
	{
		rv = tc_wait_fd(EPOLLIN, reader[is_slow]);
		assert(!rv);

		gettimeofday(&now, NULL);

		rv = read(reader[is_slow]->fd, &sent, sizeof(sent));
		assert(rv == sizeof(sent));

		now.tv_sec -= sent.tv_sec;
		now.tv_usec -= sent.tv_usec;
		if (now.tv_usec < 0) {
			now.tv_usec += 1e6;
			now.tv_sec --;
		}

		td = now.tv_sec * 1e6 + now.tv_usec;
                sum += td;
                sumsq += td*td;
		c = 0;
		for(h=0; h<tc_thread_count(); h++)
			c += atomic_read(&counter[h]);
		printf("%5d, %s, %8li, %lu\n",
                    i, type, td, c);
	}

        avg = sum/i;
        fprintf(stderr, "%s: avg %9.2fusec, stdabw %9.2f\n",
            type, avg,
            sqrt((sumsq - sum*sum/i)/(i-1)));

        if (is_slow)
          assert(avg > 100.0);
        else
          assert(avg < 100.0);
}


void signalling(void *_)
{
	struct timeval now;
	long d;

	while (1) {
		d = (float)0.001e9 + (float)0.2e9 * rand()/RAND_MAX;
//		printf("sleeping %6ld usec\n", d);
		tc_sleep(CLOCK_MONOTONIC, 0, d);


//		printf("ping\n");
		gettimeofday(&now, NULL);
		write(writer[0]->fd, &now, sizeof(now));
		gettimeofday(&now, NULL);
		write(writer[1]->fd, &now, sizeof(now));
	}
}

void busy(void *_)
{
	int i, fd, j;
	int k = (long)_;

	j = k;
	while (1) {
          if ((atomic_inc(&counter[k]) & 0xfffff) == 0 ||
              (rand() & 0xfffff) == 0)
            tc_wait_fd(EPOLLOUT, writer[1]);
//            tc_sched_yield();
          continue;

		for(i=0; i<1e6; i++) {
			j += i + rand();
			if (0)
			if (! (i % 1719)) {
				tc_sched_yield();
				/* sched_yield isn't enough?? should be always possible. */
				tc_wait_fd(EPOLLOUT, writer[1]);
			}
		}
		continue;

	/* give kernel a chance for scheduling */
		fd = open("/never-there", O_RDONLY);
		if (fd != -1) {
			/* makes gcc think we're using the result */
			j += read(fd, &j, sizeof(j));
			close(fd);
		}
	}
}

void starter(void *unused)
{
	struct tc_thread_ref f,s;


	if (-1 == pipe(fd)) exit(1);
	reader[0] = tc_register_fd(fd[0]);
	writer[0] = tc_register_fd(fd[1]);

	if (-1 == pipe(fd)) exit(1);
	/* FD has to be registered in correct domain */

//	tc_sleep(CLOCK_MONOTONIC, 1, 100e6);
	sched2 = tc_new_domain(0);
	tc_renice_domain(sched2, 10);

	printf("run, type, usec, counter\n");

	f = tc_thread_new_ref( timestamp_receiver, NULL, "high");
	s = tc_thread_new_ref_in_domain( timestamp_receiver, (void*)1, "low", sched2);



	tc_dump_threads(NULL);
	tc_thread_new( signalling, NULL, "writer");

	tc_thread_wait_ref(&f);
	tc_thread_wait_ref(&s);

	exit(0);
}


int main(int argc, char *args[])
{
	if (argc == 1)
	{
		tc_run(starter, NULL, "test", 0);
	}

	return 0;
}
