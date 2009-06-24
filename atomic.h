#ifndef ATOMIC_H
#define ATOMIC_H

typedef struct {
	int counter;
} atomic_t;

#define atomic_read(v)		((v)->counter)
#define atomic_set(v, i)	(((v)->counter) = (i))

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

typedef struct {
	int lock;
} spinlock_t;

static inline void spin_lock_init(spinlock_t *l)
{
	l->lock = 0;
}

static inline void spin_lock(spinlock_t *l)
{
	while (!__sync_bool_compare_and_swap(&l->lock, 0, 1))
	       ;
}

static inline void spin_unlock(spinlock_t *l)
{
	l->lock = 0;
}

#define atomic_dec(v) atomic_sub_return(1, v)
#define atomic_inc(v) atomic_add_return(1, v)
#endif
