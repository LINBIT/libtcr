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

#define atomic_dec(v) atomic_sub_return(1, v)
#define atomic_inc(v) atomic_add_return(1, v)
#endif
