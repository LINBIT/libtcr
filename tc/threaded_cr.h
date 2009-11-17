#ifndef THREADED_CR_H
#define THREADED_CR_H

#include <sys/queue.h>
#include <stddef.h>
#include <pthread.h>
#include <stdarg.h>

#include "config.h"
#include "compat.h"
#include "atomic.h"
#include "spinlock.h"
#include "coroutines.h"

enum tc_event_flag {
	EF_READY       = 1, /* I am ready to run */
	EF_EXITING     = 2, /* Free my struct tc_thread */
	EF_SIGNAL      = 3, /* Like EF_ALL, and free the event. */
};

enum tc_rv {
	RV_OK        = 0, /* The event you waited for happened */
	RV_INTR      = 1, /* A tc_signal, initializied in this co_thread, was fired. */
	RV_THREAD_NA = 2, /* Thread not existing (already exited?) */
	RV_FAILED    = 3
};

struct event {
	CIRCLEQ_ENTRY(event) e_chain;
	struct tc_thread *tc;
	union {
		__uint32_t ep_events; /* EPOLLIN, EPOLLOUT, ... */
		struct tc_fd *tcfd;   /* when it is attaache to an tc_thread */
		struct tc_signal *signal;
	};
	enum tc_event_flag flags;
	struct event_list *el;
};

CIRCLEQ_HEAD(events, event);

struct event_list {
	struct events events;
	spinlock_t lock; /* protects the events list */
};

struct tc_fd {
	int fd;
	__uint32_t ep_events;  /* current mask */
	struct event_list events;
};

struct tc_waitq {
	struct event_list waiters;
};

struct tc_mutex {
	atomic_t count;
	struct tc_waitq wq;
};

struct tc_signal_sub; /* signal subscription */
LIST_HEAD(tc_signal_subs, tc_signal_sub);

struct tc_signal {
	struct tc_waitq wq;
	struct tc_signal_subs sss; /* protected by wq.waiters.lock */
};

struct tc_thread;
LIST_HEAD(tc_thread_pool, tc_thread);

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
void tc_sched_yield();

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
static inline struct tc_thread *tc_current()
{
	return (struct tc_thread *)cr_uptr(cr_current());
}
static inline char *tc_thread_name(struct tc_thread *tc)
{
	return *(char **)tc;
}

typedef int (*diagnostic_fn)(const char *format, va_list ap);
void tc_set_diagnostic_fn(diagnostic_fn f);

/* Bundles of threads
   tc_thread_pool_new() creates a tc_thread instance of the supplied function on
   every worker.
 */
void tc_thread_pool_new(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name);
enum tc_rv tc_thread_pool_wait(struct tc_thread_pool *threads);

/* FDs
   Register each fd you want to wait on. if there are multiple concurrent
   tc_thread_pool waiting in one tc_wait_fd() on the same tcfd, only one will
   get woken up. That one should as soon as possible call tc_rearm, so that
   the next tc_thread_pool will get woken up, to continue consuming data
   from the fd.
 */
struct tc_fd *tc_register_fd(int fd);
void tc_unregister_fd(struct tc_fd *tcfd);
enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd);
void tc_rearm(struct tc_fd *the_tc_fd);
static inline int tc_fd(struct tc_fd *tcfd)
{
	return tcfd->fd;
}

/* Mutex. */
void tc_mutex_init(struct tc_mutex *m);
enum tc_rv tc_mutex_lock(struct tc_mutex *m);
void tc_mutex_unlock(struct tc_mutex *m);
void tc_mutex_destroy(struct tc_mutex *m);
enum tc_rv tc_mutex_trylock(struct tc_mutex *m);

/* Signals
   After a signal has been initialized , it might be enabled in multiple
   tc_thread_pool. When a signal gets fired, all tc_threads that enabled that
   signal will get interrupted. I.e. Any sleeping tc_XXX function will 
   return RV_INTR instead of RV_OK once.
   A tc_thread that enabled a signal, and exits for an other reason, than
   being waked up by that signal, should disable that signal again.
   I.e. you should disable all signals in the exit path, that got enabled
   tc_signal_unsubscribe is idempotent.
*/
void tc_signal_init(struct tc_signal *s);
struct tc_signal_sub *tc_signal_subscribe(struct tc_signal *s);
void tc_signal_unsubscribe(struct tc_signal *s, struct tc_signal_sub *ss);
void tc_signal_fire(struct tc_signal *s);
void tc_signal_destroy(struct tc_signal *s);

/* Waitqueues.*/
void tc_waitq_init(struct tc_waitq *wq);
void tc_waitq_wakeup_all(struct tc_waitq *wq);
void tc_waitq_wakeup_one(struct tc_waitq *wq);
void tc_waitq_unregister(struct tc_waitq *wq);

/* Source compatibility stuff. In new code use the _one or _all variant*/
#define tc_waitq_wakeup(WQ) tc_waitq_wakeup_all(WQ)

/* tc_sleep clockid = CLOCK_REALTIME or CLOCK_MONOTONIC */
enum tc_rv tc_sleep(int clockid, time_t sec, long nsec);

/* The following are intended to be used via the tc_waitq_wait_event(wq, condition) macro */
void tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e);
int tc_waitq_finish_wait(struct tc_waitq *wq, struct event *e);

#ifdef WAIT_DEBUG
extern __thread char *_caller_file;
extern __thread int _caller_line;
#define SET_CALLER   _caller_file = __FILE__, _caller_line = __LINE__
#define UNSET_CALLER _caller_file = "untracked tc_scheduler() call", _caller_line = 0
#else
#define SET_CALLER
#define UNSET_CALLER
#endif

#define __tc_wait_event(wq, cond, rv)					\
do {									\
	struct event e;							\
	while (1) {							\
		tc_waitq_prepare_to_wait(wq, &e);			\
		if (cond) {						\
			tc_waitq_finish_wait(wq, &e);			\
			break;						\
		}							\
		tc_scheduler();						\
		if (tc_waitq_finish_wait(wq, &e)) {			\
			rv = RV_INTR;					\
			break;						\
		}							\
	}								\
} while (0)

#define tc_waitq_wait_event(wq, cond)			\
({							\
	enum tc_rv rv = RV_OK;				\
	SET_CALLER;					\
	if (!(cond))					\
		__tc_wait_event(wq, cond, rv);		\
	UNSET_CALLER;					\
	rv;						\
 })


#define container_of(ptr, type, member) ({                      \
	const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	(type *)( (char *)__mptr - offsetof(type,member) );})


#ifdef WAIT_DEBUG
#define tc_sched_yield()	({ SET_CALLER; tc_sched_yield(); UNSET_CALLER; })
#define tc_wait_fd(E, T)	({ enum tc_rv rv; SET_CALLER; rv = tc_wait_fd(E, T); UNSET_CALLER; rv; })
#define tc_mutex_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_mutex_lock(M); UNSET_CALLER; rv; })
#define tc_thread_wait(T)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_wait(T); UNSET_CALLER; rv; })
#define tc_waitq_wait(W)	({ SET_CALLER; tc_waitq_wait(W); UNSET_CALLER; })
#define tc_thread_pool_wait(P)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_pool_wait(P); UNSET_CALLER; rv; })
#define tc_sleep(C, S, N)	({ enum tc_rv rv; SET_CALLER; rv = tc_sleep(C, S, N); UNSET_CALLER; rv; })
#endif /* ifdef WAIT_DEBUG */

#endif /* ifndef THREADED_CR_H */
