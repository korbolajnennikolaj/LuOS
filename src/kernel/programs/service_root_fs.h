#ifndef SERVICE_ROOT_FS_H
#define SERVICE_ROOT_FS_H

#include "programs.h"

#include "kernel/rootfs.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOTFS_MAX_MOUNTS 8
#define ROOTFS_MOUNT_PATH_MAX 64
#define ROOTFS_DEVICE_NAME_MAX 16

#define ROOTFS_OK 0
#define ROOTFS_ERR_PARAM -1
#define ROOTFS_ERR_BUSY -2
#define ROOTFS_ERR_NODEV -3
#define ROOTFS_ERR_NOSPACE -4
#define ROOTFS_ERR_NOROOT -5
#define ROOTFS_ERR_FS -6
#define ROOTFS_ERR_NOENT -7

typedef struct rootfs_mount {
    char path[ROOTFS_MOUNT_PATH_MAX];
    char device[ROOTFS_DEVICE_NAME_MAX];
    fs_t fs;
    bool mounted;
} rootfs_mount_t;

int rootfs_mount_device(const char *device_name, const char *mount_point, int *out_fs_err);
int rootfs_unmount_point(const char *mount_point);

int rootfs_mount_count(void);
const rootfs_mount_t *rootfs_mount_at(int index);
const rootfs_mount_t *rootfs_mount_for_path(const char *abs_path);

fs_t *rootfs_resolve(const char *abs_path, char *out_rel, size_t out_cap);

const char *rootfs_type_name(enum fs_type type);
const char *rootfs_root_device(void);

service_t *get_root_fs_service(void);

#endif
