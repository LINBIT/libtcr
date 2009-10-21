#include "threaded_cr.h"
#include "atomic.h"
#include <sys/epoll.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/queue.h>
#include <semaphore.h>
#include <arpa/inet.h>


struct tc_waitq memory_wq;

spinlock_t used_mem_spinlock;

size_t used_mem, max_mem;

static int enough_memory(int thread_no)
{
	fprintf(stderr, "%d: Testing if enough memory (%zd <= %zd)\n", thread_no, used_mem, max_mem);
	return used_mem <= max_mem;
}

void *sleeping_malloc(size_t bytes, int thread_no)
{
	enum tc_rv rv;

	if ((rv = tc_waitq_wait_event(&memory_wq, enough_memory(thread_no))) < 0) {
		fprintf(stderr, "tc_waitq_wait returned %d\n", rv);
		return NULL;
	}

	fprintf(stderr, "%d: reader: mallocing %d bytes (wq is %p)\n", thread_no, bytes, &memory_wq);
	spin_lock(&used_mem_spinlock);
	used_mem += bytes;
	spin_unlock(&used_mem_spinlock);

	return (void*) 1;   // tobe ignored
}


void awakening_free(void *p, size_t bytes)
{
	spin_lock(&used_mem_spinlock);
	used_mem -= bytes;
	spin_unlock(&used_mem_spinlock);

	fprintf(stderr, "writer: used_mem: %zd max_mem: %zd\n", used_mem, max_mem);
	fprintf(stderr, "writer: waking up memory wq (%d bytes) wq is %p\n", bytes, &memory_wq);
	tc_waitq_wakeup(&memory_wq);
}


int do_write_packets(void)
{
	int count;

	count = 0;
	for (count = 0; count < 10; count++) {
		awakening_free(NULL, 1);
		fprintf(stderr, "do_write_packets: %d\n", count);
		tc_sleep(CLOCK_MONOTONIC, 1, 0); // works
		// sleep(1); // works not
	}
	return 0;

}

void drbd_connection_writer(void *unused)
{
	int ret;

	while (1) {
		fprintf(stderr, "drbd_writer: into do_write_packets\n");
		ret = do_write_packets();
		fprintf(stderr, "drbd_writer: out of do_write_packets\n");
		if (ret < 0) {
			break;
		}
	}
}


void drbd_connection(void *unused)
{
	void *ignore;
	static int thread_no = 0;
	int my_thread_no = thread_no++;

	fprintf(stderr, "DRBD reader %d started.\n", my_thread_no);

	while (1) {
		fprintf(stderr, "%d: into sleeping_malloc 1\n", my_thread_no);
		if ((ignore = sleeping_malloc(1, my_thread_no)) == NULL) {
			break;
		}
		fprintf(stderr, "%d: out of sleeping_malloc 1\n", my_thread_no);
	}
}


static void starter(void *unused)
{
	struct tc_threads threads;

	tc_waitq_init(&memory_wq);

	spin_lock_init(&used_mem_spinlock);
	used_mem = 0;
	max_mem = 5;

	tc_thread_pool_new(&threads, drbd_connection, NULL, "DRBD conn");
	tc_thread_wait(tc_thread_new(drbd_connection_writer, NULL, "writer"));
}


int main()
{
	tc_run(starter, NULL, "test", 4);
	return 0;
}
