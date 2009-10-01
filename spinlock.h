#ifndef SPINLOCK_H
#define SPINLOCK_H

#ifdef DEBUG
#include "spinlock_debug.h"
#else
#include "spinlock_plain.h"
#endif

static inline void spin_lock_init(spinlock_t *l)
{
	__sync_lock_release(&l->lock);
}

#endif
