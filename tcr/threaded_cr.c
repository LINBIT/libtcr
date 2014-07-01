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

#define _GNU_SOURCE /* for asprintf() */

#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/time.h>
#include <libaio.h>
#include <aio.h>
#include <assert.h>
#include <sched.h>

#include "compat.h"
#include "atomic.h"
#include "spinlock.h"
#include "coroutines.h"
#include "threaded_cr.h"
#include "clist.h"

#define DEFAULT_STACK_SIZE (1024 * 16)

#define FREE_WORKER -1
#define ANY_WORKER -2


/* There are no special variables like in Lisp ... but fixing _all_ sites,
 * recursively, is bad.
 * Would extend all the arguments list for this one corner case...
 * so, as we're having a cooperative scheduler and can thus be sure to be
 * "safe" we just do this. */
#define WITH_OTHER_DOMAIN_BEGIN(__domain)					\
		struct tc_domain *__prev_domain;					\
		__prev_domain = tc_this_pthread_domain;				\
		tc_this_pthread_domain = __domain;

#define WITH_OTHER_DOMAIN_END()								\
		tc_this_pthread_domain = __prev_domain;

enum thread_flags {
	TF_THREADS =   1 << 0, /* is on threads chain*/
	TF_RUNNING =   1 << 1,
	TF_FREE_NAME = 1 << 2,
	TF_AFFINE  =   1 << 3, /* Wants to stay on that worker thread.*/
};

struct tc_thread {
	char *name;		/* Leave that first, for debugging spinlocks */
	void *per_thread_data;	/* Leave that second. */
	LIST_ENTRY(tc_thread) tc_chain;      /* list of all threads */
	LIST_ENTRY(tc_thread) threads_chain; /* list of threads created with one call to tc_thread_pool_new() */
	struct coroutine *cr;
	struct tc_waitq exit_waiters;
	atomic_t refcnt;
	spinlock_t running;
	enum thread_flags flags; /* flags protected by pending.lock */
	/* List of events that got signalled */
	struct event_list pending;
	/* Stack of events (via next_in_stack) - the topmost is the "current"
	 * event. Protected via spinlock in pending. */
	struct event *event_stack;
	struct event e;  /* Used during start and stop. */
	int worker_nr;
	int id;
	atomic_t signals_since_last_query;

	struct tc_domain *domain;
	struct worker_struct *last_worker;

	struct tc_aio_data aio[TC_AIO_REQUESTS_PER_TC_THREAD];
	struct tc_waitq aio_wq;

#ifdef WAIT_DEBUG
	const char *sleep_file;
	int sleep_line;
#endif
};

struct worker_struct {
	int nr;
	struct tc_thread main_thread;
	struct tc_thread sched_p2;
	struct event *woken_by_event; /* always set after tc_scheduler()   */
	struct tc_fd *woken_by_tcfd;  /* might be set after tc_scheduler() */
	struct clist_entry sleeping_chain;
	LIST_ENTRY(worker_struct) worker_chain;
	int is_on_sleeping_list:1;
	int must_sync:1;
	int is_init:1;
	pid_t tid;
};

enum inter_worker_interrupts {
	IWI_SYNC,
	IWI_IMMEDIATE,
	IWI_ASYNC_IO,
};


#define NOT_ON_TIMERLIST ((struct timer_waiter*)-1)
struct timer_waiter {
	struct tc_waitq wq;
	struct timespec abs_end;
	struct tc_thread *tc;
	LIST_ENTRY(timer_waiter) tl;
};


LIST_HEAD(timer_list, timer_waiter);
LIST_HEAD(worker_list_t, worker_struct);

struct tc_domain {
	int aio_eventfd;
	io_context_t aio_ctx;

	spinlock_t lock;           /* protects the threads list */
	struct tc_thread_pool threads;
	int nr_of_workers;
	struct clist_entry sleeping_workers; // global? would mean that sync_world gets much more expensive ...
	int efd;                   /* epoll fd */
	int immediate_fd;          /* IWI immediate */
	spinlock_t sync_lock;
	atomic_t sync_barrier;
	struct tc_fd *free_list;
	int stack_size;            /* stack size for new tc_threads */
	int last_thread_id;
	int nice_level;

	atomic_t timer_sleepers;
	struct tc_fd timer_tcfd;
	struct tc_mutex timer_mutex;
	spinlock_t timer_lock;
	struct timer_list timer_list;

	spinlock_t worker_list_lock;
	struct worker_list_t worker_list;
	pthread_t *pthreads;

	/* a HEAD-less list. */
	SLIST_ENTRY(tc_domain) domain_chain;
};

struct common_data_t {
	cpu_set_t available_cpus;
	atomic_t pthread_counter;
	diagnostic_fn diagnostic;
	struct event_list immediate;
};

static int fprintf_stderr(const char *fmt, va_list ap);

static struct common_data_t common = {
	.available_cpus = { { 0 } },
	.pthread_counter = { 0 },
	.diagnostic = fprintf_stderr,
};


#ifdef WAIT_DEBUG
#undef tc_sched_yield
#undef tc_wait_fd
#undef tc_wait_fd_prio
#undef tc_mutex_lock
#undef tc_thread_wait
#undef tc_waitq_wait
#undef tc_thread_pool_wait
#undef tc_sleep
#undef tc_rw_w_lock
#undef tc_rw_w_trylock
#undef tc_rw_r_lock

__thread char *_caller_file = "untracked tc_scheduler() call";
__thread int _caller_line = 0;
#endif

static void _signal_gets_delivered(struct event *e);
static void signal_cancel_pending();
static void worker_prepare_sleep();
static void worker_after_sleep();
static void store_for_later_free(struct tc_fd *tcfd);
static void iwi_immediate(struct tc_domain *dom);
static void _tc_fd_init(struct tc_fd *tcfd, int fd);
static void _remove_event(struct event *e, struct event_list *el);
static struct event_list *remove_event(struct event *e);
static void tc_waitq_wakeup_one_owner(struct tc_waitq *wq, atomic_t *woken);

__thread struct tc_domain *tc_this_pthread_domain = NULL;
/* I don't want to make "tc_this_pthread_domain" visible globally, but  */
#define tc_this_pthread_domain tc_this_pthread_domain

static struct tc_thread_ref tc_main;
static __thread struct worker_struct worker;

static void new_domain(struct tc_domain **pd)
{
	struct tc_domain *d;

	d = calloc(1, sizeof(*d));
	if (pd)
		*pd = d;
	if (!d)
		return;

	d->stack_size = DEFAULT_STACK_SIZE;
	d->nice_level = getpriority(PRIO_PROCESS, 0);
}


static int fprintf_stderr(const char *fmt, va_list ap)
{
	return vfprintf(stderr, fmt, ap);
}

void tc_set_diagnostic_fn(diagnostic_fn f)
{
	common.diagnostic = f;
}

void tc_set_stack_size(int s)
{
	tc_this_pthread_domain->stack_size = s;
}

void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
void msg_exit(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	common.diagnostic(fmt, ap);
	va_end(ap);

	exit(code);
}


/* Remove an event from its list, checking whether it's on a locked list.
 * If it's on a locked list, we may not lock again; there's a small race
 * condition, because the event might get _moved_ to one of the lists while
 * we're checking ... */
static inline void remove_event_holding_locks(struct event *e, ...) __attribute__((sentinel));
static inline void remove_event_holding_locks(struct event *e, ...)
{
	va_list va;
	struct event_list *el;

	if (!e->el)
		return;

	va_start(va, e);
	while (1) {
		el = va_arg(va, struct event_list*);
		if (!el)
			break;

		if (e->el == el) {
			_remove_event(e, e->el);
			return;
		}
	}

	remove_event(e);
}


void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	common.diagnostic(fmt, ap);
	va_end(ap);
}

/* must_hold tcfd->lock */
static __uint32_t calc_epoll_event_mask(struct events *es)
{
	__uint32_t em = 0;

	struct event *e;
	CIRCLEQ_FOREACH(e, es, e_chain) {
		em |= e->ep_events;
	}

	return em;
}

/* must_hold tcfd->lock */
static void move_to_immediate(struct event *e)
{
	spin_lock(&common.immediate.lock);
	CIRCLEQ_REMOVE(&e->el->events, e, e_chain);
	e->el = &common.immediate;
	CIRCLEQ_INSERT_TAIL(&common.immediate.events, e, e_chain);
	spin_unlock(&common.immediate.lock);
}

/* must_hold tcfd->lock */
static struct event *matching_event(__uint32_t em, struct events *es)
{
	struct event *e;
	struct event *in  = NULL;
	struct event *out = NULL;
	struct event *ew  = NULL;  /* match on this worker */

	CIRCLEQ_FOREACH(e, es, e_chain) {
		if (em & e->ep_events & EPOLLIN) {
			if (!in)
				in = e;
			if (e->tc->worker_nr == worker.nr)
				ew = in = e;
			if (e->flags == EF_PRIORITY) {
				em &= ~EPOLLIN;
				in = e;
			}

		}
		if (em & e->ep_events & EPOLLOUT) {
			if (!out)
				out = e;
			if (e->tc->worker_nr == worker.nr)
				ew = out = e;
			if (e->flags == EF_PRIORITY) {
				em &= ~EPOLLOUT;
				out = e;
			}
		}
		if (!em)
			break;
	}

	if (ew) {
		e = (ew == in ? out : in);
		if (e)
			move_to_immediate(e);
		return ew;
	} else {
		if (in && out) {
			move_to_immediate(in);
			return out;
		}
		return in ? in : out;
	}
}

/* must_hold tcfd->lock */
static struct event *wakeup_all_events(struct events *es)
{
	struct event *e;
	struct event *next;
	struct event *ex  = NULL;
	struct event *ew  = NULL;  /* match on this worker */

	/* We cannot use
	 *   CIRCLEQ_FOREACH(e, es, e_chain)
	 * here, as this macro uses the pointers from the current element (e) to
	 * get to the next - but by then the pointers are already changed by
	 * move_to_immediate(). */
	for (e = es->cqh_first;
			e != (const void *)es;
			e = next)
	{
		/* Remember next element; we don't have the pointer anymore after the
		 * move_to_immediate().  */
		next = e->e_chain.cqe_next;
		if (!ew && e->tc->worker_nr == worker.nr) {
			ew = e;
			continue;
		}
		if (!ex) {
			ex = e;
			continue;
		}
		move_to_immediate(e);
	}

	if (ew && ex)
		move_to_immediate(ex);

	return ew ? ew : ex;
}

static inline int tcfd_epoll_ctl(int op, struct tc_fd *tcfd, struct epoll_event *epe)
{
	epe->data.ptr = tcfd;
	return epoll_ctl(tc_this_pthread_domain->efd, op, tcfd->fd, epe);
}


/* must_hold tcfd->lock */
static int arm(struct tc_fd *tcfd)
{
	struct epoll_event epe;

	epe.events = calc_epoll_event_mask(&tcfd->events.events);
	if (epe.events == tcfd->ep_events)
		return 0;

	tcfd->ep_events = epe.events;

	return tcfd_epoll_ctl(EPOLL_CTL_MOD, tcfd, &epe);
}

static void event_list_init(struct event_list *el)
{
	CIRCLEQ_INIT(&el->events);
	spin_lock_init(&el->lock);
}

/* must_hold el->lock */
static void _remove_event(struct event *e, struct event_list *el)
{
	CIRCLEQ_REMOVE(&el->events, e, e_chain);
	atomic_dec(&e->tc->refcnt);
	e->el = NULL;
}

static struct event_list *remove_event(struct event *e)
{
	struct event_list *el;

	/* The event can be moved to an other list while we try to grab
	   the list lock... */
	while(1) {
		do el = ((volatile struct event *)e)->el; while (el == NULL);
		spin_lock(&el->lock);
		if (el == ((volatile struct event *)e)->el)
			break;
		spin_unlock(&el->lock);
	}

	_remove_event(e, el);
	spin_unlock(&el->lock);
	return el;
}

/* must_hold el->lock */
static void _add_event(struct event *e, struct event_list *el, struct tc_thread *tc)
{
	atomic_inc(&tc->refcnt);
	e->tc = tc;
	if (e->el)
		msg_exit(1, "Event %p is still on a list (%p)\n", e, e->el);
	e->el = el;

	CIRCLEQ_INSERT_TAIL(&el->events, e, e_chain);
}

int add_event_fd(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_fd *tcfd)
{
	int rv;

	e->ep_events = ep_events;
	e->flags = flags;
	tc_event_init(e);

	spin_lock(&tcfd->events.lock);
	rv = RV_FAILED;
	if (atomic_read(&tcfd->err_hup))
		goto unlock;

	_add_event(e, &tcfd->events, tc_current());
	rv = arm(tcfd);
	if (rv != RV_OK)
		_remove_event(e, &tcfd->events);
unlock:
	spin_unlock(&tcfd->events.lock);
	return rv;
}

inline static void _tc_event_on_tc_stack(struct event *e, struct tc_thread *tc)
{
	spin_lock(&tc->pending.lock);
	e->next_in_stack = tc->event_stack;
	tc->event_stack = e;
	assert((long)tc->event_stack != 0xafafafafafafafaf);
	spin_unlock(&tc->pending.lock);
}


static void add_event_cr(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_thread *tc)
{
	e->ep_events = ep_events;
	e->flags = flags;

	spin_lock(&common.immediate.lock);
	_add_event(e, &common.immediate, tc);
	spin_unlock(&common.immediate.lock);
}

void remove_event_fd(struct event *e, struct tc_fd *tcfd)
{
	remove_event(e);
}

static void _iwi_immediate(struct tc_domain *dom);
void tc_thread_free(struct tc_thread *tc)
{
	struct tc_domain *dom;

	dom = tc->domain;
	spin_lock(&tc->running); /* Make sure it has reached switch_to(), after posting EF_EXITING */
	tc_waitq_unregister(&tc->exit_waiters);
	cr_delete(tc->cr);
	tc->cr = NULL;
	if (tc->flags & TF_FREE_NAME)
		free(tc->name);
	memset(tc, 0xaf, sizeof(*tc));
	free(tc);
	_iwi_immediate(dom);
}

static void _switch_to(struct tc_thread *new)
{
	struct tc_thread *previous;

	/* previous = tc_current();
	   printf(" (%d) switch: %s -> %s\n", worker.nr, previous->name, new->name); */

	if (!new->cr)
		msg_exit(1, "die!\n");

	/* It can happen that the stack frame we want to switch to is still active,
	   in a rare condition: A tc_thread reads a few byte from an fd, and calls
	   tc_wait_fd(), although there is still input available. That call enables
	   the epoll-event again. Before that tc_thread makes it to the scheduler
	   (and epoll_wait), an other worker returns from its epoll_wait call and
	   switches to the same tc_thread again. That causes bad stack corruption
	   of course.
	   To circumvent that we spin here until the tc_thread is no longer running */
	spin_lock(&new->running);

	cr_call(new->cr);

	previous = (struct tc_thread *)cr_uptr(cr_caller());
	spin_unlock(&previous->running);
}

static void switch_to(struct tc_thread *new)
{
	int nr = worker.nr;

	if (new->worker_nr != nr) {
		if (!(new->flags & TF_AFFINE))
			new->worker_nr = nr;
	}
	new->last_worker = &worker;
	_switch_to(new);
}

static void arm_aio_efd(int op)
{
	struct epoll_event epe;

	epe.data.u64 = IWI_ASYNC_IO;
	epe.events = EPOLLIN | EPOLLONESHOT;

	if (epoll_ctl(tc_this_pthread_domain->efd, op, tc_this_pthread_domain->aio_eventfd, &epe))
		msg_exit(1, "epoll_ctl for AIO arm failed with %m\n");
}

static void arm_immediate(int op)
{
	struct epoll_event epe;

	epe.data.ptr = (void*)IWI_IMMEDIATE;
	epe.events = EPOLLIN | EPOLLONESHOT;

	if (epoll_ctl(tc_this_pthread_domain->efd, op, tc_this_pthread_domain->immediate_fd, &epe))
		msg_exit(1, "epoll_ctl for immediate arm failed with %m\n");
}

/* The event is not on any list. */
static struct tc_thread *run_or_queue(struct event *e)
{
	struct tc_thread *tc = e->tc;

	if (e->flags == EF_EXITING)
		return tc;

	spin_lock(&tc->pending.lock);
	if (tc->flags & TF_RUNNING)
		goto queue;
	if (e->flags == EF_SIGNAL)
		goto start_thread;
	if (!tc->event_stack)
		goto start_thread;
	if (e == tc->event_stack)
		goto start_thread;

	/* Some event, but not the top-most event of this thread.
	 * Eg. tc_waitq_wait_event() had a tc_sleep() within the
	 * condition, and the outer wq has fired. */
	{
queue:
		if (e->flags != EF_SIGNAL)
			e->tcfd = worker.woken_by_tcfd;
		_add_event(e, &tc->pending, tc);
		spin_unlock(&tc->pending.lock);
		return NULL;
	}

start_thread:
	tc->flags |= TF_RUNNING;
	spin_unlock(&tc->pending.lock);

#ifdef WAIT_DEBUG
	tc->sleep_file = "running";
	tc->sleep_line = 0;
#endif

	if (e->flags == EF_SIGNAL)
		_signal_gets_delivered(e);

	worker.woken_by_event = e;

	return tc;
}

static int _run_immediate(int nr)
{
	struct event *e;
	struct tc_thread* tc;
	int wanted;

search_loop:
	spin_lock(&common.immediate.lock);
search_loop_locked:
	worker.woken_by_tcfd  = NULL;
	CIRCLEQ_FOREACH(e, &common.immediate.events, e_chain) {
		assert(e->domain == e->tc->domain);
		if (e->domain != tc_this_pthread_domain)
			continue;
		wanted = (nr == ANY_WORKER) ||
			(e->tc->worker_nr == nr) ||
			(nr == FREE_WORKER ?
			 !(e->tc->flags & TF_AFFINE) : 0);
		if (!wanted)
			continue;
		_remove_event(e, &common.immediate);
		tc = run_or_queue(e);
		if (!tc) {
			/* We don't know what the queue looks like, so start at the
			 * beginning again. */
			goto search_loop_locked;
		}
		switch (e->flags) {
		case EF_PRIORITY:
		case EF_READY:
		case EF_SIGNAL:
			if (!CIRCLEQ_EMPTY(&common.immediate.events))
				iwi_immediate(tc_this_pthread_domain); /* More work available, wakeup an worker */
			spin_unlock(&common.immediate.lock);
			switch_to(tc);
			return 1;
		case EF_EXITING:
			spin_unlock(&common.immediate.lock);
			tc_thread_free(e->tc);
			/* We cannot simply take the first or next element of
			 * tc_this_pthread_domain->immediate - we've given up the lock, and so the queue
			 * might be *anything*. We have to start afresh. */
			goto search_loop;
			continue;
		default:
			msg_exit(1, "Wrong e->flags in immediate list\n");
		}
	}
	spin_unlock(&common.immediate.lock);

	return 0;
}

static void run_immediate()
{
	int did;
	did = 1;
	while (did) {
		did = _run_immediate(worker.nr);
		did = _run_immediate(FREE_WORKER) || did;
		did = _run_immediate(ANY_WORKER) || did;
	}
}

static void rearm_immediate()
{
	eventfd_t c;

	if (read(tc_this_pthread_domain->immediate_fd, &c, sizeof(c)) != sizeof(c))
		msg_exit(1, "read() failed with %m\n");

	arm_immediate(EPOLL_CTL_MOD);
}

static void _iwi_immediate(struct tc_domain *dom)
{
	eventfd_t c = 1;

	if (write(dom->immediate_fd, &c, sizeof(c)) != sizeof(c))
		msg_exit(1, "write() failed with: %m\n");
}

static void iwi_immediate(struct tc_domain *dom)
{
	/* Some other worker should please process the queued immediate events. */
	if (!CLIST_EMPTY(&dom->sleeping_workers)) {
		_iwi_immediate(dom);
	}
}

int tc_sched_yield()
{
	struct tc_thread *tc = tc_current();
	struct event e;
	int ret;

	ret = RV_OK;
	tc_event_init(&e);
	_tc_event_on_tc_stack(&e, tc);
	add_event_cr(&e, 0, EF_READY, tc);
	tc_scheduler();
	if (worker.woken_by_event != &e)
	{
		ret = RV_INTR;
		remove_event(&e);
	}
	else
		/* May not be accessed anymore. */
		worker.woken_by_event = NULL;
	tc->event_stack = e.next_in_stack;

	return ret;
}

void tc_scheduler(void)
{
	struct event *e, *sig;
	struct tc_thread *tc = tc_current();

	spin_lock(&tc->pending.lock);
	if (!CIRCLEQ_EMPTY(&tc->pending.events)) {
		if (!tc->event_stack)
			e = CIRCLEQ_FIRST(&tc->pending.events);
		else {
			sig = NULL;
			CIRCLEQ_FOREACH(e, &tc->pending.events, e_chain) {
				if (e == tc->event_stack) {
					goto return_this;
				}
				if (e->flags == EF_SIGNAL)
					sig = e;
			}

			/* Wanted event not found; signal might still be returned. */
			if (!sig)
				goto wait_for_another;

			e = sig;
		}

return_this:
		_remove_event(e, &tc->pending);
		spin_unlock(&tc->pending.lock);
		if (e->flags == EF_SIGNAL)
			_signal_gets_delivered(e);
		worker.woken_by_tcfd  = e->tcfd;
		worker.woken_by_event = e;
		return;
	}

wait_for_another:
	tc->flags &= ~TF_RUNNING;
	spin_unlock(&tc->pending.lock);

#ifdef WAIT_DEBUG
	tc->sleep_file = _caller_file;
	tc->sleep_line = _caller_line;
#endif

	_switch_to(&worker.sched_p2); /* always -> scheduler_part2()*/
}


#define AIOs_AT_ONCE (8)
static inline void handle_aio_event()
{
	int e2read;
	int rv;
	struct io_event ioe[AIOs_AT_ONCE];
	eventfd_t count;
	struct timespec ts;
	struct tc_aio_data *ad;
	struct tc_waitq *wq;


	if (read(tc_this_pthread_domain->aio_eventfd, &count, sizeof(count)) != sizeof(count))
		msg_exit(1, "read() failed with %m\n");

	ts.tv_sec = ts.tv_nsec = 0;
	while (count > 0) {
		e2read = count > AIOs_AT_ONCE ? AIOs_AT_ONCE : count;
		rv = io_getevents(tc_this_pthread_domain->aio_ctx, e2read, AIOs_AT_ONCE, ioe, &ts);
		if (rv < 0)
			msg_exit(1, "io_getevents failed with %m\n");


		while (rv>0) {
			rv--;
			ad = ioe[rv].data;

			ad->res = ioe[rv].res;
			ad->res2 = ioe[rv].res2;
			/* Order important? Other way than in tc_aio_wait()? */
			wq = ad->notify;
			tc_aio_set_data_done(ad);
//			ad->notify = NULL; ??

			if (wq)
				tc_waitq_wakeup_all(wq);
		}

		count -= e2read;
	}

	arm_aio_efd(EPOLL_CTL_MOD);
}


static void scheduler_part2()
{
	struct epoll_event epe;
	struct tc_fd *tcfd;
	struct event *e;
	struct tc_thread *tc;
	int er;


	tc = (struct tc_thread *)cr_uptr(cr_caller());
	spin_unlock(&tc->running);

	/* The own stack frame is needed, because we have multiple workers. If we would
	   sleep in the stack frame of the caller of tc_scheduler(), the stack frame
	   would become active twice, when it gets woken up on a different worker */

	while(1) {
		run_immediate();

		worker_prepare_sleep();
		while (1) {
			er = epoll_wait(tc_this_pthread_domain->efd, &epe, 1, -1);
			if (er >= 0)
				break; /* There's something to handle */
			if (errno == EINTR) {
				if (worker.must_sync) {
					/* Sync necessary */
					epe.data.ptr = IWI_SYNC;
					break;
				}
				/* Else continue loop */
			}
			else
				msg_exit(1, "epoll_wait() failed with: %m\n");
		}


		switch ((long)epe.data.ptr) {
		case IWI_SYNC:
			worker_after_sleep();
			continue;
		case IWI_ASYNC_IO:
			handle_aio_event();
			worker_after_sleep();
			continue;
		case IWI_IMMEDIATE:
			worker_after_sleep();
			rearm_immediate();
			/* run_immediate(); at top of loop. */
			continue;
		}


		tcfd = epe.data.ptr;

		spin_lock(&tcfd->events.lock);

		if (atomic_read(&tcfd->err_hup))
		{
			/* Already as invalid marked - should get unregistered soon.
			 * Just wake up all waiters. */
			e = wakeup_all_events(&tcfd->events.events);
		}
		else
		/* in case of an error condition, wake all waiters on the FD,
		   no matter what they are waiting for */
		if (epe.events & (EPOLLERR | EPOLLHUP)) {
			atomic_set(&tcfd->err_hup, 1);
			e = wakeup_all_events(&tcfd->events.events);
		} else {
			e = matching_event(epe.events, &tcfd->events.events);
			/* If there are still waiting threads, we have to make sure that
			 * the kernels event mask matches the needed one.
			 *
			 * If two threads are waiting on EPOLLIN, and one thread gets
			 * notified, the other one will not get woken up.
			 * And telling "but the first thread didn't call tc_rearm()" is not
			 * a valid argument - a third thread might have interfered with
			 * EPOLLOUT in the middle.
			 *
			 * So the conclusion is - the tc_threads have to use locking
			 * around fd-handling code; hoping to have
			 *     tc_wait_fd() to tc_rearm()
			 * atomic doesn't work. */
			/* Even if there was no event waiting, the kernel won't notify us
			 * again, so we have to remember that we have to be subscribe again. */
			tcfd->ep_events = 0;
			/* At least the returned event might be left on the event list;
			 * so we cannot simply use CIRCLEQ_EMPTY(). */
			/* If empty, don't rearm; but: if only one event left, the one we
			 * just found, don't rearm, as this is being alerted. */
			if (!CIRCLEQ_EMPTY(&tcfd->events.events) &&
					(CIRCLEQ_FIRST(&tcfd->events.events) != e ||
					 CIRCLEQ_LAST(&tcfd->events.events) != e))
			{
				er = arm(tcfd);
				if (er)
					msg("cannot re-arm fd %u (tcfd %p)\n", tcfd->fd, tcfd);
			}
		}
		if (!e) {
			/* That can happen if a fd was enabled by a call to tc_wait_fd(),
			   that was interrupted by a tc_signal later. Then an event on the
			   fd happened, be no tc_library thread waits for that event any
			   longer. No need to worry, since we use EPOLLONESHOT always,
			   we can simply return to epoll_wait() */

			if (epe.events & EPOLLERR || epe.events & EPOLLHUP) {
				/* EPOLLERR and EPOLLHUP do not obey the single shot
				   semantics. Need to remove an FD with an HUP or ERR
				   condition immediately. Since this might be done by
				   all workers concurrently, ignore failures here.*/
				epoll_ctl(tc_this_pthread_domain->efd, EPOLL_CTL_DEL, tcfd->fd, NULL);
			}

			spin_unlock(&tcfd->events.lock);
			worker_after_sleep();
			continue;
		}

		worker.woken_by_tcfd = tcfd;
		_remove_event(e, &tcfd->events);
		tc = run_or_queue(e);

		spin_unlock(&tcfd->events.lock);

		worker_after_sleep();

		if (tc)
			switch_to(tc);
	}
}

void tc_worker_init(void)
{
	cpu_set_t cpu_mask;
	int cpus_seen = 0, ci, my_cpu, rv = 0;
	int i;

	/* From tc_run() for main thread, but not twice
	 * (from tc_new_domain()) */
	if (worker.is_init)
		return;

	i = atomic_inc(&common.pthread_counter) - 1;

	cr_init();

	my_cpu = i % CPU_COUNT(&common.available_cpus);
	CPU_ZERO(&cpu_mask);

	for (ci = 0; ci < CPU_SETSIZE; ci++) {
		if (CPU_ISSET(ci, &common.available_cpus)) {
			if (cpus_seen == my_cpu) {
				CPU_SET(ci, &cpu_mask);
				goto found_cpu;
			}
			cpus_seen++;
		}
	}
	msg_exit(1, "could not find my CPU\n", i);

found_cpu:
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_mask), &cpu_mask))
		msg_exit(1, "sched_setaffinity(%d): %m\n", i);

	worker.nr = i;
	rv |= asprintf(&worker.main_thread.name, "main_thread_%d", i);
	worker.main_thread.domain = tc_this_pthread_domain;
	worker.main_thread.cr = cr_current();
	cr_set_uptr(cr_current(), &worker.main_thread);
	tc_waitq_init(&worker.main_thread.exit_waiters);
	atomic_set(&worker.main_thread.refcnt, 0);
	spin_lock_init(&worker.main_thread.running);
	spin_lock(&worker.main_thread.running); /* runs currently */
	worker.main_thread.flags = TF_RUNNING;
	event_list_init(&worker.main_thread.pending);
	worker.main_thread.worker_nr = i;
	/* LIST_INSERT_HEAD(&tc_this_pthread_domain->threads, &worker.main_thread, tc_chain); */
	worker.tid = syscall(__NR_gettid);

	rv |= asprintf(&worker.sched_p2.name, "sched_%d", worker.tid);
	if (rv == -1)
		msg_exit(1, "allocation in asprintf() failed\n");
	worker.sched_p2.cr = cr_create(scheduler_part2, NULL, NULL, DEFAULT_STACK_SIZE);
	if (!worker.sched_p2.cr)
		msg_exit(1, "allocation of worker.sched_p2 failed\n");

	cr_set_uptr(worker.sched_p2.cr, &worker.sched_p2);
	tc_waitq_init(&worker.sched_p2.exit_waiters);
	atomic_set(&worker.sched_p2.refcnt, 0);
	spin_lock_init(&worker.sched_p2.running);
	worker.sched_p2.flags = 0;
	event_list_init(&worker.sched_p2.pending);
	worker.sched_p2.worker_nr = i;
	worker.must_sync = 0;
	worker.is_on_sleeping_list = 0;
	worker.is_init = 1;

	spin_lock(&tc_this_pthread_domain->worker_list_lock);
	LIST_INSERT_HEAD(&tc_this_pthread_domain->worker_list, &worker, worker_chain);
	spin_unlock(&tc_this_pthread_domain->worker_list_lock);
	setpriority(PRIO_PROCESS, worker.tid, tc_this_pthread_domain->nice_level);


}

static void ignore_signal(int sig)
{
	/* Just needed to get out of epoll_wait() */
}


void tc_init()
{
	int fd;


	LIST_INIT(&tc_this_pthread_domain->threads.list);
	spin_lock_init(&tc_this_pthread_domain->lock);
	if (SIGNAL_FOR_WAKEUP > SIGRTMAX)
		msg_exit(1, "libTCR: bad value for SIGNAL_FOR_WAKEUP\n");

	signal(SIGNAL_FOR_WAKEUP, ignore_signal);

	spin_lock_init(&tc_this_pthread_domain->sync_lock);
	atomic_set(&tc_this_pthread_domain->sync_barrier, 0);
	CLIST_INIT(&tc_this_pthread_domain->sleeping_workers);

	tc_this_pthread_domain->efd = epoll_create(1);
	if (tc_this_pthread_domain->efd < 0)
		msg_exit(1, "epoll_create failed with %m\n");

	spin_lock_init(&tc_this_pthread_domain->timer_lock);
	atomic_set(&tc_this_pthread_domain->timer_sleepers, 0);
	tc_mutex_init(&tc_this_pthread_domain->timer_mutex);
	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC ); //| TFD_NONBLOCK);
	if (fd == -1)
		msg_exit(1, "timerfd_create with %m\n");
	_tc_fd_init(&tc_this_pthread_domain->timer_tcfd, fd);

	tc_this_pthread_domain->immediate_fd = eventfd(0, 0);
	tc_this_pthread_domain->aio_eventfd = eventfd(0, 0);
	if (tc_this_pthread_domain->immediate_fd == -1 ||
			tc_this_pthread_domain->aio_eventfd == -1)
		msg_exit(1, "eventfd() failed with: %m\n");

	if (CPU_COUNT(&common.available_cpus) == 0) {
		if (sched_getaffinity(0, sizeof(common.available_cpus), &common.available_cpus))
			msg_exit(1, "sched_getaffinity: %m\n");
	}

	spin_lock_init(&tc_this_pthread_domain->worker_list_lock);
	LIST_INIT(&tc_this_pthread_domain->worker_list);

	arm_immediate(EPOLL_CTL_ADD);
	arm_aio_efd(EPOLL_CTL_ADD);

	tc_this_pthread_domain->aio_ctx = NULL;
}


static void tc_aio_init(void)
{
	int max;

	max = tc_this_pthread_domain->nr_of_workers * 4;
	if (max > 256)
		max = 256;
	if (io_setup(max, &tc_this_pthread_domain->aio_ctx))
		msg_exit(1, "io_setup failed with %m\n");
}


static void *worker_pthread(void *_s)
{
	assert(_s);
	tc_this_pthread_domain = _s;

	tc_worker_init();
	tc_thread_wait_ref(&tc_main); /* calls tc_scheduler() */

	_iwi_immediate(tc_this_pthread_domain); /* All other workers need to get woken UNCONDITIONALLY
			     So that the complete program can terminate */
	return NULL;
}


struct tc_domain *_setup_domain(struct tc_domain *n_d, int nr_of_workers)
{
	pthread_t *threads;
	int avail_cpu;

	WITH_OTHER_DOMAIN_BEGIN(n_d);

	tc_init();
	tc_worker_init();


	avail_cpu = CPU_COUNT(&common.available_cpus);
	if (nr_of_workers <= 0) {
		nr_of_workers = avail_cpu + nr_of_workers;
		if (nr_of_workers < 1)
			nr_of_workers = 1;
		else if (nr_of_workers > avail_cpu)
			nr_of_workers = avail_cpu;
	}
	else if (nr_of_workers > avail_cpu)
		msg("tc_run(): got more workers (%d) than available CPUs (%d)\n",
				nr_of_workers, avail_cpu);

	n_d->nr_of_workers = nr_of_workers;
	n_d->last_thread_id = rand();

	tc_aio_init();


	threads = malloc(sizeof(pthread_t) * nr_of_workers);
	tc_this_pthread_domain->pthreads = threads;
	if (!threads)
		msg_exit(1, "malloc() in tc_run failed\n");

	WITH_OTHER_DOMAIN_END();
	if (tc_this_pthread_domain)
		n_d->stack_size = tc_this_pthread_domain->stack_size;
	return n_d;
}

static void start_pthreads(struct tc_domain *n_d, int reuse_pthread)
{
	int i;

	/* The initial pthread is used, too; but on starting a new domain no
	 * thread can be "recycled". */
	if (reuse_pthread) {
		n_d->pthreads[0] = pthread_self(); /* actually unused */
		i = 1;
	}
	else
		i = 0;

	for (; i < n_d->nr_of_workers; i++)
		pthread_create(n_d->pthreads + i, NULL, worker_pthread, n_d);
}

struct tc_domain *tc_new_domain(int nr_of_workers)
{
	struct tc_domain *n_d;

	n_d = NULL;
	new_domain(&n_d);

	_setup_domain(n_d, nr_of_workers);

	/* Is this racy? */
	SLIST_INSERT_AFTER(tc_this_pthread_domain, n_d, domain_chain);

	start_pthreads(n_d, 0);
	return n_d;
}

void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers)
{
	int i;
	struct tc_domain *n_d;


	event_list_init(&common.immediate);

	new_domain(&n_d);
	_setup_domain(n_d, nr_of_workers);

	/* There's no SLIST_INIT_ENTRY(). */
	n_d->domain_chain.sle_next = n_d;

	/* New scheduler domain becomes "default" */
	tc_this_pthread_domain = n_d;

	tc_worker_init();
	tc_main = tc_thread_new_ref(func, data, name);

	start_pthreads(n_d, 1);

	tc_thread_wait_ref(&tc_main); /* calls tc_scheduler() */

	for (i = 1; i < nr_of_workers; i++)
		pthread_join(n_d->pthreads[i], NULL);
}


int tc_thread_count(void)
{
	return tc_this_pthread_domain->nr_of_workers;
}


enum tc_rv tc_rearm(struct tc_fd *the_tc_fd)
{
	int rv;

	spin_lock(&the_tc_fd->events.lock);
	rv = arm(the_tc_fd);
	spin_unlock(&the_tc_fd->events.lock);
	return rv ? RV_FAILED : RV_OK;
}

enum tc_rv _tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd, enum tc_event_flag ef)
{
	struct event e;
	struct tc_thread *tc = tc_current();
	int r;

	if (add_event_fd(&e, ep_events | EPOLLONESHOT, ef, tcfd))
		return RV_FAILED;
	_tc_event_on_tc_stack(&e, tc);
	tc_scheduler();
	r = (worker.woken_by_event != &e);
	worker.woken_by_event = NULL;
	tc->event_stack = e.next_in_stack;
	/* Have to reprogram either way. */
	tcfd->ep_events = 0;
	if (r) {
		remove_event_fd(&e, tcfd);
		return RV_INTR;
	}
	return atomic_read(&tcfd->err_hup) ? RV_FAILED : RV_OK;
}

void tc_die()
{
	struct tc_thread *tc = tc_current();

	/* printf(" (%d) exiting: %s\n", worker.nr, tc->name); */

	spin_lock(&tc_this_pthread_domain->lock);
	LIST_REMOVE(tc, tc_chain);
	if (tc->flags & TF_THREADS)
		LIST_REMOVE(tc, threads_chain);
	spin_unlock(&tc_this_pthread_domain->lock);

	tc_aio_wait();

	if (atomic_read(&tc->refcnt) > 0) {
		signal_cancel_pending();
		if (atomic_read(&tc->refcnt) > 0) {
			msg_exit(1, "tc_die(%p, %s): refcnt = %d. Signals still enabled?\n",
					tc, tc->name, atomic_read(&tc->refcnt));
		}
	}

	tc_waitq_wakeup_all(&tc->exit_waiters);

	add_event_cr(&tc->e, 0, EF_EXITING, tc);  /* The scheduler will free me */
	iwi_immediate(tc_this_pthread_domain);
	_switch_to(&worker.sched_p2); /* like tc_scheduler(); but avoids deadlocks */
	msg_exit(1, "tc_scheduler() returned in tc_die() [flags = %d]\n", &tc->flags);
}

void tc_setup(void *arg1, void *arg2)
{
	struct tc_thread *previous = (struct tc_thread *)cr_uptr(cr_caller());
	void (*func)(void *) = arg1;

	spin_unlock(&previous->running);

	worker.woken_by_event = NULL;

	func(arg2);

	tc_die();
}

static struct tc_thread *_tc_thread_setup(void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;
	int i;

	tc = malloc(sizeof(struct tc_thread));
	if (!tc)
		goto fail2;

	memset(tc, 0, sizeof(*tc));

	tc->cr = cr_create(tc_setup, func, data, tc_this_pthread_domain->stack_size);
	if (!tc->cr)
		goto fail3;

	cr_set_uptr(tc->cr, (void *)tc);
	tc->name = name;
	if (cr_current() && tc_current())
		tc->per_thread_data = tc_thread_var_get();
	tc_waitq_init(&tc->exit_waiters);
	atomic_set(&tc->refcnt, 0);
	spin_lock_init(&tc->running);
	tc->flags = 0;
	tc_event_init(&tc->e);
	event_list_init(&tc->pending);
	tc->worker_nr = FREE_WORKER;
	atomic_set(&tc->signals_since_last_query, 0);
	tc->event_stack = NULL;
	tc->domain = tc_this_pthread_domain;
	tc->last_worker = &worker;

	for(i=0; i<TC_AIO_REQUESTS_PER_TC_THREAD; i++)
		tc_aio_data_init(tc->aio + i);
	tc_waitq_init(&tc->aio_wq);

	return tc;

fail3:
	free(tc);
fail2:
	return NULL;
}

static void _tc_thread_insert(struct tc_thread *tc)
{
	struct tc_domain *d;

	d = tc_this_pthread_domain;
	spin_lock(&d->lock);
	d->threads.domain = d;
	LIST_INSERT_HEAD(&d->threads.list, tc, tc_chain);
	d->last_thread_id++;
	if (d->last_thread_id == 0)
		d->last_thread_id = 1;
	tc->id = d->last_thread_id;
	spin_unlock(&d->lock);
}


static struct tc_thread *_tc_thread_new(void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;

	tc = _tc_thread_setup(func, data, name);
	if (tc)
		_tc_thread_insert(tc);
	return tc;
}


struct tc_thread *tc_thread_new(void (*func)(void *), void *data, char* name)
{
	return tc_thread_new_ref(func, data, name).thr;
}

struct tc_thread_ref tc_thread_new_ref_in_domain(void (*func)(void *), void *data, char* name, struct tc_domain *domain)
{
	struct tc_thread_ref t = { 0 };

	WITH_OTHER_DOMAIN_BEGIN(domain);
	t.thr = _tc_thread_new(func, data, name);
	t.domain = domain;

	if (t.thr) {
		t.id = t.thr->id;
		add_event_cr(&t.thr->e, 0, EF_READY, t.thr);
		iwi_immediate(tc_this_pthread_domain);
	}

	WITH_OTHER_DOMAIN_END();
	return t;
}

struct tc_thread_ref tc_thread_new_ref(void (*func)(void *), void *data, char* name)
{
	return tc_thread_new_ref_in_domain(func, data, name, tc_this_pthread_domain);
}

void tc_thread_pool_new_in_domain(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name, int excess, struct tc_domain *domain)
{
	struct tc_thread *tc;
	int i;
	char *ename;

	WITH_OTHER_DOMAIN_BEGIN(domain);

	threads->domain = domain;
	LIST_INIT(&threads->list);
	for (i = 0; i < domain->nr_of_workers + excess; i++) {
		if (asprintf(&ename, name, i) == -1)
			msg_exit(1, "allocation in asprintf() failed\n");
		tc = _tc_thread_new(func, data, ename);
		if (!tc)
			continue;
		tc->flags |= TF_THREADS | TF_FREE_NAME;
		if (i < domain->nr_of_workers) {
			tc->worker_nr = i;
			tc->flags |= TF_AFFINE;
		}
		spin_lock(&tc_this_pthread_domain->lock);
		LIST_INSERT_HEAD(&threads->list, tc, threads_chain);
		spin_unlock(&tc_this_pthread_domain->lock);
		add_event_cr(&tc->e, 0, EF_READY, tc);
	}
	iwi_immediate(domain);

	WITH_OTHER_DOMAIN_END();
}

void tc_thread_pool_new(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name, int excess)
{
	tc_thread_pool_new_in_domain(threads, func, data, name, excess, tc_this_pthread_domain);
}

enum tc_rv tc_thread_pool_wait(struct tc_thread_pool *threads)
{
	struct tc_thread *tc;
	enum tc_rv r, rv = RV_THREAD_NA;
	struct tc_domain *d;

	d = threads->domain;

	spin_lock(&d->lock);
	while ((tc = LIST_FIRST(&threads->list))) {
		spin_unlock(&d->lock);
		r = tc_thread_wait(tc);
		switch(r) {
		case RV_INTR:
			return r;
		case RV_OK:
			if (rv == RV_THREAD_NA)
		case RV_FAILED:
				rv = r;
		case RV_THREAD_NA:
			break;
		}
		spin_lock(&d->lock);
	}
	spin_unlock(&d->lock);

	return RV_OK;
}


/* There might be races between making the tcfd, and closing the file.
 * Just exit()ing the program isn't nice, so we simply set the tcfd as dead. */
static void _tc_fd_init(struct tc_fd *tcfd, int fd)
{
	struct epoll_event epe;
	int arg;
	char *err;

	tcfd->fd = fd;
	tcfd->free_list_next = NULL;
	event_list_init(&tcfd->events);
	tcfd->ep_events = 0;
	atomic_set(&tcfd->err_hup, 0);

	/* The fd has to be non blocking */
	arg = fcntl(fd, F_GETFL, NULL);
	if (arg < 0)
		goto fcntl_err;

	arg |= O_NONBLOCK;

	if (fcntl(fd, F_SETFL, arg) < 0)
		goto fcntl_err;

	epe.events = 0;

	if (tcfd_epoll_ctl(EPOLL_CTL_ADD, tcfd, &epe) == 0)
		return;

	err = "epoll_ctl failed with %m\n";
	goto invalid;

fcntl_err:
		err = "fcntl() failed: %m\n";

invalid:
	atomic_set(&tcfd->err_hup, 1);
	/* We process the message last, so that the fd is dead as soon as possible.
	 * */
	msg(err);
	return;
}

struct tc_fd *tc_register_fd(int fd)
{
	struct tc_fd * tcfd;

	tcfd = malloc(sizeof(struct tc_fd));
	if (!tcfd)
		msg_exit(1, "malloc() failed with: %m\n");

	_tc_fd_init(tcfd, fd);

	return tcfd;
}

static void _tc_fd_free(struct tc_fd *tcfd)
{
	memset(tcfd, 0xbb, sizeof(*tcfd));
	free(tcfd);
}

static void _tc_fd_unregister(struct tc_fd *tcfd, int free_later)
{
	struct epoll_event epe = { };

	/* Make this tcfd not appear again in the epoll_wait loop.
	 * We're ignoring the return value of epoll_ctl() here on intention. */
	epoll_ctl(tc_this_pthread_domain->efd, EPOLL_CTL_DEL, tcfd->fd, &epe);

	spin_lock(&tcfd->events.lock);
	/* Make the fd invalid for further accesses */
	atomic_set(&tcfd->err_hup, 1);
	if (!CIRCLEQ_EMPTY(&tcfd->events.events))
		msg_exit(1, "event list not empty in tc_unregister_fd()\n");
	spin_unlock(&tcfd->events.lock);

	if (free_later)
		store_for_later_free(tcfd);
	else
		_tc_fd_free(tcfd);
}

void tc_unregister_fd(struct tc_fd *tcfd)
{
	_tc_fd_unregister(tcfd, 1);
}

static void _process_free_list(spinlock_t *lock_to_free)
{
	struct tc_fd *cur,*next;

	cur = tc_this_pthread_domain->free_list;
	tc_this_pthread_domain->free_list = NULL;
	spin_unlock(lock_to_free);

	while (cur) {
		next = cur->free_list_next;
		_tc_fd_free(cur);
		cur = next;
	}
}

static void worker_prepare_sleep()
{
	spin_lock(&tc_this_pthread_domain->sync_lock);
	if (!worker.is_on_sleeping_list) {
		CLIST_INSERT_AFTER(&tc_this_pthread_domain->sleeping_workers, &worker.sleeping_chain);
	}
	worker.is_on_sleeping_list = 1;
	spin_unlock(&tc_this_pthread_domain->sync_lock);
}

static void worker_after_sleep()
{
	int new;
	int have_lock;

	/* These two checks have to made atomically w.r.t. sync_lock. */
	spin_lock(&tc_this_pthread_domain->sync_lock);
	have_lock = 1;
	if (worker.is_on_sleeping_list)
	{
		CLIST_REMOVE(&worker.sleeping_chain);
		worker.is_on_sleeping_list = 0;
	}
	if (worker.must_sync) {
		worker.must_sync = 0;
		new = atomic_dec(&tc_this_pthread_domain->sync_barrier);
		if (new == 0)
		{
			_process_free_list(&tc_this_pthread_domain->sync_lock);
			have_lock = 0;
		}
	}

	if (have_lock)
		spin_unlock(&tc_this_pthread_domain->sync_lock);
}


/* When removing a FD that might fire it is essential to make sure
 * that we do not get any events of that FD in, after this point,
 * since we want to delete the data structure describing that FD.
 *
 * _tc_fd_unregister() and add_event_fd() make sure that no events are defined
 * for this fd, and that no new events can be registered anymore. So all
 * tc_threads that are currently busy cannot make use of it anymore, and are
 * therefore safe.
 * (If they still access the tcfd, racing with it's destruction by
 * tc_unregister_fd(), it's their fault.)
 *
 * Only the currently sleeping threads have to be told that this tcfd is no
 * longer valid. We try to wake up all of them at once (via an IWI_SYNC event),
 * and the last one free()s the memory. */
static void store_for_later_free(struct tc_fd *tcfd)
{
	struct clist_entry *list;
	struct worker_struct *w;


	spin_lock(&tc_this_pthread_domain->sync_lock);
	tcfd->free_list_next = tc_this_pthread_domain->free_list;
	tc_this_pthread_domain->free_list = tcfd;

	if (CLIST_EMPTY(&tc_this_pthread_domain->sleeping_workers)) {
		/* No new sleepers. */
	}
	else {
		/* Process the sleeper-list. */
		list = tc_this_pthread_domain->sleeping_workers.cl_next;
		while (list != &tc_this_pthread_domain->sleeping_workers) {
			w = container_of(list, struct worker_struct, sleeping_chain);
			/* When a synchronize_world() is run while a thread waits for the lock
			 * in worker_prepare_sleep(), then the worker would be on the
			 * sleeping_workers list again.
			 * If the next function is a synchronize_world() again (likely in
			 * leak_test2), then this would try to get the thread again ... */
			if (!w->must_sync) {
				w->must_sync = 1;
				atomic_inc(&tc_this_pthread_domain->sync_barrier);
				w->is_on_sleeping_list = 0;
				tgkill(getpid(), w->tid, SIGNAL_FOR_WAKEUP);
			}
			list = list->cl_next;
		}
		CLIST_INIT(&tc_this_pthread_domain->sleeping_workers);
	}

	if (atomic_read(&tc_this_pthread_domain->sync_barrier)) {
		spin_unlock(&tc_this_pthread_domain->sync_lock);
		/* See comment in worker_after_sleep */
	}
	else
		_process_free_list(&tc_this_pthread_domain->sync_lock);
}



void tc_mutex_init(struct tc_mutex *m)
{
	assert( sizeof(m->owner) == sizeof(m->a_owner));
	m->owner = NULL;
	tc_waitq_init(&m->wq);
}

enum tc_rv tc_mutex_lock(struct tc_mutex *m)
{
	int rv;
	struct tc_thread *me = tc_current();
	long me_l = (long)me;

	if (m->owner == me)
		msg_exit(1, "mutex %p recursively locked in thread %p\n", m, me);


	rv = RV_OK;
	if (atomic_set_if_eq(me_l, 0, &m->a_owner)) {
		goto got_it;
	}

	rv = tc_waitq_wait_event( &m->wq,
			({ 
			 /* Given by last owner */
			 (m->owner == me) ||
			 /* or got free */
			 atomic_set_if_eq((long)tc_current(), 0, &m->a_owner);
			 }) );

got_it:
	return rv;
}

void tc_mutex_unlock(struct tc_mutex *m)
{
	/* We cannot be sure that tc_current() == m->owner
	 * because of lock passing. */

	if (!m->owner)
		msg_exit(1, "tc_mutex_unlocked() called on an unlocked mutex\n");

	tc_waitq_wakeup_one_owner(&m->wq, &m->a_owner);
}

enum tc_rv tc_mutex_trylock(struct tc_mutex *m)
{
	if (atomic_set_if_eq((long)tc_current(), 0, &m->a_owner)) {
		return RV_OK;
	}

	return RV_FAILED;
}

void tc_mutex_destroy(struct tc_mutex *m)
{
	tc_waitq_unregister(&m->wq);
}


static enum tc_rv _thread_valid(struct tc_domain *domain, struct tc_thread *look_for)
{
	struct tc_thread *tc;

	LIST_FOREACH(tc, &domain->threads.list, tc_chain) {
		if (tc == look_for)
			return RV_OK;
	}
	return RV_THREAD_NA;
}

enum tc_rv tc_thread_wait_ref(struct tc_thread_ref *_ref)
{
	struct tc_thread_ref ref = *_ref;
	struct tc_domain *d;
	struct event e;
	enum tc_rv rv;

	if (!ref.thr)
		return RV_OK;

	d = ref.domain;
	tc_event_init(&e);
	spin_lock(&d->lock);
	rv = _thread_valid(d, ref.thr);  /* might have already exited */
	if (rv == RV_OK) {
		if (ref.id && ref.thr->id != ref.id)
			rv = RV_THREAD_NA;
		else {
			tc_waitq_prepare_to_wait(&ref.thr->exit_waiters, &e);
		}
	}

	spin_unlock(&d->lock);
	ref.thr = NULL;
	if (rv == RV_THREAD_NA)
		return rv;

	tc_scheduler();

	/* Do not pass wait_for->exit_waiters, since wait_for might be already freed. */
	if (tc_waitq_finish_wait(NULL, &e))
		rv = RV_INTR;

	return rv;
}

void tc_waitq_init(struct tc_waitq *wq)
{
	event_list_init(&wq->waiters);
}

static void _tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e, struct tc_thread *tc)
{
	spin_lock(&wq->waiters.lock);
	_add_event(e, &wq->waiters, tc);
	spin_unlock(&wq->waiters.lock);

	if (e->flags != EF_SIGNAL)
		_tc_event_on_tc_stack(e, tc);
}

void tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	e->ep_events = 0; /* unused */
	e->flags = EF_READY;
	_tc_waitq_prepare_to_wait(wq, e, tc_current());
}

int tc_waitq_finish_wait(struct tc_waitq *wq, struct event *e)
{
	int was_on_top, was_active;
	struct tc_thread *tc = tc_current();


	/* We need to hold the immediate lock here so that no other thread might
	 * take the event and put it on the pending queue while we're trying to
	 * free it. */
	spin_lock(&common.immediate.lock);
	spin_lock(&tc->pending.lock);
	was_on_top = tc->event_stack == e;
	if (was_on_top &&
			(!worker.woken_by_event ||
			 worker.woken_by_event == e)) {
		/* Optimal case - the event that got active was the one we expected. */
		tc->event_stack = e->next_in_stack;
		assert((long)tc->event_stack != 0xafafafafafafafaf);

		remove_event_holding_locks(e, &tc->pending, &common.immediate, NULL);
		assert(!e->el);
		spin_unlock(&tc->pending.lock);
		spin_unlock(&common.immediate.lock);
		worker.woken_by_event = NULL;

		return 0;
	}


	/* We need to hold the locks here, so that no other thread can put the
	 * signal event on the immediate queue. */
	assert(worker.woken_by_event->flags == EF_SIGNAL);
	was_active = (e->el == &tc->pending) || (e->el == &common.immediate);
	remove_event_holding_locks(e, &tc->pending, &common.immediate, NULL);
	/* worker.woken_by_event should be on the signal list, and stay
	 * there.  */

	if (was_active) {
		/* We got a signal, but the expected event got active, too.
		 * Requeue the signal and return OK. */
		remove_event_holding_locks(worker.woken_by_event, &tc->pending, &common.immediate, NULL);
		assert(!e->el);
		_add_event(worker.woken_by_event, &tc->pending, tc);
		worker.woken_by_event = NULL;

		tc->event_stack = e->next_in_stack;
		assert((long)tc->event_stack != 0xafafafafafafafaf);
	}

	spin_unlock(&common.immediate.lock);
	spin_unlock(&tc->pending.lock);
	return !was_active;
}

int tc_waitq_wait(struct tc_waitq *wq)
{
	struct event e;
	int rv;

	tc_event_init(&e);
	tc_waitq_prepare_to_wait(wq, &e);
	tc_scheduler();
	rv = tc_waitq_finish_wait(wq, &e);
	return rv;
}

void tc_waitq_wakeup_one_owner(struct tc_waitq *wq, atomic_t *woken)
{
	int wake = 0;
	struct event *e;
	struct tc_domain *dom;

	/* We need to lock the immediate queue first; but it might be the wrong
	 * one. */
	spin_lock(&common.immediate.lock);
	spin_lock(&wq->waiters.lock);
	dom = tc_this_pthread_domain;
	if (CIRCLEQ_EMPTY(&wq->waiters.events)) {
		if (woken)
			atomic_set(woken, 0);
	} else {
		e = CIRCLEQ_FIRST(&wq->waiters.events);
		dom = e->domain;
		CIRCLEQ_REMOVE(&wq->waiters.events, e, e_chain);
		e->el = &common.immediate;
		CIRCLEQ_INSERT_HEAD(&common.immediate.events, e, e_chain);
		wake = 1;
		if (woken)
			atomic_set(woken, (long)e->tc);
	}
	spin_unlock(&wq->waiters.lock);
	spin_unlock(&common.immediate.lock);

	if (wake)
		iwi_immediate(dom);
}

void tc_waitq_wakeup_one(struct tc_waitq *wq)
{
	tc_waitq_wakeup_one_owner(wq, NULL);
}

/* What's a bit ugly is that "elm" could be specified as
 * CIRCLEQ_FIRST(head) - then setting head->cqh_first would
 * destroy the value for elm!
 * This can't be a function (generally), because the types
 * might be _any_ pointers.....
 * I WANT GENSYMS! (and clean macros.) */
#define	CIRCLEQ_CUT_BEFORE(head, elm, new_cq, field) do {	\
		typeof (head) __t_head = head;						\
		typeof (elm) __t_elm  = elm;						\
		typeof (new_cq)  __t_new  = new_cq;					\
		typeof (elm) head_last; /* or head_new_last? */		\
		typeof (elm) new_last; /* or new_cq_last? */		\
		assert(__t_elm);									\
		assert(!CIRCLEQ_EMPTY(__t_head));					\
		assert(__t_elm != (void*)__t_head);					\
		head_last = (__t_elm)->field.cqe_prev;				\
		new_last = __t_head->cqh_last;						\
		/* Operations for the old cq */						\
		__t_head->cqh_last = head_last;						\
		if (head_last == (void*)__t_head)					\
			__t_head->cqh_first = (void*)__t_head;			\
		else												\
			head_last->field.cqe_next = (void*)__t_head;	\
		/* Operations for the new cq */						\
		__t_new->cqh_first = (__t_elm);						\
		__t_new->cqh_last = new_last;						\
		new_last->field.cqe_next = (void*)__t_new;			\
		(__t_elm)->field.cqe_prev = (void*)__t_new;			\
} while (0)


void tc_waitq_wakeup_all(struct tc_waitq *wq)
{
	struct event *e;
	struct tc_domain *alerted[10], *dom;
	int alerted_max, i;

	alerted_max = 0;
	spin_lock(&common.immediate.lock);
	spin_lock(&wq->waiters.lock);
	while(!CIRCLEQ_EMPTY(&wq->waiters.events)) {
		e = CIRCLEQ_FIRST(&wq->waiters.events);
		CIRCLEQ_REMOVE(&wq->waiters.events, e, e_chain);
		e->el = &common.immediate;
		dom = e->domain;
		CIRCLEQ_INSERT_HEAD(&common.immediate.events, e, e_chain);

		/* Would be nice in tc_signal_fire(), but that would
		 * need to traverse the list a second time. */
		if (e->flags == EF_SIGNAL)
			atomic_inc(&e->tc->signals_since_last_query);

		for(i=0; i<alerted_max; i++) {
			if (alerted[i] == dom)
				break;
		}
		if (i == alerted_max) {
			/* If there's space, remember for later alerting.
			 * If there no more space, wakeup immediately -
			 * but that can be multiple times. */
			if (alerted_max < sizeof(alerted)/sizeof(alerted[0]))
				alerted[ alerted_max++ ] = dom;
			else
				_iwi_immediate(dom);
		}
	}
	spin_unlock(&wq->waiters.lock);
	spin_unlock(&common.immediate.lock);

	/* We publish that there's something to be done.
	 * The iwi_immediate() would only wakeup _idle_ workers, so (if there's
	 * none) the event might get lost; the _iwi_immediate() function makes sure
	 * the next epoll_wait() call terminates. */
	for(i=0; i<alerted_max; i++) {
		_iwi_immediate(alerted[i]);
	}
}


int tc_thread_worker_nr(struct tc_thread *tc)
{
	if (!tc)
		tc = tc_current();

	return tc->worker_nr;
}


void tc_waitq_unregister(struct tc_waitq *wq)
{
	if (!CIRCLEQ_EMPTY(&wq->waiters.events))
		msg_exit(1, "there are still waiters in tc_waitq_unregister()");
}

void tc_signal_init(struct tc_signal *s)
{
	tc_waitq_init(&s->wq);
	LIST_INIT(&s->sss);
}

static void _signal_gets_delivered(struct event *e)
{
	_tc_waitq_prepare_to_wait(&e->signal->wq, e, e->tc);
}

struct tc_signal_sub *tc_signal_subscribe_exist(struct tc_signal *s, struct tc_signal_sub *ss)
{
	/* First set the whole signal data correctly, then insert into event lists.
	 * tc gets set by _add_event only. */
	ss->event.signal = s;
	ss->event.flags = EF_SIGNAL;
	tc_event_init(&ss->event);
	_tc_waitq_prepare_to_wait(&s->wq, &ss->event, tc_current());

	spin_lock(&s->wq.waiters.lock);
	LIST_INSERT_HEAD(&s->sss, ss, se_chain);
	spin_unlock(&s->wq.waiters.lock);

	return ss;
}

struct tc_signal_sub *tc_signal_subscribe(struct tc_signal *s)
{
	struct tc_signal_sub *ss;

	ss = malloc(sizeof(struct tc_signal_sub));
	if (!ss)
		msg_exit(1, "malloc of tc_signal_sub failed in tc_signal_subscribe\n");

	return tc_signal_subscribe_exist(s, ss);
}


/* Signals have one ugly point - they can be called anytime, anywhere, even
 * while we're trying to unsubscribe.
 * So we have to hold a number of locks to keep the tc_signal_sub event within
 * our reach during unsubscribe etc. */
void tc_signal_unsubscribe_nofree(struct tc_signal *s, struct tc_signal_sub *ss)
{
	struct tc_thread *tc = ss->event.tc;

	/* The event might be added to tc_this_pthread_domain->immediate or tc->pending by some
	 * wakeup_all call while we're waiting for the signals' wq lock, so we have
	 * to remove it from there first.  */
	spin_lock(&common.immediate.lock);
	spin_lock(&s->wq.waiters.lock);
	spin_lock(&tc->pending.lock);
	LIST_REMOVE(ss, se_chain);
	remove_event_holding_locks(&ss->event, &s->wq.waiters, &tc->pending, &common.immediate, NULL);
	spin_unlock(&s->wq.waiters.lock);
	spin_unlock(&tc->pending.lock);
	spin_unlock(&common.immediate.lock);
}

void tc_signal_unsubscribe(struct tc_signal *s, struct tc_signal_sub *ss)
{
	tc_signal_unsubscribe_nofree(s, ss);
	free(ss);
}

static void _cancel_signal(struct event *e, struct event_list *el)
{
	struct tc_signal_sub *ss;

	_remove_event(e, el);
	ss = container_of(e, struct tc_signal_sub, event);
	spin_lock(&e->signal->wq.waiters.lock);
	LIST_REMOVE(ss, se_chain);
	spin_unlock(&e->signal->wq.waiters.lock);
	free(ss);
}

static void signal_cancel_pending()
{
	struct event *e;
	struct tc_thread *tc = tc_current();

	spin_lock(&common.immediate.lock);
	CIRCLEQ_FOREACH(e, &common.immediate.events, e_chain) {
		if (e->tc == tc && e->flags == EF_SIGNAL)
			_cancel_signal(e, &common.immediate);
	}
	spin_unlock(&common.immediate.lock);

	spin_lock(&tc->pending.lock);
	CIRCLEQ_FOREACH(e, &tc->pending.events, e_chain) {
		if (e->flags == EF_SIGNAL)
			_cancel_signal(e, &tc->pending);
	}
	spin_unlock(&tc->pending.lock);
}

void tc_signal_destroy(struct tc_signal *s)
{
	struct tc_signal_sub *ss;

	spin_lock(&s->wq.waiters.lock);
	LIST_FOREACH(ss, &s->sss, se_chain) {
		remove_event(&ss->event);
		LIST_REMOVE(ss, se_chain);
	}
	spin_unlock(&s->wq.waiters.lock);

	tc_waitq_unregister(&s->wq);
}

void tc_signal_fire(struct tc_signal *s)
{
	tc_waitq_wakeup_all(&s->wq);
}


void tc_renice_domain(struct tc_domain *d, int new_nice)
{
	struct worker_struct *w;

	d->nice_level = new_nice;
	spin_lock(&d->worker_list_lock);
	LIST_FOREACH(w, &d->worker_list, worker_chain) {
		/* syscall within spinlock isn't nice --
		 * but that should be fast enough. */
		setpriority(PRIO_PROCESS, w->tid, new_nice);
	}
	spin_unlock(&d->worker_list_lock);
}


inline static int compare_ts_times(struct timespec *ts1, struct timespec *ts2)
{
	if (ts1->tv_sec < ts2->tv_sec)
		return -1;
	if (ts1->tv_sec > ts2->tv_sec)
		return +1;

	if (ts1->tv_nsec < ts2->tv_nsec)
		return -1;
	if (ts1->tv_nsec > ts2->tv_nsec)
		return +1;

	return 0;
}


static void remove_from_timer_list_wakeup(struct timer_waiter *to_remove)
{
	struct timer_waiter *tw;

	spin_lock(&tc_this_pthread_domain->timer_lock);

	if (LIST_NEXT(to_remove, tl) != NOT_ON_TIMERLIST) {
		LIST_REMOVE(to_remove, tl);
	}

	/* It might be nicer to use the longest-sleeping thread as new master,
	 * but we'd have to traverse the list or store the last element ...
	 * TODO */
	tw = LIST_FIRST(&tc_this_pthread_domain->timer_list);
	if (tw)
		tc_waitq_wakeup_all(&tw->wq);

	spin_unlock(&tc_this_pthread_domain->timer_lock);

	return;
}


static inline int timer_delta(struct timespec *dest, struct timespec *later, struct timespec *earlier)
{
	dest->tv_sec  = later->tv_sec  - earlier->tv_sec;
	dest->tv_nsec = later->tv_nsec - earlier->tv_nsec;
	while (dest->tv_nsec < 0) {
		dest->tv_sec--;
		dest->tv_nsec += 1e9;
	}
	if (dest->tv_sec < 0 ||
			(dest->tv_sec == 0 && dest->tv_nsec <= 0))
		return 1;
	return 0;
}


/* Should hold timer_lock */
static int timerfd_reprogram_valid(struct timespec *ts)
{
	struct itimerspec its;
	struct timespec ts2, delta;
	int make_abs;


	if (!ts) {
		ts = &ts2;
		clock_gettime(CLOCK_MONOTONIC, ts);
	}

	if (timer_delta(&delta, & LIST_FIRST(&tc_this_pthread_domain->timer_list)->abs_end, ts))
		return 0;

	/* If there's enough difference, just use the absolute time.
	 * If it get's too near, use the relative delta. */
	make_abs = delta.tv_sec > 0 ? TFD_TIMER_ABSTIME : 0;

	its.it_value = make_abs ? LIST_FIRST(&tc_this_pthread_domain->timer_list)->abs_end : delta;
	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;

	if (timerfd_settime(tc_fd(&tc_this_pthread_domain->timer_tcfd), make_abs, &its, NULL))
		msg_exit(1, "timerfd_settime failed with %m\n");
	return 1;
}


static void insert_into_timer_list(struct timer_waiter *to_insert)
{
	struct timer_waiter *cur, *prev;

	spin_lock(&tc_this_pthread_domain->timer_lock);
	cur = LIST_FIRST(&tc_this_pthread_domain->timer_list);
	prev = NULL;

	while (cur) {
		if (compare_ts_times(&to_insert->abs_end, &cur->abs_end) < 0)
			goto put_here;

		prev = cur;
		assert(cur != LIST_NEXT(cur, tl));
		cur = LIST_NEXT(cur, tl);
	}

	/* Nothing left, append. */
	if (prev)
		LIST_INSERT_AFTER(prev, to_insert, tl);
	else
		LIST_INSERT_HEAD(&tc_this_pthread_domain->timer_list, to_insert, tl);
	goto check_for_reprogram;

put_here:
	LIST_INSERT_BEFORE(cur, to_insert, tl);

check_for_reprogram:
	if (LIST_FIRST(&tc_this_pthread_domain->timer_list) == to_insert) {
		/* Re-program the soonest wakeup */
		timerfd_reprogram_valid(NULL);
	}

	spin_unlock(&tc_this_pthread_domain->timer_lock);
	return;
}


static inline int waiting_done(struct timer_waiter *tw, struct timespec *ts)
{
	if (LIST_NEXT(tw, tl) == NOT_ON_TIMERLIST)
		return 1;

	clock_gettime(CLOCK_MONOTONIC, ts);
	return  compare_ts_times(ts, &tw->abs_end) >= 0;
}



/* Each tc_thread sorts its timer_waiter into the scheduler.timer_list; one of
 * them becomes the master, and wakes up all other threads as necessary, until
 * it is done, and another one becomes master.  */
enum tc_rv tc_sleep(int clockid, time_t sec, long nsec)
{
	struct timespec ts;
	enum tc_rv rv;
	struct timer_waiter tw;
	struct timer_waiter *two, *two2;
	int have_lock, wait_done;
	uint64_t c;


	/* We'd need another timerfd for CLOCK_REALTIME ... and then there are the
	 * issues about NTP etc., so only CLOCK_MONOTONIC is supported right now.
	 * */
	assert(clockid == CLOCK_MONOTONIC);

	have_lock = 0;
	wait_done = 0;


	/* Do that as soon as possible ... as if a few cycles would make
	 * a difference ;) */
	clock_gettime(CLOCK_MONOTONIC, &tw.abs_end);
	tw.abs_end.tv_sec += sec;
	tw.abs_end.tv_nsec += nsec;
	while (tw.abs_end.tv_nsec > 1e9) {
		tw.abs_end.tv_nsec -= 1e9;
		tw.abs_end.tv_sec  +=   1;
	}

	tw.tc = tc_current();
	tc_waitq_init(&tw.wq);
	LIST_NEXT(&tw, tl) = NOT_ON_TIMERLIST;

	c = atomic_inc(&tc_this_pthread_domain->timer_sleepers);

	/* For very small timeouts we might not even get into the scheduler.
	 * The contract with the previous version of tc_sleep() was to _always_
	 * activate the scheduler, so that tc_sleep() in a while(1) loop wouldn't
	 * keep the binary from exit()ing.
	 * So we have to force that ... */
	rv = tc_sched_yield();
	if (rv)
		goto quit;


	insert_into_timer_list(&tw);

	have_lock = 0;
	wait_done = 0;
	rv = RV_OK;


	/* Don't botch CALLER */
	__tc_wait_event(&tw.wq, ({
			 wait_done = waiting_done(&tw, &ts);
			 if (!wait_done)
				 have_lock = !tc_mutex_trylock(&tc_this_pthread_domain->timer_mutex);

			 wait_done || have_lock;
			 }), rv);
	if (rv || wait_done)
		goto quit;


	assert(have_lock);

	/* master */
	while (1) {
		if (waiting_done(&tw, &ts))
			break;

		spin_lock(&tc_this_pthread_domain->timer_lock);
		if (timerfd_reprogram_valid(&ts)) {
			spin_unlock(&tc_this_pthread_domain->timer_lock);
			rv = tc_wait_fd(EPOLLIN, &tc_this_pthread_domain->timer_tcfd);
			if (rv)
				break;

			read(tc_fd(&tc_this_pthread_domain->timer_tcfd), &c, sizeof(c));
		}
		else
			spin_unlock(&tc_this_pthread_domain->timer_lock);


		/* Look which threads can be woken up. */
		spin_lock(&tc_this_pthread_domain->timer_lock);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		two = LIST_FIRST(&tc_this_pthread_domain->timer_list);
		while (two) {
			if (compare_ts_times(&ts, &two->abs_end) < 0)
				break;

			two2 = LIST_NEXT(two, tl);
			assert(two != two2);
			LIST_REMOVE(two, tl);

			LIST_NEXT(two, tl) = NOT_ON_TIMERLIST;
			tc_waitq_wakeup_all(&two->wq);

			two = two2;
		}
		spin_unlock(&tc_this_pthread_domain->timer_lock);

		/* Restart the loop, perhaps this one is finished. */
	}


quit:
	if (have_lock)
		tc_mutex_unlock(&tc_this_pthread_domain->timer_mutex);
	/* Tell another one to take over. */
	remove_from_timer_list_wakeup(&tw);

	atomic_dec(&tc_this_pthread_domain->timer_sleepers);
	return rv;
}


void tc_rw_init(struct tc_rw_lock *l)
{
	tc_mutex_init(&l->mutex);
	atomic_set(&l->readers, 0);
	tc_waitq_init(&l->wr_wq);
}

/* This "slow" way of locking a mutex is needed to guarantee fairness against
 * a writer; doing just an atomic_inc() and checking whether it's allowed might
 * prohibit writers from _ever_ running.  */
enum tc_rv tc_rw_r_lock(struct tc_rw_lock *l)
{
	enum tc_rv rv;

	rv = tc_mutex_lock(&l->mutex);
	if (rv == RV_OK) {
		atomic_inc(&l->readers);
		tc_mutex_unlock(&l->mutex);
	}
	return rv;
}

enum tc_rv tc_rw_w_lock(struct tc_rw_lock *l)
{
	enum tc_rv rv;

	rv = tc_mutex_lock(&l->mutex);
	if (rv == RV_OK) {
		rv = tc_waitq_wait_event(&l->wr_wq,
				atomic_read(&l->readers) == 0);
		/* In case the waiting was interrupted, we have to release the mutex
		 * - there might still be readers. */
		if (rv)
			tc_mutex_unlock(&l->mutex);
	}
	return rv;
}

enum tc_rv tc_rw_w_trylock(struct tc_rw_lock *l)
{
	enum tc_rv rv;

	rv = tc_mutex_trylock(&l->mutex);
	if (rv == RV_OK) {
		if (atomic_read(&l->readers)) {
			tc_mutex_unlock(&l->mutex);
			rv = RV_FAILED;
		}
	}
	return rv;
}


int tc_signals_since_last_call(void)
{
	int v;
	struct tc_thread *tc;

	tc = tc_current();

	/* The various exchange-and-set in GCC are not universal;
	 * they might only store a 1.
	 * Therefore we get it via fetch and subtract. */
	v = atomic_read(&tc->signals_since_last_query);
	atomic_sub_return(v, &tc->signals_since_last_query);
	return v;
}


/* These functions need access to internals, so we have to include that here.
 *
 * See also
 *   http://www.xmailserver.org/eventfd-aio-test.c
 *   http://ozlabs.org/~rusty/index.cgi/tech/2008-01-08.html
 *   http://www.monkey.org/~provos/libevent/
 *
 */


/* Does wait for outstanding (simple) IO requests of this tc_thread.
 * Returns 0 for ok.
 *
 * Does only check the few internally-allocated AIO operations, ie.
 * with the (struct tc_aio_data) embedded in (struct tc_thread). */
int tc_aio_wait(void)
{
	struct tc_thread *tc;
	int i;
	int rv;
	struct tc_aio_data *ad;

	tc = tc_current();
	rv = 0;

	for(i=0; i<TC_AIO_REQUESTS_PER_TC_THREAD; i++)
	{
		ad = tc->aio + i;

		if (ad->notify) {
			/* save and restore the wq?
			 * but then do a wakeup_one or all?
			 * risks duplicate notifications ...
			 * TODO */
			/* If the caller wanted to be notified, he will get notified ... */
			continue;
		}

		ad->notify = &tc->aio_wq;

		rv = tc_waitq_wait_event(&tc->aio_wq,
				tc_aio_data_done(ad)) || rv;

		/* This wq will be gone soon */
		ad->notify = NULL;

		if (tc_aio_data_done(ad) == TC_AIO_FLAG_AD_FREE)
			continue;

		if (ad->res < 0 && !rv)
			rv = ad->res;
		if (ad->res2 && !rv)
			rv = ad->res2;

#if 0
		ad->res = 0;
		ad->res2 = 0;
#endif
		/* Result returned, don't return again */
		tc_aio_set_data_free(ad);
	}

	return rv;
}


static int _tc_aio_get_aio_data(struct tc_aio_data **rad)
{
	int rv;
	int i;
	struct tc_thread *tc;
	struct tc_aio_data *ad;

	tc = tc_current();

	while (1) {
		for(i=0; i<TC_AIO_REQUESTS_PER_TC_THREAD; i++) {
			ad = tc->aio + i;
			if (tc_aio_data_done(ad) == TC_AIO_FLAG_AD_FREE) {
				*rad = ad;
				return 0;
			}
		}

		rv = tc_aio_wait();
		if (rv)
			return rv;
	}
}


inline static int _tc_aio_submit_keep_notify(struct tc_aio_data *ad)
{
	int rv;
	struct iocb *cb;
	struct tc_waitq *wq;

	/* TODO: not needed, cb.obj is sufficient */
	ad->cb.data = ad;
	ad->res = 0;
	ad->res2 = 0;
	io_set_eventfd(&ad->cb, tc_this_pthread_domain->aio_eventfd);

	cb = &ad->cb;
	rv = io_submit(tc_this_pthread_domain->aio_ctx, 1, &cb);

	if (rv == 1)
		return 0;

	wq = ad->notify;
	tc_aio_set_data_done(ad);
	if (wq)
		tc_waitq_wakeup_all(wq);

	return -rv;
}


inline static int _tc_aio_submit(struct tc_aio_data *ad)
{
	ad->notify = NULL;
	return _tc_aio_submit_keep_notify(ad);
}


/* Submits a sync request, doesn't wait.
 * After the next tc_aio_wait() the written data should be on stable storage.
 * */
int tc_aio_submit_sync_notify(int fd, int data_only, struct tc_aio_data *ad, struct tc_waitq *wq)
{
	static int warned = 0;
	int rv;

	if (!ad) {
		rv = _tc_aio_get_aio_data(&ad);
		if (rv)
			return rv;
	}

	if (!warned) {
		warned = 1;
		fprintf(stderr, "#warning libtcr AIO: aio_sync not available, no kernel support\n");
	}


	(data_only ? io_prep_fdsync : io_prep_fsync)
		(&ad->cb, fd);
	ad->notify = wq;
	return _tc_aio_submit_keep_notify(ad);
}


/* Does only submit IO; does not wait for completion. */
int tc_aio_submit_write_notify(int fh, void *buffer, size_t size, off_t offset,
		struct tc_aio_data *ad, struct tc_waitq *wq)
{
	int rv;

	if (!ad) {
		rv = _tc_aio_get_aio_data(&ad);
		if (rv)
			return RV_FAILED;
	}

	io_prep_pwrite(&ad->cb, fh, buffer, size, offset);
	ad->notify = wq;
	return _tc_aio_submit_keep_notify(ad);
}


/* Does wait for completion. */
int tc_aio_read(int fh, void *buffer, size_t size, off_t offset)
{
	struct tc_aio_data *ad;
	int rv;

	rv = _tc_aio_get_aio_data(&ad);
	if (rv)
		return RV_FAILED;

	io_prep_pread(&ad->cb, fh, buffer, size, offset);
	rv = _tc_aio_submit(ad);
	if (rv)
		return rv;

	rv = tc_aio_wait();
	if (rv)
		return rv;

	if (ad->res == size)
		return 0;

	/* There's something wrong, don't return 0. */
	return ad->res ?: -1;
}


struct _tc_aio_sync_notify_struct {
	int fd;
	int data_only;
	struct tc_aio_data *ad;
	struct tc_waitq *wq;
};


static inline void __tc_aio_sync(int fh, int data_only, struct tc_aio_data *ad, struct tc_waitq *wq)
{
	struct timeval tv1, tv2;
	uint64_t delta;
	int ret;

	tc_aio_data_init(ad);
	tc_aio_set_data_pending(ad);

	gettimeofday(&tv1, NULL);
	ret = (data_only ? fdatasync : fsync)(fh);
	ad->res = ret ? errno : 0;
	gettimeofday(&tv2, NULL);

	delta = (tv2.tv_sec - tv1.tv_sec) * 1e6 +
		(tv2.tv_usec - tv1.tv_usec);
	ad->res2 = delta;
	tc_aio_set_data_done(ad);

	if (wq)
		tc_waitq_wakeup_all(wq);
}


void *_tc_aio_sync_notify(void *_s)
{
	struct _tc_aio_sync_notify_struct *s = _s;
	void *a[10] = {},
		 *b;

	pthread_detach(pthread_self());
	b = &a;
	__cr_current = (struct coroutine*)&b;

	__tc_aio_sync(s->fd, s->data_only, s->ad, s->wq);

	free(s);
	return NULL;
}


/* There's no AIO sync yet. */
int tc_aio_sync_notify(int fd, int data_only, struct tc_aio_data *ad, struct tc_waitq *wq)
{
	if (!ad)
		return EINVAL;

#if 1
	__tc_aio_sync(fd, data_only, ad, wq);
	return ad->res;
#else
	struct _tc_aio_sync_notify_struct *s;
	pthread_t pt;
	int ret;

	s = malloc(sizeof(*s));
	if (!s)
		msg_exit(1, "OOM tc_aio_sync_notify");

	s->fd = fd;
	s->wq = wq;
	s->ad = ad;
	s->data_only = data_only;

	ret = pthread_create(&pt, NULL, &_tc_aio_sync_notify, s);
	return ret;
#endif

#if 0
	struct tc_aio_data *ad;
	int rv;

	rv = _tc_aio_get_aio_data(&ad);
	if (rv)
		return rv;

	io_prep_fsync(&ad->cb, fd);
	ad->notify = wq;
	return _tc_aio_submit_keep_notify(ad);
#endif
}


int tc_aio_sync(int fd, int data_only)
{
	int rv, rv2;
	struct tc_aio_data ad;
	struct tc_waitq wq;

	tc_waitq_init(&wq);

	tc_aio_data_init(&ad);
	tc_aio_set_data_pending(&ad);

	rv2 = 1;
	rv = tc_waitq_wait_event(&wq,
			({
			 if (rv2)
			 rv2 = tc_aio_sync_notify(fd, data_only, &ad, &wq);
			 rv2 || tc_aio_data_done(&ad);
			 }));

	return rv || rv2;
}


#ifdef WAIT_DEBUG

/* This is a handy function to be called from within gdb */

void tc_dump_threads(void)
{
	struct tc_thread *t;
	struct tc_domain *d;

	d = tc_this_pthread_domain;
	do  {
		msg("TC domain %p:\n", d);
		LIST_FOREACH(t, &d->threads.list, tc_chain) {
			if (t->sleep_line)
				msg("Thread %s(%p) stack at %p base %p, worker %p=%u, waiting at %s:%d\n", t->name, t, cr_get_sp_from_cr(t->cr), cr_get_stack_from_cr(t->cr), t->last_worker, t->last_worker->tid, t->sleep_file, t->sleep_line);
			else
				msg("Thread %s(%p) stack at %p base %p, worker %p=%u, running\n", t->name, t, cr_get_sp_from_cr(t->cr), cr_get_stack_from_cr(t->cr), t->last_worker, t->last_worker->tid);
		}
		d = SLIST_NEXT(d, domain_chain);
	} while (tc_this_pthread_domain != d);
}

#endif
