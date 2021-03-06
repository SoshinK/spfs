#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>


#include "include/log.h"
#include "include/util.h"

static int log_level = LOG_DEBUG;
FILE *stream;

static bool print_timestamp;

static char *print_time(char *buf, size_t size)
{
	struct timeval tv;
	struct tm *tm;

	if (gettimeofday(&tv, NULL))
		return NULL;

	tm = localtime(&tv.tv_sec);
	if (!tm)
		return NULL;

	if (!strftime(buf, size, "%a %b %e %Y %H:%M:%S", tm))
		return NULL;

	sprintf(buf + strlen(buf), ".%06ld", tv.tv_usec);

	return buf;
}

static int print_on_level_va(unsigned int level, const char *format, va_list args)
{
	int saved_errno = errno, res;
	FILE *out = (stream) ? stream : stdout;

	if (level > log_level)
		return 0;

	res = vfprintf(out, format, args);

	errno -= saved_errno;
	return res;
}

int print_on_level(unsigned int loglevel, const char *format, ...)
{
	char buffer[4096];
	char time[64];
	va_list params;
	int res;
	const char *ptr = format;

	if (print_timestamp) {
		snprintf(buffer, sizeof(buffer), "%s  %s",
				print_time(time, sizeof(time)) ? time : "(none)",
				format);
		ptr = buffer;
	}

	va_start(params, format);
	res = print_on_level_va(loglevel, ptr, params);
	va_end(params);
	return res;
}

void set_log_level(FILE *log, int level)
{
	log_level = LOG_ERR + level;
	if (log_level > LOG_DEBUG)
		log_level = LOG_DEBUG;
	pr_info("Log level set to %d\n", log_level);
}

void log_ts_control(bool enable)
{
	print_timestamp = enable;
}

int setup_log_ts(const char *log_file, int verbosity, bool enable_ts)
{
	int fd;
	FILE *log;

	fd = open(log_file, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0) {
		pr_perror("%s: failed to open log file", __func__);
		return -errno;
	}
	fd = save_fd(fd, O_CLOEXEC);
	if (fd < 0) {
		pr_crit("Failed to save log fd\n");
		return fd;
	}
	pr_debug("Log fd: %d\n", fd);
	log = fdopen(fd, "w+");
	if (!log) {
		pr_perror("failed to open log stream");
		close(fd);
		return -errno;
	}
	log_ts_control(enable_ts);
	setvbuf(log, NULL, _IONBF, 0);
	set_log_level(log, verbosity);
	stream = log;
	pr_info("Log file initialized. Verbosity level: %d\n", verbosity);
	return 0;
}

int setup_log(const char *log_file, int verbosity)
{
	return setup_log_ts(log_file, verbosity, true);
}
