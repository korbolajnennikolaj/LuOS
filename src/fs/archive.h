#ifndef LUOS_ARCHIVE_H
#define LUOS_ARCHIVE_H

#include "drivers/Storage/block_device.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAR_BLOCK_SIZE 512u
#define TAR_MAGIC_OFFSET 257u
#define CPIO_NEWC_HEADER_SIZE 110u

enum archive_format {
    ARCHIVE_FORMAT_NONE = 0,
    ARCHIVE_FORMAT_TAR = 1,
    ARCHIVE_FORMAT_CPIO = 2,
};

typedef struct archive_node {
    char name[FS_MAX_NAME + 1];
    bool is_dir;
    uint64_t offset;
    uint64_t size;
    struct archive_node *parent;
    struct archive_node *first_child;
    struct archive_node *next_sibling;
} archive_node_t;

typedef struct archive_fs {
    struct block_device *dev;
    enum archive_format format;
    archive_node_t *root;
    uint32_t file_count;
    uint32_t dir_count;
    uint8_t *bounce;
} archive_fs_t;

typedef struct archive_file {
    archive_fs_t *fs;
    archive_node_t *node;
} archive_file_t;

typedef struct archive_diriter {
    archive_node_t *next;
} archive_diriter_t;

extern const fs_ops_t archive_ops;

#endif
