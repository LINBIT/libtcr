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
#include <signal.h>
#include <assert.h>
#include <sched.h>

#include "compat.h"
#include "atomic.h"
#include "spinlock.h"
#include "coroutines.h"
#include "threaded_cr.h"
#include "clist.h"

#define DEFAULT_STACK_SIZE (1024 * 16)

#define ANY_WORKER -1

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
	struct event_list pending;
	struct event e;  /* Used during start and stop. */
	int worker_nr;
#ifdef WAIT_DEBUG
	char *sleep_file;
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
	int is_on_sleeping_list:1;
	int must_sync:1;
	pid_t tid;
};

enum inter_worker_interrupts {
	IWI_SYNC,
	IWI_IMMEDIATE,
};

struct scheduler {
	spinlock_t lock;           /* protects the threads list */
	struct tc_thread_pool threads;
	struct event_list immediate;
	int nr_of_workers;
	int efd;                   /* epoll fd */
	int immediate_fd;          /* IWI immediate */
	spinlock_t sync_lock;
	atomic_t sync_barrier;
	struct clist_entry sleeping_workers;
	struct tc_fd *free_list;
	diagnostic_fn diagnostic;
	int stack_size;            /* stack size for new tc_threads */
	cpu_set_t available_cpus;    /* CPUs to use for the worker threads */
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
static void iwi_immediate();
static int fprintf_stderr(const char *fmt, va_list ap);

static struct scheduler sched = {
	.diagnostic = fprintf_stderr,
	.stack_size = DEFAULT_STACK_SIZE,
	/* Everything else gets initialized in tc_int(). These two here so
	   that tc_set_*() can be used before tc_run(). */
};
static struct tc_thread *tc_main;
static __thread struct worker_struct worker;

static int fprintf_stderr(const char *fmt, va_list ap)
{
	return vfprintf(stderr, fmt, ap);
}

void tc_set_diagnostic_fn(diagnostic_fn f)
{
	sched.diagnostic = f;
}

void tc_set_stack_size(int s)
{
	sched.stack_size = s;
}

void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
void msg_exit(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sched.diagnostic(fmt, ap);
	va_end(ap);

	exit(code);
}

void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sched.diagnostic(fmt, ap);
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
	spin_lock(&sched.immediate.lock);
	CIRCLEQ_REMOVE(&e->el->events, e, e_chain);
	e->el = &sched.immediate;
	CIRCLEQ_INSERT_TAIL(&sched.immediate.events, e, e_chain);
	spin_unlock(&sched.immediate.lock);
}

/* must_hold tcfd->lock */
static struct event *matching_event(__uint32_t em, struct events *es, __uint32_t *remove_handled_bits)
{
	struct event *e;
	struct event *in  = NULL;
	struct event *out = NULL;
	struct event *ew  = NULL;  /* match on this worker */

	CIRCLEQ_FOREACH(e, es, e_chain) {
		if (em & e->ep_events & EPOLLIN) {
			if (remove_handled_bits)
				*remove_handled_bits &= ~EPOLLIN;
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
			if (remove_handled_bits)
				*remove_handled_bits &= ~EPOLLOUT;
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

/* must_hold tcfd->lock */
static int arm(struct tc_fd *tcfd)
{
	struct epoll_event epe;

	epe.events = calc_epoll_event_mask(&tcfd->events.events);
	if (epe.events == tcfd->ep_events)
		return 0;

	epe.data.ptr = tcfd;
	tcfd->ep_events = epe.events;

	return epoll_ctl(sched.efd, EPOLL_CTL_MOD, tcfd->fd, &epe);
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

static void add_event_cr(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_thread *tc)
{
 	e->ep_events = ep_events;
 	e->flags = flags;

	spin_lock(&sched.immediate.lock);
	_add_event(e, &sched.immediate, tc);
	spin_unlock(&sched.immediate.lock);
}

void remove_event_fd(struct event *e, struct tc_fd *tcfd)
{
	remove_event(e);
}

static void _iwi_immediate();
void tc_thread_free(struct tc_thread *tc)
{
	spin_lock(&tc->running); /* Make sure it has reached switch_to(), after posting EF_EXITING */
	tc_waitq_unregister(&tc->exit_waiters);
	cr_delete(tc->cr);
	tc->cr = NULL;
	if (tc->flags & TF_FREE_NAME)
		free(tc->name);
	memset(tc, 0xaf, sizeof(*tc));
	free(tc);
	_iwi_immediate();
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
	_switch_to(new);
}

static void arm_immediate(int op)
{
	struct epoll_event epe;

	epe.data.ptr = (void*)IWI_IMMEDIATE;
	epe.events = EPOLLIN | EPOLLONESHOT;

	if (epoll_ctl(sched.efd, op, sched.immediate_fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
}

static struct tc_thread *run_or_queue(struct event *e)
{
	struct tc_thread *tc = e->tc;

	if (e->flags == EF_EXITING)
		return tc;

	spin_lock(&tc->pending.lock);
	if (tc->flags & TF_RUNNING) {
		if (e->flags != EF_SIGNAL)
			e->tcfd = worker.woken_by_tcfd;
		_add_event(e, &tc->pending, tc);
		spin_unlock(&tc->pending.lock);
		return NULL;
	}
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

search_loop:
	spin_lock(&sched.immediate.lock);
search_loop_locked:
	worker.woken_by_tcfd  = NULL;
	CIRCLEQ_FOREACH(e, &sched.immediate.events, e_chain) {
		if (!(nr == ANY_WORKER || e->tc->worker_nr == nr))
			continue;
		_remove_event(e, &sched.immediate);
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
			if (!CIRCLEQ_EMPTY(&sched.immediate.events))
				iwi_immediate(); /* More work available, wakeup an worker */
			spin_unlock(&sched.immediate.lock);
			switch_to(tc);
			return 1;
		case EF_EXITING:
			spin_unlock(&sched.immediate.lock);
			tc_thread_free(e->tc);
			/* We cannot simply take the first or next element of
			 * sched.immediate - we've given up the lock, and so the queue
			 * might be *anything*. We have to start afresh. */
			goto search_loop;
			continue;
		default:
			msg_exit(1, "Wrong e->flags in immediate list\n");
		}
	}
	spin_unlock(&sched.immediate.lock);

	return 0;
}

static void run_immediate()
{
	while (_run_immediate(worker.nr) || _run_immediate(ANY_WORKER))
		;
}

static void rearm_immediate()
{
	eventfd_t c;

	if (read(sched.immediate_fd, &c, sizeof(c)) != sizeof(c))
		msg_exit(1, "read() failed with %m");

	arm_immediate(EPOLL_CTL_MOD);
}

static void _iwi_immediate()
{
	eventfd_t c = 1;

	if (write(sched.immediate_fd, &c, sizeof(c)) != sizeof(c))
		msg_exit(1, "write() failed with: %m\n");
}

static void iwi_immediate()
{
	/* Some other worker should please process the queued immediate events. */

	if (!CLIST_EMPTY(&sched.sleeping_workers))
		_iwi_immediate();
}

int tc_sched_yield()
{
	struct tc_thread *tc = tc_current();
	struct event e;
	int ret;

	ret = RV_OK;
	tc_event_init(&e);
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

	return ret;
}

void tc_scheduler(void)
{
	struct event *e;
	struct tc_thread *tc = tc_current();

	spin_lock(&tc->pending.lock);
	if (!CIRCLEQ_EMPTY(&tc->pending.events)) {
		e = CIRCLEQ_FIRST(&tc->pending.events);
		_remove_event(e, &tc->pending);
		spin_unlock(&tc->pending.lock);
		if (e->flags == EF_SIGNAL)
			_signal_gets_delivered(e);
		worker.woken_by_tcfd  = e->tcfd;
		worker.woken_by_event = e;
		return;
	}
	tc->flags &= ~TF_RUNNING;
	spin_unlock(&tc->pending.lock);

#ifdef WAIT_DEBUG
	tc->sleep_file = _caller_file;
	tc->sleep_line = _caller_line;
#endif

	_switch_to(&worker.sched_p2); /* always -> scheduler_part2()*/
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
			er = epoll_wait(sched.efd, &epe, 1, -1);
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
		case IWI_IMMEDIATE:
			worker_after_sleep();
			rearm_immediate();
			/* run_immediate(); at top of loop. */
			continue;
		}

		tcfd = (struct tc_fd *)epe.data.ptr;

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
			e = matching_event(epe.events, &tcfd->events.events, &tcfd->ep_events);
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
			/* At least the returned event might be left on the event list;
			 * so we cannot simply use CIRCLEQ_EMPTY(). */
			if (tcfd->ep_events &&
					/* If empty, don't rearm */
					!CIRCLEQ_EMPTY(&tcfd->events.events) &&
					/* If only the event we just found left, don't rearm */
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
				epoll_ctl(sched.efd, EPOLL_CTL_DEL, tcfd->fd, &epe);
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

void tc_worker_init(int i)
{
	cpu_set_t cpu_mask;
	int cpus_seen = 0, ci, my_cpu, rv = 0;

	cr_init();

	my_cpu = i % CPU_COUNT(&sched.available_cpus);
	CPU_ZERO(&cpu_mask);

	for (ci = 0; ci < CPU_SETSIZE; ci++) {
		if (CPU_ISSET(ci, &sched.available_cpus)) {
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
	worker.main_thread.cr = cr_current();
	cr_set_uptr(cr_current(), &worker.main_thread);
	tc_waitq_init(&worker.main_thread.exit_waiters);
	atomic_set(&worker.main_thread.refcnt, 0);
	spin_lock_init(&worker.main_thread.running);
	spin_lock(&worker.main_thread.running); /* runs currently */
	worker.main_thread.flags = TF_RUNNING;
	event_list_init(&worker.main_thread.pending);
	worker.main_thread.worker_nr = i;
	/* LIST_INSERT_HEAD(&sched.threads, &worker.main_thread, tc_chain); */

	rv |= asprintf(&worker.sched_p2.name, "sched_%d", i);
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
	worker.tid = syscall(__NR_gettid);
}

static void ignore_signal(int sig)
{
	/* Just needed to get out of epoll_wait() */
}


void tc_init()
{
	event_list_init(&sched.immediate);
	LIST_INIT(&sched.threads);
	spin_lock_init(&sched.lock);
	if (SIGNAL_FOR_WAKEUP > SIGRTMAX)
		msg_exit(1, "libTCR: bad value for SIGNAL_FOR_WAKEUP\n");

	signal(SIGNAL_FOR_WAKEUP, ignore_signal);

	spin_lock_init(&sched.sync_lock);
	atomic_set(&sched.sync_barrier, 0);
	CLIST_INIT(&sched.sleeping_workers);
	
	sched.efd = epoll_create(1);
	if (sched.efd < 0)
		msg_exit(1, "epoll_create failed with %m\n");

	sched.immediate_fd = eventfd(0, 0);
	if (sched.immediate_fd == -1)
		msg_exit(1, "eventfd() failed with: %m\n");

	if (sched_getaffinity(0, sizeof(sched.available_cpus), &sched.available_cpus))
		msg_exit(1, "sched_getaffinity: %m\n");

	arm_immediate(EPOLL_CTL_ADD);
}

static void *worker_pthread(void *arg)
{
	int nr = (int)(long)arg;

	tc_worker_init(nr);
	tc_thread_wait(tc_main); /* calls tc_scheduler() */

	_iwi_immediate(); /* All other workers need to get woken UNCONDITIONALLY
			     So that the complete program can terminate */
	return NULL;
}


void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers)
{
	pthread_t *threads;
	int i;
	int avail_cpu;

	tc_init();
	tc_worker_init(0);

	avail_cpu = CPU_COUNT(&sched.available_cpus);
	if (nr_of_workers == 0)
		nr_of_workers = avail_cpu;
	else if (nr_of_workers > avail_cpu)
		msg("tc_run(): got more workers (%d) than available CPUs (%d)\n",
				nr_of_workers, avail_cpu);

	sched.nr_of_workers = nr_of_workers;

	tc_main = tc_thread_new(func, data, name);


	threads = alloca(sizeof(pthread_t) * nr_of_workers);
	if (!threads)
		msg_exit(1, "alloca() in tc_run failed\n");

	threads[0] = pthread_self(); /* actually unused */
	for (i = 1; i < nr_of_workers; i++)
		pthread_create(threads + i, NULL, worker_pthread, (void*)(long)i);

	tc_thread_wait(tc_main); /* calls tc_scheduler() */

	for (i = 1; i < nr_of_workers; i++)
		pthread_join(threads[i], NULL);
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
	int r;

	if (add_event_fd(&e, ep_events | EPOLLONESHOT, ef, tcfd))
		return RV_FAILED;
	tc_scheduler();
	r = (worker.woken_by_event != &e);
	worker.woken_by_event = NULL;
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

	spin_lock(&sched.lock);
	LIST_REMOVE(tc, tc_chain);
	if (tc->flags & TF_THREADS)
		LIST_REMOVE(tc, threads_chain);
	spin_unlock(&sched.lock);

	if (atomic_read(&tc->refcnt) > 0) {
		signal_cancel_pending();
		if (atomic_read(&tc->refcnt) > 0) {
			msg_exit(1, "tc_die(%p, %s): refcnt = %d. Signals still enabled?\n",
					tc, tc->name, atomic_read(&tc->refcnt));
		}
	}

	tc_waitq_wakeup_all(&tc->exit_waiters);

	add_event_cr(&tc->e, 0, EF_EXITING, tc);  /* The scheduler will free me */
	iwi_immediate();
	_switch_to(&worker.sched_p2); /* like tc_scheduler(); but avoids deadlocks */
	msg_exit(1, "tc_scheduler() returned in tc_die() [flags = %d]\n", &tc->flags);
}

void tc_setup(void *arg1, void *arg2)
{
	struct tc_thread *previous = (struct tc_thread *)cr_uptr(cr_caller());
	void (*func)(void *) = arg1;

	spin_unlock(&previous->running);

	func(arg2);

	tc_die();
}

static struct tc_thread *_tc_thread_new(void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;

	tc = malloc(sizeof(struct tc_thread));
	if (!tc)
		goto fail2;

	tc->cr = cr_create(tc_setup, func, data, sched.stack_size);
	if (!tc->cr)
		goto fail3;

	cr_set_uptr(tc->cr, (void *)tc);
	tc->name = name;
	tc->per_thread_data = tc_thread_var_get();
	tc_waitq_init(&tc->exit_waiters);
	atomic_set(&tc->refcnt, 0);
	spin_lock_init(&tc->running);
	tc->flags = 0;
	tc_event_init(&tc->e);
	event_list_init(&tc->pending);
	tc->worker_nr = ANY_WORKER;

	spin_lock(&sched.lock);
	LIST_INSERT_HEAD(&sched.threads, tc, tc_chain);
	spin_unlock(&sched.lock);

	return tc;

fail3:
	free(tc);
fail2:
	return NULL;
}

struct tc_thread *tc_thread_new(void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc = _tc_thread_new(func, data, name);

	if (tc) {
		add_event_cr(&tc->e, 0, EF_READY, tc);
		iwi_immediate();
	}

	return tc;
}

void tc_thread_pool_new(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name, int excess)
{
	struct tc_thread *tc;
	int i;
	char *ename;

	LIST_INIT(threads);
	for (i = 0; i < sched.nr_of_workers + excess; i++) {
		if (asprintf(&ename, name, i) == -1)
			msg_exit(1, "allocation in asprintf() failed\n");
		tc = _tc_thread_new(func, data, ename);
		if (!tc)
			continue;
		tc->flags |= TF_THREADS | TF_FREE_NAME;
		if (i < sched.nr_of_workers) {
			tc->worker_nr = i;
			tc->flags |= TF_AFFINE;
		}
		spin_lock(&sched.lock);
		LIST_INSERT_HEAD(threads, tc, threads_chain);
		spin_unlock(&sched.lock);
		add_event_cr(&tc->e, 0, EF_READY, tc);
	}
	iwi_immediate();
}

enum tc_rv tc_thread_pool_wait(struct tc_thread_pool *threads)
{
	struct tc_thread *tc;
	enum tc_rv r, rv = RV_THREAD_NA;

	spin_lock(&sched.lock);
	while ((tc = LIST_FIRST(threads))) {
		spin_unlock(&sched.lock);
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
		spin_lock(&sched.lock);
	}
	spin_unlock(&sched.lock);

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

	epe.data.ptr = tcfd;
	epe.events = 0;

	if (epoll_ctl(sched.efd, EPOLL_CTL_ADD, fd, &epe) == 0)
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
	epoll_ctl(sched.efd, EPOLL_CTL_DEL, tcfd->fd, &epe);

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

	cur = sched.free_list;
	sched.free_list = NULL;
	spin_unlock(lock_to_free);

	while (cur) {
		next = cur->free_list_next;
		_tc_fd_free(cur);
		cur = next;
	}
}

static void worker_prepare_sleep()
{
	spin_lock(&sched.sync_lock);
	if (!worker.is_on_sleeping_list) {
		CLIST_INSERT_AFTER(&sched.sleeping_workers, &worker.sleeping_chain);
	}
	worker.is_on_sleeping_list = 1;
	spin_unlock(&sched.sync_lock);
}

static void worker_after_sleep()
{
	int new;
	int have_lock;

	/* These two checks have to made atomically w.r.t. sync_lock. */
	spin_lock(&sched.sync_lock);
	have_lock = 1;
	if (worker.is_on_sleeping_list)
	{
		CLIST_REMOVE(&worker.sleeping_chain);
		worker.is_on_sleeping_list = 0;
	}
	if (worker.must_sync) {
		worker.must_sync = 0;
		new = atomic_dec(&sched.sync_barrier);
		if (new == 0)
		{
			_process_free_list(&sched.sync_lock);
			have_lock = 0;
		}
	}

	if (have_lock)
		spin_unlock(&sched.sync_lock);
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


	spin_lock(&sched.sync_lock);
	tcfd->free_list_next = sched.free_list;
	sched.free_list = tcfd;

	if (CLIST_EMPTY(&sched.sleeping_workers)) {
		/* No new sleepers. */
	}
	else {
		/* Process the sleeper-list. */
		list = sched.sleeping_workers.cl_next;
		while (list != &sched.sleeping_workers) {
			w = container_of(list, struct worker_struct, sleeping_chain);
			/* When a synchronize_world() is run while a thread waits for the lock
			 * in worker_prepare_sleep(), then the worker would be on the
			 * sleeping_workers list again.
			 * If the next function is a synchronize_world() again (likely in
			 * leak_test2), then this would try to get the thread again ... */
			if (!w->must_sync) {
				w->must_sync = 1;
				atomic_inc(&sched.sync_barrier);
				w->is_on_sleeping_list = 0;
				tgkill(getpid(), w->tid, SIGNAL_FOR_WAKEUP);
			}
			list = list->cl_next;
		}
		CLIST_INIT(&sched.sleeping_workers);
	}

	if (atomic_read(&sched.sync_barrier)) {
		spin_unlock(&sched.sync_lock);
		/* See comment in worker_after_sleep */
	}
	else
		_process_free_list(&sched.sync_lock);
}



void tc_mutex_init(struct tc_mutex *m)
{
	atomic_set(&m->count, 0);
	tc_waitq_init(&m->wq);
}

enum tc_rv tc_mutex_lock(struct tc_mutex *m)
{
	struct event e;

	if (atomic_set_if_eq(1, 0, &m->count))
		return RV_OK;

	tc_event_init(&e);
	tc_waitq_prepare_to_wait(&m->wq, &e);
	if (atomic_add_return(1, &m->count) > 1) {
		tc_scheduler();
		if (tc_waitq_finish_wait(&m->wq, &e)) {
			atomic_dec(&m->count);
			return RV_INTR;
		} else
			return RV_OK;
	} else {
		tc_waitq_finish_wait(&m->wq, &e);
		return RV_OK;
	}
	/* The event is not usable anymore, as
	 * we're leaving the frame.  */
	if (e.el)
		remove_event(&e);
	return RV_OK;
}

void tc_mutex_unlock(struct tc_mutex *m)
{
	int r;

	r = atomic_sub_return(1, &m->count);

	if (r > 0)
		tc_waitq_wakeup_one(&m->wq);
	else if (r < 0)
		msg_exit(1, "tc_mutex_unlocked() called on an unlocked mutex\n");
}

enum tc_rv tc_mutex_trylock(struct tc_mutex *m)
{
	if (atomic_set_if_eq(1, 0, &m->count))
		return RV_OK;

	return RV_FAILED;
}

void tc_mutex_destroy(struct tc_mutex *m)
{
	tc_waitq_unregister(&m->wq);
}

int tc_mutex_waiters(struct tc_mutex *m)
{
	return atomic_read(&m->count)-1;
}


static enum tc_rv _thread_valid(struct tc_thread *look_for)
{
	struct tc_thread *tc;

	LIST_FOREACH(tc, &sched.threads, tc_chain) {
		if (tc == look_for)
			return RV_OK;
	}
	return RV_THREAD_NA;
}

enum tc_rv tc_thread_wait(struct tc_thread *wait_for)
{
	struct event e;
	enum tc_rv rv;

	tc_event_init(&e);
	spin_lock(&sched.lock);
	rv = _thread_valid(wait_for);  /* wait_for might have already exited */
	if (rv == RV_OK)
		tc_waitq_prepare_to_wait(&wait_for->exit_waiters, &e);

	spin_unlock(&sched.lock);
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
}

void tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	e->ep_events = 0; /* unused */
	e->flags = EF_READY;
	_tc_waitq_prepare_to_wait(wq, e, tc_current());
}

int tc_waitq_finish_wait(struct tc_waitq *wq, struct event *e)
{
	int interrupted = (worker.woken_by_event && worker.woken_by_event != e);
	struct event_list *el;

	if (worker.woken_by_event != e) {
		el = remove_event(e);

		if (el != &wq->waiters && worker.woken_by_event) {
			/* We got woken up by an signal, but the event we were waiting
			   for became ready at the same time.*/
			struct tc_thread *tc = tc_current();

#if 0
			el was exit_waiters of another thread,too
			if (el != &sched.immediate && el != &tc->pending)
				msg_exit(1, "Event removed from unknown list\n");
#endif

			/* Requeue the signal for later delivery */
			e = worker.woken_by_event;
			if (e->flags != EF_SIGNAL)
				msg_exit(1, "Interrupted by an unexpected event (%d)\n", e->flags);

			el = remove_event(e);
#if 0
			if (el != &e->signal->wq.waiters)
				msg_exit(1, "Signal event on unexpected list\n");
#endif

			spin_lock(&tc->pending.lock);
			_add_event(e, &tc->pending, tc);
			spin_unlock(&tc->pending.lock);

			interrupted = 0;
		}
	}
	worker.woken_by_event = NULL;

	return interrupted;
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

void tc_waitq_wakeup_one(struct tc_waitq *wq)
{
	int wake = 0;
	struct event *e;

	spin_lock(&sched.immediate.lock);
	spin_lock(&wq->waiters.lock);
	if (!CIRCLEQ_EMPTY(&wq->waiters.events)) {
		e = CIRCLEQ_FIRST(&wq->waiters.events);
		CIRCLEQ_REMOVE(&wq->waiters.events, e, e_chain);
		e->el = &sched.immediate;
		CIRCLEQ_INSERT_HEAD(&sched.immediate.events, e, e_chain);
		wake = 1;
	}
	spin_unlock(&wq->waiters.lock);
	spin_unlock(&sched.immediate.lock);

	if (wake)
		iwi_immediate();
}

void tc_waitq_wakeup_all(struct tc_waitq *wq)
{
	struct event *e;
	int wake = 0;

	spin_lock(&sched.immediate.lock);
	spin_lock(&wq->waiters.lock);
	while(!CIRCLEQ_EMPTY(&wq->waiters.events)) {
		e = CIRCLEQ_FIRST(&wq->waiters.events);
		CIRCLEQ_REMOVE(&wq->waiters.events, e, e_chain);
		e->el = &sched.immediate;
		CIRCLEQ_INSERT_HEAD(&sched.immediate.events, e, e_chain);
		wake++;
	}
	spin_unlock(&wq->waiters.lock);
	spin_unlock(&sched.immediate.lock);

	/* If wake is non-zero, we publish that there's something to be done.
	 * The iwi_immediate() would only wakeup _idle_ workers, so (if there's
	 * none) the event might get lost; the _iwi_immediate() function makes sure
	 * the next epoll_wait() call terminates. */
	if (wake)
		_iwi_immediate();
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


void tc_signal_unsubscribe_nofree(struct tc_signal *s, struct tc_signal_sub *ss)
{
	spin_lock(&s->wq.waiters.lock);
	LIST_REMOVE(ss, se_chain);
	spin_unlock(&s->wq.waiters.lock);
	remove_event(&ss->event);
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

	spin_lock(&sched.immediate.lock);
	CIRCLEQ_FOREACH(e, &sched.immediate.events, e_chain) {
		if (e->tc == tc && e->flags == EF_SIGNAL)
			_cancel_signal(e, &sched.immediate);
	}
	spin_unlock(&sched.immediate.lock);

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

enum tc_rv tc_sleep(int clockid, time_t sec, long nsec)
{
	struct itimerspec ts;
	struct tc_fd *tcfd;
	enum tc_rv rv;
	int fd;

	ts.it_value.tv_sec = sec;
	ts.it_value.tv_nsec = nsec;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;

	fd = timerfd_create(clockid, 0);
	if (fd == -1)
		msg_exit(1, "timerfd_create with %m\n");

	tcfd = tc_register_fd(fd);
	if (!tcfd || timerfd_settime(fd, 0, &ts, NULL))
		rv = RV_FAILED;
	else
		rv = tc_wait_fd(EPOLLIN, tcfd);
	/* If we got interrupted by a signal, another thread might see/use the
	 * tc_fd - so we have to clean up. */
	_tc_fd_unregister(tcfd, rv == RV_INTR);
	/* The close must happen after the unregister call.
	 * If it's before the kernel might return the same fd to another thread
	 * which would fail because the fd gets removed from the efd set in
	 * _tc_fd_unregister(). */
	close(fd);
	return rv;
}


void tc_rw_init(struct tc_rw_lock *l)
{
	tc_mutex_init(&l->mutex);
	atomic_set(&l->readers, 0);
	tc_waitq_init(&l->wr_wq);
}

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

#ifdef WAIT_DEBUG

/* This is a handy function to be called from within gdb */

void tc_dump_threads(void)
{
	struct tc_thread *t;

	LIST_FOREACH(t, &sched.threads, tc_chain) {
		if (t->sleep_line)
			msg("Thread %s(%p) waiting at %s:%d\n", t->name, t, t->sleep_file, t->sleep_line);
		else
			msg("Thread %s(%p) running\n", t->name, t);
	}
}

#endif
