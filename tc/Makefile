ARCH	:= $(shell uname -m | sed -e s/i.86/i386/)

.PHONY: clean

all: libtc.a

clean:
	rm -f *.o *.a

libtc.a: threaded_cr.o coroutines.o swapcontext_fast_$(ARCH).o
	ar rcs $@ $^

threaded_cr.o: atomic.h coroutines.h spinlock.h spinlock_plain.h spinlock_debug.h threaded_cr.h ../config.h
coroutines.o: coroutines.h
