#define _GNU_SOURCE /* for asprintf() */ 

#include "config.h"

#include <sys/epoll.h>
#ifdef HAVE_SYS_EVENTFD_H
#include <sys/eventfd.h>
#else
#include <stdint.h>
typedef uint64_t eventfd_t;
extern int eventfd (int __count, int __flags);
extern int eventfd_read (int __fd, eventfd_t *__value);
extern int eventfd_write (int __fd, eventfd_t value);
#endif
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "atomic.h"
#include "spinlock.h"
#include "coroutines.h"
#include "threaded_cr.h"

#define DEFAULT_STACK_SIZE (1024 * 16)

#ifndef HAVE_TIMERFD_CREATE
#include <sys/syscall.h>
int timerfd_create (clockid_t __clock_id, int __flags)
{
	return syscall(SYS_timerfd_create, __clock_id, __flags);
}
#endif

#ifndef HAVE_TIMERFD_SETTIME
#include <sys/syscall.h>
extern int timerfd_settime (int __ufd, int __flags,
                            __const struct itimerspec *__utmr,
                            struct itimerspec *__otmr)
{
	return syscall(SYS_timerfd_settime, __ufd, __flags, __utmr, __otmr);
}
#endif

#ifndef HAVE_TIMERFD_GETTIME
#include <sys/syscall.h>
extern int timerfd_gettime (int __ufd, struct itimerspec *__otmr)
{
	return syscall(SYS_timerfd_gettime, __ufd, __otmr);
}
#endif

LIST_HEAD(waitq_evs, waitq_ev);

enum thread_flags {
	TF_THREADS = 1 << 0, /* is on threads chain*/
	TF_RUNNING = 1 << 1,
};

struct tc_thread {
	char *name;		/* Leafe that first, for debugging spinlocks */
	LIST_ENTRY(tc_thread) tc_chain;      /* list of all threads*/
	LIST_ENTRY(tc_thread) threads_chain; /* list of thrads created with one call to tc_threads_new() */
	struct coroutine *cr;
	struct tc_waitq exit_waiters;
	atomic_t refcnt;
	spinlock_t running;
	unsigned int flags; /* flags protected by pending.lock */
	struct event_list pending;
	struct event e;  /* Used during start and stop. */
};

struct setup_info {
	void *data;
	void (*func)(void *);
};

struct worker_struct {
	int nr;
	struct tc_thread main_thread;
	struct tc_thread sched_p2;
	struct event *woken_by_event; /* always set after tc_scheduler()   */
	struct tc_fd *woken_by_tcfd;  /* might be set after tc_scheduler() */
};

enum inter_worker_interrupts {
	IWI_SYNC,
	IWI_IMMEDIATE,
};

struct scheduler {
	spinlock_t lock;           /* protects the threads list */
	struct tc_threads threads;
	struct event_list immediate;
	int nr_of_workers;
	int efd;                   /* epoll fd */
	int sync_fd;               /* IWI event fd to synchronize all workers */
	int immediate_fd;          /* IWI immediate */
	atomic_t sync_cnt;
	pthread_barrier_t *sync_b;
	spinlock_t sync_lock;
};

static struct scheduler sched;
static struct tc_thread *tc_main;
static __thread struct worker_struct worker;

static void _signal_gets_delivered2(struct event *e);
static void signal_cancel_pending();
static void _synchronize_world();
static void synchronize_world();

static inline struct tc_thread *tc_current()
{
	return (struct tc_thread *)cr_uptr(cr_current());
}

void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
void msg_exit(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(code);
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
static struct event *matching_event(__uint32_t em, struct events *es)
{
	struct event *e;
	CIRCLEQ_FOREACH(e, es, e_chain) {
		if (em & e->ep_events)
			return e;
	}
	return NULL;
}

/* must_hold tcfd->lock */
static void arm(struct tc_fd *tcfd)
{
	struct epoll_event epe;

	epe.events = calc_epoll_event_mask(&tcfd->events.events);
	if (epe.events == tcfd->ep_events)
		return;

	epe.data.ptr = tcfd;
	tcfd->ep_events = epe.events;

	if (epoll_ctl(sched.efd, EPOLL_CTL_MOD, tcfd->fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
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
}

static void remove_event(struct event *e)
{
	spin_lock(&e->el->lock);
	_remove_event(e, e->el);
	spin_unlock(&e->el->lock);
}

/* must_hold el->lock */
static void _add_event(struct event *e, struct event_list *el)
{
	atomic_inc(&tc_current()->refcnt);
	e->tc = tc_current();
	e->el = el;

	CIRCLEQ_INSERT_HEAD(&el->events, e, e_chain);
}

static void add_event(struct event *e, struct event_list *el)
{
	spin_lock(&el->lock);
	_add_event(e, el);
	spin_unlock(&el->lock);
}


void add_event_fd(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_fd *tcfd)
{
 	e->ep_events = ep_events;
 	e->flags = flags;

	spin_lock(&tcfd->events.lock);
	_add_event(e, &tcfd->events);
	arm(tcfd);
	spin_unlock(&tcfd->events.lock);
}

static void add_event_cr(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_thread *tc)
{
 	e->ep_events = ep_events;
 	e->flags = flags;

	add_event(e, &sched.immediate);
}

void remove_event_fd(struct event *e, struct tc_fd *tcfd)
{
	remove_event(e);
}

void tc_thread_free(struct tc_thread *tc)
{
	spin_lock(&tc->running); /* Make sure it has reached switch_to(), after posting EF_EXITING */
	tc_waitq_unregister(&tc->exit_waiters);
	cr_delete(tc->cr);
	tc->cr = NULL;
	free(tc);
}

static void switch_to(struct tc_thread *new)
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

static void arm_immediate(int op)
{
	struct epoll_event epe;

	epe.data.u32 = IWI_IMMEDIATE;
	epe.events = EPOLLIN | EPOLLONESHOT;

	if (epoll_ctl(sched.efd, op, sched.immediate_fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
}

static int run_immediate(struct tc_thread *not_for_tc)
{
	struct event *e;

	spin_lock(&sched.immediate.lock);
	CIRCLEQ_FOREACH(e, &sched.immediate.events, e_chain) {
		worker.woken_by_event = e;
		worker.woken_by_tcfd  = NULL;
		if (e->tc != not_for_tc) {
			_remove_event(e, &sched.immediate);
			spin_unlock(&sched.immediate.lock);
			switch (e->flags) {
			case EF_READY:
				switch_to(e->tc);
				return 1; /* must cause tc_schedulre() to return! */
			case EF_EXITING:
				tc_thread_free(e->tc);
				spin_lock(&sched.lock);
				e = CIRCLEQ_FIRST(&sched.immediate.events);
				continue;
			default:
				msg_exit(1, "Wrong e->flags in immediate list\n");
			}
		}
	}
	spin_unlock(&sched.immediate.lock);

	return 0;
}

static void process_immediate()
{
	eventfd_t c;

	if (read(sched.immediate_fd, &c, sizeof(c)) != sizeof(c))
		msg_exit(1, "read() failed with %m");

	arm_immediate(EPOLL_CTL_MOD);
	run_immediate(NULL);
}

static void iwi_immediate()
{
	/* Some other worker should please process the queued immediate events. */
	eventfd_t c = 1;

	if (write(sched.immediate_fd, &c, sizeof(c)) != sizeof(c))
		msg_exit(1, "write() failed with: %m\n");
}

void tc_sched_yield()
{
	struct tc_thread *tc = tc_current();
	struct event e;

	add_event_cr(&e, 0, EF_READY, tc); /* use tc->e ? */
	if (!run_immediate(tc)) {
		spin_lock(&sched.lock);
		_remove_event(&e, &sched.immediate);
		spin_unlock(&sched.lock);
	}
}

void tc_scheduler(void)
{
	struct event *e;
	struct tc_thread *tc = tc_current();

	spin_lock(&tc->pending.lock);
	if (!CIRCLEQ_EMPTY(&tc->pending.events)) {
		e = CIRCLEQ_FIRST(&tc->pending.events);
		CIRCLEQ_REMOVE(&tc->pending.events, e, e_chain);
		spin_unlock(&tc->pending.lock);
		if (e->flags == EF_SIGNAL)
			_signal_gets_delivered2(e);
		worker.woken_by_tcfd  = e->tcfd;
		worker.woken_by_event = e;
		return;
	}
	tc->flags &= ~TF_RUNNING;
	spin_unlock(&tc->pending.lock);

	if (run_immediate(tc))
		return;

	switch_to(&worker.sched_p2); /* always -> scheduler_part2()*/
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
		do {
			er = epoll_wait(sched.efd, &epe, 1, -1);
		} while (er < 0 && errno == EINTR);
		if (er < 0)
			msg_exit(1, "epoll_wait() failed with: %m\n");

		switch (epe.data.u32) {
		case IWI_SYNC:
			_synchronize_world();
			continue;
		case IWI_IMMEDIATE:
			process_immediate();
			continue;
		}

		tcfd = (struct tc_fd *)epe.data.ptr;

		spin_lock(&tcfd->events.lock);
		tcfd->ep_events = -1; /* recalc them */

		e = matching_event(epe.events, &tcfd->events.events);
		if (!e) {
			/* That can happen if the event_fd of a signal wakes us up just
			   after _signal_cancel was called */
			spin_unlock(&tcfd->events.lock);
			continue;
		}

		tc = e->tc;
		_remove_event(e, &tcfd->events);

		spin_unlock(&tcfd->events.lock);

		if (e->flags == EF_SIGNAL)
			tcfd = NULL; /* Do not expose the tcfd in case it was a signal. */

		spin_lock(&tc->pending.lock);
		if (tc->flags & TF_RUNNING) {
			e->tcfd = tcfd;
			CIRCLEQ_INSERT_HEAD(&tc->pending.events, e, e_chain);
			spin_unlock(&tc->pending.lock);
			continue;
		}
		tc->flags |= TF_RUNNING;
		spin_unlock(&tc->pending.lock);

		if (e->flags == EF_SIGNAL)
			_signal_gets_delivered2(e);

		worker.woken_by_event = e;
		worker.woken_by_tcfd = tcfd;

		switch_to(tc);
	}
}

void tc_worker_init(int i)
{
	cr_init();

	worker.nr = i;
	asprintf(&worker.main_thread.name, "main_thread_%d", i);
	worker.main_thread.cr = cr_current();
	cr_set_uptr(cr_current(), &worker.main_thread);
	tc_waitq_init(&worker.main_thread.exit_waiters);
	atomic_set(&worker.main_thread.refcnt, 0);
	spin_lock_init(&worker.main_thread.running);
	spin_lock(&worker.main_thread.running); /* runs currently */
	worker.main_thread.flags = TF_RUNNING;
	event_list_init(&worker.main_thread.pending);
	/* LIST_INSERT_HEAD(&sched.threads, &worker.main_thread, tc_chain); */

	asprintf(&worker.sched_p2.name, "sched_%d", i);
	worker.sched_p2.cr = cr_create(scheduler_part2, NULL, DEFAULT_STACK_SIZE);
	if (!worker.sched_p2.cr)
		msg_exit(1, "allocation of worker.sched_p2 failed\n");

	cr_set_uptr(worker.sched_p2.cr, &worker.sched_p2);
	tc_waitq_init(&worker.sched_p2.exit_waiters);
	atomic_set(&worker.sched_p2.refcnt, 0);
	spin_lock_init(&worker.sched_p2.running);
	worker.sched_p2.flags = 0;
	event_list_init(&worker.sched_p2.pending);
}

void tc_init()
{
	struct epoll_event epe;

	event_list_init(&sched.immediate);
	LIST_INIT(&sched.threads);
	spin_lock_init(&sched.lock);

	spin_lock_init(&sched.sync_lock);
	atomic_set(&sched.sync_cnt, 0);
	sched.sync_b = NULL;
	sched.sync_fd = eventfd(0, 0);
	if (sched.sync_fd == -1)
		msg_exit(1, "eventfd() failed with: %m\n");

	sched.efd = epoll_create(1);
	if (sched.efd < 0)
		msg_exit(1, "epoll_create failed with %m\n");

	epe.data.u32 = IWI_SYNC;
	epe.events = EPOLLIN;

	if (epoll_ctl(sched.efd, EPOLL_CTL_ADD, sched.sync_fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");

	sched.immediate_fd = eventfd(0, 0);
	if (sched.immediate_fd == -1)
		msg_exit(1, "eventfd() failed with: %m\n");

	arm_immediate(EPOLL_CTL_ADD);
}

static void *worker_pthread(void *arg)
{
	int nr = (int)arg;

	tc_worker_init(nr);
	tc_thread_wait(tc_main); /* calls tc_scheduler() */
	return NULL;
}


void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers)
{
	pthread_t *threads;
	int i;

	threads = alloca(sizeof(pthread_t) * nr_of_workers);
	if (!threads)
		msg_exit(1, "alloca() in tc_run failed\n");

	tc_init();
	tc_worker_init(0);

	sched.nr_of_workers = nr_of_workers;

	tc_main = tc_thread_new(func, data, name);

	threads[0] = pthread_self(); /* actually unused */
	for (i = 1; i < nr_of_workers; i++)
		pthread_create(threads + i, NULL, worker_pthread, (void*)i);

	tc_thread_wait(tc_main); /* calls tc_scheduler() */

	for (i = 1; i < nr_of_workers; i++)
		pthread_join(threads[i], NULL);
}


void tc_rearm()
{
	if (worker.woken_by_tcfd) {
		spin_lock(&worker.woken_by_tcfd->events.lock);
		arm(worker.woken_by_tcfd);
		spin_unlock(&worker.woken_by_tcfd->events.lock);
	}
}

enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd)
{
	struct event e;

	add_event_fd(&e, ep_events | EPOLLONESHOT, EF_READY, tcfd);
	tc_scheduler();
	if (worker.woken_by_event != &e) {
		remove_event_fd(&e, tcfd);
		return RV_INTR;
	}
	return RV_OK;
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

	tc_waitq_wakeup_all(&tc->exit_waiters);

	if (atomic_read(&tc->refcnt) > 0) {
		signal_cancel_pending();
		if (atomic_read(&tc->refcnt) > 0) {
			msg_exit(1, "tc_die(%s): refcnt = %d. Signals still enabled?\n",
				 tc->name, atomic_read(&tc->refcnt));
		}
	}

	add_event_cr(&tc->e, 0, EF_EXITING, tc);  /* The scheduler will free me */
	iwi_immediate();
	switch_to(&worker.sched_p2); /* like tc_scheduler(); but avoids deadlocks */
	msg_exit(1, "tc_scheduler() returned in tc_die() [flags = %d]\n", &tc->flags);
}

void tc_setup(void *data)
{
	struct setup_info *i = (struct setup_info *)data;
	struct tc_thread *previous;
	void (*func)(void *), *func_data;

	previous = (struct tc_thread *)cr_uptr(cr_caller());
	spin_unlock(&previous->running);

	func_data = i->data;
	func = i->func;
	free(i);

	func(func_data);

	tc_die();
}

struct tc_thread *tc_thread_new(void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;
	struct setup_info *i;

	i = malloc(sizeof(struct setup_info));
	if (!i)
		goto fail1;

	tc = malloc(sizeof(struct tc_thread));
	if (!tc)
		goto fail2;

	tc->cr = cr_create(tc_setup, i, DEFAULT_STACK_SIZE);
	if (!tc->cr)
		goto fail3;

	i->func = func;
	i->data = data;

	cr_set_uptr(tc->cr, (void *)tc);
	tc->name = name;
	tc_waitq_init(&tc->exit_waiters);
	atomic_set(&tc->refcnt, 0);
	spin_lock_init(&tc->running);
	tc->flags = 0;
	event_list_init(&tc->pending);

	spin_lock(&sched.lock);
	LIST_INSERT_HEAD(&sched.threads, tc, tc_chain);
	spin_unlock(&sched.lock);
	add_event_cr(&tc->e, 0, EF_READY, tc);           /* removed in the tc_scheduler */
	iwi_immediate();

	return tc;

fail3:
	free(tc);
fail2:
	free(i);
fail1:
	return NULL;
}

void tc_threads_new(struct tc_threads *threads, void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;
	int i;
	char *ename;

	LIST_INIT(threads);
	for (i = 0; i < sched.nr_of_workers; i++) {
		asprintf(&ename, name, i);
		tc = tc_thread_new(func, data, ename);
		spin_lock(&sched.lock);
		LIST_INSERT_HEAD(threads, tc, threads_chain);
		tc->flags |= TF_THREADS;
		spin_unlock(&sched.lock);
	}
}

enum tc_rv tc_threads_wait(struct tc_threads *threads)
{
	struct tc_thread *tc;
	enum tc_rv rv;

	spin_lock(&sched.lock);
	while ((tc = LIST_FIRST(threads))) {
		spin_unlock(&sched.lock);
		rv = tc_thread_wait(tc);
		if (rv != RV_OK)
			return rv;
		spin_lock(&sched.lock);
	}
	spin_unlock(&sched.lock);

	return RV_OK;
}

static void _tc_fd_init(struct tc_fd *tcfd, int fd)
{
	struct epoll_event epe;
	int arg;

	tcfd->fd = fd;
	event_list_init(&tcfd->events);
	tcfd->ep_events = 0;

	/* The fd has to be non blocking */
	arg = fcntl(fd, F_GETFL, NULL);
	if (arg < 0)
		msg_exit(1, "fcntl() failed: %m\n");

	arg |= O_NONBLOCK;

	if (fcntl(fd, F_SETFL, arg) < 0)
		msg_exit(1, "fcntl() failed: %m\n");

	epe.data.ptr = tcfd;
	epe.events = 0;

	if (epoll_ctl(sched.efd, EPOLL_CTL_ADD, fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
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

static void _tc_fd_unregister(struct tc_fd *tcfd, int sync)
{
	struct epoll_event epe = { };

	spin_lock(&tcfd->events.lock);
	if (!CIRCLEQ_EMPTY(&tcfd->events.events))
		msg_exit(1, "event list not empty in tc_unregister_fd()\n");
	spin_unlock(&tcfd->events.lock);

	if (epoll_ctl(sched.efd, EPOLL_CTL_DEL, tcfd->fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");

	if (sync)
		synchronize_world();
}

void tc_unregister_fd(struct tc_fd *tcfd)
{
	_tc_fd_unregister(tcfd, 1);
	free(tcfd);
}

static void _synchronize_world()
{
	pthread_barrier_t *b;
	eventfd_t c;

	/* We might need to wait until the first caller of synchronize_world() finished the malloc. */
	do b = sched.sync_b; while (b == NULL);

	if (atomic_sub_return(1, &sched.sync_cnt) == 0) {
		if (read(sched.sync_fd, &c, sizeof(c)) != sizeof(c))
			msg_exit(1, "read() failed with %m");
		sched.sync_b = NULL;
		spin_unlock(&sched.sync_lock);
	}

	if (pthread_barrier_wait(b) == PTHREAD_BARRIER_SERIAL_THREAD) {
		pthread_barrier_destroy(b);
		free(b);
	}
}

static void synchronize_world()
{
	/* When removing a FD that might fire it is essential to make sure
	   that we do not get any events of that FD in, after this point,
	   since we want to delete the data structure describing that FD */

	eventfd_t c = 1;

	if (atomic_set_if_eq(sched.nr_of_workers, 0, &sched.sync_cnt)) {
		pthread_barrier_t *b;

		spin_lock(&sched.sync_lock);
		b = malloc(sizeof(pthread_barrier_t));
		if (b == NULL)
			msg_exit(1, "failed to malloc() a pthread_barrier_t");

		pthread_barrier_init(b, NULL, sched.nr_of_workers);

		sched.sync_b = b;

		if (write(sched.sync_fd, &c, sizeof(c)) != sizeof(c))
			msg_exit(1, "write() failed with: %m\n");
	}

	_synchronize_world();
}

void tc_mutex_init(struct tc_mutex *m)
{
	int ev_fd;
	atomic_set(&m->count, 0);
	ev_fd = eventfd(0, 0);
	if (ev_fd == -1)
		msg_exit(1, "eventfd() failed with: %m\n");

	_tc_fd_init(&m->read_tcfd, ev_fd);
}

enum tc_rv tc_mutex_lock(struct tc_mutex *m)
{
	enum tc_rv rv;
	eventfd_t c;
	int r;

	if (atomic_add_return(1, &m->count) > 1) {
		while (1) {
			rv = tc_wait_fd(EPOLLIN, &m->read_tcfd);
			if (rv != RV_OK)
				return rv;
			r = read(m->read_tcfd.fd, &c, sizeof(c));
			if (r == sizeof(c))
				break;
			/* fprintf(stderr, "in tc_mutex_lock read() = %d errno = %m\n", r); */
		}

		tc_rearm();
	}

	return RV_OK;
}

void tc_mutex_unlock(struct tc_mutex *m)
{
	eventfd_t c = 1;
	int r;

	r = atomic_sub_return(1, &m->count);

	if (r > 0) {
		if (write(m->read_tcfd.fd, &c, sizeof(c)) != sizeof(c))
			msg_exit(1, "write() failed with: %m\n");
	} else if (r < 0) {
		msg_exit(1, "tc_mutex_unlocked() called on an unlocked mutex\n");
	}
}

enum tc_rv tc_mutex_trylock(struct tc_mutex *m)
{
	if (atomic_set_if_eq(1, 0, &m->count))
		return RV_OK;

	return RV_FAILED;
}

void tc_mutex_unregister(struct tc_mutex *m)
{
	_tc_fd_unregister(&m->read_tcfd, 0);
	close(m->read_tcfd.fd);
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
	wq->nr_waiters = 0;
}

static void _tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	e->ep_events = 0; /* unused */
	spin_lock(&wq->waiters.lock);
	wq->nr_waiters++;
	_add_event(e, &wq->waiters);
	spin_unlock(&wq->waiters.lock);
}

void tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	e->flags = EF_READY;
	_tc_waitq_prepare_to_wait(wq, e);
}

int tc_waitq_finish_wait(struct tc_waitq *wq, struct event *e)
{
	int r = (worker.woken_by_event != e);

	if (r)
		remove_event(e);

	/* Do not expose wakening event/tcfd, user should not tc_rearm() on them */
	worker.woken_by_event = NULL;
	worker.woken_by_tcfd  = NULL;

	return r;
}

void tc_waitq_wait(struct tc_waitq *wq) /* do not use! */
{
	struct event e;

	tc_waitq_prepare_to_wait(wq, &e);
	tc_scheduler();
	tc_waitq_finish_wait(wq, &e);
}

void tc_waitq_wakeup_one(struct tc_waitq *wq)
{
	int wake = 0;
	struct event *e;

	spin_lock(&wq->waiters.lock);
	if (wq->nr_waiters) {
		spin_lock(&sched.immediate.lock);
		e = CIRCLEQ_LAST(&wq->waiters.events);
		CIRCLEQ_REMOVE(&wq->waiters.events, e, e_chain);
		CIRCLEQ_INSERT_HEAD(&sched.immediate.events, e, e_chain);
		spin_unlock(&sched.immediate.lock);
		wq->nr_waiters--;
		wake = 1;
	}
	spin_unlock(&wq->waiters.lock);

	if (wake)
		iwi_immediate();
}

/* Head1 gets all elements, head2 needs to be initialized afterwards */
#define	CIRCLEQ_CONCAT(head1, head2, field) do {			\
	(head1)->cqh_first->field.cqe_prev = (head2)->cqh_last;		\
	(head2)->cqh_last->field.cqe_next = (head1)->cqh_first;		\
	(head1)->cqh_first = (head2)->cqh_first;			\
	(head2)->cqh_first->field.cqe_prev = (void *)head1;		\
} while (/*CONSTCOND*/0)

void tc_waitq_wakeup_all(struct tc_waitq *wq)
{
	int wake = 0;

	spin_lock(&wq->waiters.lock);
	if (wq->nr_waiters) {
		spin_lock(&sched.immediate.lock);
		CIRCLEQ_CONCAT(&sched.immediate.events, &wq->waiters.events, e_chain);
		CIRCLEQ_INIT(&wq->waiters.events);
		spin_unlock(&sched.immediate.lock);
		wake = wq->nr_waiters;
		wq->nr_waiters = 0;
	}
	spin_unlock(&wq->waiters.lock);

	if (wake) {
		if (wake == 1)
			iwi_immediate();
		else /* wake > 1 */
			iwi_immediate(); /* todo wake all */
	}
}

void tc_waitq_unregister(struct tc_waitq *wq)
{
	if (wq->nr_waiters)
		msg_exit(1, "there are still waiters in tc_waitq_unregister()");
}

void tc_signal_init(struct tc_signal *s)
{
	tc_waitq_init(&s->wq);
}

struct event *tc_signal_enable(struct tc_signal *s)
{
	struct event *e;

	e = malloc(sizeof(struct event));
	if (!e)
		msg_exit(1, "malloc of event failed in tc_signal_enable\n");

	/* printf(" (%d) signal_enabled e=%p for %s\n", worker.nr, e, tc_current()->name); */

	e->flags = EF_SIGNAL;
	_tc_waitq_prepare_to_wait(&s->wq, e);

	return e;
}

static void _signal_gets_delivered2(struct event *e)
{
	free(e);
}

void tc_signal_disable(struct tc_signal *s, struct event *e)
{
	remove_event(e);
	free(e);
}

static void signal_cancel_pending()
{
	struct event *e;
	struct tc_thread *tc = tc_current();
	/* Search my own run list */
	/* Search for my in the immedate list */

	spin_lock(&sched.immediate.lock);
	CIRCLEQ_FOREACH(e, &sched.immediate.events, e_chain) {
		if (e->tc == tc && e->flags == EF_SIGNAL) {
			_remove_event(e, &sched.immediate);
			free(e);
		}
	}
	spin_unlock(&sched.immediate.lock);

	spin_lock(&tc->pending.lock);
	CIRCLEQ_FOREACH(e, &tc->pending.events, e_chain) {
		if (e->flags == EF_SIGNAL) {
			_remove_event(e, &tc->pending);
			free(e);
		}
	}
	spin_unlock(&tc->pending.lock);
}


void tc_signal_unregister(struct tc_signal *s)
{
	tc_waitq_unregister(&s->wq);
}

void tc_signal_fire(struct tc_signal *s)
{
	tc_waitq_wakeup_all(&s->wq);
}

enum tc_rv tc_sleep(int clockid, time_t sec, long nsec)
{
	struct itimerspec ts;
	struct tc_fd tcfd;
	enum tc_rv rv;
	int fd;

	ts.it_value.tv_sec = sec;
	ts.it_value.tv_nsec = nsec;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;

	fd = timerfd_create(clockid, 0);
	if (fd == -1)
		msg_exit(1, "timerfd_create with %m\n");

	_tc_fd_init(&tcfd, fd);
	timerfd_settime(fd, 0, &ts, NULL);
	rv = tc_wait_fd(EPOLLIN, &tcfd);
	_tc_fd_unregister(&tcfd, rv == RV_INTR);
	close(tcfd.fd);
	return rv;
}
