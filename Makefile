CFLAGS +=-Wall -g
LDFLAGS +=-lpthread -g

all: libtc.a tc_main tc_mutex_test1 tc_waitq_test tc_waitq_test2 tc_waitq_test3 tc_waitq_test5

tc_main: main.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc

tc_mutex_test1: tc_mutex_test1.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc

tc_waitq_test: tc_waitq_test.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc

tc_waitq_test2: tc_waitq_test2.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc

tc_waitq_test3: tc_waitq_test3.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc

tc_waitq_test5: tc_waitq_test5.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc

clean:
	rm -f *.o *.a

libtc.a: threaded_cr.o coroutines.o
	ar rcs $@ $^


threaded_cr.o: atomic.h coroutines.h spinlock.h spinlock_plain.h spinlock_debug.h
coroutines.o: coroutines.h
