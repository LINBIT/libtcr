dnl To produce configure script and config.h.in run autoconf *and* autoheader.
AC_INIT([libtcr],[0.99],[philipp.reisner@linbit.com])
AC_CONFIG_HEADERS(tcr/config.h:tcr/config.in)
AC_PROG_CC

AC_CHECK_HEADERS([sys/timerfd.h])
AC_CHECK_HEADERS([sys/eventfd.h])
AC_CHECK_FUNCS([timerfd_create])
AC_CHECK_FUNCS([timerfd_settime])

# -O1 would be ok?
# at least enable -Og
EXTRA_CFLAGS="-Wall -Wextra -Wno-unused-parameter -g"

AC_ARG_ENABLE([spinlock-debug],
	      [AS_HELP_STRING([--enable-spinlock-debug],
			      [Enable Spinlock debugging])],
	      [AC_DEFINE(SPINLOCK_DEBUG, 1, [Define to enable spinlock debugging])
              CFLAGS=$EXTRA_CFLAGS]
)
AC_ARG_ENABLE([stack-overflow-protection],
	      [AS_HELP_STRING([--enable-stack-overflow-protection],
			      [Enable stack overflow protection])],
	      [AC_DEFINE(STACK_OVERFLOW_PROTECTION, 1, [Define to enable stack overflow protection])
              CFLAGS=$EXTRA_CFLAGS]
)
AC_ARG_ENABLE([wait-debug],
	      [AS_HELP_STRING([--enable-wait-debug],
			      [Enable thread wait debugging])],
	      [AC_DEFINE(WAIT_DEBUG, 1, [Define to enable thread wait debugging])
              CFLAGS=$EXTRA_CFLAGS]
)
AC_ARG_ENABLE([spinlock-abort],
	      [AS_HELP_STRING([--enable-spinlock-abort],
			      [Enable spinlock abort])],
	      [AC_DEFINE(SPINLOCK_ABORT, 1, [Define to enable abort()ing the program if a spinlock cannot be acquired for too long.])
              CFLAGS=$EXTRA_CFLAGS]
)

test -n "$CFLAGS" || CFLAGS="-Wall -O3"

AC_OUTPUT([
tests/Makefile
tcr/Makefile
Makefile
])
