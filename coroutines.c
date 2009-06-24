#include <sys/mman.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "coroutines.h"

#define STACK_ALIGN 16

struct coroutine {
	void* uptr;               /* is first by intention. so we can keep coroutine opaque */
	struct coroutine *caller; /* second by intention. */
	ucontext_t ctx;
	void *arg;
	void (*func)(void *);
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

	co_main.arg = NULL;
	co_main.func = NULL;
	co_main.uptr = NULL;
}


static void cr_setup()
{
	struct coroutine *cr = __cr_current;

	cr->func(cr->arg);

	fprintf(stderr, "func() returned.\n");
	exit(1);
}

#define STACK_OVERFLOW_PROTECTION

struct coroutine *cr_create(void (*func)(void *), void *arg, int stack_size)
{
	struct coroutine *cr;
	void *stack;

#ifdef STACK_OVERFLOW_PROTECTION
	int ps = sysconf(_SC_PAGE_SIZE);
	void *ovp;

	stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_STACK,
		     -1 ,0);
	if (stack == MAP_FAILED)
		return NULL;

	ovp = mmap(stack - ps, ps, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_FIXED,
		   -1, 0);

	if (stack == MAP_FAILED) {
		munmap(stack, stack_size);
		return NULL;
	}

	cr = malloc(sizeof(struct coroutine));
	if (!cr) {
		munmap(ovp, ps);
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
	cr->ctx.uc_stack.ss_size = stack_size;
	cr->ctx.uc_link = NULL;
	cr->arg = arg;
	cr->func = func;

	/* use cr_setup here */
	makecontext(&cr->ctx, cr_setup, 0);

	return cr;
}

void cr_delete(struct coroutine *cr)
{
#ifdef STACK_OVERFLOW_PROTECTION
	int ps = sysconf(_SC_PAGE_SIZE);

	munmap(cr->ctx.uc_stack.ss_sp, cr->ctx.uc_stack.ss_size);
	munmap(cr->ctx.uc_stack.ss_sp - ps, ps);
	free(cr);
#else
	free(cr);
#endif
}


void cr_call(struct coroutine *cr)
{
	struct coroutine *previous = __cr_current;

	cr->caller = previous;
	__cr_current = cr;

	if (swapcontext(&previous->ctx, &cr->ctx)) {
		fprintf(stderr, "swapcontext() failed.\n");
		exit(1);
	}
}

void cr_return()
{
	cr_call(__cr_current->caller);
}
