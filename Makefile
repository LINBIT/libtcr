CFLAGS=-Wall -g
LDFLAGS=-lpthread -g

all: tc_main libtc.so

tc_main: main.o threaded_cr.o coroutines.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o

libtc.so:
	gcc -o libtc.so -shared threaded_cr.o coroutines.o
