#ifndef LUOS_RAMFS_H
#define LUOS_RAMFS_H

#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAMFS_DEVICE_NAME "ramfs"

typedef struct ramfs_node {
    char name[FS_MAX_NAME + 1];
    bool is_dir;
    bool unlinked;
    uint32_t open_count;

    uint8_t *data;
    uint64_t size;
    uint64_t capacity;

    struct ramfs_node *parent;
    struct ramfs_node *first_child;
    struct ramfs_node *next_sibling;
} ramfs_node_t;

typedef struct ramfs_fs {
    ramfs_node_t *root;
    uint64_t bytes_used;
    uint32_t node_count;
} ramfs_fs_t;

typedef struct ramfs_file {
    ramfs_fs_t *fs;
    ramfs_node_t *node;
} ramfs_file_t;

typedef struct ramfs_diriter {
    ramfs_node_t *dir;
    uint32_t index;
} ramfs_diriter_t;

extern const fs_ops_t ramfs_ops;

#endif
