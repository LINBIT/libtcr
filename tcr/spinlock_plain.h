#ifndef SPINLOCK_PLAIN_H
#define SPINLOCK_PLAIN_H

#include "atomic.h"

typedef struct {
	int lock;
} spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
	while (!__sync_bool_compare_and_swap(&l->lock, 0, 1))
		;
}

static inline int spin_trylock(spinlock_t *l)
{
	return __sync_bool_compare_and_swap(&l->lock, 0, 1);
}

static inline void spin_unlock(spinlock_t *l)
{
	__sync_lock_release(&l->lock);
}

#endif
