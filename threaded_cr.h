#ifndef THREADED_CR_H
#define THREADED_CR_H

#include <sys/queue.h>
#include <stddef.h>
#include <pthread.h>

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
	pthread_mutex_t mutex; /* protects the events list */
	__uint32_t ep_events;  /* current mask */
};

struct tc_mutex {
	atomic_t count;
	int pipe_fd[2];
	struct tc_fd *read_tcfd;
};

struct waitq_ev;

LIST_HEAD(wait_evs, waitq_ev);

struct tc_waitq {
	struct waitq_ev *active;
	struct waitq_ev *spare;
	struct wait_evs wes;
	pthread_mutex_t mutex;
};

struct tc_signal {
	struct tc_waitq wq;
};

struct tc_thread;
LIST_HEAD(tc_threads, tc_thread);

void tc_run(void (*func)(void *), void *data, char* name, int nr_of_workers);
void tc_init();
void tc_worker_init(int i);
void tc_scheduler();

struct tc_thread *tc_thread_new(void (*func)(void *), void *data, char* name);
enum tc_rv tc_thread_wait(struct tc_thread *tc);
void tc_die();

void tc_threads_new(struct tc_threads *threads, void (*func)(void *), void *data, char* name);
void tc_threads_wait(struct tc_threads *threads);

struct tc_fd *tc_register_fd(int fd);
void tc_unregister_fd(struct tc_fd *tcfd);
enum tc_rv tc_wait_fd(__uint32_t ep_events, struct tc_fd *tcfd);
void tc_rearm();

void tc_mutex_init(struct tc_mutex *m);
enum tc_rv tc_mutex_lock(struct tc_mutex *m);
void tc_mutex_unlock(struct tc_mutex *m);
void tc_mutex_unregister(struct tc_mutex *m);

void tc_signal_init(struct tc_signal *s);
void tc_signal_enable(struct tc_signal *s);
void tc_signal_disable(struct tc_signal *s);
void tc_signal_fire(struct tc_signal *s);
void tc_signal_unregister(struct tc_signal *s);

void tc_waitq_init(struct tc_waitq *wq);
void tc_waitq_wakeup(struct tc_waitq *wq);
struct waitq_ev *tc_waitq_prepare_to_wait(struct tc_waitq *wq, struct event *e);
void tc_waitq_finish_wait(struct waitq_ev *we);
void _waitq_before_schedule(struct event *e, struct waitq_ev *we);
int _waitq_after_schedule(struct event *e, struct waitq_ev *we);


#define __tc_wait_event(wq, cond)					\
do {									\
	struct waitq_ev *we;						\
	struct event e;							\
	we = tc_waitq_prepare_to_wait(wq, &e);				\
	while (1) {							\
		if (cond)						\
			break;						\
		_waitq_before_schedule(&e, we);				\
		tc_scheduler();						\
		if (_waitq_after_schedule(&e, we))			\
			break;						\
	}								\
	tc_waitq_finish_wait(we);					\
} while (0)

#define tc_waitq_wait_event(wq, cond)			\
do {							\
	if (cond)					\
		break;					\
	__tc_wait_event(wq, cond);			\
} while (0)


static inline int tc_fd(struct tc_fd *tcfd)
{
	return tcfd->fd;
}


#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#endif
