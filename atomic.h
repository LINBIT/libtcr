#ifndef ATOMIC_H
#define ATOMIC_H

#include <stdio.h>
#include <stdlib.h>

typedef struct {
	int counter;
} atomic_t;

static inline int atomic_read(atomic_t *v)
{
	__sync_synchronize();
	return v->counter;
}

static inline void atomic_set(atomic_t *v, int i)
{
	v->counter = i;
	__sync_synchronize();
}

static inline int atomic_add_return(int i, atomic_t *v)
{
	return __sync_add_and_fetch(&v->counter, i);
}

static inline int atomic_sub_return(int i, atomic_t *v)
{
	return __sync_sub_and_fetch(&v->counter, i);
}

static inline int atomic_set_if_eq(int new_val, int eq_val, atomic_t *v)
{
	return __sync_bool_compare_and_swap(&v->counter, eq_val, new_val);
}

static inline int atomic_swap(atomic_t *v, int i)
{
	__sync_synchronize();
	return __sync_lock_test_and_set(&v->counter, i);
}

static inline void atomic_set_bit(int bnr, atomic_t *v)
{
	__sync_or_and_fetch(&v->counter, 1 << bnr);
}

static inline void atomic_clear_bit(int bnr, atomic_t *v)
{
	__sync_and_and_fetch(&v->counter, ~(1 << bnr));
}

static inline int atomic_test_bit(int bnr, atomic_t *v)
{
	__sync_synchronize();
	return v->counter & (1 << bnr);
}

typedef struct {
	int lock;
} spinlock_t;

static inline void spin_lock_init(spinlock_t *l)
{
	__sync_lock_release(&l->lock);
}

/*
static inline void spin_lock(spinlock_t *l)
{
	while (!__sync_bool_compare_and_swap(&l->lock, 0, 1))
	       ;
}
*/

static inline void spin_lock(spinlock_t *l)
{
	int i = 0;
	while (!__sync_bool_compare_and_swap(&l->lock, 0, 1)) {
		i++;
		if ((i & ((1<<12)-1)) == 0) /* every 4096 spins, call sched_yield() */
			sched_yield();

		if ((i>>22) & 1) { /* eventually abort the program. */
			fprintf(stderr, "spinning too long in spin_lock()\n");
			exit(1);
		}
	}
}

static inline void spin_unlock(spinlock_t *l)
{
	__sync_lock_release(&l->lock);
}

#define atomic_dec(v) atomic_sub_return(1, v)
#define atomic_inc(v) atomic_add_return(1, v)
#endif
