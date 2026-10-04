#ifndef LUOS_FS_H
#define LUOS_FS_H

#include "drivers/Storage/block_device.h"
#include "kernel/scheduler/spinlock.h"

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
    FS_TYPE_FAT12 = 6,
    FS_TYPE_FAT16 = 7,
    FS_TYPE_RAMFS = 8,
    FS_TYPE_NTFS = 9,
    FS_TYPE_ARCHIVE = 10,
    FS_TYPE_EXT2 = 11,
    FS_TYPE_EXT3 = 12,
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

typedef struct fs_lock {
    spinlock_t spin;
    void *volatile owner;
    volatile uint32_t depth;
} fs_lock_t;

typedef struct fs {
    enum fs_type type;
    struct block_device *dev;
    const fs_ops_t *ops;
    char label[32];
    void *priv;
    fs_lock_t vlock;
} fs_t;

void fs_volume_lock_init(fs_t *fs);
void fs_volume_lock(fs_t *fs);
void fs_volume_unlock(fs_t *fs);

int fat32_probe(struct block_device *dev);
int fat32_mount(struct block_device *dev, fs_t *out);

int ext4_probe(struct block_device *dev);
int ext4_mount(struct block_device *dev, fs_t *out);

int ext2_probe(struct block_device *dev);
int ext2_mount(struct block_device *dev, fs_t *out);
int ext2_is_ext3(struct block_device *dev);
int ext2_format(struct block_device *dev, const char *label, int is_ext3);

int exfat_probe(struct block_device *dev);
int exfat_mount(struct block_device *dev, fs_t *out);

int iso9660_probe(struct block_device *dev);
int iso9660_mount(struct block_device *dev, fs_t *out);

int ntfs_probe(struct block_device *dev);
int ntfs_mount(struct block_device *dev, fs_t *out);

int archive_probe(struct block_device *dev);
int archive_mount(struct block_device *dev, fs_t *out);

int ramfs_mount(fs_t *out);

int fat_format(struct block_device *dev, enum fs_type type, const char *label, uint64_t hidden_sectors);
int exfat_format(struct block_device *dev, const char *label, uint64_t hidden_sectors);

int fs_dev_zero(struct block_device *dev, uint64_t lba, uint64_t count);
int fs_dev_wipe(struct block_device *dev);
uint32_t fs_new_serial(void);
int fs_format_type_from_name(const char *name, struct block_device *dev, enum fs_type *out);
int fs_format(struct block_device *dev, enum fs_type type, const char *label, uint64_t hidden_sectors);

const char *fs_type_name(enum fs_type type);

int fs_mount_auto(struct block_device *dev, fs_t *out);

void fs_lock(void);
void fs_unlock(void);
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
    fs_volume_lock(fs);
    int r = fs->ops->open(fs, path, create, truncate, out);
    fs_volume_unlock(fs);
    return r;
}

static inline int fs_close(fs_file_t *f) {
    if (!f || !f->fs || !f->fs->ops->close) return FS_ERR_NOSUPP;
    fs_volume_lock(f->fs);
    int r = f->fs->ops->close(f);
    fs_volume_unlock(f->fs);
    return r;
}

static inline int64_t fs_read(fs_file_t *f, void *buf, uint64_t size) {
    if (!f || !f->fs || !f->fs->ops->read) return FS_ERR_NOSUPP;
    fs_volume_lock(f->fs);
    int64_t r = f->fs->ops->read(f, buf, size);
    fs_volume_unlock(f->fs);
    return r;
}

static inline int64_t fs_write(fs_file_t *f, const void *buf, uint64_t size) {
    if (!f || !f->fs || !f->fs->ops->write) return FS_ERR_NOSUPP;
    fs_volume_lock(f->fs);
    int64_t r = f->fs->ops->write(f, buf, size);
    fs_volume_unlock(f->fs);
    return r;
}

static inline int fs_seek(fs_file_t *f, uint64_t pos) {
    if (!f || !f->fs || !f->fs->ops->seek) return FS_ERR_NOSUPP;
    fs_volume_lock(f->fs);
    int r = f->fs->ops->seek(f, pos);
    fs_volume_unlock(f->fs);
    return r;
}

static inline int fs_truncate(fs_file_t *f, uint64_t size) {
    if (!f || !f->fs || !f->fs->ops->truncate) return FS_ERR_NOSUPP;
    fs_volume_lock(f->fs);
    int r = f->fs->ops->truncate(f, size);
    fs_volume_unlock(f->fs);
    return r;
}

static inline int fs_opendir(fs_t *fs, const char *path, fs_dir_t *out) {
    if (!fs || !fs->ops || !fs->ops->opendir || !out) return FS_ERR_NOSUPP;
    out->fs = fs;
    out->priv = NULL;
    fs_volume_lock(fs);
    int r = fs->ops->opendir(fs, path, out);
    fs_volume_unlock(fs);
    return r;
}

static inline int fs_readdir(fs_dir_t *dir, fs_dirent_t *out) {
    if (!dir || !dir->fs || !dir->fs->ops->readdir) return FS_ERR_NOSUPP;
    fs_volume_lock(dir->fs);
    int r = dir->fs->ops->readdir(dir, out);
    fs_volume_unlock(dir->fs);
    return r;
}

static inline int fs_closedir(fs_dir_t *dir) {
    if (!dir || !dir->fs || !dir->fs->ops->closedir) return FS_ERR_NOSUPP;
    fs_volume_lock(dir->fs);
    int r = dir->fs->ops->closedir(dir);
    fs_volume_unlock(dir->fs);
    return r;
}

static inline int fs_mkdir(fs_t *fs, const char *path) {
    if (!fs || !fs->ops || !fs->ops->mkdir) return FS_ERR_NOSUPP;
    fs_volume_lock(fs);
    int r = fs->ops->mkdir(fs, path);
    fs_volume_unlock(fs);
    return r;
}

static inline int fs_unlink(fs_t *fs, const char *path) {
    if (!fs || !fs->ops || !fs->ops->unlink) return FS_ERR_NOSUPP;
    fs_volume_lock(fs);
    int r = fs->ops->unlink(fs, path);
    fs_volume_unlock(fs);
    return r;
}

static inline int fs_rmdir(fs_t *fs, const char *path) {
    if (!fs || !fs->ops || !fs->ops->rmdir) return FS_ERR_NOSUPP;
    fs_volume_lock(fs);
    int r = fs->ops->rmdir(fs, path);
    fs_volume_unlock(fs);
    return r;
}

static inline int fs_stat(fs_t *fs, const char *path, fs_dirent_t *out) {
    if (!fs || !fs->ops || !fs->ops->stat) return FS_ERR_NOSUPP;
    fs_volume_lock(fs);
    int r = fs->ops->stat(fs, path, out);
    fs_volume_unlock(fs);
    return r;
}

static inline int fs_unmount(fs_t *fs) {
    if (!fs || !fs->ops || !fs->ops->unmount) return FS_ERR_NOSUPP;
    fs_lock();
    fs_volume_lock(fs);
    int r = fs->ops->unmount(fs);
    fs_volume_unlock(fs);
    fs_unlock();
    return r;
}

#endif
