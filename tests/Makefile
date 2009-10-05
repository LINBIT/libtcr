CFLAGS  += -I../.. -I..
LDFLAGS += -L.. -ltc

TARGETS := main tc_mutex_test1 tc_waitq_test tc_waitq_test2 tc_waitq_test3 tc_waitq_test5 leak_test

all: $(TARGETS)

.SUFFIXES: .o

%.o : %.c
	$(CC) -c -o $@ $^ $(CFLAGS)

% : %.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f *.o $(TARGETS)
