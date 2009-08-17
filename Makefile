CFLAGS=-Wall -g
LDFLAGS=-lpthread -g

all: libtc.a tc_main tc_mutex_test1

tc_main: main.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc 

tc_mutex_test1: tc_mutex_test1.o
	$(CC) -o $@ $^ $(LDFLAGS) -L. -ltc 

clean:
	rm -f *.o *.a

libtc.a: threaded_cr.o coroutines.o
	ar rcs $@ $^

