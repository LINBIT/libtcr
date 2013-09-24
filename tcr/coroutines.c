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

#include <limits.h>
#include <sys/mman.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "config.h"
#include "compat.h"
#include "coroutines.h"

#define STACK_ALIGN 16

int swapcontext_fast(ucontext_t *oucp, ucontext_t *ucp);

struct coroutine {
	void* uptr;               /* is first by intention. so we can keep coroutine opaque */
	struct coroutine *caller; /* second by intention. */
	ucontext_t ctx;
};

static __thread struct coroutine co_main;
__thread struct coroutine *__cr_current;

void cr_init()
{
	__cr_current = &co_main;

	if (getcontext(&co_main.ctx)) {
		fprintf(stderr, "getcontext() failed.\n");
		exit(1);
	}

	co_main.uptr = NULL;
}

/* Arglbargl: Why the hell passes makecontext only integer arguments instead of void*
 */
#if (__WORDSIZE == 64)
static void cr_setup(unsigned int i1, unsigned int i2, unsigned int i3, unsigned int i4,
	unsigned int i5, unsigned int i6)
{
	void (*func)(void *, void *) = (void *)(((unsigned long)i1 << 32) | (unsigned long)i2);
	void *arg1 = (void *)(((unsigned long)i3 << 32) | (unsigned long)i4);
	void *arg2 = (void *)(((unsigned long)i5 << 32) | (unsigned long)i6);

	func(arg1, arg2);

	fprintf(stderr, "func() returned.\n");
	exit(1);
}
#elif (__WORDSIZE == 32)
static void cr_setup(unsigned int i1, unsigned int i2, unsigned int i3)
{
	void (*func)(void *, void *) = (void *)i1;
	void *arg1 = (void *)i2;
	void *arg2 = (void *)i3;

	func(arg1, arg2);

	fprintf(stderr, "func() returned.\n");
	exit(1);
}
#else
#error only 32 bits and 64 bits wordsizes considered...
#endif

struct coroutine *cr_create(void (*func)(void *, void *), void *arg1, void *arg2, int stack_size)
{
	struct coroutine *cr;
	void *stack;
	int ps = 0;

#ifdef STACK_OVERFLOW_PROTECTION
	ps = sysconf(_SC_PAGE_SIZE);

	stack = mmap(NULL, stack_size + ps, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_STACK,
		     -1 ,0);
	if (stack == MAP_FAILED)
		return NULL;

	if (mprotect(stack, ps, PROT_NONE)) {
		munmap(stack, stack_size);
		return NULL;
	}

	cr = malloc(sizeof(struct coroutine));
	if (!cr) {
		munmap(stack, stack_size);
		return NULL;
	}
#else

	cr = malloc((sizeof(struct coroutine) + stack_size + STACK_ALIGN - 1) & ~(STACK_ALIGN - 1));
	if (!cr)
		return NULL;

	stack = (void *)(((unsigned long)cr + sizeof(struct coroutine) + STACK_ALIGN - 1) & ~(STACK_ALIGN - 1));

#endif
	if (getcontext(&cr->ctx)) {
		free(cr);
		return NULL;
	}

	cr->ctx.uc_stack.ss_flags = 0;
	cr->ctx.uc_stack.ss_sp = stack;
	cr->ctx.uc_stack.ss_size = stack_size + ps;
	cr->ctx.uc_link = NULL;

#if (__WORDSIZE == 64)
	makecontext(&cr->ctx, (void (*)())cr_setup, 6,
		    (unsigned int)((unsigned long)func >> 32), (unsigned int)(unsigned long)func,
		    (unsigned int)((unsigned long)arg1 >> 32), (unsigned int)(unsigned long)arg1,
		    (unsigned int)((unsigned long)arg2 >> 32), (unsigned int)(unsigned long)arg2);
#elif (__WORDSIZE == 32)
	makecontext(&cr->ctx, (void (*)())cr_setup, 3, (unsigned int)func, (unsigned int)arg1,
		    (unsigned int)arg2);
#endif

	return cr;
}

void cr_delete(struct coroutine *cr)
{
#ifdef STACK_OVERFLOW_PROTECTION
	munmap(cr->ctx.uc_stack.ss_sp, cr->ctx.uc_stack.ss_size);
#endif
	free(cr);
}


void cr_call(struct coroutine *cr)
{
	struct coroutine *previous = __cr_current;

	cr->caller = previous;
	__cr_current = cr;

	if (swapcontext_fast(&previous->ctx, &cr->ctx)) {
		fprintf(stderr, "swapcontext() failed.\n");
		exit(1);
	}
}

void cr_return()
{
	cr_call(__cr_current->caller);
}

void *cr_get_sp_from_cr(struct coroutine *cr)
{
	void **regs = (void**)&cr->ctx;
	return regs[
#if __WORDSIZE == 64
		0x14
#else
		0x12
#endif
		];
}

void *cr_get_stack_from_cr(struct coroutine *cr)
{
	return cr->ctx.uc_stack.ss_sp - cr->ctx.uc_stack.ss_size;
}
