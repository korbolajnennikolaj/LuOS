#ifndef LUOS_FS_H
#define LUOS_FS_H

#include "drivers/Storage/block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_OK 0
#define FS_ERR_IO -1
#define FS_ERR_NOENT -2
#define FS_ERR_EXIST -3
#define FS_ERR_NOTDIR -4
#define FS_ERR_ISDIR -5
#define FS_ERR_NOSPACE -6
#define FS_ERR_PARAM -7
#define FS_ERR_NOSUPP -8
#define FS_ERR_NOMEM -9
#define FS_ERR_CORRUPT -10
#define FS_ERR_NOTMOUNTED -11
#define FS_ERR_NOTEMPTY -12
#define FS_ERR_EOF -13

#define FS_MAX_NAME 255
#define FS_MAX_PATH 512

enum fs_type {
    FS_TYPE_UNKNOWN = 0,
    FS_TYPE_FAT32 = 1,
    FS_TYPE_EXT4 = 2,
    FS_TYPE_EXFAT = 4,
    FS_TYPE_ISO9660 = 5,
};

enum fs_entry_type {
    FS_ENTRY_FILE = 0,
    FS_ENTRY_DIR = 1,
};

typedef struct fs_dirent {
    char name[FS_MAX_NAME + 1];
    enum fs_entry_type type;
    uint64_t size;
} fs_dirent_t;

struct fs;

typedef struct fs_file {
    struct fs *fs;
    void *priv;
    uint64_t size;
    uint64_t pos;
    bool writable;
} fs_file_t;

typedef struct fs_dir {
    struct fs *fs;
    void *priv;
} fs_dir_t;

typedef struct fs_ops {
    int (*open)(struct fs *fs, const char *path, bool create, bool truncate, fs_file_t *out);
    int (*close)(fs_file_t *f);
    int64_t (*read)(fs_file_t *f, void *buf, uint64_t size);
    int64_t (*write)(fs_file_t *f, const void *buf, uint64_t size);
    int (*seek)(fs_file_t *f, uint64_t pos);
    int (*truncate)(fs_file_t *f, uint64_t size);

    int (*opendir)(struct fs *fs, const char *path, fs_dir_t *out);
    int (*readdir)(fs_dir_t *dir, fs_dirent_t *out);
    int (*closedir)(fs_dir_t *dir);

    int (*mkdir)(struct fs *fs, const char *path);
    int (*unlink)(struct fs *fs, const char *path);
    int (*rmdir)(struct fs *fs, const char *path);
    int (*stat)(struct fs *fs, const char *path, fs_dirent_t *out);

    int (*unmount)(struct fs *fs);
} fs_ops_t;

typedef struct fs {
    enum fs_type type;
    struct block_device *dev;
    const fs_ops_t *ops;
    char label[32];
    void *priv;
} fs_t;

int fat32_probe(struct block_device *dev);
int fat32_mount(struct block_device *dev, fs_t *out);

int ext4_probe(struct block_device *dev);
int ext4_mount(struct block_device *dev, fs_t *out);

int exfat_probe(struct block_device *dev);
int exfat_mount(struct block_device *dev, fs_t *out);

int iso9660_probe(struct block_device *dev);
int iso9660_mount(struct block_device *dev, fs_t *out);

int fs_mount_auto(struct block_device *dev, fs_t *out);
int fs_mount_type(struct block_device *dev, enum fs_type type, fs_t *out);

typedef struct fs_path_parts {
    char buf[FS_MAX_PATH];
    char *comps[64];
    int count;
} fs_path_parts_t;

static inline int fs_split_path(const char *path, fs_path_parts_t *out) {
    size_t len = 0;
    while (path[len] != '\0') {
        if (len + 1 >= FS_MAX_PATH) return FS_ERR_PARAM;
        len++;
    }
    for (size_t i = 0; i <= len; i++) out->buf[i] = path[i];

    out->count = 0;
    char *p = out->buf;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        if (out->count >= 64) return FS_ERR_PARAM;
        out->comps[out->count++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
    }
    return FS_OK;
}

static inline int fs_open(fs_t *fs, const char *path, bool create, bool truncate, fs_file_t *out) {
    if (!fs || !fs->ops || !fs->ops->open || !out) return FS_ERR_NOSUPP;
    out->fs = fs;
    out->priv = NULL;
    out->size = 0;
    out->pos = 0;
    out->writable = false;
    return fs->ops->open(fs, path, create, truncate, out);
}

static inline int fs_close(fs_file_t *f) {
    if (!f || !f->fs || !f->fs->ops->close) return FS_ERR_NOSUPP;
    return f->fs->ops->close(f);
}

static inline int64_t fs_read(fs_file_t *f, void *buf, uint64_t size) {
    if (!f || !f->fs || !f->fs->ops->read) return FS_ERR_NOSUPP;
    return f->fs->ops->read(f, buf, size);
}

static inline int64_t fs_write(fs_file_t *f, const void *buf, uint64_t size) {
    if (!f || !f->fs || !f->fs->ops->write) return FS_ERR_NOSUPP;
    return f->fs->ops->write(f, buf, size);
}

static inline int fs_seek(fs_file_t *f, uint64_t pos) {
    if (!f || !f->fs || !f->fs->ops->seek) return FS_ERR_NOSUPP;
    return f->fs->ops->seek(f, pos);
}

static inline int fs_truncate(fs_file_t *f, uint64_t size) {
    if (!f || !f->fs || !f->fs->ops->truncate) return FS_ERR_NOSUPP;
    return f->fs->ops->truncate(f, size);
}

static inline int fs_opendir(fs_t *fs, const char *path, fs_dir_t *out) {
    if (!fs || !fs->ops || !fs->ops->opendir || !out) return FS_ERR_NOSUPP;
    out->fs = fs;
    out->priv = NULL;
    return fs->ops->opendir(fs, path, out);
}

static inline int fs_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    if (!dir || !dir->fs || !dir->fs->ops->readdir) return FS_ERR_NOSUPP;
    return dir->fs->ops->readdir(dir, out);
}

static inline int fs_closedir(fs_dir_t *dir) {
    if (!dir || !dir->fs || !dir->fs->ops->closedir) return FS_ERR_NOSUPP;
    return dir->fs->ops->closedir(dir);
}

static inline int fs_mkdir(fs_t *fs, const char *path) {
    if (!fs || !fs->ops || !fs->ops->mkdir) return FS_ERR_NOSUPP;
    return fs->ops->mkdir(fs, path);
}

static inline int fs_unlink(fs_t *fs, const char *path) {
    if (!fs || !fs->ops || !fs->ops->unlink) return FS_ERR_NOSUPP;
    return fs->ops->unlink(fs, path);
}

static inline int fs_rmdir(fs_t *fs, const char *path) {
    if (!fs || !fs->ops || !fs->ops->rmdir) return FS_ERR_NOSUPP;
    return fs->ops->rmdir(fs, path);
}

static inline int fs_stat(fs_t *fs, const char *path, fs_dirent_t *out) {
    if (!fs || !fs->ops || !fs->ops->stat) return FS_ERR_NOSUPP;
    return fs->ops->stat(fs, path, out);
}

static inline int fs_unmount(fs_t *fs) {
    if (!fs || !fs->ops || !fs->ops->unmount) return FS_ERR_NOSUPP;
    return fs->ops->unmount(fs);
}

#endif
