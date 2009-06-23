CFLAGS=-Wall -g
LDFLAGS=-lpthread -g

all: tc_main

tc_main: main.o threaded_cr.o coroutines.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o
