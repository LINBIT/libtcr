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

#define atomic_dec(v) atomic_sub_return(1, v)
#define atomic_inc(v) atomic_add_return(1, v)
#endif
