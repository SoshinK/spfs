#ifndef __SPFS_MANAGER_CONTEXT_H_
#define __SPFS_MANAGER_CONTEXT_H_

#include <stdbool.h>
#include <stddef.h>
#include <semaphore.h>

#include <spfs/context.h>

struct spfs_mounts_s;

struct spfs_manager_context_s {
	const char	*progname;

	char	*work_dir;
	char	*log_file;
	char	*socket_path;
	int	verbosity;
	bool	daemonize;
	bool	exit_with_spfs;

	int	sock;

	struct shared_list *spfs_mounts;
	struct shared_list *freeze_cgroups;
};

struct spfs_manager_context_s *create_context(int argc, char **argv);

extern int spfs_manager_packet_handler(int sock, void *data, void *package, size_t psize);

int join_namespaces(int pid, const char *namespaces);

#endif
