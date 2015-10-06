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

#ifndef COROUTINES_H
#define COROUTINES_H

#include <stdlib.h>

struct coroutine;

extern __thread struct coroutine *__cr_current;

struct coroutine *cr_create(void (*func)(void *, void *), void *arg1, void *arg2, int stack_size);
void cr_call(struct coroutine *cr);
void cr_return();
void cr_init(struct coroutine *cr);
void cr_exit();
void cr_delete(struct coroutine *cr);

static inline struct coroutine *cr_current(void)
{
	return __cr_current;
}

static inline void *cr_uptr(struct coroutine *cr)
{
	return *(void **)cr; /* uptr is first member */
}

static inline void cr_set_uptr(struct coroutine *cr, void *uptr)
{
	if (cr)
		*(void **)cr = uptr;
}

static inline struct coroutine *cr_caller(void)
{
	return (struct coroutine *)(((void **)__cr_current)[1]);
}

void *cr_get_sp_from_cr(struct coroutine *cr);
void *cr_get_stack_from_cr(struct coroutine *cr);

#endif
