#ifndef COMPAT_H
#define COMPAT_H

#include "config.h"

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

static inline int tgkill (pid_t tgid, pid_t tid, int sig)
{
	return syscall(SYS_tgkill, tgid, tid, sig);
}


#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC 02000000
#endif
#ifndef TFD_TIMER_ABSTIME
#define TFD_TIMER_ABSTIME (1 << 0)
#endif


#ifndef HAVE_MAP_STACK
#define MAP_STACK       0x20000
#endif

/** BUILD_BUG *********************************************************/
#if GCC_VERSION >= 40300
#define __cold			__attribute__((__cold__))
# define __compiletime_warning(message) __attribute__((warning(message)))
# define __compiletime_error(message) __attribute__((error(message)))
#endif /* GCC_VERSION >= 40300 */

#ifndef __compiletime_error
# define __compiletime_error(message)
#endif

#define __compiletime_assert(condition, msg, prefix, suffix)		\
	do {								\
		bool __cond = !(condition);				\
		extern void prefix ## suffix(void) __compiletime_error(msg); \
		if (__cond)						\
			prefix ## suffix();				\
		__compiletime_error_fallback(__cond);			\
	} while (0)
#  define __compiletime_error_fallback(condition) \
	do { ((void)sizeof(char[1 - 2 * condition])); } while (0)
#define _compiletime_assert(condition, msg, prefix, suffix) \
	__compiletime_assert(condition, msg, prefix, suffix)
#define compiletime_assert(condition, msg) \
	_compiletime_assert(condition, msg, __compiletime_assert_, __LINE__)
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)
#define BUILD_BUG_ON(condition) \
	BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON failed: " #condition)
#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")
/**********************************************************************/

#endif
