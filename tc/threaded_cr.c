#define _GNU_SOURCE /* for asprintf() */

#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "compat.h"
#include "atomic.h"
#include "spinlock.h"
#include "coroutines.h"
#include "threaded_cr.h"

#define DEFAULT_STACK_SIZE (1024 * 16)

struct tc_signal_sub {
	LIST_ENTRY(tc_signal_sub) se_chain;
	struct event event;
};

enum thread_flags {
	TF_THREADS =   1 << 0, /* is on threads chain*/
	TF_RUNNING =   1 << 1,
	TF_FREE_NAME = 1 << 2,
};

struct tc_thread {
	char *name;		/* Leave that first, for debugging spinlocks */
	LIST_ENTRY(tc_thread) tc_chain;      /* list of all threads*/
	LIST_ENTRY(tc_thread) threads_chain; /* list of thrads created with one call to tc_thread_pool_new() */
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
	int sync_fd;               /* IWI event fd to synchronize all workers */
	int immediate_fd;          /* IWI immediate */
	atomic_t sync_cnt;
	pthread_barrier_t *sync_b;
	spinlock_t sync_lock;
	atomic_t sleeping_workers;
};

#ifdef WAIT_DEBUG
#undef tc_sched_yield
#undef tc_wait_fd
#undef tc_mutex_lock
#undef tc_thread_wait
#undef tc_waitq_wait
#undef tc_thread_pool_wait
#undef tc_sleep

__thread char *_caller_file = "untracked tc_scheduler() call";
__thread int _caller_line = 0;
#endif

static struct scheduler sched;
static struct tc_thread *tc_main;
static __thread struct worker_struct worker;

static void _signal_gets_delivered(struct event *e);
static void signal_cancel_pending();
static void _synchronize_world();
static void synchronize_world();
static void iwi_immediate();

diagnostic_fn diagnostic = NULL;

int fprintf_stderr(const char *fmt, va_list ap)
{
	return vfprintf(stderr, fmt, ap);
}

void tc_set_diagnostic_fn(diagnostic_fn f)
{
	diagnostic = f;
}

void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
void msg_exit(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	diagnostic(fmt, ap);
	va_end(ap);

	exit(code);
}

void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	diagnostic(fmt, ap);
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
static struct event *matching_event(__uint32_t em, struct events *es)
{
	struct event *e;
	/* prefer events of threads that prefer this worker */
	CIRCLEQ_FOREACH(e, es, e_chain) {
		if (em & e->ep_events && e->tc->worker_nr == worker.nr)
			return e;
	}
	CIRCLEQ_FOREACH(e, es, e_chain) {
		if (em & e->ep_events)
			return e;
	}
	return NULL;
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

static void remove_event(struct event *e)
{
	struct event_list *el;

	/* The event can be moved to an other list while we try to grab
	   the list lock... */
	while(1) {
		do el = e->el; while (el == NULL);
		spin_lock(&el->lock);
		if (el == e->el)
			break;
		spin_unlock(&el->lock);
	}

	_remove_event(e, el);
	spin_unlock(&el->lock);
}

/* must_hold el->lock */
static void _add_event(struct event *e, struct event_list *el, struct tc_thread *tc)
{
	atomic_inc(&tc->refcnt);
	e->tc = tc;
	e->el = el;

	CIRCLEQ_INSERT_TAIL(&el->events, e, e_chain);
}

int add_event_fd(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_fd *tcfd)
{
	int rv;
 	e->ep_events = ep_events;
 	e->flags = flags;

	spin_lock(&tcfd->events.lock);
	_add_event(e, &tcfd->events, tc_current());
	rv = arm(tcfd);
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

void tc_thread_free(struct tc_thread *tc)
{
	spin_lock(&tc->running); /* Make sure it has reached switch_to(), after posting EF_EXITING */
	tc_waitq_unregister(&tc->exit_waiters);
	cr_delete(tc->cr);
	tc->cr = NULL;
	if (tc->flags & TF_FREE_NAME)
		free(tc->name);
	free(tc);
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
		if (!(new->flags & TF_THREADS))
			new->worker_nr = nr;
	}
	_switch_to(new);
}

static void arm_immediate(int op)
{
	struct epoll_event epe;

	epe.data.u32 = IWI_IMMEDIATE;
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

	spin_lock(&sched.immediate.lock);
	worker.woken_by_tcfd  = NULL;
	CIRCLEQ_FOREACH(e, &sched.immediate.events, e_chain) {
		if (nr && e->tc->worker_nr != nr)
			continue;
		_remove_event(e, &sched.immediate);
		tc = run_or_queue(e);
		if (!tc) {
			e = CIRCLEQ_FIRST(&sched.immediate.events);
			continue;
		}
		spin_unlock(&sched.immediate.lock);
		switch (e->flags) {
		case EF_READY:
		case EF_SIGNAL:
			if (!CIRCLEQ_EMPTY(&sched.immediate.events))
				iwi_immediate(); /* More work available, wakeup an worker */
			switch_to(tc);
			return 1;
		case EF_EXITING:
			tc_thread_free(e->tc);
			spin_lock(&sched.immediate.lock);
			e = CIRCLEQ_FIRST(&sched.immediate.events);
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
	while (_run_immediate(worker.nr) || _run_immediate(0))
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

	if (atomic_read(&sched.sleeping_workers))
		_iwi_immediate();
}

void tc_sched_yield()
{
	struct tc_thread *tc = tc_current();
	struct event e;

	add_event_cr(&e, 0, EF_READY, tc);
	tc_scheduler();
	if (worker.woken_by_event != &e)
		remove_event(&e);
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

		atomic_inc(&sched.sleeping_workers);
		do {
			er = epoll_wait(sched.efd, &epe, 1, -1);
		} while (er < 0 && errno == EINTR);
		atomic_dec(&sched.sleeping_workers);

		if (er < 0)
			msg_exit(1, "epoll_wait() failed with: %m\n");

		switch (epe.data.u32) {
		case IWI_SYNC:
			_synchronize_world();
			continue;
		case IWI_IMMEDIATE:
			rearm_immediate();
			/* run_immediate(); at top of loop. */
			continue;
		}

		tcfd = (struct tc_fd *)epe.data.ptr;

		spin_lock(&tcfd->events.lock);
		tcfd->ep_events = -1; /* recalc them */

		/* in case of an error condition, wake all waiters on the FD,
		   no matter what they are waiting for: Setting all bits. */
		if (epe.events & EPOLLERR || epe.events & EPOLLHUP) {
			atomic_set(&tcfd->err_hup, 1);
			epe.events = -1;
		}

		e = matching_event(epe.events, &tcfd->events.events);
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
				msg("Removed fd %d from epoll set because of ERR/HUP\n", tcfd->fd);
			}

			spin_unlock(&tcfd->events.lock);
			continue;
		}

		worker.woken_by_tcfd = tcfd;
		_remove_event(e, &tcfd->events);
		tc = run_or_queue(e);

		spin_unlock(&tcfd->events.lock);

		if (tc)
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
	worker.main_thread.worker_nr = i;
	/* LIST_INSERT_HEAD(&sched.threads, &worker.main_thread, tc_chain); */

	asprintf(&worker.sched_p2.name, "sched_%d", i);
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
}

void tc_init()
{
	struct epoll_event epe;

	if (diagnostic == NULL)
		tc_set_diagnostic_fn(fprintf_stderr);

	event_list_init(&sched.immediate);
	LIST_INIT(&sched.threads);
	spin_lock_init(&sched.lock);

	spin_lock_init(&sched.sync_lock);
	atomic_set(&sched.sync_cnt, 0);
	atomic_set(&sched.sleeping_workers, 0);
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
	int nr = (int)(long)arg;

	tc_worker_init(nr);
	tc_thread_wait(tc_main); /* calls tc_scheduler() */

	_iwi_immediate(); /* All other workers need to get woken UNCONDITINALLY
			     So that the complete program can terminate */
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

enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd)
{
	struct event e;
	int r;

	if (atomic_read(&tcfd->err_hup))
		return RV_FAILED;
	if (add_event_fd(&e, ep_events | EPOLLONESHOT, EF_READY, tcfd))
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

	tc->cr = cr_create(tc_setup, func, data, DEFAULT_STACK_SIZE);
	if (!tc->cr)
		goto fail3;

	cr_set_uptr(tc->cr, (void *)tc);
	tc->name = name;
	tc_waitq_init(&tc->exit_waiters);
	atomic_set(&tc->refcnt, 0);
	spin_lock_init(&tc->running);
	tc->flags = 0;
	event_list_init(&tc->pending);
	tc->worker_nr = -1;

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

void tc_thread_pool_new(struct tc_thread_pool *threads, void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;
	int i;
	char *ename;

	LIST_INIT(threads);
	for (i = 0; i < sched.nr_of_workers; i++) {
		asprintf(&ename, name, i);
		tc = _tc_thread_new(func, data, ename);
		if (!tc)
			continue;
		tc->flags |= TF_THREADS | TF_FREE_NAME;
		tc->worker_nr = i;
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

static void _tc_fd_init(struct tc_fd *tcfd, int fd)
{
	struct epoll_event epe;
	int arg;

	tcfd->fd = fd;
	event_list_init(&tcfd->events);
	tcfd->ep_events = 0;
	atomic_set(&tcfd->err_hup, 0);

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

	/* ignoring return value of epoll_ctl() here on intention */
	epoll_ctl(sched.efd, EPOLL_CTL_DEL, tcfd->fd, &epe);

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
	atomic_set(&m->count, 0);
	tc_waitq_init(&m->wq);
}

enum tc_rv tc_mutex_lock(struct tc_mutex *m)
{
	struct event e;

	if (atomic_set_if_eq(1, 0, &m->count))
		return RV_OK;

	tc_waitq_prepare_to_wait(&m->wq, &e);
	if (atomic_add_return(1, &m->count) > 1) {
		tc_scheduler();
		return tc_waitq_finish_wait(&m->wq, &e) ? RV_INTR : RV_OK;
	} else {
		tc_waitq_finish_wait(&m->wq, &e);
		return RV_OK;
	}
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
	int r = (worker.woken_by_event != e);

	worker.woken_by_event = NULL;
	if (r)
		remove_event(e);

	return r;
}

void tc_waitq_wait(struct tc_waitq *wq)
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

	spin_lock(&sched.immediate.lock);
	spin_lock(&wq->waiters.lock);
	if (!CIRCLEQ_EMPTY(&wq->waiters.events)) {
		e = CIRCLEQ_LAST(&wq->waiters.events);
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

	if (wake)
		iwi_immediate(); /* _run_immediate() will wakeup more if necessary */
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

struct tc_signal_sub *tc_signal_subscribe(struct tc_signal *s)
{
	struct tc_signal_sub *ss;

	ss = malloc(sizeof(struct tc_signal_sub));
	if (!ss)
		msg_exit(1, "malloc of tc_signal_sub failed in tc_signal_subscribe\n");

	/* printf(" (%d) signal_enabled e=%p for %s\n", worker.nr, e, tc_current()->name); */

	spin_lock(&s->wq.waiters.lock);
	LIST_INSERT_HEAD(&s->sss, ss, se_chain);
	spin_unlock(&s->wq.waiters.lock);

	ss->event.signal = s;
	ss->event.flags = EF_SIGNAL;
	_tc_waitq_prepare_to_wait(&s->wq, &ss->event, tc_current());

	return ss;
}

void tc_signal_unsubscribe(struct tc_signal *s, struct tc_signal_sub *ss)
{
	remove_event(&ss->event);
	spin_lock(&s->wq.waiters.lock);
	LIST_REMOVE(ss, se_chain);
	spin_unlock(&s->wq.waiters.lock);
	free(ss);
}

static void _cancel_signal(struct event *e, struct event_list *el)
{
	struct tc_signal_sub *ss;

	_remove_event(e, &sched.immediate);
	ss = container_of(e, struct tc_signal_sub, event);
	spin_lock(&e->signal->wq.waiters.lock);
	LIST_REMOVE(ss, se_chain);
	spin_unlock(&e->signal->wq.waiters.lock);
	free(e);
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

#ifdef WAIT_DEBUG


/* This is a handy function to be called from within gdb */

void tc_dump_threads(void)
{
	struct tc_thread *t;
	int i=0;

	for (t=LIST_FIRST(&sched.threads);t!=NULL;t=LIST_NEXT(t, tc_chain)) {
		fprintf(stderr, "Thread %s(%p) waiting at %s:%d\n", t->name, t, t->sleep_file, t->sleep_line);
		i++;
	}
	fprintf(stderr, "%d thread(s) listed.\n", i);
}

#endif
