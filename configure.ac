dnl To produce configure script and config.h.in run autoconf *and* autoheader.
AC_INIT([tc],[0.99],[philipp.reisner@linbit.com])
AC_CONFIG_HEADERS(tc/config.h:tc/config.in)
AC_PROG_CC

CFLAGS="-Wall -O3"

AC_CHECK_HEADERS([sys/timerfd.h])
AC_CHECK_HEADERS([sys/eventfd.h])
AC_CHECK_FUNCS([timerfd_create])
AC_CHECK_FUNCS([timerfd_settime])
AC_CHECK_FUNCS([timerfd_gettime])

AC_ARG_ENABLE([spinlock-debug],
	      [AS_HELP_STRING([--enable-spinlock-debug],
			      [Enable Spinlock debugging])],
	      [AC_DEFINE(SPINLOCK_DEBUG, 1, [Define to enable spinlock debugging])
              CFLAGS="-Wall -g"]
)
AC_ARG_ENABLE([stack-overflow-protection],
	      [AS_HELP_STRING([--enable-stack-overflow-protection],
			      [Enable stack overflow protection])],
	      [AC_DEFINE(STACK_OVERFLOW_PROTECTION, 1, [Define to enable stack overflow protection])
              CFLAGS="-Wall -g"]
)
AC_ARG_ENABLE([wait-debug],
	      [AS_HELP_STRING([--enable-wait-debug],
			      [Enable thread wait debugging])],
	      [AC_DEFINE(WAIT_DEBUG, 1, [Define to enable thread wait debugging])
              CFLAGS="-Wall -g"]
)
AC_OUTPUT([
tests/Makefile
tc/Makefile
Makefile
])
