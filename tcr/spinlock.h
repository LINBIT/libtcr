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

#include "config.h"
#include "atomic.h"


#ifdef SPINLOCK_DEBUG
    typedef struct {
		int lock;
		struct tc_thread *holding_thread;
		const char* holder;
		const char* file;
		int line;
	} spinlock_t;
#else
    typedef struct {
		int lock;
    } spinlock_t;
#endif


static inline void spin_unlock_plain(spinlock_t *l)
{
	__sync_lock_release(&l->lock);
}

static inline void spin_lock_init(spinlock_t *l)
{
	spin_unlock_plain(l);
}

static inline int spin_trylock_plain(spinlock_t *l)
{
	return __sync_bool_compare_and_swap(&l->lock, 0, 1);
}

static inline void spin_lock_plain(spinlock_t *l)
{
	while (!spin_trylock_plain(l))
		;
}

#endif


#ifdef SPINLOCK_DEBUG
    #include "spinlock_debug.h"
#else
    #define spin_lock(__L) spin_lock_plain(__L)
    #define spin_unlock(__L) spin_unlock_plain(__L)
    #define spin_trylock(__L) spin_trylock_plain(__L)
#endif



