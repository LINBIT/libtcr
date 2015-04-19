#include <assert.h>
#include <time.h>

#include "coroutines.h"
#include "spinlock.h"

static const unsigned int spins_per_delay = 30;
/* If you want the goodies, you need to properly initialize it
 * before starting to use spinlocks. */
__thread union spinlock_marker this_spinlock_owner = { .m = ~0ULL };

#define USE_NANOSLEEP 1
static inline void my_usleep(unsigned int usec)
{
	if (USE_NANOSLEEP) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = usec * 1000L };
		nanosleep(&ts, &ts);
	} else {
		struct timeval tv = { .tv_sec = 0, .tv_usec = usec };
		select(0, NULL, NULL, NULL, &tv);
	}
}

#ifndef Min
#define Min(a, b)	((a) < (b) ? (a) : (b))
#else
#error "I wanted to define Min myself?"
#endif


void msg_exit(int code, const char *fmt, ...) __attribute__ ((__noreturn__));
#ifndef SPINLOCK_DEBUG
#define __spin_lock(__L, f, l) __spin_lock(__L)
#endif

void __spin_lock(spinlock_t *l, char* file, int line)
{
#ifdef SPINLOCK_DEBUG
	static time_t last_warn = 0;
	time_t my_first_delay = 0;
	time_t now;
#endif
	union spinlock_marker current_owner;
	unsigned long delay_usec = 0;
	unsigned int delays = 0;
	unsigned int spins;
	unsigned int sp;

	if (0 == spin_trylock_plain(l))
		return; /* got it */

	sp = __sync_fetch_and_add(&l->spinners, 1);
	sp = Min(sp, 10);
	spins = sp ? spins_per_delay : 0; /* other spinners present already? */

	assert(this_spinlock_owner.m != 0);
	for (;;) {
		/* spin on dirty read first */
		current_owner.m = ACCESS_ONCE(l->lock.m);
		if (!current_owner.m)
			current_owner.m = spin_trylock_plain(l);
		if (!current_owner.m)
			break;

		/* Want deadlock detection? */
		if (current_owner.tid == this_spinlock_owner.tid) {
			msg_exit(1, "Spinlock deadlock detected");
		}

		/* if spins_per_delay exceeded,
		 * or if on same cpu as the current owner, rather sleep. */
		if (++spins > spins_per_delay || current_owner.cpu == this_spinlock_owner.cpu) {
			if (!delay_usec)
				delay_usec = (500 + 1000 * random()/RAND_MAX);

			++delays;
			sp = __sync_sub_and_fetch(&l->spinners, 1);
			sp = Min(sp, 10);
			my_usleep(delay_usec + 500 * sp);
			sp = __sync_fetch_and_add(&l->spinners, 1);
			if (sp)
				spins = spins_per_delay * sp/10;
			else
				spins = 0;

#ifdef SPINLOCK_DEBUG
			/* Regarding WAIT_DEBUG and the __thread _caller_file variables:
			 * This pthread is here. Busy. Nothing will change those. */

			time(&now);
			if (!my_first_delay)
				my_first_delay = now;
			if (now - last_warn < 30)
				continue;

			last_warn = now;
			syslog(LOG_WARNING, "libtcr in pid %d: "
					"spinlock held by tid:%u in %s:%d, "
					"wanted by tid:%u from %s:%d\n",
					getpid(),
					current_owner.tid, l->file, l->line,
					this_spinlock_owner.tid, file, line);
#ifdef SPINLOCK_ABORT
			if (now - my_first_delay < 120)
				continue;

			fprintf(stderr, "lock held by: \"%s\" in %s:%d\n",
					l->holder, l->file, l->line);
			fprintf(stderr, "\"%s\" tries to get lock in %s:%d\n",
					holder, file, line);
			msg_exit(1, "spinning too long in spin_lock()\n");
			// *(char*)91 = 22;
#endif
#endif
		}
		cpu_relax();
	}
	__sync_fetch_and_sub(&l->spinners, 1);
}
