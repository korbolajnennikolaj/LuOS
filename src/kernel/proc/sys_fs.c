// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/syscall.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "kernel/proc/linux_abi.h"
#include "kernel/proc/process.h"
#include "kernel/proc/vfs.h"
#include "kernel/scheduler/scheduler.h"

#include <string.h>

#define IO_CHUNK (64 * 1024)
#define POLL_MAX 1024

static int get_path(uint64_t upath, char *out) {
    if (!upath) return -LX_EFAULT;
    long n = strncpy_from_user(out, upath, PROC_PATH_MAX);
    if (n < 0) return (int)n;
    return 0;
}

static int resolve(struct process *p, int dirfd, uint64_t upath, char *abs) {
    char path[PROC_PATH_MAX];
    int r = get_path(upath, path);
    if (r) return r;
    r = vfs_abs_path(p, dirfd, path, abs, PROC_PATH_MAX);
    if (syscall_get_trace()) LOG_INFO("  path %d:'%s' -> '%s' (%d)", dirfd, path, r ? "" : abs, r);
    return r;
}

static long do_read(struct ufile *f, uint64_t ubuf, uint64_t n, int64_t off, bool positional) {
    if (f->ops->read_user && !positional) return f->ops->read_user(f, ubuf, n);
    if ((f->flags & LX_O_ACCMODE) == LX_O_WRONLY) return -LX_EBADF;
    if ((f->mode & LX_S_IFMT) == LX_S_IFDIR) return -LX_EISDIR;
    if (positional ? !f->ops->pread : !f->ops->read) return -LX_EINVAL;
    if (!n) return 0;
    if (!user_range_ok(ubuf, n)) return -LX_EFAULT;

    uint64_t chunk = n < IO_CHUNK ? n : IO_CHUNK;
    uint8_t *kb = kmalloc(chunk);
    if (!kb) return -LX_ENOMEM;
    uint64_t done = 0;
    long err = 0;
    while (done < n) {
        uint64_t want = n - done < chunk ? n - done : chunk;
        int64_t got = positional ? f->ops->pread(f, kb, want, (uint64_t)off + done) : f->ops->read(f, kb, want);
        if (got < 0) { err = got; break; }
        if (got == 0) break;
        if (copy_to_user(ubuf + done, kb, (size_t)got)) { err = -LX_EFAULT; break; }
        done += (uint64_t)got;
        if ((uint64_t)got < want) break;
        if ((f->mode & LX_S_IFMT) != LX_S_IFREG) break;
    }
    kfree(kb);
    return done ? (long)done : err;
}

static long do_write(struct ufile *f, uint64_t ubuf, uint64_t n, int64_t off, bool positional) {
    if (f->ops->write_user && !positional) return f->ops->write_user(f, ubuf, n);
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) return -LX_EBADF;
    if (positional ? !f->ops->pwrite : !f->ops->write) return -LX_EINVAL;
    if (!n) return 0;
    if (!user_range_ok(ubuf, n)) return -LX_EFAULT;

    uint64_t chunk = n < IO_CHUNK ? n : IO_CHUNK;
    uint8_t *kb = kmalloc(chunk);
    if (!kb) return -LX_ENOMEM;
    uint64_t done = 0;
    long err = 0;
    while (done < n) {
        uint64_t want = n - done < chunk ? n - done : chunk;
        if (copy_from_user(kb, ubuf + done, want)) { err = -LX_EFAULT; break; }
        int64_t put = positional ? f->ops->pwrite(f, kb, want, (uint64_t)off + done) : f->ops->write(f, kb, want);
        if (put < 0) { err = put; break; }
        done += (uint64_t)put;
        if ((uint64_t)put < want) break;
    }
    kfree(kb);
    return done ? (long)done : err;
}

static long do_iov(struct process *p, int fd, uint64_t uiov, uint64_t cnt, bool write, int64_t off, bool positional) {
    if (cnt > 1024) return -LX_EINVAL;
    struct ufile *f = fd_get(p, fd);
    if (!f) return -LX_EBADF;
    long total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        struct lx_iovec iov;
        if (copy_from_user(&iov, uiov + i * sizeof(iov), sizeof(iov))) { total = total ? total : -LX_EFAULT; break; }
        if (!iov.iov_len) continue;
        long r = write ? do_write(f, iov.iov_base, iov.iov_len, off + total, positional)
                       : do_read(f, iov.iov_base, iov.iov_len, off + total, positional);
        if (r < 0) { if (!total) total = r; break; }
        total += r;
        if ((uint64_t)r < iov.iov_len) break;
    }
    ufile_put(f);
    return total;
}

static long do_open(struct process *p, int dirfd, uint64_t upath, uint64_t flags, uint64_t mode) {
    char abs[PROC_PATH_MAX];
    int r = resolve(p, dirfd, upath, abs);
    if (r) return r;
    struct ufile *f = NULL;
    r = vfs_open(abs, (uint32_t)flags, (uint32_t)mode, &f);
    if (r) return r;
    int fd = fd_install(p, f, 0, (flags & LX_O_CLOEXEC) != 0);
    if (fd < 0) ufile_put(f);
    return fd;
}

static long do_stat_path(struct process *p, int dirfd, uint64_t upath, struct lx_stat *st, uint64_t flags) {
    if (flags & LX_AT_EMPTY_PATH) {
        char c = 0;
        if (!upath || (copy_from_user(&c, upath, 1) == 0 && c == '\0')) {
            struct ufile *f = (dirfd == LX_AT_FDCWD) ? NULL : fd_get(p, dirfd);
            if (!f) {
                if (dirfd != LX_AT_FDCWD) return -LX_EBADF;
                return vfs_stat(p->cwd, st);
            }
            int r = f->ops->stat ? f->ops->stat(f, st) : -LX_EINVAL;
            ufile_put(f);
            return r;
        }
    }
    char abs[PROC_PATH_MAX];
    int r = resolve(p, dirfd, upath, abs);
    if (r) return r;
    return (flags & LX_AT_SYMLINK_NOFOLLOW) ? vfs_lstat(abs, st) : vfs_stat(abs, st);
}

static long sys_fstat(struct process *p, int fd, uint64_t ust) {
    struct ufile *f = fd_get(p, fd);
    if (!f) return -LX_EBADF;
    struct lx_stat st;
    int r = f->ops->stat ? f->ops->stat(f, &st) : -LX_EINVAL;
    ufile_put(f);
    if (r) return r;
    return copy_to_user(ust, &st, sizeof(st));
}

static long sys_statx(struct process *p, int dirfd, uint64_t upath, uint64_t flags, uint64_t mask, uint64_t ubuf) {
    (void)mask;
    struct lx_stat st;
    long r = do_stat_path(p, dirfd, upath, &st, flags);
    if (r) return r;
    struct lx_statx sx;
    memset(&sx, 0, sizeof(sx));
    sx.stx_mask = 0x17FF;
    sx.stx_mnt_id = 1;
    sx.stx_blksize = (uint32_t)st.st_blksize;
    sx.stx_nlink = (uint32_t)st.st_nlink;
    sx.stx_mode = (uint16_t)st.st_mode;
    sx.stx_ino = st.st_ino;
    sx.stx_size = (uint64_t)st.st_size;
    sx.stx_blocks = (uint64_t)st.st_blocks;
    sx.stx_rdev_major = (uint32_t)(st.st_rdev >> 8);
    sx.stx_rdev_minor = (uint32_t)(st.st_rdev & 0xFF);
    sx.stx_dev_minor = 1;
    return copy_to_user(ubuf, &sx, sizeof(sx));
}

static long sys_getdents64(struct process *p, int fd, uint64_t ubuf, uint64_t n) {
    struct ufile *f = fd_get(p, fd);
    if (!f) return -LX_EBADF;
    if (!f->ops->getdents) {
        ufile_put(f);
        return -LX_ENOTDIR;
    }
    if (n > IO_CHUNK) n = IO_CHUNK;
    uint8_t *kb = kmalloc(n ? n : 1);
    if (!kb) {
        ufile_put(f);
        return -LX_ENOMEM;
    }
    int64_t r = f->ops->getdents(f, kb, n);
    if (r > 0 && copy_to_user(ubuf, kb, (size_t)r)) r = -LX_EFAULT;
    kfree(kb);
    ufile_put(f);
    return r;
}

static long sys_lseek(struct process *p, int fd, int64_t off, int whence) {
    struct ufile *f = fd_get(p, fd);
    if (!f) return -LX_EBADF;
    long r = f->ops->lseek ? f->ops->lseek(f, off, whence) : -LX_ESPIPE;
    ufile_put(f);
    return r;
}

static long sys_ioctl(struct process *p, int fd, uint64_t cmd, uint64_t arg) {
    cmd = (uint32_t)cmd;
    struct ufile *f = fd_get(p, fd);
    if (!f) return -LX_EBADF;
    long r = f->ops->ioctl ? f->ops->ioctl(f, cmd, arg) : -LX_ENOTTY;
    ufile_put(f);
    return r;
}

static long sys_dup3(struct process *p, int oldfd, int newfd, uint64_t flags) {
    if (oldfd == newfd) return -LX_EINVAL;
    if (newfd < 0 || newfd >= PROC_MAX_FDS) return -LX_EBADF;
    struct ufile *f = fd_get(p, oldfd);
    if (!f) return -LX_EBADF;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct ufile *old = p->fds[newfd];
    p->fds[newfd] = f;
    p->fd_cloexec[newfd] = (flags & LX_O_CLOEXEC) ? 1 : 0;
    spin_unlock_irqrestore(&p->fd_lock, fl);
    if (old) ufile_put(old);
    return newfd;
}

static long sys_memfd_create(struct process *p, uint64_t uname, uint64_t flags) {
    char name[256];
    long n = strncpy_from_user(name, uname, sizeof(name));
    if (n < 0) return n;
    if (flags & ~(uint64_t)(LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING)) return -LX_EINVAL;
    struct ufile *f = NULL;
    int r = vfs_memfd(name, &f);
    if (r) return r;
    int fd = fd_install(p, f, 0, (flags & LX_MFD_CLOEXEC) != 0);
    if (fd < 0) ufile_put(f);
    return fd;
}

static long sys_fcntl(struct process *p, int fd, uint64_t cmd, uint64_t arg) {
    struct ufile *f = fd_get(p, fd);
    if (!f) return -LX_EBADF;
    long r;
    switch (cmd) {
        case LX_F_DUPFD:
        case LX_F_DUPFD_CLOEXEC:
            r = fd_install(p, ufile_get(f), (int)arg, cmd == LX_F_DUPFD_CLOEXEC);
            if (r < 0) ufile_put(f);
            break;
        case LX_F_GETFD:
            r = p->fd_cloexec[fd] ? LX_FD_CLOEXEC : 0;
            break;
        case LX_F_SETFD:
            p->fd_cloexec[fd] = (arg & LX_FD_CLOEXEC) ? 1 : 0;
            r = 0;
            break;
        case LX_F_GETFL:
            r = f->flags & ~LX_O_CLOEXEC;
            break;
        case LX_F_SETFL:
            f->flags = (f->flags & ~(LX_O_APPEND | LX_O_NONBLOCK)) | ((uint32_t)arg & (LX_O_APPEND | LX_O_NONBLOCK));
            r = 0;
            break;
        case LX_F_GETLK:
        case LX_F_SETLK:
        case LX_F_SETLKW:
        case LX_F_ADD_SEALS:
        case LX_F_GET_SEALS:
            r = 0;
            break;
        default:
            r = -LX_EINVAL;
            break;
    }
    ufile_put(f);
    return r;
}

static long poll_fds(struct process *p, struct lx_pollfd *fds, uint64_t nfds, int64_t timeout_ms) {
    uint64_t start = sys_monotonic_ns();
    long ready;
    for (;;) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            struct ufile *f = fd_get(p, fds[i].fd);
            if (!f) {
                fds[i].revents = LX_POLLNVAL;
                ready++;
                continue;
            }
            int m = f->ops->poll ? f->ops->poll(f) : (LX_POLLIN | LX_POLLOUT);
            ufile_put(f);
            fds[i].revents = (int16_t)(m & (fds[i].events | LX_POLLERR | LX_POLLHUP | LX_POLLNVAL));
            if (fds[i].revents) ready++;
        }
        if (ready || timeout_ms == 0) break;
        if (timeout_ms > 0 && (int64_t)((sys_monotonic_ns() - start) / 1000000ULL) >= timeout_ms) break;
        struct task *t = current_task();
        if (t && t->kill_pending) {
            ready = -LX_EINTR;
            break;
        }
        volatile int never = 0;
        scheduler_wait(vfs_poll_channel(), &never, 2);
    }
    return ready;
}

static long sys_ppoll(struct process *p, uint64_t ufds, uint64_t nfds, int64_t timeout_ms) {
    if (nfds > POLL_MAX) return -LX_EINVAL;
    struct lx_pollfd *fds = NULL;
    if (nfds) {
        fds = kmalloc(sizeof(*fds) * nfds);
        if (!fds) return -LX_ENOMEM;
        if (copy_from_user(fds, ufds, sizeof(*fds) * nfds)) {
            kfree(fds);
            return -LX_EFAULT;
        }
    }
    long ready = poll_fds(p, fds, nfds, timeout_ms);
    if (ready >= 0 && nfds && copy_to_user(ufds, fds, sizeof(*fds) * nfds)) ready = -LX_EFAULT;
    kfree(fds);
    return ready;
}

#define SELECT_MAX_FDS 1024

static long sys_select(struct process *p, int nfds, uint64_t urd, uint64_t uwr, uint64_t uex, int64_t timeout_ms) {
    uint64_t in[3][SELECT_MAX_FDS / 64];
    uint64_t uptr[3] = { urd, uwr, uex };
    if (nfds < 0) return -LX_EINVAL;
    if (nfds > SELECT_MAX_FDS) nfds = SELECT_MAX_FDS;
    size_t bytes = (size_t)((nfds + 63) / 64) * 8;
    for (int k = 0; k < 3; k++) {
        memset(in[k], 0, sizeof(in[k]));
        if (uptr[k] && bytes && copy_from_user(in[k], uptr[k], bytes)) return -LX_EFAULT;
    }
    int count = 0;
    for (int fd = 0; fd < nfds; fd++)
        if ((in[0][fd / 64] | in[1][fd / 64] | in[2][fd / 64]) & (1ULL << (fd % 64))) count++;
    struct lx_pollfd *fds = count ? kmalloc(sizeof(*fds) * (size_t)count) : NULL;
    if (count && !fds) return -LX_ENOMEM;
    int n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1ULL << (fd % 64);
        int16_t ev = 0;
        if (in[0][fd / 64] & bit) ev |= LX_POLLIN;
        if (in[1][fd / 64] & bit) ev |= LX_POLLOUT;
        if (in[2][fd / 64] & bit) ev |= LX_POLLPRI;
        if (!ev) continue;
        fds[n].fd = fd;
        fds[n].events = ev;
        fds[n].revents = 0;
        n++;
    }
    long r = poll_fds(p, fds, (uint64_t)n, timeout_ms);
    if (r < 0) {
        kfree(fds);
        return r;
    }
    uint64_t out[3][SELECT_MAX_FDS / 64];
    memset(out, 0, sizeof(out));
    long ready = 0;
    for (int i = 0; i < n; i++) {
        int fd = fds[i].fd;
        uint64_t bit = 1ULL << (fd % 64);
        if (fds[i].revents & LX_POLLNVAL) {
            kfree(fds);
            return -LX_EBADF;
        }
        if ((fds[i].events & LX_POLLIN) && (fds[i].revents & (LX_POLLIN | LX_POLLHUP | LX_POLLERR))) { out[0][fd / 64] |= bit; ready++; }
        if ((fds[i].events & LX_POLLOUT) && (fds[i].revents & (LX_POLLOUT | LX_POLLERR))) { out[1][fd / 64] |= bit; ready++; }
        if ((fds[i].events & LX_POLLPRI) && (fds[i].revents & LX_POLLPRI)) { out[2][fd / 64] |= bit; ready++; }
    }
    kfree(fds);
    for (int k = 0; k < 3; k++)
        if (uptr[k] && bytes && copy_to_user(uptr[k], out[k], bytes)) return -LX_EFAULT;
    return ready;
}

static long sys_pipe2(struct process *p, uint64_t ufds, uint64_t flags) {
    struct ufile *rd, *wr;
    int r = vfs_pipe(&rd, &wr, (uint32_t)flags);
    if (r) return r;
    bool ce = (flags & LX_O_CLOEXEC) != 0;
    int a = fd_install(p, rd, 0, ce);
    if (a < 0) {
        ufile_put(rd);
        ufile_put(wr);
        return a;
    }
    int b = fd_install(p, wr, 0, ce);
    if (b < 0) {
        fd_close(p, a);
        ufile_put(wr);
        return b;
    }
    int32_t pair[2] = { a, b };
    if (copy_to_user(ufds, pair, sizeof(pair))) {
        fd_close(p, a);
        fd_close(p, b);
        return -LX_EFAULT;
    }
    return 0;
}

static long sys_readlink(struct process *p, int dirfd, uint64_t upath, uint64_t ubuf, uint64_t n) {
    char abs[PROC_PATH_MAX];
    int r = get_path(upath, abs);
    if (r) return r;
    if (!abs[0] && dirfd != LX_AT_FDCWD) {
        struct ufile *f = fd_get(p, dirfd);
        if (!f) return -LX_EBADF;
        strncpy(abs, f->path, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = 0;
        ufile_put(f);
    } else {
        r = resolve(p, dirfd, upath, abs);
        if (r) return r;
    }
    char link[PROC_PATH_MAX];
    long len = vfs_readlink(abs, link, sizeof(link));
    if (len < 0) return len;
    if ((uint64_t)len > n) len = (long)n;
    if (copy_to_user(ubuf, link, (size_t)len)) return -LX_EFAULT;
    return len;
}

static long sys_getcwd(struct process *p, uint64_t ubuf, uint64_t n) {
    size_t len = strlen(p->cwd) + 1;
    if (len > n) return -LX_ERANGE;
    if (copy_to_user(ubuf, p->cwd, len)) return -LX_EFAULT;
    return (long)len;
}

static long sys_chdir_abs(struct process *p, const char *abs) {
    struct lx_stat st;
    int r = vfs_stat(abs, &st);
    if (r) return r;
    if ((st.st_mode & LX_S_IFMT) != LX_S_IFDIR) return -LX_ENOTDIR;
    strncpy(p->cwd, abs, sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = '\0';
    return 0;
}

static long sys_statfs(struct process *p, uint64_t nr, uint64_t a0, uint64_t ubuf) {
    char abs[PROC_PATH_MAX];
    if (nr == 137) {
        struct lx_stat st;
        int r = resolve(p, LX_AT_FDCWD, a0, abs);
        if (r) return r;
        r = vfs_stat(abs, &st);
        if (r) return r;
    } else {
        struct ufile *f = fd_get(p, (int)a0);
        if (!f) return -LX_EBADF;
        strncpy(abs, f->path, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = 0;
        ufile_put(f);
    }
    uint64_t sf[15];
    memset(sf, 0, sizeof(sf));
    sf[0] = vfs_fs_magic(abs);
    sf[1] = 4096;
    if (sf[0] == 0x4d44) {
        sf[2] = 1 << 20;
        sf[3] = 1 << 19;
        sf[4] = 1 << 19;
    }
    sf[8] = 255;
    sf[9] = 4096;
    return copy_to_user(ubuf, sf, sizeof(sf));
}

long sys_fs_dispatch(struct registers *r, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, int *handled) {
    (void)r;
    (void)a5;
    struct process *p = proc_current();
    *handled = 1;
    char abs[PROC_PATH_MAX];
    struct lx_stat st;
    long rv;

    switch (nr) {
        case 0: case 17: {
            struct ufile *f = fd_get(p, (int)a0);
            if (!f) return -LX_EBADF;
            rv = do_read(f, a1, a2, (int64_t)a3, nr == 17);
            ufile_put(f);
            return rv;
        }
        case 1: case 18: {
            struct ufile *f = fd_get(p, (int)a0);
            if (!f) return -LX_EBADF;
            rv = do_write(f, a1, a2, (int64_t)a3, nr == 18);
            ufile_put(f);
            return rv;
        }
        case 2: return do_open(p, LX_AT_FDCWD, a0, a1, a2);
        case 3: return fd_close(p, (int)a0);
        case 4: case 6:
            rv = do_stat_path(p, LX_AT_FDCWD, a0, &st, nr == 6 ? LX_AT_SYMLINK_NOFOLLOW : 0);
            return rv ? rv : copy_to_user(a1, &st, sizeof(st));
        case 5: return sys_fstat(p, (int)a0, a1);
        case 7: return sys_ppoll(p, a0, a1, (int64_t)(int32_t)a2);
        case 8: return sys_lseek(p, (int)a0, (int64_t)a1, (int)a2);
        case 16: return sys_ioctl(p, (int)a0, a1, a2);
        case 19: return do_iov(p, (int)a0, a1, a2, false, 0, false);
        case 20: return do_iov(p, (int)a0, a1, a2, true, 0, false);
        case 295: return do_iov(p, (int)a0, a1, a2, false, (int64_t)a3, true);
        case 296: return do_iov(p, (int)a0, a1, a2, true, (int64_t)a3, true);
        case 21:
            rv = resolve(p, LX_AT_FDCWD, a0, abs);
            return rv ? rv : vfs_stat(abs, &st);
        case 22: return sys_pipe2(p, a0, 0);
        case 293: return sys_pipe2(p, a0, a1);
        case 32: {
            struct ufile *f = fd_get(p, (int)a0);
            if (!f) return -LX_EBADF;
            rv = fd_install(p, f, 0, false);
            if (rv < 0) ufile_put(f);
            return rv;
        }
        case 33:
            if (a0 == a1) {
                struct ufile *f = fd_get(p, (int)a0);
                if (!f) return -LX_EBADF;
                ufile_put(f);
                return (long)a1;
            }
            return sys_dup3(p, (int)a0, (int)a1, 0);
        case 292: return sys_dup3(p, (int)a0, (int)a1, a2);
        case 72: return sys_fcntl(p, (int)a0, a1, a2);
        case 73: case 74: case 75: case 162: return 0;
        case 76: {
            rv = resolve(p, LX_AT_FDCWD, a0, abs);
            if (rv) return rv;
            struct ufile *f = NULL;
            rv = vfs_open(abs, LX_O_WRONLY, 0, &f);
            if (rv) return rv;
            rv = f->ops->truncate ? f->ops->truncate(f, a1) : -LX_EINVAL;
            ufile_put(f);
            return rv;
        }
        case 77: {
            struct ufile *f = fd_get(p, (int)a0);
            if (!f) return -LX_EBADF;
            rv = f->ops->truncate ? f->ops->truncate(f, a1) : -LX_EINVAL;
            ufile_put(f);
            return rv;
        }
        case 79: return sys_getcwd(p, a0, a1);
        case 80:
            rv = resolve(p, LX_AT_FDCWD, a0, abs);
            return rv ? rv : sys_chdir_abs(p, abs);
        case 81: {
            struct ufile *f = fd_get(p, (int)a0);
            if (!f) return -LX_EBADF;
            rv = sys_chdir_abs(p, f->path);
            ufile_put(f);
            return rv;
        }
        case 82: case 264: case 316: return -LX_EXDEV;
        case 83:
            rv = resolve(p, LX_AT_FDCWD, a0, abs);
            return rv ? rv : vfs_mkdir(abs);
        case 258:
            rv = resolve(p, (int)a0, a1, abs);
            return rv ? rv : vfs_mkdir(abs);
        case 84:
            rv = resolve(p, LX_AT_FDCWD, a0, abs);
            return rv ? rv : vfs_unlink(abs, true);
        case 85: return do_open(p, LX_AT_FDCWD, a0, LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, a1);
        case 86: case 88: case 265: case 266: return -LX_EPERM;
        case 87:
            rv = resolve(p, LX_AT_FDCWD, a0, abs);
            return rv ? rv : vfs_unlink(abs, false);
        case 263:
            rv = resolve(p, (int)a0, a1, abs);
            return rv ? rv : vfs_unlink(abs, (a2 & LX_AT_REMOVEDIR) != 0);
        case 89: return sys_readlink(p, LX_AT_FDCWD, a0, a1, a2);
        case 267: return sys_readlink(p, (int)a0, a1, a2, a3);
        case 90: case 91: case 92: case 93: case 94: case 268: case 260: case 280: case 132: case 235:
            return 0;
        case 95: return 022;
        case 137: case 138: return sys_statfs(p, nr, a0, a1);
        case 217: return sys_getdents64(p, (int)a0, a1, a2);
        case 319: return sys_memfd_create(p, a0, a1);
        case 257: return do_open(p, (int)a0, a1, a2, a3);
        case 262:
            rv = do_stat_path(p, (int)a0, a1, &st, a3);
            return rv ? rv : copy_to_user(a2, &st, sizeof(st));
        case 269: case 439:
            rv = resolve(p, (int)a0, a1, abs);
            return rv ? rv : vfs_stat(abs, &st);
        case 271: {
            int64_t ms = -1;
            if (a2) {
                struct lx_timespec ts;
                if (copy_from_user(&ts, a2, sizeof(ts))) return -LX_EFAULT;
                ms = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
            }
            return sys_ppoll(p, a0, a1, ms);
        }
        case 23: {
            int64_t ms = -1;
            if (a4) {
                struct lx_timeval tv;
                if (copy_from_user(&tv, a4, sizeof(tv))) return -LX_EFAULT;
                ms = tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
            }
            return sys_select(p, (int)a0, a1, a2, a3, ms);
        }
        case 270: {
            int64_t ms = -1;
            if (a4) {
                struct lx_timespec ts;
                if (copy_from_user(&ts, a4, sizeof(ts))) return -LX_EFAULT;
                ms = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
            }
            return sys_select(p, (int)a0, a1, a2, a3, ms);
        }
        case 285: return -LX_EOPNOTSUPP;
        case 332: return sys_statx(p, (int)a0, a1, a2, a3, a4);
        default:
            *handled = 0;
            return -LX_ENOSYS;
    }
}
