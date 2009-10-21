#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include "threaded_cr.h"


/* mempool */

struct mempool {
	atomic_t available;
	struct tc_waitq wq;
};

void mempool_init(struct mempool *mp, int size)
{
	atomic_set(&mp->available, size);
	tc_waitq_init(&mp->wq);
}

static int _try_alloc(struct mempool *mp, int size)
{
	if (atomic_sub_return(size, &mp->available) > 0)
		return 1;

	atomic_add_return(size, &mp->available);
	return 0;
}

void *mempool_alloc(struct mempool *mp, int size)
{
	void *rv;

	if (tc_waitq_wait_event(&mp->wq, _try_alloc(mp, size)) != RV_OK)
		return NULL;

	rv = malloc(size);
	if (!rv)
		atomic_add_return(size, &mp->available);
	return rv;
}

void mempool_free(struct mempool *mp, void* mem, int size)
{
	free(mem);
	atomic_add_return(size, &mp->available);
	tc_waitq_wakeup(&mp->wq);
}

pid_t gettid(void)
{
	return syscall(__NR_gettid);
}

static int create_unix_socket(char *path)
{
	int fd = -1;
        static struct sockaddr_un addr;

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

        fd = socket(PF_UNIX, SOCK_STREAM, 0);
        if (fd == -1) {
		fprintf(stderr, "socket() failed %d %m\n", errno);
		goto err_out;
        }

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr))) {
		if (errno != EADDRINUSE) {
			fprintf(stderr, "bind() failed %d %m\n", errno);
			goto err_out;
		}
		if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
			if (errno != ECONNREFUSED) {
				fprintf(stderr, "connect to existing ctl-socket");
				goto err_out;
			}
			/* stale socket file... */
			remove(path);
			if (bind(fd, (struct sockaddr*)&addr, sizeof(addr))) {
				fprintf(stderr, "bind %d %m", errno);
				goto err_out;
			}
		} else {
			/* got a connection... */
			fprintf(stderr, "got a connection\n");
			exit(1);
		}
        }

        if (listen (fd,10)) {
		fprintf(stderr, "listen %d %m\n", errno);
		goto err_out;
        }

        return fd;

err_out:
	if (fd >= 0) {
		close (fd);
	}
	return -1;
}

struct reader_info {
	struct mempool   *mp;
	struct tc_signal *all_exit;
	struct tc_fd     *in;
};

static void unix_socket_reader(void *data)
{
	struct reader_info *ri = (struct reader_info *)data;
	struct tc_signal_sub *e;
	char b[10];
	int rr;
	void *m;

	e = tc_signal_subscribe(ri->all_exit);

	while(1) {
		if (tc_wait_fd(EPOLLIN, ri->in) != RV_OK)
			break;
		rr = read(tc_fd(ri->in), b, 10);
		tc_rearm();

		m = mempool_alloc(ri->mp, 1001);
		if (!m)
			break;

		poll(NULL, 0 ,5000); /* sleep 10 milliseconds */

		mempool_free(ri->mp, m, 1001);

		if (rr <= 0) {
			fprintf(stderr, "read() failed: %d, %m\n", errno);
			exit(1);
		}
		printf("%d: ", gettid());
		fwrite(b, rr, 1, stdout);
		fflush(stdout);
	}

	tc_signal_unsubscribe(ri->all_exit, e);
}

static void stdin_reader(void *data)
{
	struct reader_info ri;
	struct tc_signal all_exit;
	struct mempool mp;
	struct tc_fd *tcfd;
	struct tc_threads sr;
	char b[10];
	int rr;
	int fd, lfd;

	lfd = create_unix_socket("/tmp/tc_test");

	fd = accept(lfd, NULL, NULL);
	if (fd < 0) {
		fprintf(stderr,"accept %d %m\n",errno);
		exit(1);
	}

	tc_signal_init(&all_exit);

	mempool_init(&mp, 3000);

	ri.mp = &mp;
	ri.all_exit = &all_exit;
	ri.in = tc_register_fd(fd);
	tc_thread_pool_new(&sr, unix_socket_reader, &ri, "unix_socket_reader_%d");

	tcfd = tc_register_fd(fileno(stdin));
	while(1) {
		tc_wait_fd(EPOLLIN, tcfd);
		rr = read(fileno(stdin), b, 10);
		tc_rearm();
		if (rr <= 0) {
			fprintf(stderr, "read() failed: %d, %m\n", errno);
			exit(1);
		}
		fflush(stdout);
		if (strncmp(b, "exit", 4) == 0)
			break;
		fwrite(b, rr, 1, stdout);
	}

	tc_signal_fire(&all_exit);
	tc_unregister_fd(tcfd);
	tc_thread_pool_wait(&sr);
	tc_signal_destroy(&all_exit);
}

int main(int argc, char** argv)
{
	int nr_worker=4;
	int foreground=0;
	int c;

	static struct option options[] = {
		{"worker-threads", required_argument, 0, 'w'},
		{"foreground", no_argument, 0, 'f'},
		{0, 0, 0, 0}
	};

	while (1) {
		c = getopt_long(argc, argv, "m:w:fl:", options, 0);
		if (c == -1)
			break;
		switch (c) {
		case 'w':
			nr_worker = atoi(optarg);
			break;
		case 'f':
			foreground=1;
			break;
		}
	}

	tc_run(stdin_reader, NULL, "stdin_reader", 3);

	return 0;
}
