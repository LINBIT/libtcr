CFLAGS +=-Wall -g -DDEBUG
LDFLAGS +=-lpthread -g
export CFLAGS LDFLAGS

.PHONY: tests clean

all: libtc.a tests

tests:
	$(MAKE) -r -C tests

clean:
	rm -f *.o *.a
	$(MAKE) -r -C tests clean

libtc.a: threaded_cr.o coroutines.o
	ar rcs $@ $^


threaded_cr.o: atomic.h coroutines.h spinlock.h spinlock_plain.h spinlock_debug.h
coroutines.o: coroutines.h
