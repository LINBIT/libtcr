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

#ifndef ATOMIC_H
#define ATOMIC_H

#include <stdio.h>
#include <stdlib.h>

#define atomic_base_type int64_t

typedef struct {
	atomic_base_type counter;
} atomic_t;

static inline int atomic_read(atomic_t *v)
{
	__sync_synchronize();
	return v->counter;
}

static inline void atomic_set(atomic_t *v, atomic_base_type i)
{
	v->counter = i;
	__sync_synchronize();
}

static inline int atomic_add_return(atomic_base_type i, atomic_t *v)
{
	return __sync_add_and_fetch(&v->counter, i);
}

static inline int atomic_sub_return(atomic_base_type i, atomic_t *v)
{
	return __sync_sub_and_fetch(&v->counter, i);
}

static inline int atomic_set_if_eq(atomic_base_type new_val, atomic_base_type eq_val, atomic_t *v)
{
	return __sync_bool_compare_and_swap(&v->counter, eq_val, new_val);
}

static inline int atomic_swap(atomic_t *v, atomic_base_type i)
{
	__sync_synchronize();
	return __sync_lock_test_and_set(&v->counter, i);
}

static inline void atomic_set_bit(atomic_base_type bnr, atomic_t *v)
{
	__sync_or_and_fetch(&v->counter, 1 << bnr);
}

static inline void atomic_clear_bit(atomic_base_type bnr, atomic_t *v)
{
	__sync_and_and_fetch(&v->counter, ~(1 << bnr));
}

static inline int atomic_test_bit(atomic_base_type bnr, atomic_t *v)
{
	__sync_synchronize();
	return v->counter & (1 << bnr);
}

#define atomic_dec(v) atomic_sub_return(1, v)
#define atomic_inc(v) atomic_add_return(1, v)
#endif
