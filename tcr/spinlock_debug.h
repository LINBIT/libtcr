#ifndef SPINLOCK_DEBUG_H
#define SPINLOCK_DEBUG_H

#include <sched.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include "atomic.h"

#include "coroutines.h"


extern void _bug_if_holds_immediate(struct tc_thread *who);


#define spin_lock(LOCK)  __spin_lock(LOCK, *((char **)cr_uptr(cr_current())), __FILE__, __LINE__)
#define spin_trylock(LOCK)  __spin_trylock(LOCK, *((char **)cr_uptr(cr_current())), __FILE__, __LINE__)

void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
static inline void __spin_lock(spinlock_t *l, char* holder, char* file, int line)
{
	static time_t last_warn = 0;
	time_t now;
	int i = 0;
	if (cr_current())
		_bug_if_holds_immediate(cr_uptr(cr_current()));
	while (!spin_trylock_plain(l)) {
		i++;
		if ((i & ((1<<12)-1)) == 0) /* every 4096 spins, call sched_yield() */
		{
#ifdef WAIT_DEBUG
			extern __thread char *_caller_file;
			extern __thread int _caller_line;
			/* these are per-pthread, not per-tc-thread. */
			char *_clr = _caller_file;
			int lnr = _caller_line;
#endif
			sched_yield();
#ifdef WAIT_DEBUG
			_caller_file = _clr;
			_caller_line = lnr;
#endif
		}

		if ((i>>27) & 1) {/* eventually abort the program. */
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
	}
	l->file = file;
	l->line = line;
	l->holder = holder;
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
}

#endif
