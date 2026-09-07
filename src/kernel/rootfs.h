#ifndef LUOS_ROOTFS_H
#define LUOS_ROOTFS_H

#include "fs/fs.h"

int rootfs_is_mounted(void);

fs_t *rootfs_get(void);

const char *rootfs_cwd(void);

void rootfs_set_cwd(const char *path);

void rootfs_normalize_path(const char *input, char *out, size_t out_cap);

#endif
