#include <tcr/threaded_cr.h>

static void func(void *str)
{
	int i;

	for (i=0; i<100; i++) {
		printf("%d %s\n", i, (char *)str);
		tc_sched_yield();
	}
}

static void starter(void *unused)
{
	struct tc_thread *a, *b;

	a = tc_thread_new(func, "a", "func_a");
	b = tc_thread_new(func, "b", "func_b");

	tc_thread_wait(a);
	tc_thread_wait(b);
}


int main()
{
	tc_run(starter, NULL, "test", 1);
	return 0;
}
