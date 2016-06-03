#ifndef __SPFS_MANAGER_FILE_OBJ_H_
#define __SPFS_MANAGER_FILE_OBJ_H_

int create_file_obj(const char *path, unsigned flags, const char *parent,
		    void **file_obj);
int get_file_obj_fd(void *file_obj, unsigned flags);

#endif