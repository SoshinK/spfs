#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>

#include "include/list.h"
#include "include/log.h"
#include "include/util.h"
#include "include/shm.h"

#include "spfs.h"
#include "trees.h"
#include "swapfd.h"
#include "processes.h"

struct processes_collection_s {
	struct list_head	*collection;
	dev_t			src_dev;
	const char		*source_mnt;
	const char		*target_mnt;
};

char *ns_names[NS_MAX] = {
	[NS_UTS] = "uts",
	[NS_MNT] = "mnt",
	[NS_NET] = "net",
	[NS_PID] = "pid",
	[NS_USER] = "user"
};

int open_ns(pid_t pid, const char *ns_type)
{
	int fd;
	char path[PATH_MAX];

	snprintf(path, PATH_MAX, "/proc/%d/ns/%s", pid, ns_type);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		pr_perror("failed to open %s", path);
		return -errno;
	}
	return fd;
}

int set_namespaces(int *ns_fds, unsigned ns_mask)
{
	int ns_type, err;

	if (!ns_fds)
		return 0;

	for (ns_type = NS_UTS; ns_type < NS_MAX; ns_type++) {
		if ((ns_mask & (1 << ns_type)) == 0)
			continue;

		if (ns_fds[ns_type] < 0) {
			pr_err("failed to set %s ns: fd is closed\n",
					ns_names[ns_type]);
			continue;
		}

		err = setns(ns_fds[ns_type], 0);
		if (err) {
			pr_perror("failed to set ns by fd %d", ns_fds[ns_type]);
			break;
		}
	}
	return err;
}

int close_namespaces(int *ns_fds)
{
	int ns_type;

	if (!ns_fds)
		return 0;

	for (ns_type = NS_UTS; ns_type < NS_MAX; ns_type++) {
		if (ns_fds[ns_type] < 0)
			continue;
		close(ns_fds[ns_type]);
		ns_fds[ns_type] = -1;
	}
	return 0;
}

int open_namespaces(pid_t pid, int *ns_fds)
{
	int err, ns_type;

	for (ns_type = NS_UTS; ns_type < NS_MAX; ns_type++) {
		err = open_ns(pid, ns_names[ns_type]);
		if (err < 0)
			goto close_saved_fd;
		ns_fds[ns_type] = err;
	}

	return 0;

close_saved_fd:
	(void)close_namespaces(ns_fds);
	return err;
}

int change_namespaces(pid_t pid, unsigned ns_mask, int *orig_ns_fds[])
{
	int ns_fds[NS_MAX] = {
		-1, -1, -1, -1, -1
	};
	int err;

	if (orig_ns_fds) {
		err = open_namespaces(getpid(), ns_fds);
		if (err)
			return err;
	}

	err = open_namespaces(pid, ns_fds);
	if (err)
		return err;

	err = set_namespaces(ns_fds, ns_mask);

	(void)close_namespaces(ns_fds);
	return err;
}

static int seize_one_process(const struct process_info *p)
{
	if (wait_task_seized(p->pid)) {
		pr_err("failed to seize process %d\n", p->pid);
		return -EPERM;
	}
	pr_debug("seized process %d\n", p->pid);
	return 0;
}

int seize_processes(struct list_head *processes)
{
	const struct process_info *p;

	list_for_each_entry(p, processes, list) {
		if (seize_one_process(p))
			return -EPERM;
	}
	return 0;
}

static int detach_from_process(const struct process_info *p)
{
	if (detach_from_task(p->pid)) {
		pr_err("failed to detach from process %d\n", p->pid);
		return -EPERM;
	}
	pr_debug("detached from process %d\n", p->pid);
	return 0;
}

int release_processes(struct list_head *processes)
{
	struct process_info *p, *tmp;

	list_for_each_entry_safe(p, tmp, processes, list) {
		(void) detach_from_process(p);
		list_del(&p->list);
		free(p);
	}
	return 0;
}

static int attach_to_process(const struct process_info *p)
{
	if (attach_to_task(p->pid) != p->pid) {
		pr_err("failed to attach to process %d\n", p->pid);
		return -1;
	}
	pr_debug("attached to process %d\n", p->pid);
	return 0;
}

static bool is_mnt_file(int dir, const char *dentry, const char *source_mnt, dev_t device)
{
	struct stat st;
	char link[PATH_MAX];
	ssize_t bytes;

	/* First check, that link points to the desired mount (if any).
	 * This is required to be able to switch between 2 different mounts
	 * with the same superblock.
	 */
	if (source_mnt) {
		bytes = readlinkat(dir, dentry, link, PATH_MAX - 1);
		if (bytes < 0) {
			pr_perror("failed to read link %s\n", dentry);
			return -errno;
		}

		if (strncmp(link, source_mnt, strlen(source_mnt)))
			return false;

		if (link[strlen(source_mnt)] != '/')
			return false;
	}

	if (fstatat(dir, dentry, &st, 0)) {
		switch (errno) {
			case ENOENT:
			case ENOTDIR:
				break;
			default:
				pr_perror("failed to stat dentry %s", dentry);
		}
		return false;
	}
	return st.st_dev == device;
}

static int pid_is_kthread(pid_t pid)
{
	char path[PATH_MAX];
	char link[8];

	snprintf(path, PATH_MAX, "/proc/%d/exe", pid);
	if (readlink(path, link, 8) == -1)
		return true;
	return false;
}

int iterate_pids_list_name(const char *pids_list, void *data,
			   int (*actor)(pid_t pid, void *data),
			   const char *actor_name)
{
	char *list, *l, *pid;
	int err = 0;

	if (!pids_list) {
		pr_err("pids_list is NULL\n");
		return -EINVAL;
	}

	list = l = strdup(pids_list);
	if (!list) {
		pr_err("failed to allocate\n");
		return -ENOMEM;
	}

	while ((pid = strsep(&l, "\n")) != NULL) {
		long p;

		if (!strlen(pid))
			continue;

		err = xatol(pid, &p);
		if (err) {
			pr_err("failed to convert pid %s to number\n", pid);
			break;
		}

		if (pid_is_kthread(p)) {
			pr_debug("Skipping kthread %s\n", pid);
			continue;
		}

		err = actor(p, data);
		if (err) {
			pr_err("actor %s failed for pid %d\n", actor_name, p);
			break;
		}
	}
	free(list);
	return err;
}

int get_pids_list(const char *tasks_file, char **list)
{
	char *pids_list;
	int err = -ENOMEM, fd;
	char buf[4096] = { };
	ssize_t bytes;

	fd = open(tasks_file, O_RDONLY);
	if (fd < 0) {
		pr_perror("failed to open %s", tasks_file);
		return -errno;
	}

	pids_list = NULL;
	do {
		bytes = read(fd, buf, sizeof(buf) - 1);
		if (bytes < 0) {
			pr_perror("failed to read %s", tasks_file);
			err = -errno;
			goto free_pids_list;
		}
		buf[bytes] = '\0';
		if (bytes) {
			pids_list = xstrcat(pids_list, "%s", buf);
			if (!pids_list) {
				pr_err("failed to allocate\n");
				goto close_fd;
			}
		}
	} while (bytes > 0);

	*list = pids_list;
	err = 0;

	pr_debug("Pids list:\n%s\n", *list);

close_fd:
	close(fd);
	return err;

free_pids_list:
	free(pids_list);
	goto close_fd;
}

static int get_fd_flags(pid_t pid, int fd)
{
	char path[PATH_MAX];
	FILE *fdinfo;
	char buf[64];
	int flags = -ENOENT;

	snprintf(path, PATH_MAX, "/proc/%d/fdinfo/%d", pid, fd);

	fdinfo = fopen(path, "r");
	if (!fdinfo) {
		pr_perror("failed to open %s", path);
		return -errno;
	}

	while (fgets(buf, 64, fdinfo) != NULL) {
		pr_debug("fdinfo string: %s", buf);
		if (strncmp(buf, "flags", strlen("flags")))
			continue;
		if (sscanf(buf, "flags:\t%o", &flags) != 1) {
			pr_err("failed to sscanf '%s'\n", buf);
			flags = -EINVAL;
		}
		break;
	}
	if (flags < 0)
		pr_err("failed to get %s flags: %d\n", path, flags);
	return flags;
}

static void *fifo_file_obj(const char *path, unsigned flags, struct replace_fd *rfd)
{
	pr_err("fifo is not supported yet\n");
	return NULL;
}

static void *reg_file_obj(const char *path, unsigned flags, struct replace_fd *rfd)
{
	int fd;

	fd = open(path, flags);
	if (fd < 0) {
		pr_perror("failed to open %s", path);
		return NULL;
	}
	return (void *)(long)fd;
}

static int create_file_obj(const char *path, unsigned flags, struct replace_fd *rfd)
{
	/* TODO move these actors to tree creation and place them on replace_fd
	 * structure */
	switch (rfd->mode & S_IFMT) {
		case S_IFDIR:
		case S_IFREG:
			rfd->file_obj = reg_file_obj(path, flags, rfd);
			break;
		case S_IFIFO:
			rfd->file_obj = fifo_file_obj(path, flags, rfd);
		case S_IFSOCK:
		case S_IFLNK:
		case S_IFBLK:
		case S_IFCHR:
			return -ENOTSUP;
		default:
			pr_err("unknown file mode: 0%o\n", rfd->mode & S_IFMT);
			return -EINVAL;
	}
	return rfd->file_obj ? 0 : -EPERM;
}

static int fixup_source_path(const char *source_path,
			     const char *source_mnt, const char *target_mnt,
			     char *path, size_t size)
{
	const char *sp = source_path;
	ssize_t bytes;

	if (source_mnt) {
		size_t len = strlen(source_mnt);

		if (strncmp(source_path, source_mnt, len)) {
			pr_err("link %s doesn't start with source mnt %s\n",
					source_path, source_mnt);
			return -EINVAL;
		}
		sp += len;
	}

	bytes = snprintf(path, size, "%s%s", target_mnt, sp);
	if (bytes > size) {
		pr_err("target path is too long (%ld > %ld)\n",	bytes, size);
		return -ENOMEM;
	}
	pr_debug("%s --> %s\n", source_path, path);
	return 0;
}

static int get_link_path(const char *link,
			 const char *source_mnt, const char *target_mnt,
			 char *path, size_t size)
{
	char source_path[PATH_MAX];
	ssize_t bytes;
	int err;

	bytes = readlink(link, source_path, PATH_MAX - 1);
	if (bytes < 0) {
		pr_perror("failed to read link %s\n", link);
		return -errno;
	}
	source_path[bytes] = '\0';

	err = fixup_source_path(source_path, source_mnt, target_mnt, path, size);
	if (!err)
		pr_debug("%s --> %s\n", link, path);
	return err;
}

static int open_target_fd(struct replace_fd *rfd, unsigned flags,
			  const char *source_mnt, const char *target_mnt)
{
	char link[PATH_MAX];
	char path[PATH_MAX];
	struct stat st;
	int err;

	snprintf(link, PATH_MAX, "/proc/%d/fd/%d", rfd->pid, rfd->fd);

	err = get_link_path(link, source_mnt, target_mnt, path, sizeof(path));
	if (err)
		return err;

	if (stat(path, &st)) {
		pr_perror("failed to stat %s", path);
		return -errno;
	}

	rfd->mode = st.st_mode;

	/* TODO it makes sense to open only shared files here.
	 * Private files can be opened by the process itself */

	return create_file_obj(path, flags, rfd);
}

static int get_target_fd(pid_t pid, int fd,
			 const char *source_mnt, const char *target_mnt)
{
	int err;
	struct replace_fd *rfd;
	int flags;

	flags = get_fd_flags(pid, fd);
	if (flags < 0)
		return flags;

	err = collect_fd(pid, fd, &rfd);
	if (err) {
		pr_err("failed to add /proc/%d/fd/%d to tree\n", pid, fd);
		return err;
	}

	if (!rfd->file_obj) {
		err = open_target_fd(rfd, flags, source_mnt, target_mnt);
		if (err) {
			pr_err("failed to open file object for /proc/%d/fd/%d\n",
					rfd->pid, rfd->fd);
			return err;
		}
	}

	switch (rfd->mode & S_IFMT) {
		case S_IFDIR:
		case S_IFREG:
			return (long)rfd->file_obj;
		case S_IFIFO:
		case S_IFSOCK:
		case S_IFLNK:
		case S_IFBLK:
		case S_IFCHR:
			break;
		default:
			pr_err("unknown file mode: 0%o\n", rfd->mode & S_IFMT);
			return -EINVAL;
	}
	return -ENOTSUP;
}

static int process_add_fd(struct process_info *p, int source_fd, int target_fd)
{
	struct process_fd *pfd;

	pfd = malloc(sizeof(*pfd));
	if (!pfd) {
		pr_err("failed to allocate pfd\n");
		return -ENOMEM;
	}

	pfd->source_fd = source_fd;
	pfd->target_fd = target_fd;
	list_add_tail(&pfd->list, &p->fds);
	p->fds_nr++;

	pr_debug("Added replace fd: /proc/%d/fd/%d --> /proc/%d/fd/%d\n",
			getpid(), pfd->source_fd, p->pid, pfd->target_fd);
	return 0;
}

static int process_add_mapping(struct process_info *p, int map_fd,
				off_t start, off_t end)
{
	struct process_map *mfd;

	mfd = malloc(sizeof(*mfd));
	if (!mfd) {
		pr_err("failed to allocate mfd\n");
		return -ENOMEM;
	}

	mfd->map_fd = map_fd;
	mfd->start = start;
	mfd->end = end;
	list_add_tail(&mfd->list, &p->maps);
	p->maps_nr++;

	pr_debug("Added replace mapping: /proc/%d/fd/%d (%lx-%lx)\n",
			getpid(), mfd->map_fd, mfd->start, mfd->end);
	return 0;
}

static int collect_process_fd(struct process_info *p, int dir,
			      const char *process_fd, const void *data)
{
	int err, source_fd, target_fd;
	const struct processes_collection_s *pc = data;

	if (!is_mnt_file(dir, process_fd, pc->source_mnt, pc->src_dev))
		return 0;

	pr_debug("Collecting /proc/%d/fd/%s\n", p->pid, process_fd);

	err = xatol(process_fd, (long *)&source_fd);
	if (err) {
		pr_err("failed to convert fd %s to number\n", process_fd);
		return err;
	}

	target_fd = get_target_fd(p->pid, source_fd,
				  pc->source_mnt, pc->target_mnt);
	if (target_fd < 0)
		return target_fd;

	return process_add_fd(p, source_fd, target_fd);
}

static int iterate_dir_name(const char *dpath, struct process_info *p,
		    int (*actor)(struct process_info *p, int dir,
				 const char *fd, const void *data),
		    const void *data,
		    const char *actor_name)
{
	struct dirent *dt;
	DIR *fdir;
	int dir;
	int err;

	fdir = opendir(dpath);
	if (!fdir) {
		pr_perror("failed to open %s", dpath);
		return -errno;
	}

	dir = dirfd(fdir);
	if (dir < 0) {
		pr_perror("failed to get fd for directory stream");
		err = -errno;
		goto close_dir;
	}

        while ((dt = readdir(fdir)) != NULL) {
		char *fd = dt->d_name;

		if (!strcmp(fd, ".") || !strcmp(fd, ".."))
			continue;

		err = actor(p, dir, fd, data);
		if (err) {
			pr_err("actor '%s' for %s/%s\n failed\n",
					actor_name, dpath, fd);
			break;
		}
	}

close_dir:
	closedir(fdir);
	return err;
}

static int collect_process_open_fds(struct process_info *p,
				    struct processes_collection_s *pc)
{
	char dpath[PATH_MAX];

	snprintf(dpath, PATH_MAX, "/proc/%d/fd", p->pid);
	return iterate_dir_name(dpath, p, collect_process_fd, pc, "collect_process_fd");
}

static int collect_map_file(struct process_info *p,
			    unsigned long start, unsigned long end,
			    mode_t mode, const char *path)
{
	int fd, map_fd = -1, err;

	pr_debug("Collecting /proc/%d/map_files/%lx-%lx\n", p->pid, start, end);

	fd = open(path, mode);
	if (fd < 0) {
		pr_perror("failed to open %s", path);
		return -errno;
	}

	err = collect_map_fd(fd, path, mode, &map_fd);
	if (err) {
		pr_err("failed to collect map fd for path %s\n", path);
		goto close_fd;
	}

	err = process_add_mapping(p, map_fd, start, end);

close_fd:
	if ((fd != map_fd) || err)
		close(fd);
	return err;
}

static mode_t map_open_mode(char r, char w, char p)
{
	if ((w == 'w') && (p == 's')) {
		if (r == 'r')
			return O_RDWR;
		return O_WRONLY;
	}
	/* Private write mapping have to be opened with O_RDONLY, because it
	 * can be an executable, which is used already (opened via spfs) and in
	 * this case system won't allow to open it in write mode.
	 */
	return O_RDONLY;
}

static bool is_mnt_map(int dir, unsigned long start, unsigned long end,
		       struct processes_collection_s *pc)
{
	char path[PATH_MAX];

	snprintf(path, PATH_MAX, "%lx-%lx", start, end);
	return is_mnt_file(dir, path, pc->source_mnt, pc->src_dev);
}

static int collect_process_map_fds(struct process_info *p,
				   struct processes_collection_s *pc)
{
	char map[PATH_MAX];
	FILE *fmap;
	int err = -ENOENT;
	int dir;

	snprintf(map, PATH_MAX, "/proc/%d/map_files", p->pid);
	dir = open(map, O_RDONLY | O_DIRECTORY);
	if (dir < 0) {
		pr_perror("failed to open %s", map);
		return -errno;
	}

	snprintf(map, PATH_MAX, "/proc/%d/maps", p->pid);
	fmap = fopen(map, "r");
	if (!fmap) {
		pr_perror("failed to open %s", map);
		err = -errno;
		goto close_dir;
	}


	while (fgets(map, sizeof(map), fmap)) {
		char path[PATH_MAX];
		unsigned long start, end;
		char r, w, prot;
		int ret, path_off;
		char *map_file;

		map[strlen(map)-1] = '\0';

		ret = sscanf(map, "%lx-%lx %c%c%*c%c %*x %*x:%*x %*u %n",
				&start, &end, &r, &w, &prot, &path_off);
		if (ret != 5) {
			pr_err("failed to parse '%s': %d\n", map, ret);
			err = -EINVAL;
			goto close_fmap;
		}

		if (!is_mnt_map(dir, start, end, pc))
			continue;

		map_file = map + path_off;

		err = fixup_source_path(map_file,
					pc->source_mnt, pc->target_mnt,
					path, sizeof(path));
		if (err)
			goto close_fmap;

		err = collect_map_file(p, start, end,
				       map_open_mode(r, w, prot), path);
		if (err)
			goto close_fmap;
	}
	err = 0;
close_fmap:
	fclose(fmap);
close_dir:
	close(dir);
	return err;

}

static int open_process_env(struct process_info *p,
			    struct processes_collection_s *pc,
			    const char *dentry)
{
	char path[PATH_MAX];
	char link[PATH_MAX];

	int err, fd;

	snprintf(link, PATH_MAX, "/proc/%d/%s", p->pid, dentry);
	err = get_link_path(link, pc->source_mnt, pc->target_mnt, path, sizeof(path));
	if (err)
		return err;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		pr_perror("failed to open %s", path);
		return -errno;
	}
	return fd;
}

static int collect_process_fs(struct process_info *p,
			       struct processes_collection_s *pc,
			       int dir)
{
	bool mnt_cwd, mnt_root;
	int err;
	bool exists;

	mnt_cwd = is_mnt_file(dir, "cwd", pc->source_mnt, pc->src_dev);
	mnt_root = is_mnt_file(dir, "root", pc->source_mnt, pc->src_dev);

	if (!mnt_cwd && ! mnt_root)
		return 0;

	err = collect_fs_struct(p->pid, &exists);
	if (err) {
		pr_err("failed to collect process %d fs\n", p->pid);
		return err;
	}
	if (exists) {
		pr_info("ignoring process %d fs\n", p->pid);
		return 0;
	}

	if (mnt_cwd) {
		pr_debug("Collecting /proc/%d/cwd\n", p->pid);

		p->fs.cwd_fd = open_process_env(p, pc, "cwd");
		if (p->fs.cwd_fd < 0)
			return p->fs.cwd_fd;
	}

	if (mnt_root) {
		pr_debug("Collecting /proc/%d/root\n", p->pid);

		p->fs.root_fd = open_process_env(p, pc, "root");
		if (p->fs.root_fd < 0)
			return p->fs.root_fd;
	}
	return 0;
}

static int collect_process_exe(struct process_info *p,
			       struct processes_collection_s *pc,
			       int dir)
{
	if (!is_mnt_file(dir, "exe", pc->source_mnt, pc->src_dev))
		return 0;

	pr_debug("Collecting /proc/%d/exe\n", p->pid);

	p->exe_fd = open_process_env(p, pc, "exe");
	if (p->exe_fd < 0)
		return p->exe_fd;
	return 0;
}

static int collect_process_env(struct process_info *p,
			       struct processes_collection_s *pc)
{
	int dir, err;
	char path[PATH_MAX];

	snprintf(path, PATH_MAX, "/proc/%d", p->pid);
	dir = open(path, O_RDONLY | O_DIRECTORY);
	if (dir < 0) {
		pr_perror("failed to open %s", path);
		return -errno;
	}

	err = collect_process_exe(p, pc, dir);
	if (err)
		goto close_dir;

	err = collect_process_fs(p, pc, dir);

close_dir:
	close(dir);
	return err;
}

static int collect_process_fd_table(struct process_info *p,
			            struct processes_collection_s *pc)
{
	int err;
	bool exists;

	err = collect_fd_table(p->pid, &exists);
	if (err) {
		pr_err("failed to collect process %d fd table\n", p->pid);
		return err;
	}
	if (exists) {
		pr_info("ignoring process %d fd table\n", p->pid);
		return 0;
	}

	return collect_process_open_fds(p, pc);
}

static int collect_one_process(pid_t pid, void *data)
{
	int err;
	struct processes_collection_s *pc = data;
	struct process_info *p;

	p = malloc(sizeof(*p));
	if (!p) {
		pr_err("failed to allocate process\n");
		return -ENOMEM;
	}

	p->pid = pid;
	p->fds_nr = 0;
	p->maps_nr = 0;
	p->exe_fd = -1;
	p->fs.cwd_fd = -1;
	p->fs.root_fd = -1;
	INIT_LIST_HEAD(&p->fds);
	INIT_LIST_HEAD(&p->maps);

	err = collect_process_env(p, pc);
	if (err)
		goto free_p;

	err = collect_process_fd_table(p, pc);
	if (err)
		goto free_p;

	err = collect_process_map_fds(p, pc);
	if (err)
		goto free_p;

	if ((p->fds_nr + p->maps_nr) == 0)
		goto free_p;

	err = attach_to_process(p);
	if (err)
		goto free_p;
	list_add_tail(&p->list, pc->collection);
	pr_debug("collected process %d\n", pid);
	return 0;

free_p:
	free(p);
	return err;
}

static int collect_processes(const char *pids, struct list_head *collection,
			     dev_t src_dev,
			     const char *source_mnt, const char *target_mnt)
{
	struct processes_collection_s pc = {
		.collection = collection,
		.src_dev = src_dev,
		.source_mnt = source_mnt,
		.target_mnt = target_mnt,
	};

	return iterate_pids_list(pids, &pc, collect_one_process);
}

int collect_dev_processes(const char *pids, struct list_head *collection,
			  dev_t src_dev, const char *target_mnt)
{
	return collect_processes(pids, collection, src_dev, NULL, target_mnt);
}

int collect_mnt_processes(const char *pids, struct list_head *collection,
			  const char *source_mnt, const char *target_mnt)
{
	struct stat st;

	if (stat(source_mnt, &st) < 0) {
		pr_perror("failed to stat %s", source_mnt);
		return -errno;
	}

	return collect_processes(pids, collection, st.st_dev, source_mnt, target_mnt);
}
