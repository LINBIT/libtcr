/*
   This file is part of libtcr by Philipp Reisner.

   Copyright (C) 2009-2010, LINBIT HA-Solutions GmbH.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; version 3.

   libtcr is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with libtcr; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

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
	EF_SIGNAL      = 3, /* Like EF_READY, and free the event. */
	EF_PRIORITY    = 4, /* LIKE EF_READY, but takes precedence. */
};

enum tc_rv {
	RV_OK        = 0, /* The event you waited for happened */
	RV_INTR      = 1, /* A tc_signal, initialized in this co_thread, was fired. */
	RV_THREAD_NA = 2, /* Thread not existing (already exited?) */
	RV_FAILED    = 3
};

struct event {
	CIRCLEQ_ENTRY(event) e_chain;
	struct tc_thread *tc;
	union {
		__uint32_t ep_events; /* EPOLLIN, EPOLLOUT, ... */
		struct tc_fd *tcfd;   /* when it is attached to an tc_thread */
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
	atomic_t err_hup;
	__uint32_t ep_events;  /* current mask */
	struct event_list events;
	struct tc_fd *free_list_next;
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

struct tc_rw_lock {
	struct tc_mutex mutex;
	struct tc_waitq wr_wq;
	atomic_t readers;
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

typedef int (*diagnostic_fn)(const char *format, va_list ap);
void tc_set_diagnostic_fn(diagnostic_fn f);
void tc_set_stack_size(int size);

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

/* Bundles of threads
   tc_thread_pool_new() creates a tc_thread instance of the supplied function on
   every worker.
 */
void tc_thread_pool_new(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name, int excess);
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
enum tc_rv tc_rearm(struct tc_fd *the_tc_fd);
enum tc_rv _tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd, enum tc_event_flag ef);
static inline enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd)
{
	return _tc_wait_fd(ep_events, tcfd, EF_READY);
}
static inline enum tc_rv tc_wait_fd_prio(__uint32_t ep_events, struct tc_fd *tcfd)
{
	return _tc_wait_fd(ep_events, tcfd, EF_PRIORITY);
}
static inline int tc_fd(struct tc_fd *tcfd)
{
	return tcfd->fd;
}

/* Mutex. */
void tc_mutex_init(struct tc_mutex *m);
enum tc_rv tc_mutex_lock(struct tc_mutex *m);
void tc_mutex_unlock(struct tc_mutex *m);
void tc_mutex_destroy(struct tc_mutex *m);
/* A thread holding the mutex can ask whether there are other waiting threads. 
 * */
int tc_mutex_waiters(struct tc_mutex *m);
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
struct tc_signal_sub {
	struct event event;
	LIST_ENTRY(tc_signal_sub) se_chain;
};
void tc_signal_init(struct tc_signal *s);
struct tc_signal_sub *tc_signal_subscribe(struct tc_signal *s);
void tc_signal_unsubscribe(struct tc_signal *s, struct tc_signal_sub *ss);
void tc_signal_fire(struct tc_signal *s);
void tc_signal_destroy(struct tc_signal *s);
/* Functions for stack-allocated tc_signal_sub */
struct tc_signal_sub *tc_signal_subscribe_exist(struct tc_signal *s, struct tc_signal_sub *ss);
void tc_signal_unsubscribe_nofree(struct tc_signal *s, struct tc_signal_sub *ss);

/* Waitqueues.*/
void tc_waitq_init(struct tc_waitq *wq);
void tc_waitq_wakeup_all(struct tc_waitq *wq);
void tc_waitq_wakeup_one(struct tc_waitq *wq);
void tc_waitq_unregister(struct tc_waitq *wq);
void tc_waitq_wait(struct tc_waitq *wq);

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
	struct event e;			                                \
	e.el = NULL;			                                \
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


/* The tc_parallel_for(var, init_stmt, condition_stmt, increment_stmt) macro
   is similar to the for(;;) statement of the C language.

   It executes the iterations of body in parallel.

   LIMITATIONS:

      * Each execution thread of the statement's body has in independent
        instance of the counter variable. Modifying it in the body has
	no effect on the other iterations.

      * The statement's body has to be enclosed in braces { }
        I.e. the following example does not compile:

	tc_parallel_for(i, i = 0, i < 1000, i++)
		printf("%d\n", i);

	Valid is:

	tc_parallel_for(i, i = 0, i < 1000, i++) {
		printf("%d\n", i);
	}

      * It is implemented as a multi statement macro therefore it
	can not be used as a sole statement
	I.e. the following example does not compile:

	if (...)
		tc_parallel_for(i, i = 0, i < 1000, i++) {

	Valid is:

	if (...) {
		tc_parallel_for(i, i = 0, i < 1000, i++) {
		...
		}
	}
*/


#define __TO_STRING(A) #A
#define TO_STRING(A) __TO_STRING(A)

#define __TOKEN_PASTE(X, Y) X ## Y
#define TOKEN_PASTE(X, Y) __TOKEN_PASTE(X, Y)

#define __tc_parallel_for(VAR, INIT_ST, COND_ST, INC_ST, COUNT)		\
	auto void TOKEN_PASTE(__for_body_, COUNT)(typeof(VAR) VAR);	\
	struct tc_thread_pool TOKEN_PASTE(__tp_, COUNT);		\
	spinlock_t TOKEN_PASTE(__lock_, COUNT);				\
									\
	void TOKEN_PASTE(__for_func_, COUNT)(void *unused) {		\
		typeof(VAR) __i;					\
		spin_lock(&TOKEN_PASTE(__lock_, COUNT));		\
		while (COND_ST) {					\
			__i = INC_ST;					\
			spin_unlock(&TOKEN_PASTE(__lock_, COUNT));	\
			TOKEN_PASTE(__for_body_, COUNT)(__i);		\
			spin_lock(&TOKEN_PASTE(__lock_, COUNT));	\
		}							\
		spin_unlock(&TOKEN_PASTE(__lock_, COUNT));		\
	}								\
									\
	spin_lock_init(&TOKEN_PASTE(__lock_, COUNT));			\
	INIT_ST;							\
									\
	tc_thread_pool_new(&TOKEN_PASTE(__tp_, COUNT),			\
			   &TOKEN_PASTE(__for_func_, COUNT),		\
			   NULL,					\
			   "tc_parallel_for:"				\
			   __FILE__ ":"					\
			   TO_STRING(__LINE__) " %d", 0);		\
									\
	tc_thread_pool_wait(&TOKEN_PASTE(__tp_, COUNT));		\
	void TOKEN_PASTE(__for_body_, COUNT)(typeof(VAR) VAR)		\

#define tc_parallel_for(VAR, INIT_ST, COND_ST, INC_ST) \
	__tc_parallel_for(VAR, INIT_ST, COND_ST, INC_ST, __COUNTER__)


/* The tc_parallel { body1 } tc_with { body2 } tc_parallel_end; macro
   will execute the statements in body1 parallel with the statements in
   body2.

   This macro can be used as a sole statement, also the two bodies might
   be sole statements.

	tc_parallel
		tc_parallel
			printf("p1\n");
		tc_with
			printf("p2\n");
		tc_parallel_end;
	tc_with
		tc_parallel
			printf("p3\n");
		tc_with
			printf("p4\n");
		tc_parallel_end;
	tc_parallel_end;

*/

#define tc_parallel						\
	do {							\
	auto void par_body1(void *);				\
	struct tc_thread *tc;					\
	tc = tc_thread_new(par_body1,				\
			   NULL,				\
			   "tc_parallel:" __FILE__ ":"		\
			   TO_STRING(__LINE__));		\
	void par_body1(void *unused) {				\

#define tc_with							\
	}							\
	auto void par_body2(void *);				\
	par_body2(NULL);					\
	tc_thread_wait(tc);					\
	void par_body2(void *unused) {				\

#define tc_parallel_end						\
	} } while(0)


#define container_of(ptr, type, member) ({                      \
	const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	(type *)( (char *)__mptr - offsetof(type,member) );})


/* This macro retries an operation until it doesn't return RV_INTR; this can be
 * used to *always* take the lock, ignoring interrupts.
 */
#define TC_NO_INTR(stmt) ({ enum tc_rv __rv; while ((__rv = (stmt)) == RV_INTR) ; __rv; })

/* RW-Locks.
 * We need them to be starvation-free, and mostly fair; so we use waitqueues.
 * */
void tc_rw_init(struct tc_rw_lock *l);
enum tc_rv tc_rw_r_lock(struct tc_rw_lock *l);
enum tc_rv tc_rw_w_lock(struct tc_rw_lock *l);
static inline void tc_rw_r_unlock(struct tc_rw_lock *l)
{
	if (atomic_dec(&l->readers) == 0)
		tc_waitq_wakeup_one(&l->wr_wq);
}
static inline void tc_rw_w_unlock(struct tc_rw_lock *l)
{
	tc_mutex_unlock(&l->mutex);
}
static inline void tc_rw_w2r_lock(struct tc_rw_lock *l)
{
	/* This order is important. */
	atomic_inc(&l->readers);
	tc_mutex_unlock(&l->mutex);
}
static inline int tc_rw_get_readers(struct tc_rw_lock *l)
{
	return atomic_read(&l->readers);
}


#ifdef WAIT_DEBUG
#define tc_sched_yield()	({ SET_CALLER; tc_sched_yield(); UNSET_CALLER; })
#define tc_wait_fd(E, T)	({ enum tc_rv rv; SET_CALLER; rv = tc_wait_fd(E, T); UNSET_CALLER; rv; })
#define tc_wait_fd_prio(E, T)	({ enum tc_rv rv; SET_CALLER; rv = tc_wait_fd_prio(E, T); UNSET_CALLER; rv; })
#define tc_mutex_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_mutex_lock(M); UNSET_CALLER; rv; })
#define tc_thread_wait(T)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_wait(T); UNSET_CALLER; rv; })
#define tc_waitq_wait(W)	({ SET_CALLER; tc_waitq_wait(W); UNSET_CALLER; })
#define tc_thread_pool_wait(P)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_pool_wait(P); UNSET_CALLER; rv; })
#define tc_sleep(C, S, N)	({ enum tc_rv rv; SET_CALLER; rv = tc_sleep(C, S, N); UNSET_CALLER; rv; })

#define tc_rw_w_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_rw_w_lock(M); UNSET_CALLER; rv; })
#define tc_rw_r_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_rw_r_lock(M); UNSET_CALLER; rv; })

/* This is a handy function to be called from within gdb */
void tc_dump_threads(void);

#endif /* ifdef WAIT_DEBUG */

#endif /* ifndef THREADED_CR_H */
