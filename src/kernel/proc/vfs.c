// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/vfs.h"

#include "kernel/proc/initfs.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa.h"
#include "drivers/Video/limine_video_driver.h"
#include "fs/fs.h"
#include "kernel/console.h"
#include "kernel/programs/service_root_fs.h"
#include "kernel/rootfs.h"
#include "kernel/scheduler/scheduler.h"

#include <cpuid.h>
#include <stdio.h>
#include <string.h>


struct ufile *ufile_alloc(const struct ufile_ops *ops, uint32_t flags, uint32_t mode, const char *path) {
    struct ufile *f = kmalloc(sizeof(*f));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->ops = ops;
    f->refs = 1;
    f->flags = flags;
    f->mode = mode;
    kmutex_init(&f->lock);
    if (path) {
        strncpy(f->path, path, sizeof(f->path) - 1);
        f->ino = vfs_path_ino(path);
    }
    return f;
}

struct ufile *ufile_get(struct ufile *f) {
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
    return f;
}

void ufile_put(struct ufile *f) {
    if (!f) return;
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        if (f->ops && f->ops->release) f->ops->release(f);
        kfree(f);
    }
}

uint64_t vfs_path_ino(const char *abs) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *s = abs; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}


int fd_install(struct process *p, struct ufile *f, int min_fd, bool cloexec) {
    if (min_fd < 0) min_fd = 0;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    for (int i = min_fd; i < PROC_MAX_FDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            p->fd_cloexec[i] = cloexec ? 1 : 0;
            spin_unlock_irqrestore(&p->fd_lock, fl);
            return i;
        }
    }
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return -LX_EMFILE;
}

struct ufile *fd_get(struct process *p, int fd) {
    if (!p || fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct ufile *f = p->fds[fd];
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return f;
}

int fd_close(struct process *p, int fd) {
    if (fd < 0 || fd >= PROC_MAX_FDS) return -LX_EBADF;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct ufile *f = p->fds[fd];
    p->fds[fd] = NULL;
    p->fd_cloexec[fd] = 0;
    spin_unlock_irqrestore(&p->fd_lock, fl);
    if (!f) return -LX_EBADF;
    ufile_put(f);
    return 0;
}

void fd_close_all(struct process *p) {
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fds[i]) fd_close(p, i);
}

static int fs_err(int e) {
    switch (e) {
        case FS_OK: return 0;
        case FS_ERR_NOENT: return -LX_ENOENT;
        case FS_ERR_EXIST: return -LX_EEXIST;
        case FS_ERR_NOTDIR: return -LX_ENOTDIR;
        case FS_ERR_ISDIR: return -LX_EISDIR;
        case FS_ERR_NOSPACE: return -LX_ENOSPC;
        case FS_ERR_PARAM: return -LX_EINVAL;
        case FS_ERR_NOSUPP: return -LX_EROFS;
        case FS_ERR_NOMEM: return -LX_ENOMEM;
        default: return -LX_EIO;
    }
}

int vfs_abs_path(struct process *p, int dirfd, const char *path, char *abs, size_t cap) {
    if (!path) return -LX_EFAULT;
    if (!path[0]) return -LX_ENOENT;
    const char *base = p ? p->cwd : rootfs_cwd();
    struct ufile *df = NULL;
    if (path[0] != '/' && dirfd != LX_AT_FDCWD) {
        df = fd_get(p, dirfd);
        if (!df) return -LX_EBADF;
        if ((df->mode & LX_S_IFMT) != LX_S_IFDIR) {
            ufile_put(df);
            return -LX_ENOTDIR;
        }
        base = df->path;
    }
    rootfs_normalize_path_from(base, path, abs, cap);
    if (df) ufile_put(df);
    return 0;
}

static void fill_stat(struct lx_stat *st, uint64_t ino, uint32_t mode, uint64_t size, uint64_t rdev) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = ino;
    st->st_nlink = (mode & LX_S_IFMT) == LX_S_IFDIR ? 2 : 1;
    st->st_mode = mode;
    st->st_rdev = rdev;
    st->st_size = (int64_t)size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((size + 511) / 512);
}


static int64_t rf_pread_locked(struct ufile *f, void *buf, uint64_t n, uint64_t off) {
    fs_file_t *ff = f->priv;
    if (off >= ff->size) return 0;
    if (n > ff->size - off) n = ff->size - off;
    int r = fs_seek(ff, off);
    if (r != FS_OK) return fs_err(r);
    int64_t got = fs_read(ff, buf, n);
    return got < 0 ? fs_err((int)got) : got;
}

static int64_t rf_read(struct ufile *f, void *buf, uint64_t n) {
    kmutex_lock(&f->lock);
    int64_t r = rf_pread_locked(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t rf_pread(struct ufile *f, void *buf, uint64_t n, uint64_t off) {
    kmutex_lock(&f->lock);
    int64_t r = rf_pread_locked(f, buf, n, off);
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t rf_pwrite_locked(struct ufile *f, const void *buf, uint64_t n, uint64_t off) {
    fs_file_t *ff = f->priv;
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) return -LX_EBADF;
    int r = fs_seek(ff, off);
    if (r != FS_OK) return fs_err(r);
    int64_t put = fs_write(ff, buf, n);
    return put < 0 ? fs_err((int)put) : put;
}

static int64_t rf_write(struct ufile *f, const void *buf, uint64_t n) {
    kmutex_lock(&f->lock);
    fs_file_t *ff = f->priv;
    if (f->flags & LX_O_APPEND) f->pos = ff->size;
    int64_t r = rf_pwrite_locked(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t rf_pwrite(struct ufile *f, const void *buf, uint64_t n, uint64_t off) {
    kmutex_lock(&f->lock);
    int64_t r = rf_pwrite_locked(f, buf, n, off);
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t rf_lseek(struct ufile *f, int64_t off, int whence) {
    kmutex_lock(&f->lock);
    fs_file_t *ff = f->priv;
    int64_t base = whence == LX_SEEK_SET ? 0 : whence == LX_SEEK_CUR ? (int64_t)f->pos
                 : whence == LX_SEEK_END ? (int64_t)ff->size : -1;
    int64_t np = base + off;
    int64_t r = (base < 0 || np < 0) ? -LX_EINVAL : np;
    if (r >= 0) f->pos = (uint64_t)np;
    kmutex_unlock(&f->lock);
    return r;
}

static int rf_stat(struct ufile *f, struct lx_stat *st) {
    fs_file_t *ff = f->priv;
    fill_stat(st, f->ino, LX_S_IFREG | 0755, ff->size, 0);
    return 0;
}

static int rf_truncate(struct ufile *f, uint64_t size) {
    kmutex_lock(&f->lock);
    int r = fs_err(fs_truncate((fs_file_t *)f->priv, size));
    kmutex_unlock(&f->lock);
    return r;
}

static int rf_poll(struct ufile *f) {
    (void)f;
    return LX_POLLIN | LX_POLLOUT | LX_POLLRDNORM | LX_POLLWRNORM;
}

static void rf_release(struct ufile *f) {
    fs_file_t *ff = f->priv;
    if (ff) {
        fs_close(ff);
        kfree(ff);
    }
}

static const struct ufile_ops regular_ops = {
    .read = rf_read,
    .write = rf_write,
    .pread = rf_pread,
    .pwrite = rf_pwrite,
    .lseek = rf_lseek,
    .poll = rf_poll,
    .stat = rf_stat,
    .truncate = rf_truncate,
    .release = rf_release,
};


struct dir_state {
    fs_t *fs;
    char rel[FS_MAX_PATH];
    fs_dir_t dir;
    bool open;
    int stage;
    bool have_pending;
    fs_dirent_t pending;
    int synth_index;
    const char *const *synth;
    int dev_index;
    uint64_t index;
    int ifs_dir;
    uint32_t ifs_cursor;
    bool ifs_pending;
    char ifs_name[256];
    uint8_t ifs_type;
};

static const char *const root_synth[] = { "dev", "proc", "sys", NULL };
static const char *const dev_entries[] = { "null", "zero", "random", "urandom", "tty", "console", NULL };

static int64_t dir_emit(uint8_t *buf, uint64_t cap, uint64_t *used, uint64_t ino, int64_t off, uint8_t type,
                        const char *name) {
    size_t nl = strlen(name);
    uint64_t reclen = (sizeof(struct lx_dirent64) + nl + 1 + 7) & ~7ULL;
    if (*used + reclen > cap) return 0;
    struct lx_dirent64 *d = (struct lx_dirent64 *)(buf + *used);
    memset(d, 0, reclen);
    d->d_ino = ino;
    d->d_off = off;
    d->d_reclen = (uint16_t)reclen;
    d->d_type = type;
    memcpy(d->d_name, name, nl + 1);
    *used += reclen;
    return 1;
}

static bool synth_has(const char *const *synth, const char *name) {
    for (; *synth; synth++)
        if (strcmp(*synth, name) == 0) return true;
    return false;
}

static int64_t dir_getdents(struct ufile *f, void *kbuf, uint64_t cap) {
    struct dir_state *ds = f->priv;
    uint8_t *buf = kbuf;
    uint64_t used = 0;
    kmutex_lock(&f->lock);

    while (ds->stage < 2) {
        const char *n = ds->stage == 0 ? "." : "..";
        if (!dir_emit(buf, cap, &used, f->ino + (uint64_t)ds->stage, (int64_t)ds->index + 1, LX_DT_DIR, n)) goto full;
        ds->stage++;
        ds->index++;
    }
    while (ds->synth && ds->synth[ds->synth_index]) {
        const char *n = ds->synth[ds->synth_index];
        uint8_t type = ds->synth == root_synth ? LX_DT_DIR : LX_DT_CHR;
        if (!dir_emit(buf, cap, &used, vfs_path_ino(n), (int64_t)ds->index + 1, type, n)) goto full;
        ds->synth_index++;
        ds->index++;
    }
    while (ds->ifs_dir >= 0) {
        if (!ds->ifs_pending) {
            int t;
            if (!initfs_next_child((uint32_t)ds->ifs_dir, &ds->ifs_cursor, ds->ifs_name, sizeof(ds->ifs_name), &t, NULL)) break;
            if (ds->synth && synth_has(ds->synth, ds->ifs_name)) continue;
            ds->ifs_type = t == INITFS_DIR ? LX_DT_DIR : t == INITFS_LINK ? LX_DT_LNK : LX_DT_REG;
            ds->ifs_pending = true;
        }
        char full[FS_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", f->path, ds->ifs_name);
        if (!dir_emit(buf, cap, &used, vfs_path_ino(full), (int64_t)ds->index + 1, ds->ifs_type, ds->ifs_name)) goto full;
        ds->ifs_pending = false;
        ds->index++;
    }
    if (!ds->fs && strncmp(f->path, "/dev", 4) == 0) {
        size_t plen = strlen(f->path);
        for (;;) {
            const char *dp = vfs_devnode_path(ds->dev_index);
            if (!dp) break;
            char name[64];
            uint8_t type = LX_DT_CHR;
            bool emit = false;
            if (strncmp(dp, f->path, plen) == 0 && dp[plen] == '/') {
                const char *rest = dp + plen + 1;
                const char *slash = strchr(rest, '/');
                size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
                if (nl < sizeof(name)) {
                    memcpy(name, rest, nl);
                    name[nl] = 0;
                    type = slash ? LX_DT_DIR : LX_DT_CHR;
                    emit = true;
                    for (int k = 0; k < ds->dev_index && emit && slash; k++) {
                        const char *prev = vfs_devnode_path(k);
                        if (prev && strncmp(prev, dp, (size_t)(slash - dp) + 1) == 0) emit = false;
                    }
                }
            }
            if (emit && !dir_emit(buf, cap, &used, vfs_path_ino(dp), (int64_t)ds->index + 1, type, name)) goto full;
            ds->dev_index++;
            if (emit) ds->index++;
        }
    }
    while (ds->open) {
        if (!ds->have_pending) {
            int r = fs_readdir(&ds->dir, &ds->pending);
            if (r != FS_OK) break;
            if (strcmp(ds->pending.name, ".") == 0 || strcmp(ds->pending.name, "..") == 0) continue;
            if (ds->synth == root_synth && synth_has(root_synth, ds->pending.name)) continue;
            if (ds->ifs_dir >= 0 && initfs_has_child((uint32_t)ds->ifs_dir, ds->pending.name)) continue;
            ds->have_pending = true;
        }
        char full[FS_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", f->path, ds->pending.name);
        uint8_t type = ds->pending.type == FS_ENTRY_DIR ? LX_DT_DIR : LX_DT_REG;
        if (!dir_emit(buf, cap, &used, vfs_path_ino(full), (int64_t)ds->index + 1, type, ds->pending.name)) goto full;
        ds->have_pending = false;
        ds->index++;
    }
full:
    kmutex_unlock(&f->lock);
    if (used == 0 && (ds->have_pending || ds->ifs_pending || (ds->synth && ds->synth[ds->synth_index]))) return -LX_EINVAL;
    return (int64_t)used;
}

static int dir_reopen(struct dir_state *ds) {
    if (ds->open) fs_closedir(&ds->dir);
    ds->open = false;
    ds->stage = 0;
    ds->have_pending = false;
    ds->synth_index = 0;
    ds->dev_index = 0;
    ds->index = 0;
    ds->ifs_cursor = 0;
    ds->ifs_pending = false;
    if (ds->fs) {
        int r = fs_opendir(ds->fs, ds->rel, &ds->dir);
        if (r != FS_OK) return fs_err(r);
        ds->open = true;
    }
    return 0;
}

static int64_t dir_lseek(struct ufile *f, int64_t off, int whence) {
    if (whence != LX_SEEK_SET || off != 0) return -LX_EINVAL;
    kmutex_lock(&f->lock);
    int r = dir_reopen(f->priv);
    kmutex_unlock(&f->lock);
    return r ? r : 0;
}

static int dir_stat(struct ufile *f, struct lx_stat *st) {
    fill_stat(st, f->ino, LX_S_IFDIR | 0755, 4096, 0);
    return 0;
}

static void dir_release(struct ufile *f) {
    struct dir_state *ds = f->priv;
    if (!ds) return;
    if (ds->open) fs_closedir(&ds->dir);
    kfree(ds);
}

static const struct ufile_ops dir_ops = {
    .lseek = dir_lseek,
    .stat = dir_stat,
    .getdents = dir_getdents,
    .release = dir_release,
};

static int dir_open_ifs(const char *abs, fs_t *fs, const char *rel, const char *const *synth, int ifs_dir,
                        struct ufile **out) {
    struct dir_state *ds = kmalloc(sizeof(*ds));
    if (!ds) return -LX_ENOMEM;
    memset(ds, 0, sizeof(*ds));
    ds->ifs_dir = ifs_dir;
    ds->fs = fs;
    if (rel) strncpy(ds->rel, rel, sizeof(ds->rel) - 1);
    ds->synth = synth;
    int r = dir_reopen(ds);
    if (r) {
        kfree(ds);
        return r;
    }
    struct ufile *f = ufile_alloc(&dir_ops, LX_O_RDONLY | LX_O_DIRECTORY, LX_S_IFDIR | 0755, abs);
    if (!f) {
        dir_release(&(struct ufile){ .priv = ds });
        return -LX_ENOMEM;
    }
    f->priv = ds;
    *out = f;
    return 0;
}

static int dir_open(const char *abs, fs_t *fs, const char *rel, const char *const *synth, struct ufile **out) {
    uint32_t ifs;
    int dir = initfs_lookup(abs, true, &ifs) == 0 && initfs_type(ifs) == INITFS_DIR ? (int)ifs : -1;
    return dir_open_ifs(abs, fs, rel, synth, dir, out);
}


static int64_t con_read(struct ufile *f, void *buf, uint64_t n) {
    (void)f;
    uint8_t *b = buf;
    uint64_t got = 0;
    while (got < n) {
        int c = luos_console_getc();
        if (c == LUOS_CONSOLE_EOF || c == LUOS_CONSOLE_INTR) break;
        b[got++] = (uint8_t)c;
        if (c == '\n') break;
    }
    return (int64_t)got;
}

static int64_t con_write(struct ufile *f, const void *buf, uint64_t n) {
    (void)f;
    char chunk[257];
    const char *s = buf;
    uint64_t done = 0;
    while (done < n) {
        uint64_t k = n - done;
        if (k > 256) k = 256;
        memcpy(chunk, s + done, k);
        chunk[k] = '\0';
        for (uint64_t i = 0; i < k; i++)
            if (!chunk[i]) chunk[i] = ' ';
        luos_console_write(chunk);
        done += k;
    }
    return (int64_t)n;
}

static void con_size(uint16_t *rows, uint16_t *cols) {
    *rows = 50;
    *cols = 160;
}

static int con_ioctl(struct ufile *f, uint64_t cmd, uint64_t arg) {
    (void)f;
    switch (cmd) {
        case LX_TIOCGWINSZ: {
            struct lx_winsize ws = { 0, 0, 0, 0 };
            con_size(&ws.ws_row, &ws.ws_col);
            return copy_to_user(arg, &ws, sizeof(ws));
        }
        case LX_TCGETS: {
            struct lx_termios t;
            memset(&t, 0, sizeof(t));
            t.c_iflag = 0x500;
            t.c_oflag = 0x5;
            t.c_cflag = 0xBF;
            t.c_lflag = 0x8A3B;
            t.c_cc[0] = 3;
            t.c_cc[4] = 4;
            t.c_cc[6] = 1;
            return copy_to_user(arg, &t, sizeof(t));
        }
        case LX_TCSETS:
        case LX_TCSETSW:
        case LX_TCSETSF:
        case LX_TIOCSPGRP:
        case LX_FIONBIO:
            return 0;
        case LX_TIOCGPGRP: {
            int32_t pg = proc_current() ? proc_current()->pid : 1;
            return copy_to_user(arg, &pg, sizeof(pg));
        }
        case LX_FIONREAD: {
            int32_t n = luos_console_has_key() ? 1 : 0;
            return copy_to_user(arg, &n, sizeof(n));
        }
        default:
            return -LX_ENOTTY;
    }
}

static int con_poll(struct ufile *f) {
    (void)f;
    int m = LX_POLLOUT | LX_POLLWRNORM;
    if (luos_console_has_key()) m |= LX_POLLIN | LX_POLLRDNORM;
    return m;
}

static int con_stat(struct ufile *f, struct lx_stat *st) {
    fill_stat(st, f->ino, LX_S_IFCHR | 0620, 0, (136ULL << 8) | 0);
    return 0;
}

static const struct ufile_ops console_ops = {
    .read = con_read,
    .write = con_write,
    .ioctl = con_ioctl,
    .poll = con_poll,
    .stat = con_stat,
};

struct ufile *vfs_console_open(void) {
    return ufile_alloc(&console_ops, LX_O_RDWR, LX_S_IFCHR | 0620, "/dev/console");
}


static int64_t null_read(struct ufile *f, void *buf, uint64_t n) { (void)f; (void)buf; (void)n; return 0; }
static int64_t null_write(struct ufile *f, const void *buf, uint64_t n) { (void)f; (void)buf; return (int64_t)n; }
static int64_t zero_read(struct ufile *f, void *buf, uint64_t n) { (void)f; memset(buf, 0, n); return (int64_t)n; }

static uint64_t rng_state;

static uint64_t rng_next(void) {
    uint64_t s = __atomic_load_n(&rng_state, __ATOMIC_RELAXED);
    if (!s) {
        uint64_t t;
        asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(t) :: "rdx");
        s = t | 1;
    }
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    __atomic_store_n(&rng_state, s, __ATOMIC_RELAXED);
    uint64_t t;
    asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(t) :: "rdx");
    return s ^ (t * 0x9E3779B97F4A7C15ULL);
}

void vfs_random_bytes(void *buf, uint64_t n) {
    uint8_t *b = buf;
    for (uint64_t i = 0; i < n; i += 8) {
        uint64_t v = rng_next();
        for (uint64_t k = 0; k < 8 && i + k < n; k++) b[i + k] = (uint8_t)(v >> (k * 8));
    }
}

static int64_t rand_read(struct ufile *f, void *buf, uint64_t n) { (void)f; vfs_random_bytes(buf, n); return (int64_t)n; }

static int dev_poll(struct ufile *f) { (void)f; return LX_POLLIN | LX_POLLOUT | LX_POLLRDNORM | LX_POLLWRNORM; }

static int dev_stat(struct ufile *f, struct lx_stat *st) {
    fill_stat(st, f->ino, LX_S_IFCHR | 0666, 0, (1ULL << 8) | 3);
    return 0;
}

static const struct ufile_ops null_ops = { .read = null_read, .write = null_write, .poll = dev_poll, .stat = dev_stat };
static const struct ufile_ops zero_ops = { .read = zero_read, .write = null_write, .poll = dev_poll, .stat = dev_stat };
static const struct ufile_ops rand_ops = { .read = rand_read, .write = null_write, .poll = dev_poll, .stat = dev_stat };

int gpu_dev_default(void);

static int gpu_node_ioctl(struct ufile *f, uint64_t cmd, uint64_t arg) {
    switch (cmd) {
        case GPU_IOC_ATTACH: {
            int dev;
            if (copy_from_user(&dev, arg, sizeof(dev))) return -LX_EFAULT;
            gpu_core_refresh();
            int r = gpu_translator_attach(dev);
            if (r == GPU_OK) mesa_host_attached();
            return r;
        }
        case GPU_IOC_DETACH: {
            int dev;
            if (copy_from_user(&dev, arg, sizeof(dev))) return -LX_EFAULT;
            return gpu_translator_detach(dev);
        }
        case GPU_IOC_RING: {
            struct gpu_ring_info info;
            int dev;
            if (copy_from_user(&dev, arg, sizeof(dev))) return -LX_EFAULT;
            if (gpu_translator_ring_info(dev, &info)) return -LX_EINVAL;
            if (copy_to_user(arg, &info, sizeof(info))) return -LX_EFAULT;
            return 0;
        }
        case GPU_IOC_API: {
            struct gpu_api_info info;
            if (gpu_core_api_info(&info)) return -LX_EINVAL;
            if (copy_to_user(arg, &info, sizeof(info))) return -LX_EFAULT;
            return 0;
        }
        case GPU_IOC_PIX: {
            struct gpu_pix_xfer *x = kmalloc(sizeof(*x));
            if (!x) return -LX_ENOMEM;
            if (copy_from_user(x, arg, sizeof(*x))) {
                kfree(x);
                return -LX_EFAULT;
            }
            if (x->len > MESA_PIX_CHUNK) {
                kfree(x);
                return -LX_EINVAL;
            }
            int r = mesa_pix_write(x->seq, x->off, x->data, x->len);
            kfree(x);
            return r;
        }
        case GPU_IOC_HOSTINFO: {
            struct mesa_host_info info;
            if (mesa_host_info(&info)) return -LX_EINVAL;
            if (copy_to_user(arg, &info, sizeof(info))) return -LX_EFAULT;
            return 0;
        }
        default:
            return -LX_ENOTTY;
    }
}

static int64_t gpu_node_read(struct ufile *f, void *buf, uint64_t n) {
    (void)f;
    if (n < 4096) return -LX_EINVAL;
    int dev = gpu_dev_default();
    uint32_t len = 0;
    if (mesa_frame_drain(dev, buf, (uint32_t)n, &len)) return 0;
    return (int64_t)len;
}

static int64_t gpu_node_write(struct ufile *f, const void *buf, uint64_t n) {
    (void)f;
    if (n < 24 || n > 24 + 8 + 56 * 1024 + 192 * 8) return -LX_EINVAL;
    uint32_t head[6];
    memcpy(head, buf, 24);
    if (head[5] > 56 * 1024) return -LX_EINVAL;
    return mesa_frame_complete(head[0], head[1], head[2], buf, (uint32_t)n);
}

static int gpu_node_poll(struct ufile *f) {
    (void)f;
    int m = 0;
    int dev = gpu_dev_default();
    if (mesa_frame_pending(dev)) m |= LX_POLLIN | LX_POLLRDNORM;
    return m | LX_POLLOUT | LX_POLLWRNORM;
}

static int gpu_node_stat(struct ufile *f, struct lx_stat *st) {
    fill_stat(st, f->ino, LX_S_IFCHR | 0666, 0, (1ULL << 8) | 3);
    return 0;
}

static const struct ufile_ops gpu_node_ops = {
    .read = gpu_node_read,
    .write = gpu_node_write,
    .ioctl = gpu_node_ioctl,
    .poll = gpu_node_poll,
    .stat = gpu_node_stat,
};

int gpu_dev_default(void) {
    int count = gpu_get_device_count();
    int bound = -1;
    for (int i = 0; i < count; i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (!d) continue;
        if (gpu_translator_attached(i)) return i;
        if (bound < 0 && (d->flags & GPU_DEV_FLAG_BOUND)) bound = i;
    }
    if (bound >= 0) return bound;
    return count > 0 ? 0 : -1;
}

static int dev_open(const char *abs, uint32_t flags, struct ufile **out) {
    const char *n = abs + 5;
    const struct ufile_ops *ops = NULL;
    if (strcmp(n, "null") == 0) ops = &null_ops;
    else if (strcmp(n, "zero") == 0) ops = &zero_ops;
    else if (strcmp(n, "random") == 0 || strcmp(n, "urandom") == 0) ops = &rand_ops;
    else if (strcmp(n, "gpu") == 0) ops = &gpu_node_ops;
    else if (strcmp(n, "tty") == 0 || strcmp(n, "console") == 0) {
        *out = vfs_console_open();
        return *out ? 0 : -LX_ENOMEM;
    } else {
        return -LX_ENOENT;
    }
    *out = ufile_alloc(ops, flags, LX_S_IFCHR | 0666, abs);
    return *out ? 0 : -LX_ENOMEM;
}


#define PIPE_SIZE 65536

struct pipe {
    struct kmutex lock;
    uint8_t *buf;
    uint32_t head;
    uint32_t count;
    volatile int readers;
    volatile int writers;
    volatile int rd_ready;
    volatile int wr_ready;
    volatile int refs;
};

static void pipe_unref(struct pipe *p) {
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        kfree(p->buf);
        kfree(p);
    }
}

static int64_t pipe_read(struct ufile *f, void *buf, uint64_t n) {
    struct pipe *p = f->priv;
    for (;;) {
        kmutex_lock(&p->lock);
        if (p->count) {
            uint64_t k = n < p->count ? n : p->count;
            for (uint64_t i = 0; i < k; i++) ((uint8_t *)buf)[i] = p->buf[(p->head + i) % PIPE_SIZE];
            p->head = (uint32_t)((p->head + k) % PIPE_SIZE);
            p->count -= (uint32_t)k;
            __atomic_store_n(&p->wr_ready, 1, __ATOMIC_RELEASE);
            kmutex_unlock(&p->lock);
            scheduler_wake_all((void *)&p->wr_ready);
            return (int64_t)k;
        }
        if (!p->writers) {
            kmutex_unlock(&p->lock);
            return 0;
        }
        if (f->flags & LX_O_NONBLOCK) {
            kmutex_unlock(&p->lock);
            return -LX_EAGAIN;
        }
        p->rd_ready = 0;
        kmutex_unlock(&p->lock);
        scheduler_wait((void *)&p->rd_ready, &p->rd_ready, 200);
        struct task *t = current_task();
        if (t && t->kill_pending) return -LX_EINTR;
    }
}

static int64_t pipe_write(struct ufile *f, const void *buf, uint64_t n) {
    struct pipe *p = f->priv;
    uint64_t done = 0;
    while (done < n) {
        kmutex_lock(&p->lock);
        if (!p->readers) {
            kmutex_unlock(&p->lock);
            return done ? (int64_t)done : -LX_EPIPE;
        }
        uint64_t space = PIPE_SIZE - p->count;
        if (space) {
            uint64_t k = n - done < space ? n - done : space;
            for (uint64_t i = 0; i < k; i++)
                p->buf[(p->head + p->count + i) % PIPE_SIZE] = ((const uint8_t *)buf)[done + i];
            p->count += (uint32_t)k;
            done += k;
            __atomic_store_n(&p->rd_ready, 1, __ATOMIC_RELEASE);
            kmutex_unlock(&p->lock);
            scheduler_wake_all((void *)&p->rd_ready);
            continue;
        }
        if (f->flags & LX_O_NONBLOCK) {
            kmutex_unlock(&p->lock);
            return done ? (int64_t)done : -LX_EAGAIN;
        }
        p->wr_ready = 0;
        kmutex_unlock(&p->lock);
        scheduler_wait((void *)&p->wr_ready, &p->wr_ready, 200);
        struct task *t = current_task();
        if (t && t->kill_pending) return done ? (int64_t)done : -LX_EINTR;
    }
    return (int64_t)done;
}

static int pipe_poll(struct ufile *f) {
    struct pipe *p = f->priv;
    int m = 0;
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) {
        if (p->count) m |= LX_POLLIN | LX_POLLRDNORM;
        if (!p->writers) m |= LX_POLLHUP;
    } else {
        if (p->count < PIPE_SIZE) m |= LX_POLLOUT | LX_POLLWRNORM;
        if (!p->readers) m |= LX_POLLERR;
    }
    return m;
}

static int pipe_stat(struct ufile *f, struct lx_stat *st) {
    fill_stat(st, (uint64_t)(uintptr_t)f->priv, LX_S_IFIFO | 0600, 0, 0);
    return 0;
}

static void pipe_release(struct ufile *f) {
    struct pipe *p = f->priv;
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) {
        __atomic_sub_fetch(&p->readers, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&p->wr_ready, 1, __ATOMIC_RELEASE);
        scheduler_wake_all((void *)&p->wr_ready);
    } else {
        __atomic_sub_fetch(&p->writers, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&p->rd_ready, 1, __ATOMIC_RELEASE);
        scheduler_wake_all((void *)&p->rd_ready);
    }
    pipe_unref(p);
}

static const struct ufile_ops pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .poll = pipe_poll,
    .stat = pipe_stat,
    .release = pipe_release,
};

int vfs_pipe(struct ufile **rd, struct ufile **wr, uint32_t flags) {
    struct pipe *p = kmalloc(sizeof(*p));
    if (!p) return -LX_ENOMEM;
    memset(p, 0, sizeof(*p));
    p->buf = kmalloc(PIPE_SIZE);
    if (!p->buf) {
        kfree(p);
        return -LX_ENOMEM;
    }
    kmutex_init(&p->lock);
    p->readers = 1;
    p->writers = 1;
    p->refs = 2;
    uint32_t nb = flags & LX_O_NONBLOCK;
    *rd = ufile_alloc(&pipe_ops, LX_O_RDONLY | nb, LX_S_IFIFO | 0600, "pipe:");
    *wr = ufile_alloc(&pipe_ops, LX_O_WRONLY | nb, LX_S_IFIFO | 0600, "pipe:");
    if (!*rd || !*wr) {
        kfree(*rd);
        kfree(*wr);
        kfree(p->buf);
        kfree(p);
        return -LX_ENOMEM;
    }
    (*rd)->priv = p;
    (*wr)->priv = p;
    return 0;
}


struct shmem {
    uint64_t size;
    uint64_t *pages;
    uint64_t cap;
};

static uint64_t shmem_page_locked(struct shmem *sh, uint64_t index) {
    if (index >= sh->cap) {
        uint64_t ncap = sh->cap ? sh->cap : 16;
        while (ncap <= index) ncap *= 2;
        uint64_t *np = krealloc(sh->pages, ncap * sizeof(uint64_t));
        if (!np) return 0;
        memset(np + sh->cap, 0, (ncap - sh->cap) * sizeof(uint64_t));
        sh->pages = np;
        sh->cap = ncap;
    }
    if (!sh->pages[index]) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return 0;
        memset((void *)mm_phys_to_virt(phys), 0, PAGE_SIZE);
        sh->pages[index] = phys;
    }
    return sh->pages[index];
}

static uint64_t shmem_page(struct ufile *f, uint64_t index) {
    kmutex_lock(&f->lock);
    uint64_t phys = shmem_page_locked(f->priv, index);
    kmutex_unlock(&f->lock);
    return phys;
}

static int64_t shmem_rw_locked(struct ufile *f, void *buf, uint64_t n, uint64_t off, bool write) {
    struct shmem *sh = f->priv;
    if (!write) {
        if (off >= sh->size) return 0;
        if (n > sh->size - off) n = sh->size - off;
    }
    uint64_t done = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t in = pos & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        uint64_t phys = shmem_page_locked(sh, pos / PAGE_SIZE);
        if (!phys) return done ? (int64_t)done : -LX_ENOSPC;
        uint8_t *page = (uint8_t *)mm_phys_to_virt(phys);
        if (write) memcpy(page + in, (const uint8_t *)buf + done, chunk);
        else memcpy((uint8_t *)buf + done, page + in, chunk);
        done += chunk;
    }
    if (write && off + done > sh->size) sh->size = off + done;
    return (int64_t)done;
}

static int64_t shmem_pread(struct ufile *f, void *buf, uint64_t n, uint64_t off) {
    kmutex_lock(&f->lock);
    int64_t r = shmem_rw_locked(f, buf, n, off, false);
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t shmem_pwrite(struct ufile *f, const void *buf, uint64_t n, uint64_t off) {
    kmutex_lock(&f->lock);
    int64_t r = shmem_rw_locked(f, (void *)buf, n, off, true);
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t shmem_read(struct ufile *f, void *buf, uint64_t n) {
    kmutex_lock(&f->lock);
    int64_t r = shmem_rw_locked(f, buf, n, f->pos, false);
    if (r > 0) f->pos += (uint64_t)r;
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t shmem_write(struct ufile *f, const void *buf, uint64_t n) {
    kmutex_lock(&f->lock);
    struct shmem *sh = f->priv;
    if (f->flags & LX_O_APPEND) f->pos = sh->size;
    int64_t r = shmem_rw_locked(f, (void *)buf, n, f->pos, true);
    if (r > 0) f->pos += (uint64_t)r;
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t shmem_lseek(struct ufile *f, int64_t off, int whence) {
    kmutex_lock(&f->lock);
    struct shmem *sh = f->priv;
    int64_t base = whence == LX_SEEK_SET ? 0 : whence == LX_SEEK_CUR ? (int64_t)f->pos
                 : whence == LX_SEEK_END ? (int64_t)sh->size : -1;
    int64_t np = base + off;
    int64_t r = (base < 0 || np < 0) ? -LX_EINVAL : np;
    if (r >= 0) f->pos = (uint64_t)np;
    kmutex_unlock(&f->lock);
    return r;
}

static int shmem_truncate(struct ufile *f, uint64_t size) {
    kmutex_lock(&f->lock);
    struct shmem *sh = f->priv;
    if (size < sh->size) {
        for (uint64_t pos = size; pos < sh->size;) {
            uint64_t index = pos / PAGE_SIZE, in = pos & (PAGE_SIZE - 1);
            if (index < sh->cap && sh->pages[index])
                memset((uint8_t *)mm_phys_to_virt(sh->pages[index]) + in, 0, PAGE_SIZE - in);
            pos += PAGE_SIZE - in;
        }
    }
    sh->size = size;
    kmutex_unlock(&f->lock);
    return 0;
}

static int shmem_stat(struct ufile *f, struct lx_stat *st) {
    struct shmem *sh = f->priv;
    fill_stat(st, f->ino, LX_S_IFREG | 0777, sh->size, 0);
    return 0;
}

static void shmem_release(struct ufile *f) {
    struct shmem *sh = f->priv;
    if (!sh) return;
    for (uint64_t i = 0; i < sh->cap; i++)
        if (sh->pages[i]) pmm_free_page(sh->pages[i]);
    kfree(sh->pages);
    kfree(sh);
}

static const struct ufile_ops shmem_ops = {
    .read = shmem_read,
    .write = shmem_write,
    .pread = shmem_pread,
    .pwrite = shmem_pwrite,
    .lseek = shmem_lseek,
    .poll = rf_poll,
    .stat = shmem_stat,
    .truncate = shmem_truncate,
    .release = shmem_release,
    .page = shmem_page,
};

int vfs_memfd(const char *name, struct ufile **out) {
    struct shmem *sh = kmalloc(sizeof(*sh));
    if (!sh) return -LX_ENOMEM;
    memset(sh, 0, sizeof(*sh));
    char path[PROC_PATH_MAX];
    snprintf(path, sizeof(path), "/memfd:%s (deleted)", name);
    struct ufile *f = ufile_alloc(&shmem_ops, LX_O_RDWR, LX_S_IFREG | 0777, path);
    if (!f) {
        kfree(sh);
        return -LX_ENOMEM;
    }
    static uint64_t serial;
    f->ino = 0x6d656d0000000000ULL | __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
    f->priv = sh;
    *out = f;
    return 0;
}


int lkpi_chrdev_open(unsigned int major, unsigned int minor, unsigned int flags, void **lx_file);
long lkpi_file_read(void *lx, uint64_t ubuf, uint64_t n);
long lkpi_file_write(void *lx, uint64_t ubuf, uint64_t n);
long lkpi_file_ioctl(void *lx, unsigned int cmd, unsigned long arg);
int lkpi_file_poll(void *lx);
long lkpi_file_llseek(void *lx, int64_t off, int whence);
long lkpi_file_mmap(void *lx, void *proc, uint64_t start, uint64_t len, uint32_t prot, uint32_t flags,
                    uint64_t pgoff, void **ctx);
void lkpi_file_put(void *lx);
void lkpi_file_get(void *lx);
int lkpi_file_info(void *lx, uint64_t *rdev, uint32_t *mode, uint64_t *size);

#define DEVNODE_MAX 64

static struct {
    char path[64];
    unsigned int major;
    unsigned int minor;
    bool used;
} devnodes[DEVNODE_MAX];
static spinlock_t devnode_lock = SPINLOCK_INIT;

int vfs_devnode_register(const char *path, unsigned int major, unsigned int minor) {
    uint64_t fl = spin_lock_irqsave(&devnode_lock);
    for (int i = 0; i < DEVNODE_MAX; i++) {
        if (devnodes[i].used) continue;
        devnodes[i].used = true;
        strncpy(devnodes[i].path, path, sizeof(devnodes[i].path) - 1);
        devnodes[i].major = major;
        devnodes[i].minor = minor;
        spin_unlock_irqrestore(&devnode_lock, fl);
        LOG_INFO("device node %s (%u:%u)", path, major, minor);
        return 0;
    }
    spin_unlock_irqrestore(&devnode_lock, fl);
    return -LX_ENOSPC;
}

void vfs_devnode_unregister(const char *path) {
    uint64_t fl = spin_lock_irqsave(&devnode_lock);
    for (int i = 0; i < DEVNODE_MAX; i++)
        if (devnodes[i].used && strcmp(devnodes[i].path, path) == 0) devnodes[i].used = false;
    spin_unlock_irqrestore(&devnode_lock, fl);
}

int vfs_devnode_count(void) {
    int n = 0;
    for (int i = 0; i < DEVNODE_MAX; i++) n += devnodes[i].used ? 1 : 0;
    return n;
}

const char *vfs_devnode_path(int index) {
    for (int i = 0; i < DEVNODE_MAX; i++) {
        if (!devnodes[i].used) continue;
        if (index-- == 0) return devnodes[i].path;
    }
    return NULL;
}

static int devnode_find(const char *abs, unsigned int *major, unsigned int *minor) {
    int r = -1;
    uint64_t fl = spin_lock_irqsave(&devnode_lock);
    for (int i = 0; i < DEVNODE_MAX; i++) {
        if (devnodes[i].used && strcmp(devnodes[i].path, abs) == 0) {
            *major = devnodes[i].major;
            *minor = devnodes[i].minor;
            r = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&devnode_lock, fl);
    return r;
}

static bool devnode_is_dir(const char *abs) {
    size_t n = strlen(abs);
    bool r = false;
    uint64_t fl = spin_lock_irqsave(&devnode_lock);
    for (int i = 0; i < DEVNODE_MAX && !r; i++)
        if (devnodes[i].used && strncmp(devnodes[i].path, abs, n) == 0 && devnodes[i].path[n] == '/') r = true;
    spin_unlock_irqrestore(&devnode_lock, fl);
    return r;
}

static int64_t lx_read_user(struct ufile *f, uint64_t ubuf, uint64_t n) { return lkpi_file_read(f->priv, ubuf, n); }
static int64_t lx_write_user(struct ufile *f, uint64_t ubuf, uint64_t n) { return lkpi_file_write(f->priv, ubuf, n); }
static int lx_ioctl(struct ufile *f, uint64_t cmd, uint64_t arg) { return (int)lkpi_file_ioctl(f->priv, (unsigned int)cmd, arg); }
static int lx_poll(struct ufile *f) { return lkpi_file_poll(f->priv); }
static int64_t lx_lseek(struct ufile *f, int64_t off, int whence) { return lkpi_file_llseek(f->priv, off, whence); }

static int lx_stat(struct ufile *f, struct lx_stat *st) {
    uint64_t rdev = 0, size = 0;
    uint32_t mode = LX_S_IFCHR | 0666;
    lkpi_file_info(f->priv, &rdev, &mode, &size);
    fill_stat(st, f->ino, mode, size, rdev);
    return 0;
}

static void lx_release(struct ufile *f) {
    if (f->priv) lkpi_file_put(f->priv);
}

static int64_t lx_mmap(struct ufile *f, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, uint64_t off) {
    struct process *p = proc_current();
    if (!p) return -LX_EINVAL;
    if (off & (PAGE_SIZE - 1)) return -LX_EINVAL;
    uint64_t start = 0;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!len) return -LX_EINVAL;
    uint32_t hold = LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE));
    int r = mm_map(&p->mm, addr, len, LX_PROT_NONE, hold, NULL, 0, 0, &start);
    if (r) return r;
    void *ctx = NULL;
    long lr = lkpi_file_mmap(f->priv, p, start, len, prot, flags, off >> 12, &ctx);
    if (lr < 0) {
        mm_unmap(&p->mm, start, len);
        return lr;
    }
    uint64_t out = 0;
    r = mm_map_device(&p->mm, start, len, prot, (flags & ~(uint32_t)LX_MAP_FIXED_NOREPLACE) | LX_MAP_FIXED, ctx, &out);
    if (r) {
        if (mm_device_hooks) mm_device_hooks->close(ctx);
        mm_unmap(&p->mm, start, len);
        return r;
    }
    return (int64_t)out;
}

static const struct ufile_ops lx_file_ops = {
    .ioctl = lx_ioctl,
    .poll = lx_poll,
    .lseek = lx_lseek,
    .stat = lx_stat,
    .release = lx_release,
    .read_user = lx_read_user,
    .write_user = lx_write_user,
    .mmap = lx_mmap,
};

struct ufile *vfs_wrap_lx_file(void *lx_file, uint32_t flags) {
    uint64_t rdev = 0, size = 0;
    uint32_t mode = LX_S_IFCHR | 0666;
    lkpi_file_info(lx_file, &rdev, &mode, &size);
    struct ufile *f = ufile_alloc(&lx_file_ops, flags, mode, "lx:");
    if (!f) return NULL;
    f->priv = lx_file;
    return f;
}

void *vfs_ufile_lx(struct ufile *f) {
    if (!f || f->ops != &lx_file_ops) return NULL;
    lkpi_file_get(f->priv);
    return f->priv;
}

static const struct ufile_ops reserved_ops = { 0 };
struct ufile vfs_reserved_fd = { .ops = &reserved_ops, .refs = 1 << 30, .path = "reserved" };


int lkpi_sysfs_lookup(const char *path, int follow, int *type, unsigned long *size, unsigned long *ino);
long lkpi_sysfs_read(const char *path, char *buf, unsigned long cap);
long lkpi_sysfs_readlink(const char *path, char *buf, unsigned long cap);
int lkpi_sysfs_readdir(const char *path, unsigned long index, char *name, unsigned long cap, int *type);

#define SYSFS_DIR 1
#define SYSFS_FILE 2
#define SYSFS_LINK 3
#define SYSFS_FILE_MAX 4096

struct sysfs_file {
    uint64_t size;
    char data[SYSFS_FILE_MAX];
};

struct sysfs_dir {
    uint64_t index;
    uint64_t emitted;
};

static bool is_sys_path(const char *abs) {
    return strcmp(abs, "/sys") == 0 || strncmp(abs, "/sys/", 5) == 0;
}

static int64_t sf_pread(struct ufile *f, void *buf, uint64_t n, uint64_t off) {
    struct sysfs_file *sf = f->priv;
    if (off >= sf->size) return 0;
    if (n > sf->size - off) n = sf->size - off;
    memcpy(buf, sf->data + off, n);
    return (int64_t)n;
}

static int64_t sf_read(struct ufile *f, void *buf, uint64_t n) {
    kmutex_lock(&f->lock);
    int64_t r = sf_pread(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t sf_lseek(struct ufile *f, int64_t off, int whence) {
    struct sysfs_file *sf = f->priv;
    kmutex_lock(&f->lock);
    int64_t base = whence == LX_SEEK_SET ? 0 : whence == LX_SEEK_CUR ? (int64_t)f->pos
                 : whence == LX_SEEK_END ? (int64_t)sf->size : -1;
    int64_t np = base + off;
    int64_t r = (base < 0 || np < 0) ? -LX_EINVAL : np;
    if (r >= 0) f->pos = (uint64_t)np;
    kmutex_unlock(&f->lock);
    return r;
}

static int sf_stat(struct ufile *f, struct lx_stat *st) {
    struct sysfs_file *sf = f->priv;
    fill_stat(st, f->ino, LX_S_IFREG | 0444, sf->size, 0);
    return 0;
}

static void sf_release(struct ufile *f) {
    kfree(f->priv);
}

static const struct ufile_ops sysfs_file_ops = {
    .read = sf_read,
    .pread = sf_pread,
    .lseek = sf_lseek,
    .poll = rf_poll,
    .stat = sf_stat,
    .release = sf_release,
};

static uint8_t sysfs_dt(int type) {
    return type == SYSFS_DIR ? LX_DT_DIR : type == SYSFS_LINK ? LX_DT_LNK : LX_DT_REG;
}

static int64_t sd_getdents(struct ufile *f, void *kbuf, uint64_t cap) {
    struct sysfs_dir *sd = f->priv;
    uint8_t *buf = kbuf;
    uint64_t used = 0;
    kmutex_lock(&f->lock);
    while (sd->emitted < 2) {
        const char *n = sd->emitted == 0 ? "." : "..";
        if (!dir_emit(buf, cap, &used, f->ino + sd->emitted, (int64_t)sd->emitted + 1, LX_DT_DIR, n)) goto full;
        sd->emitted++;
    }
    for (;;) {
        char name[64], full[PROC_PATH_MAX];
        int type = 0;
        int r = lkpi_sysfs_readdir(f->path, sd->index, name, sizeof(name), &type);
        if (r <= 0) break;
        snprintf(full, sizeof(full), "%s/%s", f->path, name);
        if (!dir_emit(buf, cap, &used, vfs_path_ino(full), (int64_t)sd->emitted + 1, sysfs_dt(type), name)) goto full;
        sd->index++;
        sd->emitted++;
    }
full:
    kmutex_unlock(&f->lock);
    if (used == 0 && sd->emitted < 2) return -LX_EINVAL;
    return (int64_t)used;
}

static int64_t sd_lseek(struct ufile *f, int64_t off, int whence) {
    if (whence != LX_SEEK_SET || off != 0) return -LX_EINVAL;
    struct sysfs_dir *sd = f->priv;
    kmutex_lock(&f->lock);
    sd->index = 0;
    sd->emitted = 0;
    kmutex_unlock(&f->lock);
    return 0;
}

static const struct ufile_ops sysfs_dir_ops = {
    .lseek = sd_lseek,
    .stat = dir_stat,
    .getdents = sd_getdents,
    .release = sf_release,
};

static int link_stat(struct ufile *f, struct lx_stat *st) {
    char target[PROC_PATH_MAX];
    long n = vfs_readlink(f->path, target, sizeof(target));
    fill_stat(st, f->ino, LX_S_IFLNK | 0777, n > 0 ? (uint64_t)n : 0, 0);
    return 0;
}

static const struct ufile_ops link_ops = {
    .stat = link_stat,
};

static int snapshot_open(const char *abs, uint32_t flags, const char *data, size_t len, uint64_t ino, struct ufile **out) {
    struct sysfs_file *sf = kmalloc(sizeof(*sf));
    if (!sf) return -LX_ENOMEM;
    if (len > sizeof(sf->data)) len = sizeof(sf->data);
    memcpy(sf->data, data, len);
    sf->size = len;
    struct ufile *f = ufile_alloc(&sysfs_file_ops, flags, LX_S_IFREG | 0444, abs);
    if (!f) {
        kfree(sf);
        return -LX_ENOMEM;
    }
    f->priv = sf;
    if (ino) f->ino = ino;
    *out = f;
    return 0;
}

static int sysfs_open(const char *abs, uint32_t flags, bool wants_write, struct ufile **out) {
    int type = 0;
    unsigned long size = 0, ino = 0;
    int r = lkpi_sysfs_lookup(abs, !(flags & LX_O_NOFOLLOW), &type, &size, &ino);
    if (r) return r;
    struct ufile *f;
    if (type == SYSFS_LINK) {
        if (!(flags & LX_O_PATH)) return -LX_ELOOP;
        f = ufile_alloc(&link_ops, flags, LX_S_IFLNK | 0777, abs);
        if (!f) return -LX_ENOMEM;
    } else if (type == SYSFS_DIR) {
        if (wants_write) return -LX_EISDIR;
        struct sysfs_dir *sd = kmalloc(sizeof(*sd));
        if (!sd) return -LX_ENOMEM;
        memset(sd, 0, sizeof(*sd));
        f = ufile_alloc(&sysfs_dir_ops, LX_O_RDONLY | LX_O_DIRECTORY, LX_S_IFDIR | 0755, abs);
        if (!f) {
            kfree(sd);
            return -LX_ENOMEM;
        }
        f->priv = sd;
    } else {
        if (wants_write) return -LX_EACCES;
        if (flags & LX_O_DIRECTORY) return -LX_ENOTDIR;
        struct sysfs_file *sf = kmalloc(sizeof(*sf));
        if (!sf) return -LX_ENOMEM;
        long n = lkpi_sysfs_read(abs, sf->data, sizeof(sf->data));
        if (n < 0) {
            kfree(sf);
            return (int)n;
        }
        sf->size = (uint64_t)n;
        f = ufile_alloc(&sysfs_file_ops, flags, LX_S_IFREG | 0444, abs);
        if (!f) {
            kfree(sf);
            return -LX_ENOMEM;
        }
        f->priv = sf;
    }
    f->ino = ino;
    *out = f;
    return 0;
}

static int sysfs_stat(const char *abs, int follow, struct lx_stat *st) {
    int type = 0;
    unsigned long size = 0, ino = 0;
    int r = lkpi_sysfs_lookup(abs, follow, &type, &size, &ino);
    if (r) return r;
    uint32_t mode = type == SYSFS_DIR ? LX_S_IFDIR | 0755 : type == SYSFS_LINK ? LX_S_IFLNK | 0777 : LX_S_IFREG | 0444;
    fill_stat(st, ino, mode, size, 0);
    return 0;
}


enum { PROCFS_ROOT, PROCFS_SELF, PROCFS_FDDIR, PROCFS_FD, PROCFS_EXE, PROCFS_MOUNTS };

static const char procfs_mounts[] =
    "rootfs / vfat rw 0 0\n"
    "devtmpfs /dev devtmpfs rw 0 0\n"
    "proc /proc proc rw 0 0\n"
    "sysfs /sys sysfs rw 0 0\n";

static bool is_proc_path(const char *abs) {
    return strcmp(abs, "/proc") == 0 || strncmp(abs, "/proc/", 6) == 0;
}

static bool parse_uint(const char *s, size_t len, int *out) {
    int v = 0;
    if (!len || len > 9) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

static struct ufile *procfs_fd_file(struct process *p, int fd) {
    struct ufile *f = fd_get(p, fd);
    if (f == &vfs_reserved_fd) {
        ufile_put(f);
        return NULL;
    }
    return f;
}

static int procfs_parse(const char *abs, int *kind, int *fd) {
    struct process *p = proc_current();
    const char *s = abs + 5;
    *fd = -1;
    if (!*s) {
        *kind = PROCFS_ROOT;
        return 0;
    }
    s++;
    if (strcmp(s, "mounts") == 0) {
        *kind = PROCFS_MOUNTS;
        return 0;
    }
    if (!p) return -LX_ENOENT;
    const char *e = strchr(s, '/');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    int pid;
    if (!(len == 4 && strncmp(s, "self", 4) == 0) && !(parse_uint(s, len, &pid) && pid == p->pid)) return -LX_ENOENT;
    if (!e) {
        *kind = PROCFS_SELF;
        return 0;
    }
    s = e + 1;
    if (strcmp(s, "exe") == 0) *kind = PROCFS_EXE;
    else if (strcmp(s, "mounts") == 0) *kind = PROCFS_MOUNTS;
    else if (strcmp(s, "fd") == 0) *kind = PROCFS_FDDIR;
    else if (strncmp(s, "fd/", 3) == 0 && parse_uint(s + 3, strlen(s + 3), fd)) {
        struct ufile *f = procfs_fd_file(p, *fd);
        if (!f) return -LX_ENOENT;
        ufile_put(f);
        *kind = PROCFS_FD;
    } else {
        return -LX_ENOENT;
    }
    return 0;
}

static long procfs_readlink(const char *abs, char *buf, size_t cap) {
    int kind, fd;
    int r = procfs_parse(abs, &kind, &fd);
    if (r) return r;
    struct process *p = proc_current();
    const char *target;
    struct ufile *f = NULL;
    if (kind == PROCFS_EXE) {
        target = p->exe;
    } else if (kind == PROCFS_FD) {
        f = procfs_fd_file(p, fd);
        if (!f) return -LX_ENOENT;
        target = f->path[0] ? f->path : "anon_inode:[file]";
    } else {
        return -LX_EINVAL;
    }
    size_t len = strlen(target);
    if (len > cap) len = cap;
    memcpy(buf, target, len);
    if (f) ufile_put(f);
    return (long)len;
}

static int procfs_entry(struct ufile *dir, int kind, uint64_t index, char *name, size_t cap, uint8_t *type) {
    struct process *p = proc_current();
    if (kind == PROCFS_ROOT) {
        if (index == 0) { snprintf(name, cap, "self"); *type = LX_DT_LNK; return 1; }
        if (index == 1) { snprintf(name, cap, "mounts"); *type = LX_DT_REG; return 1; }
        if (index == 2 && p) { snprintf(name, cap, "%d", p->pid); *type = LX_DT_DIR; return 1; }
        return 0;
    }
    if (kind == PROCFS_SELF) {
        static const char *const names[] = { "exe", "fd", "mounts" };
        static const uint8_t types[] = { LX_DT_LNK, LX_DT_DIR, LX_DT_REG };
        if (index >= 3) return 0;
        snprintf(name, cap, "%s", names[index]);
        *type = types[index];
        return 1;
    }
    if (kind == PROCFS_FDDIR && p) {
        for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
            struct ufile *f = procfs_fd_file(p, fd);
            if (!f) continue;
            ufile_put(f);
            if (index-- == 0) {
                snprintf(name, cap, "%d", fd);
                *type = LX_DT_LNK;
                return 1;
            }
        }
    }
    (void)dir;
    return 0;
}

static int64_t pd_getdents(struct ufile *f, void *kbuf, uint64_t cap) {
    struct sysfs_dir *sd = f->priv;
    uint8_t *buf = kbuf;
    uint64_t used = 0;
    int kind, fd;
    if (procfs_parse(f->path, &kind, &fd)) return -LX_ENOENT;
    kmutex_lock(&f->lock);
    while (sd->emitted < 2) {
        const char *n = sd->emitted == 0 ? "." : "..";
        if (!dir_emit(buf, cap, &used, f->ino + sd->emitted, (int64_t)sd->emitted + 1, LX_DT_DIR, n)) goto full;
        sd->emitted++;
    }
    for (;;) {
        char name[32], full[PROC_PATH_MAX];
        uint8_t type = 0;
        if (!procfs_entry(f, kind, sd->index, name, sizeof(name), &type)) break;
        snprintf(full, sizeof(full), "%s/%s", f->path, name);
        if (!dir_emit(buf, cap, &used, vfs_path_ino(full), (int64_t)sd->emitted + 1, type, name)) goto full;
        sd->index++;
        sd->emitted++;
    }
full:
    kmutex_unlock(&f->lock);
    if (used == 0 && sd->emitted < 2) return -LX_EINVAL;
    return (int64_t)used;
}

static int pd_stat(struct ufile *f, struct lx_stat *st) {
    fill_stat(st, f->ino, LX_S_IFDIR | 0555, 0, 0);
    return 0;
}

static const struct ufile_ops procfs_dir_ops = {
    .lseek = sd_lseek,
    .stat = pd_stat,
    .getdents = pd_getdents,
    .release = sf_release,
};

static int procfs_open(const char *abs, uint32_t flags, bool wants_write, struct ufile **out) {
    int kind, fd;
    int r = procfs_parse(abs, &kind, &fd);
    if (r) return r;
    struct process *p = proc_current();
    if (kind == PROCFS_ROOT || kind == PROCFS_SELF || kind == PROCFS_FDDIR) {
        if (wants_write) return -LX_EISDIR;
        struct sysfs_dir *sd = kmalloc(sizeof(*sd));
        if (!sd) return -LX_ENOMEM;
        memset(sd, 0, sizeof(*sd));
        struct ufile *f = ufile_alloc(&procfs_dir_ops, LX_O_RDONLY | LX_O_DIRECTORY, LX_S_IFDIR | 0555, abs);
        if (!f) {
            kfree(sd);
            return -LX_ENOMEM;
        }
        f->priv = sd;
        *out = f;
        return 0;
    }
    if (kind == PROCFS_MOUNTS) {
        if (wants_write) return -LX_EACCES;
        return snapshot_open(abs, flags, procfs_mounts, sizeof(procfs_mounts) - 1, 0, out);
    }
    if (flags & LX_O_NOFOLLOW) {
        if (!(flags & LX_O_PATH)) return -LX_ELOOP;
        *out = ufile_alloc(&link_ops, flags, LX_S_IFLNK | 0777, abs);
        return *out ? 0 : -LX_ENOMEM;
    }
    if (kind == PROCFS_EXE) return vfs_open(p->exe, flags, 0, out);
    struct ufile *f = procfs_fd_file(p, fd);
    if (!f) return -LX_ENOENT;
    if (f->path[0] == '/' && strcmp(f->path, abs) != 0 && vfs_open(f->path, flags & ~(uint32_t)(LX_O_CREAT | LX_O_TRUNC | LX_O_EXCL), 0, out) == 0) {
        ufile_put(f);
        return 0;
    }
    *out = f;
    return 0;
}

static int procfs_stat(const char *abs, int follow, struct lx_stat *st) {
    int kind, fd;
    int r = procfs_parse(abs, &kind, &fd);
    if (r) return r;
    struct process *p = proc_current();
    uint64_t ino = vfs_path_ino(abs);
    switch (kind) {
        case PROCFS_ROOT: case PROCFS_SELF: case PROCFS_FDDIR:
            fill_stat(st, ino, LX_S_IFDIR | 0555, 0, 0);
            return 0;
        case PROCFS_MOUNTS:
            fill_stat(st, ino, LX_S_IFREG | 0444, 0, 0);
            return 0;
    }
    if (!follow) {
        char target[PROC_PATH_MAX];
        long n = procfs_readlink(abs, target, sizeof(target));
        fill_stat(st, ino, LX_S_IFLNK | 0777, n > 0 ? (uint64_t)n : 0, 0);
        return 0;
    }
    if (kind == PROCFS_EXE) return vfs_stat(p->exe, st);
    struct ufile *f = procfs_fd_file(p, fd);
    if (!f) return -LX_ENOENT;
    r = f->ops->stat ? f->ops->stat(f, st) : -LX_EINVAL;
    ufile_put(f);
    return r;
}


static int64_t ifs_pread(struct ufile *f, void *buf, uint64_t n, uint64_t off) {
    uint32_t idx = (uint32_t)(uintptr_t)f->priv;
    uint64_t size = initfs_size(idx);
    if (off >= size) return 0;
    const uint8_t *data = initfs_data(idx);
    if (!data) return -LX_ENOMEM;
    if (n > size - off) n = size - off;
    memcpy(buf, data + off, n);
    return (int64_t)n;
}

static int64_t ifs_read(struct ufile *f, void *buf, uint64_t n) {
    kmutex_lock(&f->lock);
    int64_t r = ifs_pread(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    kmutex_unlock(&f->lock);
    return r;
}

static int64_t ifs_lseek(struct ufile *f, int64_t off, int whence) {
    kmutex_lock(&f->lock);
    int64_t base = whence == LX_SEEK_SET ? 0 : whence == LX_SEEK_CUR ? (int64_t)f->pos
                 : whence == LX_SEEK_END ? (int64_t)initfs_size((uint32_t)(uintptr_t)f->priv) : -1;
    int64_t np = base + off;
    int64_t r = (base < 0 || np < 0) ? -LX_EINVAL : np;
    if (r >= 0) f->pos = (uint64_t)np;
    kmutex_unlock(&f->lock);
    return r;
}

static void ifs_fill_stat(uint32_t idx, struct lx_stat *st) {
    int t = initfs_type(idx);
    uint32_t mode = t == INITFS_DIR ? LX_S_IFDIR | 0755 : t == INITFS_LINK ? LX_S_IFLNK | 0777
                  : LX_S_IFREG | (initfs_mode(idx) & 07777);
    fill_stat(st, initfs_ino(idx), mode, initfs_size(idx), 0);
}

static int ifs_stat(struct ufile *f, struct lx_stat *st) {
    ifs_fill_stat((uint32_t)(uintptr_t)f->priv, st);
    return 0;
}

static const struct ufile_ops initfs_file_ops = {
    .read = ifs_read,
    .pread = ifs_pread,
    .lseek = ifs_lseek,
    .poll = rf_poll,
    .stat = ifs_stat,
};

static int initfs_open(const char *abs, uint32_t idx, uint32_t flags, bool wants_write, struct ufile **out) {
    int t = initfs_type(idx);
    if (t == INITFS_LINK) {
        if (!(flags & LX_O_PATH)) return -LX_ELOOP;
        *out = ufile_alloc(&link_ops, flags, LX_S_IFLNK | 0777, abs);
        return *out ? 0 : -LX_ENOMEM;
    }
    if (t == INITFS_DIR) {
        if (wants_write) return -LX_EISDIR;
        char rel[FS_MAX_PATH];
        fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
        if (fs && strcmp(rel, "/") != 0) {
            fs_dirent_t d;
            if (fs_stat(fs, rel, &d) != FS_OK || d.type != FS_ENTRY_DIR) fs = NULL;
        }
        return dir_open_ifs(abs, fs, fs ? rel : NULL, strcmp(abs, "/") == 0 ? root_synth : NULL, (int)idx, out);
    }
    if ((flags & LX_O_CREAT) && (flags & LX_O_EXCL)) return -LX_EEXIST;
    if (flags & LX_O_DIRECTORY) return -LX_ENOTDIR;
    if (wants_write || (flags & LX_O_TRUNC)) return -LX_EROFS;
    struct ufile *f = ufile_alloc(&initfs_file_ops, flags, LX_S_IFREG | (initfs_mode(idx) & 07777), abs);
    if (!f) return -LX_ENOMEM;
    f->priv = (void *)(uintptr_t)idx;
    f->ino = initfs_ino(idx);
    *out = f;
    return 0;
}

uint64_t vfs_fs_magic(const char *abs) {
    if (is_sys_path(abs)) return 0x62656572;
    if (is_proc_path(abs)) return 0x9fa0;
    if (strcmp(abs, "/dev") == 0 || strncmp(abs, "/dev/", 5) == 0) return 0x01021994;
    return 0x4d44;
}


static bool is_dev_path(const char *abs) {
    return strcmp(abs, "/dev") == 0 || strncmp(abs, "/dev/", 5) == 0;
}

int vfs_open(const char *abs, uint32_t flags, uint32_t mode, struct ufile **out) {
    (void)mode;
    *out = NULL;
    uint32_t acc = flags & LX_O_ACCMODE;
    bool wants_write = acc == LX_O_WRONLY || acc == LX_O_RDWR;

    if (is_sys_path(abs)) return sysfs_open(abs, flags, wants_write, out);
    if (is_proc_path(abs)) return procfs_open(abs, flags, wants_write, out);
    if (is_dev_path(abs)) {
        unsigned int major, minor;
        if (devnode_find(abs, &major, &minor) == 0) {
            void *lx = NULL;
            int r = lkpi_chrdev_open(major, minor, flags, &lx);
            if (r) return r;
            *out = vfs_wrap_lx_file(lx, flags);
            if (!*out) {
                lkpi_file_put(lx);
                return -LX_ENOMEM;
            }
            strncpy((*out)->path, abs, sizeof((*out)->path) - 1);
            (*out)->ino = vfs_path_ino(abs);
            return 0;
        }
        if (strcmp(abs, "/dev") == 0 || devnode_is_dir(abs)) {
            if (wants_write) return -LX_EISDIR;
            return dir_open(abs, NULL, NULL, strcmp(abs, "/dev") == 0 ? dev_entries : NULL, out);
        }
        return dev_open(abs, flags, out);
    }

    uint32_t ifs;
    if (initfs_lookup(abs, !(flags & LX_O_NOFOLLOW), &ifs) == 0) return initfs_open(abs, ifs, flags, wants_write, out);

    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
    if (!fs) return -LX_ENOENT;

    fs_dirent_t st;
    bool is_root = strcmp(rel, "/") == 0;
    int sr = is_root ? FS_OK : fs_stat(fs, rel, &st);
    bool exists = sr == FS_OK;
    bool is_dir = is_root || (exists && st.type == FS_ENTRY_DIR);

    if (exists && is_dir) {
        if (wants_write) return -LX_EISDIR;
        return dir_open(abs, fs, rel, strcmp(abs, "/") == 0 ? root_synth : NULL, out);
    }
    if (exists && (flags & LX_O_CREAT) && (flags & LX_O_EXCL)) return -LX_EEXIST;
    if (exists && (flags & LX_O_DIRECTORY)) return -LX_ENOTDIR;
    if (!exists && sr != FS_ERR_NOENT) return fs_err(sr);
    if (!exists && !(flags & LX_O_CREAT)) return -LX_ENOENT;

    fs_file_t *ff = kmalloc(sizeof(*ff));
    if (!ff) return -LX_ENOMEM;
    bool trunc = (flags & LX_O_TRUNC) && wants_write;
    int r = fs_open(fs, rel, !exists, trunc, ff);
    if (r != FS_OK) {
        kfree(ff);
        return fs_err(r);
    }
    struct ufile *f = ufile_alloc(&regular_ops, flags, LX_S_IFREG | 0755, abs);
    if (!f) {
        fs_close(ff);
        kfree(ff);
        return -LX_ENOMEM;
    }
    f->priv = ff;
    *out = f;
    return 0;
}

int vfs_stat(const char *abs, struct lx_stat *st) {
    if (is_sys_path(abs)) return sysfs_stat(abs, 1, st);
    if (is_proc_path(abs)) return procfs_stat(abs, 1, st);
    if (is_dev_path(abs)) {
        unsigned int major, minor;
        if (devnode_find(abs, &major, &minor) == 0) {
            fill_stat(st, vfs_path_ino(abs), LX_S_IFCHR | 0666, 0, ((uint64_t)major << 8) | (minor & 0xFF) | ((uint64_t)(minor & ~0xFFu) << 12));
            return 0;
        }
        if (strcmp(abs, "/dev") == 0 || devnode_is_dir(abs)) {
            fill_stat(st, vfs_path_ino(abs), LX_S_IFDIR | 0755, 4096, 0);
            return 0;
        }
        struct ufile *f = NULL;
        int r = dev_open(abs, LX_O_RDONLY, &f);
        if (r) return r;
        r = f->ops->stat(f, st);
        ufile_put(f);
        return r;
    }
    uint32_t ifs;
    if (initfs_lookup(abs, true, &ifs) == 0) {
        ifs_fill_stat(ifs, st);
        return 0;
    }
    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
    if (!fs) return -LX_ENOENT;
    if (strcmp(rel, "/") == 0) {
        fill_stat(st, vfs_path_ino(abs), LX_S_IFDIR | 0755, 4096, 0);
        return 0;
    }
    fs_dirent_t d;
    int r = fs_stat(fs, rel, &d);
    if (r != FS_OK) return fs_err(r);
    if (d.type == FS_ENTRY_DIR) fill_stat(st, vfs_path_ino(abs), LX_S_IFDIR | 0755, 4096, 0);
    else fill_stat(st, vfs_path_ino(abs), LX_S_IFREG | 0755, d.size, 0);
    return 0;
}

int vfs_lstat(const char *abs, struct lx_stat *st) {
    if (is_sys_path(abs)) return sysfs_stat(abs, 0, st);
    if (is_proc_path(abs)) return procfs_stat(abs, 0, st);
    uint32_t ifs;
    if (!is_dev_path(abs) && initfs_lookup(abs, false, &ifs) == 0) {
        ifs_fill_stat(ifs, st);
        return 0;
    }
    return vfs_stat(abs, st);
}

long vfs_readlink(const char *abs, char *buf, size_t cap) {
    if (is_sys_path(abs)) return lkpi_sysfs_readlink(abs, buf, cap);
    if (is_proc_path(abs)) return procfs_readlink(abs, buf, cap);
    uint32_t ifs;
    if (!is_dev_path(abs) && initfs_lookup(abs, false, &ifs) == 0) return initfs_readlink(ifs, buf, cap);
    struct lx_stat st;
    int r = vfs_stat(abs, &st);
    return r ? r : -LX_EINVAL;
}

int vfs_mkdir(const char *abs) {
    uint32_t ifs;
    if (is_dev_path(abs) || is_sys_path(abs) || is_proc_path(abs)) return -LX_EEXIST;
    if (initfs_lookup(abs, false, &ifs) == 0) return -LX_EEXIST;
    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
    if (!fs) return -LX_ENOENT;
    return fs_err(fs_mkdir(fs, rel));
}

int vfs_unlink(const char *abs, bool dir) {
    uint32_t ifs;
    if (is_dev_path(abs) || is_sys_path(abs) || is_proc_path(abs)) return -LX_EPERM;
    if (initfs_lookup(abs, false, &ifs) == 0) return -LX_EROFS;
    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
    if (!fs) return -LX_ENOENT;
    return fs_err(dir ? fs_rmdir(fs, rel) : fs_unlink(fs, rel));
}

int vfs_rename(const char *from, const char *to) {
    (void)from;
    (void)to;
    return -LX_EXDEV;
}
