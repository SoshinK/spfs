#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/capability.h>

#include "include/log.h"
#include "include/util.h"

#define SILLYNAME_PREFIX ".nfs"
#define SILLYNAME_PREFIX_LEN ((unsigned)sizeof(SILLYNAME_PREFIX) - 1)
#define SILLYNAME_FILEID_LEN ((unsigned)sizeof(uint64_t) << 1)
#define SILLYNAME_COUNTER_LEN ((unsigned)sizeof(unsigned int) << 1)
#define SILLYNAME_LEN (SILLYNAME_PREFIX_LEN + \
		                SILLYNAME_FILEID_LEN + \
		                SILLYNAME_COUNTER_LEN)

/*
 * This function reallocates passed str pointer.
 * It means:
 *  1) passed pointer can be either NULL, or previously allocated by malloc.
 *  2) Passed pointer can' be reused. It's either freed in case of error or can
 *     be changed.
 */
char *xvstrcat(char *str, const char *fmt, va_list args)
{
	size_t offset = 0, delta;
	int ret;
	char *new;
	va_list tmp;

	if (str)
		offset = strlen(str);
	delta = strlen(fmt) * 2;

	do {
		ret = -ENOMEM;
		new = realloc(str, offset + delta);
		if (new) {
			va_copy(tmp, args);
			ret = vsnprintf(new + offset, delta, fmt, tmp);
			if (ret >= delta) {
				/* NOTE: vsnprintf returns the amount of bytes
				 *                                  * to allocate. */
				delta = ret +1;
				str = new;
				ret = 0;
			}
			va_end(tmp);
		}
	} while (ret == 0);

	if (ret == -ENOMEM) {
		/* realloc failed. We must release former string */
		free(str);
	} else if (ret < 0) {
		/* vsnprintf failed */
		free(new);
		new = NULL;
	}
	return new;
}

char *xstrcat(char *str, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	str = xvstrcat(str, fmt, args);
	va_end(args);

	return str;
}

char *xsprintf(const char *fmt, ...)
{
	va_list args;
	char *str;

	va_start(args, fmt);
	str = xvstrcat(NULL, fmt, args);
	va_end(args);

	return str;
}

int save_fd(int fd, unsigned flags)
{

	if (fd <= STDERR_FILENO) {
		int new_fd;
		unsigned cmd = F_DUPFD;

		if (flags & O_CLOEXEC)
			cmd = F_DUPFD_CLOEXEC;

		pr_info("Duplicating decriptor %d to region above standart "
			"descriptors\n", fd);
		/* We need to move log fd away from first 3 descriptors,
		 * because they will be closed. */
		new_fd = fcntl(fd, cmd, STDERR_FILENO + 1);
		close(fd);
		if (new_fd < 0) {
			pr_perror("duplication of fd %d failed", fd);
			return -errno;
		}
		pr_info("Descriptor %d was moved to %d\n", fd, new_fd);
		fd = new_fd;
	}
	return fd;
}

int execvp_print(const char *file, char *const argv[])
{
	const char **tmp = (const char **)&argv[1];
	char *options = NULL;

	while (*tmp)
		options = xstrcat(options, "%s ", *tmp++);

	pr_info("Executing: %s %s\n", file,
			options ? options : "none");

	free(options);

	execvp(file, argv);

	pr_perror("exec failed");

	return -errno;
}

static int xatol_base(const char *string, long *number, int base)
{
	char *endptr;
	long nr;

	errno = 0;
	nr = strtol(string, &endptr, base);
	if ((errno == ERANGE && (nr == LONG_MAX || nr == LONG_MIN))
			|| (errno != 0 && nr == 0)) {
		pr_perror("failed to convert string");
		return -EINVAL;
	}

	if ((endptr == string) || (*endptr != '\0')) {
		pr_err("String is not a number: '%s'\n", string);
		return -EINVAL;
	}
	*number = nr;
	return 0;
}

int xatol(const char *string, long *number)
{
	return xatol_base(string, number, 10);
}


int xatoi(const char *string, int *number)
{
	long tmp;
	int err;

	err = xatol(string, &tmp);
	if (!err)
		*number = (int)tmp;
	return err;
}

int create_dir(const char *fmt, ...)
{
	int err = -1;
	char *p, *d, *cp = NULL;
	va_list args;

	va_start(args, fmt);
	p = xvstrcat(NULL, fmt, args);
	va_end(args);
	if (!p) {
		pr_err("failed to allocate string\n");
		return -ENOMEM;
	}

	err = -ENOMEM;
	while ((d = strsep(&p, "/")) != NULL) {
		cp = xstrcat(cp, "%s/", d);
		if (!cp)
			goto err;

		if (mkdir(cp, 0777) && (errno != EEXIST)) {
			pr_perror("failed to mkdir %s", cp);
			err = -errno;
			goto err;
		}
	}
	err = 0;
err:
	free(cp);
	free(p);
	return err;
}

int close_inherited_fds(void)
{
	DIR *fds;
	struct dirent *d;
	char *dir = "/proc/self/fd/";

	fds = opendir(dir);
	if (!fds) {
		pr_perror("failed to open %s", dir);
		return -1;
	}

	while ((d = readdir(fds)) != NULL) {
		int fd;

		fd = atoi(d->d_name);
		if (fd > STDERR_FILENO) {
			if (close(fd))
				pr_perror("failed to close %d", fd);
		}
	}

	closedir(fds);
	return 0;
}

int collect_child(int pid, int *status, int options)
{
	int p;

	p = waitpid(pid, status, options);
	if (p < 0) {
		pr_perror("Wait for %d failed", pid);
		return -errno;
	}

	if ((p == 0) && (options & WNOHANG))
		return ECHILD;

	if (WIFSIGNALED(*status)) {
		pr_err("child %d was killed by %d\n", pid, WTERMSIG(*status));
		return -EINTR;
	} else if (WEXITSTATUS(*status)) {
		pr_err("child %d exited with error %d\n", pid, WEXITSTATUS(*status));
		return WEXITSTATUS(*status);
	}
	pr_debug("child %d exited successfully\n", pid);
	return 0;
}

int check_capabilities(unsigned long cap_set, pid_t pid)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
		.pid = pid,
	};
	struct __user_cap_data_struct data[2];
	long cap_effective;

	pr_debug("checking for capabilities 0x%016lx\n", cap_set);

	if (capget(&hdr, data)) {
		pr_perror("capget(%d) failed", getpid());
		return -errno;
	}

	pr_debug("CapEffective: 0x%08x%08x\n", data[1].effective, data[0].effective);
	pr_debug("CapPermitted: 0x%08x%08x\n", data[1].permitted, data[0].permitted);
	pr_debug("CapInherited: 0x%08x%08x\n", data[1].inheritable, data[0].inheritable);

	cap_effective = ((long)data[1].effective << 32) | data[0].effective;
	return cap_set & cap_effective;
}

int secure_chroot(const char *root)
{
	if (strlen(root)) {
		pr_debug("change root to %s\n", root);
		if (chroot(root)) {
			pr_perror("failed to chroot to %s", root);
			return -errno;
		}
		if (chdir("/")) {
			pr_perror("failed to chdir to /");
			return -errno;
		}
	}
	return 0;
}

static char **add_exec_options_varg(char **options, va_list args)
{
	va_list tmp;
	int nr_opt = 0, nr_new;
	char **new_opt;

	if (options) {
		while(options[nr_opt])
			nr_opt++;
	}

	nr_new = nr_opt;

	va_copy(tmp, args);
	while (va_arg(tmp, char *) != NULL)
		nr_new++;
	va_end(tmp);

	new_opt = realloc(options, sizeof(char*) * nr_new + 1);
	if (new_opt) {
		char *arg;

		while ((arg = va_arg(args, char *)) != NULL)
			new_opt[nr_opt++] = arg;
		new_opt[nr_opt] = NULL;
	}

	return new_opt;
}

char **add_exec_options(char **options, ...)
{
	va_list args;
	char **new;

	va_start(args, options);

	new = add_exec_options_varg(options, args);

	va_end(args);

	return new;
}

char **exec_options(int dummy, ...)
{
	va_list args;
	char **new;

	va_start(args, dummy);

	new = add_exec_options_varg(NULL, args);

	va_end(args);

	return new;
}

bool sillyrenamed_path(const char *path)
{
	const char *name;
	int i;

	name = strrchr(path, '/');
	if (name)
		name++;
	else
		name = path;

	if (strncmp(name, SILLYNAME_PREFIX, SILLYNAME_PREFIX_LEN))
		return false;

	name += SILLYNAME_PREFIX_LEN;
	for (i = 0; i < SILLYNAME_FILEID_LEN + SILLYNAME_COUNTER_LEN; i++)
		if (!isxdigit(name[i]))
			return false;

	return true;
}

bool unlinked_path(const char *path)
{
	const char *suffix = " (deleted)";

	if (strlen(path) <= strlen(suffix))
		return false;

	if (!strcmp(path + (strlen(path) - strlen(suffix)), suffix))
		return true;
	return false;
}

void strip_deleted(char *path)
{
	const char *suffix = " (deleted)";

	if (unlinked_path(path))
		path[strlen(path) - strlen(suffix)] = '\0';
}
