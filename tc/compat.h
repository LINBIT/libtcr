#ifndef COMPAT_H
#define COMPAT_H

#include "../config.h"

#include <sys/syscall.h>
#include <unistd.h>

#ifdef HAVE_SYS_EVENTFD_H
#include <sys/eventfd.h>
#else
#include <stdint.h>
typedef uint64_t eventfd_t;
extern int eventfd (int __count, int __flags);
extern int eventfd_read (int __fd, eventfd_t *__value);
extern int eventfd_write (int __fd, eventfd_t value);
#endif

#ifdef HAVE_SYS_TIMERFD_H
#include <sys/timerfd.h>
#endif

#ifndef HAVE_TIMERFD_CREATE
static inline int timerfd_create (clockid_t __clock_id, int __flags)
{
	return syscall(SYS_timerfd_create, __clock_id, __flags);
}
#endif

#ifndef HAVE_TIMERFD_SETTIME
static inline int timerfd_settime (int __ufd, int __flags,
                            __const struct itimerspec *__utmr,
                            struct itimerspec *__otmr)
{
	return syscall(SYS_timerfd_settime, __ufd, __flags, __utmr, __otmr);
}
#endif

#ifndef HAVE_TIMERFD_GETTIME
static inline int timerfd_gettime (int __ufd, struct itimerspec *__otmr)
{
	return syscall(SYS_timerfd_gettime, __ufd, __otmr);
}
#endif

#ifndef HAVE_MAP_STACK
#define MAP_STACK       0x20000
#endif

#endif
