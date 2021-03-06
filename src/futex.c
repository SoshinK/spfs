#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <linux/futex.h>

#include "include/futex.h"
#include "include/log.h"

int futex_op(int *uaddr, int op, int val, const struct timespec *timeout,
			                 int *uaddr2, int val3)
{
	if (syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3) < 0) {
		if (errno != ETIMEDOUT)
			pr_perror("SyS_futex failed");
		return -errno;
	}
	return 0;
}

int futex_wait(int *addr, int val, const struct timespec *timeout)
{
	return futex_op(addr, FUTEX_WAIT, val, timeout, NULL, 0);
}

int futex_wake(int *addr)
{
	int err;

	err = futex_op(addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
	return err < 0 ? err : 0;
}
