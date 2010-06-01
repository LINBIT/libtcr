#include <signal.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <tcr/threaded_cr.h>

static void starter(void *arg)
{
	struct tc_fd *sig_tcfd;
	int rr, sig_fd = (int)arg;
	struct signalfd_siginfo fdsi;

	sig_tcfd = tc_register_fd(sig_fd);

	while (1) {
		tc_wait_fd(EPOLLIN, sig_tcfd);

		rr = read(sig_fd, &fdsi, sizeof(struct signalfd_siginfo));
		if (rr != sizeof(struct signalfd_siginfo))
			msg_exit(10, "read from signal_fd: ret=%d errno=%m", rr);

		if (fdsi.ssi_signo == SIGINT)
			printf("Got SIGINT\n");
		else if (fdsi.ssi_signo == SIGTERM)
			printf("Got SIGTERM\n");

		tc_sleep(CLOCK_MONOTONIC, 1, 0);
	}
}

int main()
{
	sigset_t sigset;
	int sig_fd;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &sigset, NULL))
		msg_exit(10, "sigprocmask failed with %m\n");

	sig_fd = signalfd(-1, &sigset, 0);
	if (sig_fd == -1)
		msg_exit(10, "signalfd() failed with %m\n");

	tc_run(starter, (void *)sig_fd, "test", sysconf(_SC_NPROCESSORS_ONLN));
	return 0;
}
