#include <sys/epoll.h>
#include <sys/queue.h>
#include <tcr/threaded_cr.h>
#include <errno.h>
#include <string.h>

struct req {
	char data[15];
	char nul;
	volatile int processed;
	CIRCLEQ_ENTRY(req) ll;
};

static atomic_t writing_active;
static CIRCLEQ_HEAD(, req) queue;
static spinlock_t sl;

ssize_t tc_read(struct tc_fd *t, void *buf, ssize_t buflen)
{
	ssize_t ret, len = buflen;
	enum tc_rv rv;

	while (1) {
		ret = read(tc_fd(t), buf, len);
		if (ret > 0) {
			buf += ret;
			len -= ret;
			if (len == 0)
				return buflen;
		} else if (ret < 0) {
			if (errno != EAGAIN)
				return buflen - len ? buflen - len : ret;
		} else /* ret == 0 */
			return buflen - len;

		rv = tc_wait_fd_prio(EPOLLIN, t);
		if (rv != RV_OK) {
			errno = EINTR;
			return -1;
		}
	}
}

static void worker(void *data)
{
	struct tc_fd *tcfd = (struct tc_fd *)data;
	struct req *r;

	while (1) {
		if (tc_wait_fd(EPOLLIN, tcfd))
			break;
		r = malloc(sizeof(struct req));	     /* begin of READ stage */
		if (tc_read(tcfd, r->data, sizeof(r->data)) < sizeof(r->data))
			break;
		r->nul = 0;

		r->processed = 0;
		spin_lock(&sl);
		CIRCLEQ_INSERT_TAIL(&queue, r, ll);
		spin_unlock(&sl);
		tc_rearm(tcfd);			   /* READ -> PROCESS stage */
		sleep(1);		/* simulate processing for 1 second */
		r->processed = 1;		    /* end of PROCESS stage */
		if (atomic_set_if_eq(1, 0, &writing_active)) { /* WRITE st. */
			spin_lock(&sl);
			CIRCLEQ_FOREACH(r, &queue, ll) {
				if (!r->processed)
					break;
				spin_unlock(&sl);
				write(fileno(stdout), r->data, 10);
				spin_lock(&sl);
				CIRCLEQ_REMOVE(&queue, r, ll);
				free(r);
			}
			spin_unlock(&sl);
			atomic_set(&writing_active, 0); /* end of WRITE st. */
		}
	}
}

static void tc_main(void *arg)
{
	struct tc_thread_pool tp;
	struct tc_fd *tcfd;

	atomic_set(&writing_active, 0);
	CIRCLEQ_INIT(&queue);
	spin_lock_init(&sl);
	tcfd = tc_register_fd(fileno(stdin));
	tc_thread_pool_new(&tp, worker, tcfd, "worker_%d", 0);
	tc_thread_pool_wait(&tp);
	tc_unregister_fd(tcfd);
}

int main(int argc, char** argv)
{
	alarm(10);
	tc_run(tc_main, NULL, "tc_main", 2 /* sysconf(_SC_NPROCESSORS_ONLN) */);
	return 0;
}
