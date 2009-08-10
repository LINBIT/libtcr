CFLAGS=-Wall -g
LDFLAGS=-lpthread -g

all: libtc.a tc_main tc_mutex_test1

tc_main: main.o
	$(CC) $(LDFLAGS) -L. -ltc -o $@ $^

tc_mutex_test1: tc_mutex_test1.o
	$(CC) $(LDFLAGS) -L. -ltc -o $@ $^

clean:
	rm -f *.o *.a

libtc.a: threaded_cr.o coroutines.o
	ar rcs $@ $^

