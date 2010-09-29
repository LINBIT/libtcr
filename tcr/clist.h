#ifndef CLIST_H
#define CLIST_H

/* pure CIRCLE LIST without head entries. In the style of queue.h list macros */
struct clist_entry {
	struct clist_entry *cl_next;
	struct clist_entry *cl_prev;
};

#define CLIST_INIT(elm) do {						\
	(elm)->cl_next = elm;						\
	(elm)->cl_prev = elm;						\
} while (/*CONSTCOND*/0)

#define CLIST_INSERT_AFTER(listelm, elm) do {				\
	(elm)->cl_next = (listelm)->cl_next;				\
	(elm)->cl_prev = (listelm);					\
	(listelm)->cl_next->cl_prev = elm;				\
	(listelm)->cl_next = elm;					\
} while (/*CONSTCOND*/0)

#define CLIST_REMOVE(elm) do {						\
	(elm)->cl_next->cl_prev = (elm)->cl_prev;			\
	(elm)->cl_prev->cl_next = (elm)->cl_next;			\
} while (/*CONSTCOND*/0)

#define CLIST_EMPTY(elm) ((elm)->cl_next == (elm))


#endif
