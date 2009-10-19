#CFLAGS +=-Wall
#LDFLAGS +=-lpthread

CFLAGS +=-Wall -g -DSPINLOCK_DEBUG -DSTACK_OVERFLOW_PROTECTION
LDFLAGS +=-lpthread -g

####

ARCH	:= $(shell uname -m | sed -e s/i.86/i386/)
export CFLAGS LDFLAGS

.PHONY: tests clean

all: libtc.a tests

tests:
	$(MAKE) -r -C tests

clean:
	rm -f *.o *.a
	$(MAKE) -r -C tests clean

libtc.a: threaded_cr.o coroutines.o swapcontext_fast_$(ARCH).o
	ar rcs $@ $^


threaded_cr.o: atomic.h coroutines.h spinlock.h spinlock_plain.h spinlock_debug.h threaded_cr.h config.h
coroutines.o: coroutines.h
