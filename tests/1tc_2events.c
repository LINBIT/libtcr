#define _GNU_SOURCE
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include "tcr/threaded_cr.h"

static struct tc_signal the_drbd_signal;
struct tc_mutex m;
atomic_t a_sync;

/* This test makes a victim thread that gets woken up by to simultaneous events
 * - a signal, and a released lock.
 * We expect the victim to obtain the lock, and end normally (as the event that
 * is waited for has precedence over signals).
 * */


void victim(void* a)
{
	struct tc_signal_sub *ss;
	int i;

	ss = tc_signal_subscribe(&the_drbd_signal);
	// Removing this tc_sleep changes the order on some libtcr queue, and makes 
	// this problem vanish.
	tc_sleep(CLOCK_MONOTONIC, 0, 300e6);
	printf("victim %p: waiting for lock\n", tc_current());

	/* Waiting to get a lock ...
	 * but then something horrible happens. */
	i=tc_mutex_lock(&m);
	printf("victim %p: lock %d\n", tc_current(), i);
	
	if (!i)
		tc_mutex_unlock(&m);

	tc_signal_unsubscribe(&the_drbd_signal, ss);
	printf("victim %p: end.\n", tc_current());
}


void ts(void *a)
{
	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) != 2) ;

	/* TCP connection lost! */
	tc_signal_fire(&the_drbd_signal);
	printf("   %p: fired!\n", tc_current());
}


void tl(void *a)
{
	atomic_inc(&a_sync);
	while (atomic_read(&a_sync) != 2) ;

	/* Lock not needed anymore ... */
	tc_mutex_unlock(&m);
	printf("   %p: unlocked!\n", tc_current());
}


void starter(void *unused)
{
	struct tc_thread *v, *l, *s;

	tc_signal_init(&the_drbd_signal);
	atomic_set(&a_sync, 0);
	tc_mutex_init(&m);
	tc_mutex_lock(&m);

	printf("%p: starter\n", tc_current());

	v=tc_thread_new(victim, 0, "victim");
	/* Let victim start ... */
	tc_sleep(CLOCK_MONOTONIC, 0, 100e6);

	s=tc_thread_new(ts, 0, "signal");
	l=tc_thread_new(tl, 0, "locker");

	tc_signal_fire(&the_drbd_signal);
	printf("fired\n");

	tc_thread_wait(s);
	tc_thread_wait(l);
	tc_thread_wait(v);
}


void sig(int s)
{ 
	printf("signal caught\n"); fflush(NULL); 
	exit(1); /* gcov needs a clean exit. */
}


int main()
{
	signal(SIGALRM, sig);
	signal(SIGINT, sig);

	alarm(2);
	tc_run(starter, NULL, "test", 2);
	printf("end.\n");
	return 0;
}
