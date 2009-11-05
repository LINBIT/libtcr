#ifndef COROUTINES_H
#define COROUTINES_H

#include <stdlib.h>

struct coroutine;

extern __thread struct coroutine *__cr_current;

struct coroutine *cr_create(void (*func)(void *, void *), void *arg1, void *arg2, int stack_size);
void cr_call(struct coroutine *cr);
void cr_return();
void cr_init();
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
	*(void **)cr = uptr;
}

static inline struct coroutine *cr_caller(void)
{
	return (struct coroutine *)(((void **)__cr_current)[1]);
}
#endif
