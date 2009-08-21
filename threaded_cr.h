#ifndef THREADED_CR_H
#define THREADED_CR_H

#include <sys/queue.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/timerfd.h>

#include "atomic.h"
#include "coroutines.h"

enum tc_event_flag {
	EF_ALL      = 0, /* Wake all tc_threads waiting for that event. (freed() by the scheduler)       */
	EF_ALL_FREE = 1, /* Like EF_ALL, and free the event. */
	EF_ONE      = 2, /* Wake only one tc_thread waiting for that event. Call to tc_rearm() necesary  */
	EF_EXITING  = 3, /* Free my struct tc_thread */
	EF_READY    = 4, /* I am ready to run */
};

enum tc_rv {
	RV_OK        = 0, /* The event you waited for happened */
	RV_INTR      = 1, /* A tc_signal, initializied in this co_thread, was fired. */
	RV_THREAD_NA = 2, /* Thread not existing (already exited?) */
	RV_FAILED    = 3
};

struct event {
	LIST_ENTRY(event) chain;
	struct tc_thread* tc;
	__uint32_t ep_events; /* EPOLLIN, EPOLLOUT, ... and EF_IMMEDIATE*/
	enum tc_event_flag flags;
};

LIST_HEAD(events, event);

struct tc_fd {
	int fd;
	struct events events;
	spinlock_t lock; /* protects the events list */
	__uint32_t ep_events;  /* current mask */
};

struct tc_mutex {
	atomic_t count;
	struct tc_fd read_tcfd;
};

struct waitq_ev;

LIST_HEAD(wait_evs, waitq_ev);

struct tc_waitq {
	struct waitq_ev *active;
	struct waitq_ev *spare;
	spinlock_t lock;
};

struct tc_signal {
	struct tc_waitq wq;
};

struct tc_thread;
LIST_HEAD(tc_threads, tc_thread);

/* Threaded coroutines
 tc_run() is the most convenient way to initialize the threaded coroutines
 system. Alternatively use tc_init(), start a number of pthreads, and
 call tc_worker_init() and tc_scheduler() in each pthread.

 Call this only once, else weird things may happen.
*/
void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers);
void tc_init();
void tc_worker_init(int i);
void tc_scheduler();

/* Threads
   A tc_thread is a very light object: 4K stack. You can create a thread,
   and you can wait on the termination of a thread.
   When the main function of a thread returns, it calls tc_die(), which
   cleans up the exiting thread. You might call tc_die() directly, but
   keep in mind to free resources (esp. registered fds and mutexes)
 */
struct tc_thread *tc_thread_new(void (*func)(void *), void *data, char* name);
enum tc_rv tc_thread_wait(struct tc_thread *tc);
void tc_die();

/* Bundles of threads
   tc_threads_new() creates a tc_thread instance of the supplied function on
   every worker.
 */
void tc_threads_new(struct tc_threads *threads, void (*func)(void *), void *data, char* name);
void tc_threads_wait(struct tc_threads *threads);

/* FDs
   Register each fd you want to wait on. if there are multiple concurrent
   tc_threads waiting in one tc_wait_fd() on the same tcfd, only one will
   get woken up. That one should as soon as possible call tc_rearm, so that
   the next tc_threads will get woken up, to continue consuming data
   from the fd.
 */
struct tc_fd *tc_register_fd(int fd);
void tc_unregister_fd(struct tc_fd *tcfd);
enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd);
void tc_rearm();
static inline int tc_fd(struct tc_fd *tcfd)
{
	return tcfd->fd;
}

/* Mutex. */
void tc_mutex_init(struct tc_mutex *m);
enum tc_rv tc_mutex_lock(struct tc_mutex *m);
void tc_mutex_unlock(struct tc_mutex *m);
void tc_mutex_unregister(struct tc_mutex *m);
enum tc_rv tc_mutex_trylock(struct tc_mutex *m);

/* Signals
   After a signal has been initialized , it might be enabled in multiple
   tc_threads. When a signal gets fired, all tc_threads that enabled that
   signal will get interrupted. I.e. Any sleeping tc_XXX function will 
   return RV_INTR instead of RV_OK once.
   A tc_thread that enabled a signal, and exits for an other reason, than
   being waked up by that signal, should disable that signal again.
   I.e. you should disable all signals in the exit path, that got enabled
   tc_signal_disable is idempotent.
*/
void tc_signal_init(struct tc_signal *s);
void tc_signal_enable(struct tc_signal *s);
void tc_signal_disable(struct tc_signal *s);
void tc_signal_fire(struct tc_signal *s);
void tc_signal_unregister(struct tc_signal *s);

/* Waitqueues.*/
void tc_waitq_init(struct tc_waitq *wq);
void tc_waitq_wakeup(struct tc_waitq *wq);
void tc_waitq_unregister(struct tc_waitq *wq);

/* tc_sleep clockid = CLOCK_REALTIME or CLOCK_MONOTONIC */
enum tc_rv tc_sleep(int clockid, time_t sec, long nsec);

/* The following are intended to be used via the tc_waitq_wait_event(wq, condition) macro */
struct waitq_ev *tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e);
void tc_waitq_finish_wait(struct tc_waitq *wq, struct waitq_ev *we);
void _waitq_before_schedule(struct event *e, struct waitq_ev *we);
int _waitq_after_schedule(struct event *e, struct waitq_ev *we);


#define __tc_wait_event(wq, cond, rv)					\
do {									\
	struct waitq_ev *we;						\
	struct event e;							\
	we = tc_waitq_prepare_to_wait(wq, &e);				\
	while (1) {							\
		if (cond)						\
			break;						\
		_waitq_before_schedule(&e, we);				\
		tc_scheduler();						\
		if (_waitq_after_schedule(&e, we)) {			\
			rv = RV_INTR;					\
			break;						\
		}							\
	}								\
	tc_waitq_finish_wait(wq, we);					\
} while (0)

#define tc_waitq_wait_event(wq, cond)			\
({							\
	enum tc_rv rv = RV_OK;				\
	if (!(cond))					\
		__tc_wait_event(wq, cond, rv);		\
	rv;						\
 })


#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#endif
