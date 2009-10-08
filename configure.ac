AC_INIT([tc],[0.99],[philipp.reisner@linbit.com])
AC_CONFIG_HEADERS(config.h:config.in)
AC_PROG_CC
dnl AC_EGREP_HEADER(MAP_STACK, asm/mman.h, AC_DEFINE(HAVE_MAP_STACK,[],[comment]))
AC_CHECK_HEADERS([sys/timerfd.h])
AC_CHECK_HEADERS([sys/eventfd.h])
AC_CHECK_FUNCS([timerfd_create])
AC_CHECK_FUNCS([timerfd_settime])
AC_CHECK_FUNCS([timerfd_gettime])
AC_OUTPUT
