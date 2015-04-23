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
#include <libaio.h>

#include "config.h"
#include "compat.h"
#include "atomic.h"
#include "spinlock.h"
#include "coroutines.h"


/* Signal to be used to wakeup workers. */
#define SIGNAL_FOR_WAKEUP (SIGRTMIN + 6)

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

struct tc_domain;
struct event {
	CIRCLEQ_ENTRY(event) e_chain;
	struct tc_thread *tc;
	union {
		__uint32_t ep_events; /* EPOLLIN, EPOLLOUT, ... */
		struct tc_fd *tcfd;   /* when it is attached to an tc_thread */
		struct tc_signal *signal;
	};
	enum tc_event_flag flags;
	unsigned int acked:1;
	struct event *next_in_stack;
	struct event_list *el;
	struct tc_domain *domain;
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
	int was_allocated:1;
	struct event_list events;
	struct tc_fd *free_list_next;
};

struct tc_waitq {
	struct event_list waiters;
};

struct tc_mutex {
	struct tc_thread *m_owner;
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
LIST_HEAD(_tc_thread_list, tc_thread);
struct tc_thread_pool {
	struct tc_domain *domain;
	struct _tc_thread_list list;
};

/* When a thread gets created, quits, and another one is created, the second
 * one might have the same (struct tc_thread*) as the first. To protect against
 * waiting for the wrong thread we provide an additional ID that callers
 * can use for verification - see tc_thread_new_ref() and tc_thread_wait_ref(). */
struct tc_thread_ref {
	struct tc_thread *thr;
	struct tc_domain *domain;
	int id;
};

/* Threaded coroutines
 tc_run() must be called once _for_each_pthread_group.
 It returns a scheduler "domain" that consists of the associated pthreads.
*/
void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers);
void tc_init();
void tc_worker_init();
void tc_scheduler();
int tc_sched_yield();
int tc_thread_count(void);

/* Renice a scheduling domain */
void tc_renice_domain(struct tc_domain *domain, int new_nice);
struct tc_domain *tc_new_domain(int nr_of_workers);

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
struct tc_thread_ref tc_thread_new_ref(void (*func)(void *), void *data, char* name);
struct tc_thread_ref tc_thread_new_ref_in_domain(void (*func)(void *), void *data, char* name, struct tc_domain *domain);
enum tc_rv tc_thread_wait_ref(struct tc_thread_ref *tc_r);
static inline enum tc_rv tc_thread_wait_domain(struct tc_thread *tc, struct tc_domain *domain)
{
	struct tc_thread_ref r;
	r.thr = tc;
	r.domain = domain;
	r.id = 0;
	return tc_thread_wait_ref(&r);
}
static inline enum tc_rv tc_thread_wait(struct tc_thread *tc)
{
	extern __thread struct tc_domain *tc_this_pthread_domain;
	return tc_thread_wait_domain(tc, tc_this_pthread_domain);
}
void tc_die();
static inline struct tc_thread *cr_to_tc(struct coroutine *cr)
{
	return cr_uptr(cr);
}
static inline struct tc_thread *tc_current()
{
	return cr_to_tc(cr_current());
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
int tc_thread_worker_nr(struct tc_thread *tc);

void tc_thread_pool_new_in_domain(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name, int excess, struct tc_domain *sched);

/* FDs
   Register each fd you want to wait on. if there are multiple concurrent
   tc_thread_pool waiting in one tc_wait_fd() on the same tcfd, only one will
   get woken up. That one should as soon as possible call tc_rearm, so that
   the next tc_thread_pool will get woken up, to continue consuming data
   from the fd.
 */
struct tc_fd *tc_register_fd(int fd);
int tc_register_fd_mem_still_blocking(int fd, struct tc_fd *tcfd);
void tc_unregister_fd(struct tc_fd *tcfd);
void tc_unregister_fd_mem(struct tc_fd *tcfd);
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

/* A per tc-thread opaque pointer. */
static inline void* _tc_thread_var_get(struct tc_thread *tc) 
{
	return ((void**)tc)[1];
}
static inline void* tc_thread_var_get()
{
	return _tc_thread_var_get(tc_current());
}
static inline void* _tc_thread_var_set(struct tc_thread *tc, void *data)
{
	return ((void**)tc)[1]=data;
}
static inline void* tc_thread_var_set(void *data)
{
	return _tc_thread_var_set(tc_current(), data);
}

/* Mutex. */
void tc_mutex_init(struct tc_mutex *m);
enum tc_rv tc_mutex_lock(struct tc_mutex *m);
void tc_mutex_unlock(struct tc_mutex *m);
void tc_mutex_destroy(struct tc_mutex *m);
/* A thread holding the mutex can ask whether there are other waiting threads.
 * */
enum tc_rv tc_mutex_trylock(struct tc_mutex *m);
void tc_mutex_change_owner(struct tc_mutex *m, struct tc_thread *who);

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
int tc_signals_since_last_call(void);

/* Waitqueues.*/
void tc_waitq_init(struct tc_waitq *wq);
void tc_waitq_wakeup_all(struct tc_waitq *wq);
void tc_waitq_wakeup_one(struct tc_waitq *wq);
void tc_waitq_unregister(struct tc_waitq *wq);
int tc_waitq_wait(struct tc_waitq *wq);

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


static inline void tc_event_init(struct event *e)
{
	extern __thread struct tc_domain *tc_this_pthread_domain;
	e->el = NULL;
	e->acked = 0;
	e->next_in_stack = NULL;
	e->domain = tc_this_pthread_domain;
}


#define __tc_wait_event(wq, cond, rv)					\
do {									\
	struct event e;							\
	tc_event_init(&e);						\
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
enum tc_rv tc_rw_w_trylock(struct tc_rw_lock *l);
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
#define tc_sched_yield()	({ enum tc_rv rv; SET_CALLER; rv = tc_sched_yield(); UNSET_CALLER; rv; })
#define tc_wait_fd(E, T)	({ enum tc_rv rv; SET_CALLER; rv = tc_wait_fd(E, T); UNSET_CALLER; rv; })
#define tc_wait_fd_prio(E, T)	({ enum tc_rv rv; SET_CALLER; rv = tc_wait_fd_prio(E, T); UNSET_CALLER; rv; })
#define tc_mutex_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_mutex_lock(M); UNSET_CALLER; rv; })
#define tc_thread_wait(T)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_wait(T); UNSET_CALLER; rv; })
#define tc_thread_wait_domain(T,D)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_wait_domain(T,D); UNSET_CALLER; rv; })
#define tc_waitq_wait(W)	({ enum tc_rv rv; SET_CALLER; rv = tc_waitq_wait(W); UNSET_CALLER; rv; })
#define tc_thread_pool_wait(P)	({ enum tc_rv rv; SET_CALLER; rv = tc_thread_pool_wait(P); UNSET_CALLER; rv; })
#define tc_sleep(C, S, N)	({ enum tc_rv rv; SET_CALLER; rv = tc_sleep(C, S, N); UNSET_CALLER; rv; })

#define tc_rw_w_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_rw_w_lock(M); UNSET_CALLER; rv; })
#define tc_rw_r_lock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_rw_r_lock(M); UNSET_CALLER; rv; })
#define tc_rw_w_trylock(M)	({ enum tc_rv rv; SET_CALLER; rv = tc_rw_w_trylock(M); UNSET_CALLER; rv; })

#endif /* ifdef WAIT_DEBUG */

/* This is a handy function to be called from within gdb.
 * Does nothing unless WAIT_DEBUG is enabled. */
void tc_dump_threads(const char *search);


/*
 * Async IO interface for libtcr.
 *
 * This must be used when writing to storage; using the normal write()/read()
 * calls would block the serving pthread, while these interrupt (optionally)
 * only the tc_thread, so that other tc_threads can be run by this pthread.
 *
 * It uses a limited number of iocb structures within each tc_thread; when they
 * are exhausted a tc_aio_wait() is done implicitly, to free space for the new
 * request.
 *
 */

struct tc_aio_data {
	/* Kept at offset 0 */
	struct iocb cb;

	/* return code: bytes transferred, error? */
	int64_t res;
	/* faults? */
	int64_t res2;
	/* the flag for "iocb done" is cb->aio_reserved1 - this _must_ be
	 * 0 (io_prep_* does memset(), kernel verifies), so after the kernel
	 * is done we can safely set it to 1.
	 * In userspace this seems to be called __pad2. */

	struct tc_waitq *notify;
};

#define TC_AIO_DONE_FIELD __pad2
#define TC_AIO_FLAG_AD_FREE (4)
static inline int tc_aio_data_done(struct tc_aio_data *ad)
{
	return ad->cb.TC_AIO_DONE_FIELD;
}

static inline void tc_aio_set_data_done(struct tc_aio_data *ad)
{
	ad->cb.TC_AIO_DONE_FIELD = 1;
}

static inline void tc_aio_set_data_free(struct tc_aio_data *ad)
{
	ad->cb.TC_AIO_DONE_FIELD = TC_AIO_FLAG_AD_FREE;
}

static inline void tc_aio_set_data_pending(struct tc_aio_data *ad)
{
	ad->cb.TC_AIO_DONE_FIELD = 0;
}



static inline void tc_aio_data_init(struct tc_aio_data *ad)
{
	memset(ad, 0, sizeof(*ad));
	/* Mark as "not in use" */
	tc_aio_set_data_done(ad);
}


#define TC_AIO_REQUESTS_PER_TC_THREAD (8)


/* Does wait for outstanding IO requests of this tc_thread. */
int tc_aio_wait();

/* Submits a sync request, doesn't wait.
 * After the next tc_aio_wait() the written data should be on stable storage.
 * */
int tc_aio_submit_sync_notify(int fd, int data_only,
		struct tc_aio_data *ad, struct tc_waitq *wq);

/* Does only submit IO; does not wait for completion. */
int tc_aio_submit_write_notify(int fh, void *buffer, size_t size, off_t offset,
		struct tc_aio_data *tc_aio, struct tc_waitq *wq);
static inline int tc_aio_submit_write(int fh, void *buffer, size_t size, off_t offset) {
	return tc_aio_submit_write_notify(fh, buffer, size, offset, NULL, NULL);
}

/* Does wait for completion.
 * Returns 0 for ok; should it give the number of bytes read instead? */
int tc_aio_read(int fh, void *buffer, size_t size, off_t offset);


/* Does an fsync() on the FD and waits until completion.  */
int tc_aio_sync(int fd, int data_only);
#if 0
static inline int tc_aio_sync(int fd, int data_only)
{
	int rv;

	rv = tc_aio_submit_sync_notify(fd, data_only, NULL, NULL);
	if (!rv)
		rv = tc_aio_wait();
	return rv;
}
#endif


/* Does an fsync() on the filehandle, ensures that all AIO requests from
 * _this_ tc_thread are done (incl. the sync), then calls the notification
 * function.
 * The tc_thread in which the function is called is not specified.
 * */
int tc_aio_sync_notify(int fd, int data_only, struct tc_aio_data *ad, struct tc_waitq *wq);


/* Writes data to fd and waits for completion.
 * Note: this doesn't say that the data is on stable storage, see tc_aio_sync()
 * for that. */
static inline int tc_aio_write(int fh, void *buffer, size_t size, off_t offset)
{
	int rv;

	rv = tc_aio_submit_write(fh, buffer, size, offset);
	if (!rv)
		rv = tc_aio_wait();
	return rv;
}

#endif /* ifndef THREADED_CR_H */
