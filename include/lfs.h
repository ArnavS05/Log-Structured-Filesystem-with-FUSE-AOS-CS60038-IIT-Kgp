#ifndef LFS_H
#define LFS_H

#include <cstdint>
#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>

int lfs_create(const char *path, mode_t mode);
int lfs_mkdir(const char *path, mode_t mode);
int lfs_write(const char *path, const char *buf, size_t size, off_t offset);
int lfs_rmdir(const char *path);
int lfs_lookup(const char *path, uint64_t *ino_out);
int lfs_getattr(const char *path, struct stat *stbuf);

typedef int (*lfs_dir_filler_t)(void *buf, const char *name,
                              uint64_t ino, mode_t mode);
int lfs_readdir(const char *path, void *buf, lfs_dir_filler_t filler);
int lfs_read(const char *path, char *buf, size_t size, off_t offset);
int lfs_mount(const char *img_path);
int lfs_umount(void);
// Number of segments cleaned (0 if no candidate), or negative errno.
int lfs_gc(void);

#endif
