#define _GNU_SOURCE /* for asprintf() */

#include <sys/epoll.h>
#include <sys/eventfd.h>
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

struct waitq_ev {
	LIST_ENTRY(waitq_ev) chain;
	atomic_t waiters;   /* Number of tc_threads sleeping on the wq */
	atomic_t flying;
	struct tc_fd read_tcfd;
};

LIST_HEAD(waitq_evs, waitq_ev);

enum thread_state {
	SLEEPING,
	RUNNING,
	SIGNALLED, /* Got signal while was running*/
	EXITING
};

struct tc_thread {
	char *name;		/* Leafe that first, for debugging spinlocks */
	LIST_ENTRY(tc_thread) chain;         /* list of all threads*/
	LIST_ENTRY(tc_thread) threads_chain; /* list of thrads created with one call to tc_threads_new() */
	struct coroutine *cr;
	struct tc_waitq exit_waiters;
	atomic_t refcnt;
	atomic_t state;		/* See enum thread_state */
	spinlock_t running;
	unsigned int is_on_threads_chain; /* TODO: Remove the threads_chain, and this field. Make it sane. */
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

struct scheduler {
	spinlock_t lock;           /* protects the threads and the immediate lists */
	struct tc_threads threads;
	struct events immediate;
	struct waitq_evs wevs;     /* List of 'flying' signals and waitq wakeup events */
	spinlock_t wevs_lock;      /* protects the wevs list */
	int nr_of_workers;
	int efd;                   /* epoll fd */
	int sync_fd;               /* event fd to synchronize all workers */
	atomic_t sync_cnt;
	pthread_barrier_t *sync_b;
	spinlock_t sync_lock;
};

static struct scheduler sched;
static struct tc_thread *tc_main;
static __thread struct worker_struct worker;

static void _signal_gets_delivered(struct tc_fd *tcfd, struct event *e);
static enum tc_rv _signal_cancel(struct waitq_ev *we);
static struct waitq_ev *tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e);
static void _tc_waitq_finish_wait(struct tc_waitq *wq, struct waitq_ev *we);
static void tc_waitq_free_wait_ev(struct waitq_ev *we, int sync);
static void _synchronize_world();
static void synchronize_world();
static void _tc_waitq_unregister(struct tc_waitq *wq, int sync);

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
	LIST_FOREACH(e, es, chain) {
		em |= e->ep_events | (e->flags == EF_ONE ? EPOLLONESHOT : 0);
	}

	return em;
}

/* must_hold tcfd->lock */
static struct event *matching_event(__uint32_t em, struct events *es)
{
	struct event *e;
	LIST_FOREACH(e, es, chain) {
		if (em & e->ep_events)
			return e;
	}
	return NULL;
}

/* must_hold tcfd->lock */
static void arm(struct tc_fd *tcfd)
{
	struct epoll_event epe;

	epe.events = calc_epoll_event_mask(&tcfd->events);
	if (epe.events == tcfd->ep_events)
		return;

	epe.data.ptr = tcfd;
	tcfd->ep_events = epe.events;

	if (epoll_ctl(sched.efd, EPOLL_CTL_MOD, tcfd->fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
}

/* must_hold tcfd->lock */
static inline void remove_event(struct event *e)
{
	LIST_REMOVE(e, chain);
	atomic_dec(&e->tc->refcnt);
}

void add_event_fd(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_fd *tcfd)
{
	atomic_inc(&tc_current()->refcnt);
	e->tc = tc_current();
 	e->ep_events = ep_events;
 	e->flags = flags;

	if (flags != EF_ALL_FREE)
		atomic_set_if_eq(SLEEPING, RUNNING, &tc_current()->state);

	spin_lock(&tcfd->lock);
	LIST_INSERT_HEAD(&tcfd->events, e, chain);
	arm(tcfd);
	spin_unlock(&tcfd->lock);
}

static void add_event_cr(struct event *e, __uint32_t ep_events, enum tc_event_flag flags, struct tc_thread *tc)
{
	atomic_inc(&tc->refcnt);
	e->tc = tc;
 	e->ep_events = ep_events;
 	e->flags = flags;

	atomic_set_if_eq(SLEEPING, RUNNING, &tc_current()->state);

	spin_lock(&sched.lock);
	LIST_INSERT_HEAD(&sched.immediate, e, chain);
	spin_unlock(&sched.lock);
}

void remove_event_fd(struct event *e, struct tc_fd *tcfd)
{
	spin_lock(&tcfd->lock);
	remove_event(e);
	spin_unlock(&tcfd->lock);
}

void tc_thread_free(struct tc_thread *tc)
{
	spin_lock(&tc->running); /* Make sure it has reached switch_to(), after posting EF_EXITING */
	_tc_waitq_unregister(&tc->exit_waiters, 0);
	cr_delete(tc->cr);
	free(tc);
}

static void switch_to(struct tc_thread *new)
{
	struct tc_thread *previous;

	/* previous = tc_current();
	   printf(" (%d) switch: %s -> %s\n", worker.nr, previous->name, new->name); */

	/* It can happen that the stack frame we want to switch to is still active,
	   in a rare condition: A tc_thread reads a few byte from an fd, and calls
	   tc_wait_fd(), although there is still input available. That call enables
	   the epoll-event again. Before that tc_thread makes it to the scheduler
	   (and epoll_wait), an other worker returns from its epoll_wait call and
	   switches to the same tc_thread again. That causes bad stack corruption
	   of course.
	   To circumvent that we spin here until the tc_thread is no longer running */
	spin_lock(&new->running);

	/* If it was sleeping it is running now. If it was signalled it stays signalled */
	atomic_set_if_eq(RUNNING, SLEEPING, &new->state);

	if (atomic_read(&new->state) == EXITING) {
		/* With some bad timing it can happen that a signal gets already
		   delivered while a thread decides to exit. */
		spin_unlock(&new->running);
		return;
	}

	cr_call(new->cr);

	previous = (struct tc_thread *)cr_uptr(cr_caller());
	spin_unlock(&previous->running);
}

void tc_scheduler(void)
{
	struct event *e;

	if (atomic_read(&tc_current()->state) == EXITING) /* avoid deadlocks! */
		switch_to(&worker.sched_p2);

	if (atomic_set_if_eq(RUNNING, SIGNALLED, &tc_current()->state)) {
		worker.woken_by_event = NULL; /* compares != to any real address */
		worker.woken_by_tcfd  = NULL;

		return;
	}

	spin_lock(&sched.lock);
	e = LIST_FIRST(&sched.immediate);
	while (e) {
		worker.woken_by_event = e;
		worker.woken_by_tcfd  = NULL;
		if (e->tc != tc_current()) {
			remove_event(e);
			spin_unlock(&sched.lock);
			switch (e->flags) {
			case EF_READY:
				switch_to(e->tc);
				return;
			case EF_EXITING:
				tc_thread_free(e->tc);
				spin_lock(&sched.lock);
				e = LIST_FIRST(&sched.immediate);
				continue;
			default:
				msg_exit(1, "Wrong e->flags in immediate list\n");
			}
		}
		e = LIST_NEXT(e, chain);
	}
	spin_unlock(&sched.lock);

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

		tcfd = (struct tc_fd *)epe.data.ptr;

		if (!tcfd) {
			_synchronize_world();
			continue;
		}

		spin_lock(&tcfd->lock);
		tcfd->ep_events = -1; /* recalc them */

		e = matching_event(epe.events, &tcfd->events);
		if (!e) {
			/* That can happen if the event_fd of a signal wakes us up just
			   after _signal_cancel was called */
			spin_unlock(&tcfd->lock);
			continue;
		}

		tc = e->tc;
		remove_event(e);

		spin_unlock(&tcfd->lock);

		worker.woken_by_event = e;
		worker.woken_by_tcfd = tcfd;

		if (e->flags == EF_ALL_FREE) {
			_signal_gets_delivered(tcfd, e);

			if (atomic_swap(&tc->state, SIGNALLED) >= RUNNING)
				continue;

			worker.woken_by_tcfd = NULL; /* Do not expose the tcfd in case it was a signal. */
		}

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
	atomic_set(&worker.main_thread.state, RUNNING);
	/* LIST_INSERT_HEAD(&sched.threads, &worker.main_thread, chain); */

	asprintf(&worker.sched_p2.name, "sched_%d", i);
	worker.sched_p2.cr = cr_create(scheduler_part2, NULL, DEFAULT_STACK_SIZE);
	if (!worker.sched_p2.cr)
		msg_exit(1, "allocation of worker.sched_p2 failed\n");

	cr_set_uptr(worker.sched_p2.cr, &worker.sched_p2);
	tc_waitq_init(&worker.sched_p2.exit_waiters);
	atomic_set(&worker.sched_p2.refcnt, 0);
	spin_lock_init(&worker.sched_p2.running);
	atomic_set(&worker.sched_p2.state, SLEEPING);
}

void tc_init()
{
	struct epoll_event epe;

	LIST_INIT(&sched.immediate);
	LIST_INIT(&sched.threads);
	LIST_INIT(&sched.wevs);
	spin_lock_init(&sched.lock);
	spin_lock_init(&sched.wevs_lock);

	spin_lock_init(&sched.sync_lock);
	atomic_set(&sched.sync_cnt, 0);
	sched.sync_b = NULL;
	sched.sync_fd = eventfd(0, 0);
	if (sched.sync_fd == -1)
		msg_exit(1, "eventfd() failed with: %m\n");

	sched.efd = epoll_create(1);
	if (sched.efd < 0)
		msg_exit(1, "epoll_create failed with %m\n");

	epe.data.ptr = NULL;
	epe.events = EPOLLIN;

	if (epoll_ctl(sched.efd, EPOLL_CTL_ADD, sched.sync_fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
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
		spin_lock(&worker.woken_by_tcfd->lock);
		arm(worker.woken_by_tcfd);
		spin_unlock(&worker.woken_by_tcfd->lock);
	}
}

enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd)
{
	struct event e;

	add_event_fd(&e, ep_events, EF_ONE, tcfd);
	tc_scheduler();
	if (worker.woken_by_event != &e) {
		remove_event_fd(&e, tcfd);
		return RV_INTR;
	}
	return RV_OK;
}

void tc_die()
{
	struct event e;
	struct tc_thread *tc = tc_current();

	/* printf(" (%d) exiting: %s\n", worker.nr, tc->name); */

	spin_lock(&sched.lock);
	LIST_REMOVE(tc, chain);
	if (tc->is_on_threads_chain)
		LIST_REMOVE(tc, threads_chain);
	spin_unlock(&sched.lock);

	tc_waitq_wakeup(&tc->exit_waiters);

	if (atomic_read(&tc->refcnt) > 0) {
		struct waitq_ev *we;
		/* Maybe there is an signal/waitq wakeup for me, lets try to find that: */
		spin_lock(&sched.wevs_lock);
		LIST_FOREACH(we, &sched.wevs, chain) {
			if (_signal_cancel(we) == RV_OK) {
				if (atomic_sub_return(1, &we->waiters) == 0) {
					LIST_REMOVE(we, chain);
					atomic_set(&we->flying, 0);
					tc_waitq_free_wait_ev(we, 1);
				}
			}
		}
		spin_unlock(&sched.wevs_lock);
		if (atomic_read(&tc->refcnt) > 0) {
			msg_exit(1, "tc_die(%s): refcnt = %d. Signals still enabled?\n",
				 tc->name, atomic_read(&tc->refcnt));
		}
	}

	atomic_set(&tc->state, EXITING); /* We will not get woken by sigs ;) */
	add_event_cr(&e, 0, EF_EXITING, tc);  /* The scheduler will free me */
	tc_scheduler();
	/* Not reached. */
	msg_exit(1, "tc_scheduler() returned in tc_die() [state = %d]\n", atomic_read(&tc->state));
}

void tc_setup(void *data)
{
	struct setup_info *i = (struct setup_info *)data;
	struct tc_thread *previous;

	previous = (struct tc_thread *)cr_uptr(cr_caller());
	spin_unlock(&previous->running);

	i->func(i->data);

	tc_die();
}

struct tc_thread *tc_thread_new(void (*func)(void *), void *data, char* name)
{
	struct tc_thread *tc;
	struct event e1, e2;
	struct setup_info i;

	i.func = func;
	i.data = data;

	tc = malloc(sizeof(struct tc_thread));
	if (!tc)
		return NULL;

	tc->cr = cr_create(tc_setup, &i, DEFAULT_STACK_SIZE);
	if (!tc->cr) {
		free(tc);
		return NULL;
	}
	cr_set_uptr(tc->cr, (void *)tc);
	tc->name = name;
	tc_waitq_init(&tc->exit_waiters);
	atomic_set(&tc->refcnt, 0);
	spin_lock_init(&tc->running);
	atomic_set(&tc->state, SLEEPING);
	tc->is_on_threads_chain = 0;

	spin_lock(&sched.lock);
	LIST_INSERT_HEAD(&sched.threads, tc, chain);
	spin_unlock(&sched.lock);
	add_event_cr(&e1, 0, EF_READY, tc);           /* removed in the tc_scheduler */
	add_event_cr(&e2, 0, EF_READY, tc_current()); /* removed in the tc_scheduler */
	tc_scheduler(); /* child first, policy. */

	return tc;
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
		tc->is_on_threads_chain = 1;
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
	LIST_INIT(&tcfd->events);
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

	spin_lock_init(&tcfd->lock);

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

	spin_lock(&tcfd->lock);
	if (!LIST_EMPTY(&tcfd->events))
		msg_exit(1, "event list not empty in tc_unregister_fd()\n");
	spin_unlock(&tcfd->lock);

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

	b = sched.sync_b;
	if (atomic_sub_return(1, &sched.sync_cnt) == 0) {
		/* printf("before read\n"); */
		if (read(sched.sync_fd, &c, sizeof(c)) != sizeof(c))
			msg_exit(1, "read() failed with %m");
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
		spin_lock(&sched.sync_lock);
		sched.sync_b = malloc(sizeof(pthread_barrier_t));
		if (sched.sync_b == NULL)
			msg_exit(1, "failed to malloc() a pthread_barrier_t");

		pthread_barrier_init(sched.sync_b, NULL, sched.nr_of_workers);

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

	LIST_FOREACH(tc, &sched.threads, chain) {
		if (tc == look_for)
			return RV_OK;
	}
	return RV_THREAD_NA;
}

enum tc_rv tc_thread_wait(struct tc_thread *wait_for)
{
	struct waitq_ev *we;
	struct event e;
	enum tc_rv rv;

	spin_lock(&sched.lock);
	rv = _thread_valid(wait_for);  /* wait_for might have already exited */
	if (rv == RV_OK)
		we = tc_waitq_prepare_to_wait(&wait_for->exit_waiters, &e);

	spin_unlock(&sched.lock);
	if (rv == RV_THREAD_NA)
		return rv;

	tc_scheduler();

	/* Do not pass wait_for->exit_waiters, since wait_for might be already freed. */
	if (tc_waitq_finish_wait(NULL, &e, we))
		rv = RV_INTR;

	return rv;
}

void tc_waitq_init(struct tc_waitq *wq)
{
	spin_lock_init(&wq->lock);
	wq->active = NULL;
}

/* must_hold wq->lock, must call add_event_fd() */
struct waitq_ev *__tc_waitq_prepare_to_wait(struct tc_waitq *wq)
{
	struct waitq_ev *we;
	int ev_fd;

	if (!wq->active) {
		we = malloc(sizeof(struct waitq_ev));
		if (!we)
			msg_exit(1, "malloc of waitq_ev failed in tc_waitq_init\n");

		ev_fd = eventfd(0, 0);
		if (ev_fd == -1)
			msg_exit(1, "eventfd() failed with: %m\n");

		_tc_fd_init(&we->read_tcfd, ev_fd);
		atomic_set(&we->waiters, 0);
		atomic_set(&we->flying, 0);
		wq->active = we;
	}
	we = wq->active;
	atomic_inc(&we->waiters);

	return we;
}

/* must_hold wq->lock */
struct waitq_ev *_tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	struct waitq_ev *we;

	we = __tc_waitq_prepare_to_wait(wq);
	add_event_fd(e, EPOLLIN, EF_ALL, &we->read_tcfd);

	return we;
}

static struct waitq_ev *tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	struct waitq_ev *we;

	spin_lock(&wq->lock);
	we = _tc_waitq_prepare_to_wait(wq, e);
	spin_unlock(&wq->lock);

	return we;
}


int tc_waitq_finish_wait(struct tc_waitq *wq, struct event *e, struct waitq_ev *we)
{
	int r = (worker.woken_by_event != e);

	if (r)
		remove_event_fd(e, &we->read_tcfd);

	/* Do not expose wakening event/tcfd, user should not tc_rearm() on them */
	worker.woken_by_event = NULL;
	worker.woken_by_tcfd  = NULL;

	_tc_waitq_finish_wait(wq, we);

	return r;
}

static void tc_waitq_free_wait_ev(struct waitq_ev *we, int sync)
{
	_tc_fd_unregister(&we->read_tcfd, sync);
	close(we->read_tcfd.fd);
	free(we);
}

static void _tc_waitq_finish_wait(struct tc_waitq *wq, struct waitq_ev *we)
{
	int f;
	eventfd_t c;
	int sync = 0;

	if (atomic_sub_return(1, &we->waiters) == 0) {
		if (atomic_read(&we->flying)) {
			spin_lock(&sched.wevs_lock);
			LIST_REMOVE(we, chain);
			atomic_set(&we->flying, 0);
			spin_unlock(&sched.wevs_lock);
			sync = 1;
		}

		f = 1;
		if (wq) {
			spin_lock(&wq->lock);
			if (!wq->active) {
				read(we->read_tcfd.fd, &c, sizeof(c));
				/* Do not care if that read fails. We can finish_wait even if the
				   we where never woken up... */
				wq->active = we;
				f = 0;
			}
			spin_unlock(&wq->lock);
		}

		if (f)
			tc_waitq_free_wait_ev(we, sync);
	}
}

void tc_waitq_wait(struct tc_waitq *wq) /* do not use! */
{
	struct waitq_ev *we;
	struct event e;

	we = tc_waitq_prepare_to_wait(wq, &e);
	tc_scheduler();
	tc_waitq_finish_wait(wq, &e, we);
}

void tc_waitq_wakeup(struct tc_waitq *wq)
{
	struct waitq_ev *we = NULL;
	eventfd_t c = 1;

	spin_lock(&wq->lock);
	if (wq->active && atomic_read(&wq->active->waiters)) {
		we = wq->active;
		wq->active = NULL;
		spin_lock(&sched.wevs_lock);
		LIST_INSERT_HEAD(&sched.wevs, we, chain);
		atomic_set(&we->flying, 1);
		spin_unlock(&sched.wevs_lock);
	}
	spin_unlock(&wq->lock);

	if (we) {
		if (write(we->read_tcfd.fd, &c, sizeof(c)) != sizeof(c))
			msg_exit(1, "write() failed with: %m\n");
	}
}

void tc_waitq_unregister(struct tc_waitq *wq)
{
	_tc_waitq_unregister(wq, 1);
}

static void _tc_waitq_unregister(struct tc_waitq *wq, int sync)
{
	struct waitq_ev *we;

	spin_lock(&wq->lock);
	we = wq->active;
	if (we) {
		if (atomic_read(&we->waiters))
			msg_exit(1, "there are still waiters in tc_waitq_unregister()");
		wq->active = NULL;
		tc_waitq_free_wait_ev(we, sync);
	}
	spin_unlock(&wq->lock);
}

void tc_signal_init(struct tc_signal *s)
{
	tc_waitq_init(&s->wq);
}

void tc_signal_enable(struct tc_signal *s)
{
	struct waitq_ev *we;
	struct event *e;

	e = malloc(sizeof(struct event));
	if (!e)
		msg_exit(1, "malloc of event failed in tc_signal_enable\n");

	/* printf(" (%d) signal_enabled e=%p for %s\n", worker.nr, e, tc_current()->name); */

	spin_lock(&s->wq.lock);
	we = __tc_waitq_prepare_to_wait(&s->wq);
	add_event_fd(e, EPOLLIN, EF_ALL_FREE, &we->read_tcfd);
	spin_unlock(&s->wq.lock);
}

void _signal_gets_delivered(struct tc_fd *tcfd, struct event *e)
{
	struct waitq_ev *we;

	/* printf(" (%d) signal_gets_delivered e=%p\n", worker.nr, e); */

	we = container_of(tcfd, struct waitq_ev, read_tcfd);

	_tc_waitq_finish_wait(NULL, we);
	free(e);
}


enum tc_rv _signal_cancel(struct waitq_ev *we)
{
	struct event *e;
	enum tc_rv rv;

	spin_lock(&we->read_tcfd.lock);
	LIST_FOREACH(e, &we->read_tcfd.events, chain) {
		if (e->tc == tc_current()) {
			rv = RV_OK;
			remove_event(e);
			free(e);
			/* clear mask before arm ? */
			arm(&we->read_tcfd);
			goto found;
		}
	}
	rv = RV_THREAD_NA;
 found:
	spin_unlock(&we->read_tcfd.lock);

	return rv;
}


void tc_signal_disable(struct tc_signal *s)
{
	struct waitq_ev *we;
	struct tc_waitq *wq = &s->wq;

	spin_lock(&wq->lock);
	we = wq->active;

	if (!we) {
		spin_unlock(&wq->lock);
		return;
	}

	if (_signal_cancel(we) == RV_OK)
		atomic_dec(&we->waiters); /* Might leaves a we with waiters == 0 in wq->active */

	spin_unlock(&wq->lock);
}

void tc_signal_unregister(struct tc_signal *s)
{
	tc_waitq_unregister(&s->wq);
}

void tc_signal_fire(struct tc_signal *s)
{
	tc_waitq_wakeup(&s->wq);
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
