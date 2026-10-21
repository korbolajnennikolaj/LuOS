// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef PROC_VFS_H
#define PROC_VFS_H

#include "kernel/proc/linux_abi.h"
#include "kernel/proc/process.h"

#include <stdbool.h>
#include <stdint.h>

struct ufile;

struct ufile_ops {
    int64_t (*read)(struct ufile *f, void *buf, uint64_t n);
    int64_t (*write)(struct ufile *f, const void *buf, uint64_t n);
    int64_t (*pread)(struct ufile *f, void *buf, uint64_t n, uint64_t off);
    int64_t (*pwrite)(struct ufile *f, const void *buf, uint64_t n, uint64_t off);
    int64_t (*lseek)(struct ufile *f, int64_t off, int whence);
    int (*ioctl)(struct ufile *f, uint64_t cmd, uint64_t arg);
    int (*poll)(struct ufile *f);
    int (*stat)(struct ufile *f, struct lx_stat *st);
    int64_t (*getdents)(struct ufile *f, void *buf, uint64_t n);
    int (*truncate)(struct ufile *f, uint64_t size);
    void (*release)(struct ufile *f);
    int64_t (*read_user)(struct ufile *f, uint64_t ubuf, uint64_t n);
    int64_t (*write_user)(struct ufile *f, uint64_t ubuf, uint64_t n);
    int64_t (*mmap)(struct ufile *f, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, uint64_t off);
    uint64_t (*page)(struct ufile *f, uint64_t index);
};

struct ufile {
    const struct ufile_ops *ops;
    volatile int refs;
    uint32_t flags;
    uint32_t mode;
    uint64_t pos;
    uint64_t ino;
    struct kmutex lock;
    void *priv;
    char path[PROC_PATH_MAX];
};

struct ufile *ufile_alloc(const struct ufile_ops *ops, uint32_t flags, uint32_t mode, const char *path);
struct ufile *ufile_get(struct ufile *f);
void ufile_put(struct ufile *f);

int fd_install(struct process *p, struct ufile *f, int min_fd, bool cloexec);
struct ufile *fd_get(struct process *p, int fd);
int fd_close(struct process *p, int fd);
void fd_close_all(struct process *p);

int vfs_abs_path(struct process *p, int dirfd, const char *path, char *abs, size_t cap);
int vfs_open(const char *abs, uint32_t flags, uint32_t mode, struct ufile **out);
void vfs_poll_wake(void);
void *vfs_poll_channel(void);
int vfs_stat(const char *abs, struct lx_stat *st);
int vfs_lstat(const char *abs, struct lx_stat *st);
long vfs_readlink(const char *abs, char *buf, size_t cap);
uint64_t vfs_fs_magic(const char *abs);
int vfs_mkdir(const char *abs);
int vfs_unlink(const char *abs, bool dir);
int vfs_rename(const char *from, const char *to);

struct ufile *vfs_console_open(void);
int vfs_pipe(struct ufile **rd, struct ufile **wr, uint32_t flags);
int vfs_memfd(const char *name, struct ufile **out);

uint64_t vfs_path_ino(const char *abs);

int vfs_devnode_register(const char *path, unsigned int major, unsigned int minor);
void vfs_devnode_unregister(const char *path);
int vfs_devnode_count(void);
const char *vfs_devnode_path(int index);

struct ufile *vfs_wrap_lx_file(void *lx_file, uint32_t flags);
void *vfs_ufile_lx(struct ufile *f);
extern struct ufile vfs_reserved_fd;

#endif
