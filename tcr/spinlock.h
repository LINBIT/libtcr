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
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

#include "config.h"

#ifdef SPINLOCK_ABORT
#ifndef SPINLOCK_DEBUG
#define SPINLOCK_DEBUG 1
#endif
#endif

#ifdef SPINLOCK_DEBUG
#include <assert.h>
#include <syslog.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "atomic.h"

// BUILD_BUG_ON(sizeof(pid_t) > sizeof(uint32_t));

/* same can probably be achieved within 32bit, if we limit (mask) the tid,
 * and use only some bit for the cpu */
union spinlock_marker {
	uint64_t m;
	struct {
		uint32_t tid; /* must not be 0! */
		uint32_t cpu; /* because this could be 0. */
	};
};

typedef struct spinlock {
	union spinlock_marker lock;
	int spinners;
#ifdef SPINLOCK_DEBUG
	const char* file;
	int line;
#endif
} spinlock_t;


extern __thread union spinlock_marker this_spinlock_owner;
static inline void spin_unlock_plain(spinlock_t *l)
{
#if 0 && defined(SPINLOCK_DEBUG)
	assert(l->lock.m == this_spinlock_owner.m);
#endif
	__sync_lock_release(&l->lock.m);
}

static inline void spin_lock_init(spinlock_t *l)
{
	l->line = 0;
	l->spinners = 0;
	l->file = NULL;
	__sync_lock_release(&l->lock.m);
}

static inline int spin_trylock_plain(spinlock_t *l)
{
	/* ASSERT(this_spinlock_owner.m); */
	return __sync_val_compare_and_swap(&l->lock.m, 0, this_spinlock_owner.m);
}

/* REP NOP (PAUSE) is a good thing to insert into busy-wait loops. */
static inline void rep_nop(void)
{
	asm volatile("rep; nop" ::: "memory");
}

static inline void cpu_relax(void)
{
	rep_nop();
}

#ifdef SPINLOCK_DEBUG

static inline void __must_hold(spinlock_t *l)
{
	assert(l->lock.m == this_spinlock_owner.m);
}

void __spin_lock(spinlock_t *l, char* file, int line);
#define spin_lock(__L) _spin_lock(__L, __FILE__, __LINE__)
static inline void _spin_lock(spinlock_t *l, char* file, int line)
{
	if (spin_trylock_plain(l))
		__spin_lock(l, file, line);
	l->file = file;
	l->line = line;
}

static inline void spin_unlock(spinlock_t *l)
{
	l->file = "(none)";
	l->line = 0;
	spin_unlock_plain(l);
}

#else /* SPINLOCK_DEBUG */

#define __must_hold(__L) do { } while (0)

void __spin_lock(spinlock_t *l);
static inline void spin_lock(spinlock_t *l)
{
	if (spin_trylock_plain(l))
		__spin_lock(l);
};

#define spin_unlock(__L) spin_unlock_plain(__L)

#endif /* SPINLOCK_DEBUG */

static inline int spin_trylock(spinlock_t *l) {
	return spin_trylock_plain(l) == 0;
}

#endif
