#define _GNU_SOURCE
#include <unistd.h>
#include <sys/epoll.h>
#include <assert.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include "tcr/threaded_cr.h"

int fh;

atomic_t idx;


void t(void *_v)
{
	off_t offs = (atomic_inc(&idx)-1) * 1024 * 1024;
    int i, rv, x, i2;
	struct {
		int i;
		void *id;
	} buffer;

	buffer.id = tc_current();

	for(i=0; i<4000; i++) {
		buffer.i = i;
		i2 =  (i>>2) & 0x3;
		switch (i2) {
			case 1:
				rv = tc_aio_write(fh, &buffer, sizeof(buffer), offs + i * sizeof(buffer));
				break;
				/* We want a write being in flight while we do a read. */
			case 0:
			case 2:
				rv = tc_aio_submit_write(fh, &buffer, sizeof(buffer), offs + i * sizeof(buffer));
				break;
			case 3:
				/* Read last sync write */
				rv = tc_aio_read(fh, &x, sizeof(x), offs + (i-4) * sizeof(buffer));
				assert(x == i-4);
				break;
		}

		if (rv) {
			printf("error at %d case %d: %d\n", i, i2, rv);
			assert(!rv);
		}
		if (!offs) {
			if ((i % 100) == 0 && i>0) {
				printf("SYNC\n");
				rv = tc_aio_sync(fh, 1);
//				fsync(fh); kill(getpid(), 9);
				if (rv) {
					printf("error at %d: %d for sync; wrong filesystem?\n", i, rv);
				}
			}

			if ((i % 127) == 0)
				printf("at idx %d\n", i);
		}
	}
	printf("%p enDDD %zx.\n", tc_current(), offs);
}


void starter(void *unused)
{
	struct tc_thread_pool pool;

	int flags = O_RDWR | O_ASYNC | O_DSYNC;

	// O_DIRECT |  doesn't allow fsync()?
	switch (2) {
		case 0:
			fh = open("/dev/mapper/vg-small1", flags , 0700);
			break;
		case 1:
			fh = open("/var/tmp/mist", flags | O_CREAT | O_TRUNC , 0700);
			break;
		case 2:
			fh = open("/tmp/mist", flags | O_CREAT | O_TRUNC , 0700);
			break;
	}
	if (fh < 0)
		exit(2);

	ftruncate(fh, 1024*1024*1024);

	atomic_set(&idx, 0);
	tc_thread_pool_new(&pool, t, NULL, "t-%d", 10);
	tc_thread_pool_wait(&pool);
}


void sig(int s)
{
	printf("signal caught\n"); fflush(NULL);
	exit(1); /* gcov needs a clean exit. */
}


int main(int argc, char *args[])
{
	signal(SIGALRM, sig);
	signal(SIGINT, sig);

	if (argc == 1)
	{
		tc_run(starter, NULL, "test", 0);
		printf("end.\n");
	}
	return 0;
}
