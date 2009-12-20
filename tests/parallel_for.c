#include <stdio.h>
#include "tc/threaded_cr.h"

static void starter(void *unused)
{
	int i;

	tc_parallel_for(i, i = 0, i < 10000, i++) {
		printf("%d\n", i);
	}
}

int main()
{
	tc_run(starter, NULL, "starter", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
