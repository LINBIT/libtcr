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
_EXTRA_CFLAGS="-Wall -Wextra -Wno-unused-parameter -g"

AC_ARG_ENABLE([spinlock-debug],
	      [AS_HELP_STRING([--enable-spinlock-debug],
			      [Enable Spinlock debugging])],
	      [AC_DEFINE(SPINLOCK_DEBUG, 1, [Define to enable spinlock debugging])
	      EXTRA_CFLAGS=$_EXTRA_CFLAGS]
)
AC_ARG_ENABLE([stack-overflow-protection],
	      [AS_HELP_STRING([--enable-stack-overflow-protection],
			      [Enable stack overflow protection])],
	      [AC_DEFINE(STACK_OVERFLOW_PROTECTION, 1, [Define to enable stack overflow protection])
	      EXTRA_CFLAGS=$_EXTRA_CFLAGS]
)
AC_ARG_ENABLE([wait-debug],
	      [AS_HELP_STRING([--enable-wait-debug],
			      [Enable thread wait debugging])],
	      [AC_DEFINE(WAIT_DEBUG, 1, [Define to enable thread wait debugging])
	      EXTRA_CFLAGS=$_EXTRA_CFLAGS]
)
AC_ARG_ENABLE([spinlock-abort],
	      [AS_HELP_STRING([--enable-spinlock-abort],
			      [Enable spinlock abort])],
	      [AC_DEFINE(SPINLOCK_ABORT, 1, [Define to enable abort()ing the program if a spinlock cannot be acquired for too long.])
	      EXTRA_CFLAGS=$_EXTRA_CFLAGS]
)

test -n "$EXTRA_CFLAGS" || EXTRA_CFLAGS="-Wall -O3"

# dirty hack for RHEL5
# 32bit not supported
AC_CHECK_HEADERS([/usr/src/kernels/2.6.18-406.el5-x86_64/include/asm/unistd.h], [have__rhel5=yes], [have__rhel5=no])
if test x$have__rhel5 = xyes ; then
	   AC_DEFINE(HAVE__RHEL5, 1, RHEL5 workarounds)
fi


AC_SUBST(EXTRA_CFLAGS, $EXTRA_CFLAGS)

AC_OUTPUT([
tests/Makefile
tcr/Makefile
Makefile
])
