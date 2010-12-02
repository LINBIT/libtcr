#include <sys/epoll.h>
#include <unistd.h>

#include "tcr/threaded_cr.h"


unsigned max=0;

static spinlock_t lock;
volatile int protected;
float x=2.1;

void starter(void *unused)
{
	int i;
	tc_parallel_for(i, i = 0, i < max, i++) {
		int r, j;
		spin_lock(&lock);
		r=protected;
		protected = 1;
		for(j = i & 0xff; j>=0; j--)
			x += r*j;
		protected = 0;
		spin_unlock(&lock);
		if (r)
			printf("GOT INVALID VALUE: %X\n", r);
	}
}


int main(int argc, char *args[])
{
	int tc=0;
	if (argc>1) tc=atoi(args[1]);
	if (!tc) tc=sysconf(_SC_NPROCESSORS_ONLN);

	if (argc>2) max=atoi(args[2]);
	if (!max) max=3000000;

	spin_lock_init(&lock);
	printf("tc=%d max=%d\n", tc, max);
	tc_run(starter, NULL, "test", tc);

	printf("got %f\n", x);
	return x > -1e30 ? 0 : 1;
}
