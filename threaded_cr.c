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

#include "atomic.h"
#include "coroutines.h"
#include "threaded_cr.h"

#define DEFAULT_STACK_SIZE (1024 * 16)

struct waitq_ev {
	atomic_t count;
	int pipe_fd[2];
	struct tc_fd read_tcfd;
};

struct tc_thread {
	LIST_ENTRY(tc_thread) chain;         /* list of all threads*/
	LIST_ENTRY(tc_thread) threads_chain; /* list of thrads created with one call to tc_threads_new() */
	char *name;
	struct coroutine *cr;
	struct tc_waitq exit_waiters;
	atomic_t refcnt;
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
	struct epoll_event epe;
};

struct worker_start_info {
	int nr;
	struct tc_thread *tc;
	void *data;
	void (*func)(void *);
	char *name;
	pthread_mutex_t mutex;
};

struct scheduler {
	spinlock_t lock;     /* protects the threds and the immediate lists */
	struct tc_threads threads; /* currently unused. */
	struct events immediate;
	int nr_of_workers;
	int efd;
};

static struct scheduler sched;
static __thread struct worker_struct worker;

void _signal_gets_delivered(struct tc_fd *tcfd, struct event *e);

static inline struct tc_thread *tc_current()
{
	return (struct tc_thread *)cr_uptr(cr_current());
}

static inline void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
static inline void msg_exit(int code, const char *fmt, ...)
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
	cr_delete(tc->cr);
	tc_waitq_unregister(&tc->exit_waiters);
	free(tc);
}

static void switch_to(struct tc_thread *new)
{
	struct tc_thread *previous;

	/* previous = tc_current();
	   printf(" switch(%d): %s -> %s\n", worker.nr, previous->name, new->name); */

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

void tc_scheduler(void)
{
	struct tc_thread *tc;
	struct event *e;

	spin_lock(&sched.lock);
	e = LIST_FIRST(&sched.immediate);
	while (e) {
		worker.woken_by_event = e;
		worker.woken_by_tcfd  = NULL;
		if (e->tc != tc_current()) {
			remove_event(e);
			switch (e->flags) {
			case EF_READY:
				spin_unlock(&sched.lock);
				switch_to(e->tc);
				return;
			case EF_EXITING:
				tc = e->tc;
				e = LIST_NEXT(e, chain); /* e was within the stack frame */
				tc_thread_free(tc);      /* that gets released here */
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
			er = epoll_wait(sched.efd, &worker.epe, 1, -1);
		} while (er < 0 && errno == EINTR);
		if (er < 0)
			msg_exit(1, "epoll_wait() failed with: %m\n");

		tcfd = (struct tc_fd *)worker.epe.data.ptr;

		spin_lock(&tcfd->lock);
		tcfd->ep_events = -1; /* recalc them */

		e = matching_event(worker.epe.events, &tcfd->events);
		if (!e) {
			arm(tcfd);
			spin_unlock(&tcfd->lock);
			continue;
		}

		tc = e->tc;
		remove_event(e);

		spin_unlock(&tcfd->lock);

		worker.woken_by_event = e;
		worker.woken_by_tcfd = tcfd;

		if (e->flags == EF_ALL_FREE)
			_signal_gets_delivered(tcfd, e);

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
	/* LIST_INSERT_HEAD(&sched.threads, &worker.main_thread, chain); */

	asprintf(&worker.sched_p2.name, "sched_%d", i);
	worker.sched_p2.cr = cr_create(scheduler_part2, NULL, DEFAULT_STACK_SIZE);
	if (!worker.sched_p2.cr)
		msg_exit(1, "allocation of worker.sched_p2 failed\n");

	cr_set_uptr(worker.sched_p2.cr, &worker.sched_p2);
	tc_waitq_init(&worker.sched_p2.exit_waiters);
	atomic_set(&worker.sched_p2.refcnt, 0);
	spin_lock_init(&worker.sched_p2.running);
}

void tc_init()
{
	LIST_INIT(&sched.immediate);
	LIST_INIT(&sched.threads);
	spin_lock_init(&sched.lock);

	tc_worker_init(0);

	sched.efd = epoll_create(1);
	if (sched.efd < 0)
		msg_exit(1, "epoll_create failed with %m\n");
}

static void *worker_pthread(void *arg)
{
	struct worker_start_info *wsi = (struct worker_start_info *)arg;
	int nr;

	nr = wsi->nr;
	if (nr == 0) {
		tc_init();
		wsi->tc = tc_thread_new(wsi->func, wsi->data, wsi->name);
		wsi->func = NULL;
		wsi->data = NULL;
		wsi->name = NULL;
	}
	pthread_mutex_unlock(&wsi->mutex);

	tc_worker_init(nr);
	tc_thread_wait(wsi->tc); /* calls tc_scheduler() */
	return NULL;
}


void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers)
{
	struct worker_start_info wsi;
	pthread_t *threads;
	int i;

	threads = alloca(sizeof(pthread_t) * nr_of_workers);
	if (!threads)
		msg_exit(1, "alloca() in tc_run failed\n");

	sched.nr_of_workers = nr_of_workers;

	wsi.nr = 0;
	wsi.func = func;
	wsi.data = data;
	wsi.name = name;
	pthread_mutex_init(&wsi.mutex, NULL);
	pthread_mutex_lock(&wsi.mutex);
	pthread_create(threads + 0, NULL, worker_pthread, &wsi);

	for (i = 1; i < nr_of_workers; i++) {
		pthread_mutex_lock(&wsi.mutex);
		wsi.nr = i;
		pthread_create(threads + i, NULL, worker_pthread, &wsi);
	}

	for (i = 0; i < nr_of_workers; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_mutex_destroy(&wsi.mutex);
}


void tc_rearm()
{
	arm(worker.woken_by_tcfd);
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

	/* printf("exiting: %s\n", tc->name); */

	add_event_cr(&e, 0, EF_EXITING, tc);

	spin_lock(&sched.lock);
	LIST_REMOVE(tc, chain);
	if (tc->is_on_threads_chain)
		LIST_REMOVE(tc, threads_chain);
	spin_unlock(&sched.lock);

	tc_waitq_wakeup(&tc->exit_waiters);

	/* refcnt = 1 because we have the EF_EXITING event referencing this tc */
	if (atomic_read(&tc->refcnt) > 1)
		msg_exit(1, "tc_die(%s): refcnt = %d. Signals still enabled?\n",
			 tc->name, atomic_read(&tc->refcnt));

	tc_scheduler(); /* The scheduler will free me */
	/* Not reached. */
	msg_exit(1, "tc_scheduler() returned in tc_die()\n");
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

void tc_threads_wait(struct tc_threads *threads)
{
	struct tc_thread *tc;

	spin_lock(&sched.lock);
	while ((tc = LIST_FIRST(threads))) {
		spin_unlock(&sched.lock);
		tc_thread_wait(tc);
		spin_lock(&sched.lock);
	}
	spin_unlock(&sched.lock);
}

static void _tc_fd_init(struct tc_fd *tcfd, int fd)
{
	struct epoll_event epe;
	int arg;

	tcfd->fd = fd;
	LIST_INIT(&tcfd->events);

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

	spin_lock_init(&tcfd->lock);
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

static void _tc_fd_unregister(struct tc_fd *tcfd)
{
	struct epoll_event epe = { };

	spin_lock(&tcfd->lock);
	if (!LIST_EMPTY(&tcfd->events))
		msg_exit(1, "event list not emptly in tc_unregister_fd()\n");
	spin_unlock(&tcfd->lock);

	if (epoll_ctl(sched.efd, EPOLL_CTL_DEL, tcfd->fd, &epe))
		msg_exit(1, "epoll_ctl failed with %m\n");
}

void tc_unregister_fd(struct tc_fd *tcfd)
{
	_tc_fd_unregister(tcfd);
	free(tcfd);
}

void tc_mutex_init(struct tc_mutex *m)
{
	atomic_set(&m->count, 0);
	if (pipe(m->pipe_fd))
		msg_exit(1, "pipe() failed with: %m\n");

	_tc_fd_init(&m->read_tcfd, m->pipe_fd[0]);
}

enum tc_rv tc_mutex_lock(struct tc_mutex *m)
{
	enum tc_rv rv;
	char c;

	if (atomic_add_return(1, &m->count) > 1) {
		rv = tc_wait_fd(EPOLLIN, &m->read_tcfd);
		if (rv != RV_OK)
			return rv;
		if (read(m->pipe_fd[0], &c, 1) != 1)
			msg_exit(1, "mutex_lock_ read() failed %m\n");

		tc_rearm();
	}

	return RV_OK;
}

void tc_mutex_unlock(struct tc_mutex *m)
{
	char c = 'w';

	if (atomic_sub_return(1, &m->count) < 0) {
		atomic_inc(&m->count);
		return;
	}

	if (write(m->pipe_fd[1], &c, 1) != 1)
		msg_exit(1, "write() failed with: %m\n");
}

enum tc_rv tc_mutex_trylock(struct tc_mutex *m)
{
	if (atomic_set_if_eq(1, 0, &m->count))
		return RV_OK;

	return RV_FAILED;
}

void tc_mutex_unregister(struct tc_mutex *m)
{
	_tc_fd_unregister(&m->read_tcfd);
	close(m->pipe_fd[0]);
	close(m->pipe_fd[1]);
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
	if (rv == RV_OK) {
		we = tc_waitq_prepare_to_wait(&wait_for->exit_waiters, &e);
		add_event_fd(&e, EPOLLIN, EF_ALL, &we->read_tcfd);
	}
	spin_unlock(&sched.lock);
	if (rv == RV_THREAD_NA)
		return rv;

	tc_scheduler();

	if (worker.woken_by_event != &e) {
		remove_event_fd(&e, &we->read_tcfd);
		rv = RV_INTR;
	}

	/* Do not pass wait_for->exit_waiters, since wait_for might be already freed. */
	tc_waitq_finish_wait(NULL, we);

	return rv;
}

void tc_waitq_init(struct tc_waitq *wq)
{
	spin_lock_init(&wq->lock);
	wq->active = NULL;
	wq->spare = NULL;
}

struct waitq_ev *tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e)
{
	struct waitq_ev *we;

	spin_lock(&wq->lock);
	if (!wq->active) {
		if (wq->spare) {
			wq->active = wq->spare;
			wq->spare = NULL;
		} else {
			we = malloc(sizeof(struct waitq_ev));
			if (!we)
				msg_exit(1, "malloc of waitq_ev failed in tc_waitq_init\n");

			if (pipe(we->pipe_fd))
				msg_exit(1, "pipe() failed with: %m\n");

			_tc_fd_init(&we->read_tcfd, we->pipe_fd[0]);
			atomic_set(&we->count, 0);
			wq->active = we;
		}
	}
	we = wq->active;
	spin_unlock(&wq->lock);
	atomic_inc(&we->count);

	return we;
}

void _waitq_before_schedule(struct event *e, struct waitq_ev *we)
{
	add_event_fd(e, EPOLLIN, EF_ALL, &we->read_tcfd);
}

int _waitq_after_schedule(struct event *e, struct waitq_ev *we)
{
	int r = (worker.woken_by_event != e);

	if (r)
		remove_event_fd(e, &we->read_tcfd);
	return r;
}

void tc_waitq_finish_wait(struct tc_waitq *wq, struct waitq_ev *we)
{
	int f;
	char c;

	if (atomic_sub_return(1, &we->count) == 0) {
		if (read(we->pipe_fd[0], &c, 1) != 1)
			msg_exit(1, "recycle read() in tc_waitq_wait failed %m\n");

		f = 1;
		if (wq) {
			spin_lock(&wq->lock);
			if (!wq->spare) {
				wq->spare = we;
				f = 0;
			}
			spin_unlock(&wq->lock);
		}

		if (f) {
			_tc_fd_unregister(&we->read_tcfd);
			close(we->pipe_fd[0]);
			close(we->pipe_fd[1]);
			free(we);
		}
	}
}

void tc_waitq_wait(struct tc_waitq *wq) /* do not use! */
{
	struct waitq_ev *we;
	struct event e;

	we = tc_waitq_prepare_to_wait(wq, &e);
	add_event_fd(&e, EPOLLIN, EF_ALL, &we->read_tcfd);
	tc_scheduler();
	if (worker.woken_by_event != &e)
		remove_event_fd(&e, &we->read_tcfd);
	tc_waitq_finish_wait(wq, we);
}

void tc_waitq_wakeup(struct tc_waitq *wq)
{
	struct waitq_ev *we = NULL;
	char c = 'w';

	spin_lock(&wq->lock);
	if (wq->active) {
		we = wq->active;
		wq->active = NULL;
	}
	spin_unlock(&wq->lock);

	if (we) {
		if (write(we->pipe_fd[1], &c, 1) != 1)
			msg_exit(1, "write() failed with: %m\n");
	}
}

void tc_waitq_unregister(struct tc_waitq *wq)
{
	/* Nothing to do. */
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

	/* printf("signal_enabled e=%p for %s\n", e, tc_current()->name); */

	we = tc_waitq_prepare_to_wait(&s->wq, e);
	add_event_fd(e, EPOLLIN, EF_ALL_FREE, &we->read_tcfd);
}

void _signal_gets_delivered(struct tc_fd *tcfd, struct event *e)
{
	struct waitq_ev *we;

	/* printf("signal_gets_delivered e=%p\n", e); */

	we = container_of(tcfd, struct waitq_ev, read_tcfd);

	tc_waitq_finish_wait(NULL, we);
	free(e);
}

void tc_signal_disable(struct tc_signal *s)
{
	struct waitq_ev *we;
	struct tc_waitq *wq = &s->wq;
	struct event *e;

	spin_lock(&wq->lock);
	we = wq->active;

	if (!we) {
		spin_unlock(&wq->lock);
		return;
	}

	spin_lock(&we->read_tcfd.lock);
	LIST_FOREACH(e, &we->read_tcfd.events, chain) {
		if (e->tc == tc_current()) {
			remove_event(e);
			free(e);
			arm(&we->read_tcfd);
			break;
		}
	}
	spin_unlock(&we->read_tcfd.lock);
	spin_unlock(&wq->lock);

	tc_waitq_finish_wait(wq, we);
}

void tc_signal_unregister(struct tc_signal *s)
{
	tc_waitq_unregister(&s->wq);
}

void tc_signal_fire(struct tc_signal *s)
{
	tc_waitq_wakeup(&s->wq);
}

