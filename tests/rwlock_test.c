#include <sys/epoll.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"


unsigned max=0;

static struct tc_rw_lock lock;
volatile int protected;
float x=2.1;

void starter(void *unused)
{
	int i;
	tc_parallel_for(i, i = 0, i < max, i++) {
		int r;
		r=rand() & 0xfff;
		if (r <= 3)
		{
			tc_rw_w_lock(&lock);
			protected ++;
			//printf("got a write lock: %d %X\n", i, protected);
			for (r=1; r< 16; r++)
			{
				protected++;
				tc_sleep(CLOCK_MONOTONIC, 0, 10);
			}
			protected -= 16;
			tc_rw_w2r_lock(&lock);
		}
		else
		{
			tc_rw_r_lock(&lock);
		} {
			int v;
			v=protected;
			/*
			for(v=100; v<1000; v++)
				x += 1.0/v;
				*/
//			tc_sleep(CLOCK_MONOTONIC, 0, 100);
			tc_rw_r_unlock(&lock);
			if (v)
				printf("GOT INVALID VALUE: %X\n", v);
		}
	}
}


int main(int argc, char *args[])
{
#ifdef CONTENTION
	struct contention_t *ct[] = { &lock.contention, &lock.mutex.contention };
#endif
	int tc=0;
	if (argc>1) tc=atoi(args[1]);
	if (!tc) tc=12;

	if (argc>2) max=atoi(args[2]);
	if (!max) max=1000000;

	tc_rw_init(&lock);
	tc_run(starter, NULL, "test", tc);

#ifdef CONTENTION
	char buf[1024];
	tc_contention_data(buf, sizeof(buf));
	puts(buf);
	contention_print(ct, 2, buf, sizeof(buf));
	puts(buf);
#endif

	return x > -1e30 ? 0 : 1;
}
