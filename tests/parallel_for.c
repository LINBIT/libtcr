#include <stdio.h>
#include "tcr/threaded_cr.h"

static void starter(void *unused)
{
	int i;

	tc_parallel_for(i, i = 0, i < 10000, i++) {
		printf("%d\n", i);
	}

	tc_parallel {
		printf("Hello ");
	} tc_with {
		printf("world");
	} tc_parallel_end;
}

int main()
{
	tc_run(starter, NULL, "starter", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
