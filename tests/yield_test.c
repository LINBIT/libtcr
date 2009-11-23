#include <tc/threaded_cr.h>
#include <tc/atomic.h>

struct tc_signal the_drbd_signal;
struct tc_signal the_signal;

static void func(void *str)
{
	int i;

	for (i=0; i<100; i++)
		printf("%d %s\n", i, (char *)str);
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
	tc_run(starter, NULL, "test", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
