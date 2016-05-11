#ifndef __SPFS_MANAGER_FREEZE_H_
#define __SPFS_MANAGER_FREEZE_H_

#include <unistd.h>
#include <semaphore.h>

#include "include/list.h"

struct shared_list;

struct freeze_cgroup_s {
	struct list_head	list;
	char			*path;
	sem_t			sem;
};

struct freeze_cgroup_s *__find_freeze_cgroup(const struct shared_list *groups, const char *path);

struct freeze_cgroup_s *create_freeze_cgroup(const char *path);
int lock_cgroup(struct freeze_cgroup_s *fg);
int unlock_cgroup(struct freeze_cgroup_s *fg);
int thaw_cgroup(const struct freeze_cgroup_s *fg);
int freeze_cgroup(const struct freeze_cgroup_s *fg);

int open_cgroup_state(const struct freeze_cgroup_s *fg);

#endif
