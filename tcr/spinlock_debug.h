#ifndef SPINLOCK_DEBUG_H
#define SPINLOCK_DEBUG_H

#include <sched.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include "atomic.h"
#include "spinlock.h"

#include "coroutines.h"


extern void _bug_if_holds_immediate(struct tc_thread *who);


#define spin_lock(LOCK)  __spin_lock(LOCK, *((char **)cr_uptr(cr_current())), __FILE__, __LINE__)
#define spin_trylock(LOCK)  __spin_trylock(LOCK, *((char **)cr_uptr(cr_current())), __FILE__, __LINE__)

static int spins_per_delay = 0;
/* MAYBE: randomize delay */
static unsigned const delay_usec = 50;

/* sched_yield() may not be what you think it is.
 * I suggest to either escalate to some wake-wait futex,
 * or to at least do a real sleep.
 *
 * It MAY be beneficial to set some linux process scheduler sysctls.
 */
#define USE_SCHED_YIELD 0
#define USE_NANOSLEEP 1
#define USE_SELECT 0
static inline void my_usleep(unsigned int usec)
{
	if (USE_SCHED_YIELD)
		sched_yield();
	else if (USE_NANOSLEEP) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = usec * 1000L };
		nanosleep(&ts, &ts);
	} else {
		struct timeval tv = { .tv_sec = 0, .tv_usec = usec };
		select(0, NULL, NULL, NULL, &tv);
	}
}


void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
static inline void __spin_lock(spinlock_t *l, char* holder, char* file, int line)
{
	static time_t last_warn = 0;
	time_t now;
	int spins = 0;
	int delays = 0;
	int num_delays = /* two minutes in micro seconds / micro sec per delay */
		120 * 1000 * 1000 / delay_usec;

	if (cr_current())
		_bug_if_holds_immediate(cr_uptr(cr_current()));
	if (spin_trylock_plain(l))
		goto got_it;
/*
 * Need to "relax" the cpu in the busy loop,
 * and also not spin on the atomic instruction,
 * but on some dirty read instead.
 *
 * See also:
 * https://software.intel.com/en-us/articles/implementing-scalable-atomic-locks-for-multi-core-intel-em64t-and-ia32-architectures
 */
	while (*(volatile int*)&l->lock || !spin_trylock_plain(l)) {
		++spins;
		if (spins > spins_per_delay)
		{
#ifdef WAIT_DEBUG
			extern __thread char *_caller_file;
			extern __thread int _caller_line;
			/* these are per-pthread, not per-tc-thread. */
			char *_clr = _caller_file;
			int lnr = _caller_line;
#endif
			my_usleep(delay_usec);
			++delays;
			spins = 0;
#ifdef WAIT_DEBUG
			_caller_file = _clr;
			_caller_line = lnr;
#endif
		}

		if (delays > num_delays) {/* eventually abort the program. */
			time(&now);
			if (now - last_warn < 60)
				continue;

			last_warn = now;
			syslog(LOG_WARNING, "libtcr in pid %d: "
					"spinlock held by \"%s\" in %s:%d, "
					"wanted by \"%s\"%s:%d\n",
					getpid(),
					l->holder, l->file, l->line,
					holder, file, line);

#ifdef SPINLOCK_ABORT

			fprintf(stderr, "lock held by: \"%s\" in %s:%d\n",
					l->holder, l->file, l->line);
			fprintf(stderr, "\"%s\" tries to get lock in %s:%d\n",
					holder, file, line);

			msg_exit(1, "spinning too long in spin_lock()\n");
			// *(char*)91 = 22;
#endif
		}
		cpu_relax();
	}
got_it:
	l->file = file;
	l->line = line;
	l->holder = holder;
	l->holding_thread = cr_current() ? cr_uptr(cr_current()) : (void*)&now;
}

static inline int __spin_trylock(spinlock_t *l, char* holder, char* file, int line)
{
	if (!spin_trylock_plain(l)) {
		return 0;
	}

	l->file = file;
	l->line = line;
	l->holder = holder;
	l->holding_thread = cr_current() ? cr_uptr(cr_current()) : (void*)&holder;
	return 1;
}


static inline void spin_unlock(spinlock_t *l)
{
	spin_unlock_plain(l);
	l->file = "(none)";
	l->holder = "(none)";
	l->line = 0;
	l->holding_thread = NULL;
}

#endif
