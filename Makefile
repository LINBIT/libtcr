#CFLAGS  :=-Wall -O3
#LDFLAGS :=-lpthread -O3

CFLAGS  :=-Wall -g
LDFLAGS :=-lpthread -g

####

export CFLAGS LDFLAGS

.PHONY: tests libtc.a clean 

all: libtc.a tests

tests:
	$(MAKE) -r -C tests

clean:
	$(MAKE) -C tc clean
	$(MAKE) -r -C tests clean

libtc.a:
	$(MAKE) -C tc

