// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef LINUX_ABI_H
#define LINUX_ABI_H

#include <stdint.h>

#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_ENXIO 6
#define LX_E2BIG 7
#define LX_ENOEXEC 8
#define LX_EBADF 9
#define LX_ECHILD 10
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_EEXIST 17
#define LX_EXDEV 18
#define LX_ENODEV 19
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_ENFILE 23
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_EFBIG 27
#define LX_ENOSPC 28
#define LX_ESPIPE 29
#define LX_EROFS 30
#define LX_EPIPE 32
#define LX_ERANGE 34
#define LX_ENAMETOOLONG 36
#define LX_ENOSYS 38
#define LX_ENOTEMPTY 39
#define LX_ELOOP 40
#define LX_EOVERFLOW 75
#define LX_ENOTSOCK 88
#define LX_EOPNOTSUPP 95
#define LX_EAFNOSUPPORT 97
#define LX_ETIMEDOUT 110

#define LX_O_ACCMODE 0003
#define LX_O_RDONLY 00
#define LX_O_WRONLY 01
#define LX_O_RDWR 02
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW 0400000
#define LX_O_CLOEXEC 02000000
#define LX_O_PATH 010000000

#define LX_AT_FDCWD (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_EMPTY_PATH 0x1000

#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_SETLK 6
#define LX_F_SETLKW 7
#define LX_F_GETLK 5
#define LX_F_DUPFD_CLOEXEC 1030
#define LX_F_ADD_SEALS 1033
#define LX_F_GET_SEALS 1034
#define LX_MFD_CLOEXEC 1
#define LX_MFD_ALLOW_SEALING 2
#define LX_FD_CLOEXEC 1

#define LX_SEEK_SET 0
#define LX_SEEK_CUR 1
#define LX_SEEK_END 2

#define LX_S_IFMT 0170000
#define LX_S_IFDIR 0040000
#define LX_S_IFCHR 0020000
#define LX_S_IFREG 0100000
#define LX_S_IFIFO 0010000
#define LX_S_IFLNK 0120000

#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO 1
#define LX_DT_CHR 2
#define LX_DT_DIR 4
#define LX_DT_REG 8
#define LX_DT_LNK 10

#define LX_PROT_NONE 0
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_SHARED_VALIDATE 0x03
#define LX_MAP_TYPE 0x0f
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_GROWSDOWN 0x100
#define LX_MAP_NORESERVE 0x4000
#define LX_MAP_POPULATE 0x8000
#define LX_MAP_STACK 0x20000
#define LX_MAP_FIXED_NOREPLACE 0x100000

#define LX_MADV_NORMAL 0
#define LX_MADV_DONTNEED 4
#define LX_MADV_FREE 8

#define LX_MREMAP_MAYMOVE 1
#define LX_MREMAP_FIXED 2

#define LX_CLONE_VM 0x00000100
#define LX_CLONE_FS 0x00000200
#define LX_CLONE_FILES 0x00000400
#define LX_CLONE_SIGHAND 0x00000800
#define LX_CLONE_VFORK 0x00004000
#define LX_CLONE_PARENT 0x00008000
#define LX_CLONE_THREAD 0x00010000
#define LX_CLONE_SYSVSEM 0x00040000
#define LX_CLONE_SETTLS 0x00080000
#define LX_CLONE_PARENT_SETTID 0x00100000
#define LX_CLONE_CHILD_CLEARTID 0x00200000
#define LX_CLONE_DETACHED 0x00400000
#define LX_CLONE_CHILD_SETTID 0x01000000

#define LX_FUTEX_WAIT 0
#define LX_FUTEX_WAKE 1
#define LX_FUTEX_REQUEUE 3
#define LX_FUTEX_CMP_REQUEUE 4
#define LX_FUTEX_WAKE_OP 5
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_FUTEX_PRIVATE_FLAG 128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_FUTEX_CMD_MASK (~(LX_FUTEX_PRIVATE_FLAG | LX_FUTEX_CLOCK_REALTIME))

#define LX_CLOCK_REALTIME 0
#define LX_CLOCK_MONOTONIC 1
#define LX_CLOCK_PROCESS_CPUTIME_ID 2
#define LX_CLOCK_THREAD_CPUTIME_ID 3
#define LX_CLOCK_MONOTONIC_RAW 4
#define LX_CLOCK_REALTIME_COARSE 5
#define LX_CLOCK_MONOTONIC_COARSE 6
#define LX_CLOCK_BOOTTIME 7
#define LX_TIMER_ABSTIME 1

#define LX_ARCH_SET_GS 0x1001
#define LX_ARCH_SET_FS 0x1002
#define LX_ARCH_GET_FS 0x1003
#define LX_ARCH_GET_GS 0x1004

#define LX_SIGHUP 1
#define LX_SIGINT 2
#define LX_SIGQUIT 3
#define LX_SIGILL 4
#define LX_SIGTRAP 5
#define LX_SIGABRT 6
#define LX_SIGBUS 7
#define LX_SIGFPE 8
#define LX_SIGKILL 9
#define LX_SIGSEGV 11
#define LX_SIGPIPE 13
#define LX_SIGTERM 15
#define LX_SIGCHLD 17
#define LX_NSIG 65

#define LX_POLLIN 0x001
#define LX_POLLPRI 0x002
#define LX_POLLOUT 0x004
#define LX_POLLERR 0x008
#define LX_POLLHUP 0x010
#define LX_POLLNVAL 0x020
#define LX_POLLRDNORM 0x040
#define LX_POLLWRNORM 0x100

#define LX_TCGETS 0x5401
#define LX_TCSETS 0x5402
#define LX_TCSETSW 0x5403
#define LX_TCSETSF 0x5404
#define LX_TIOCGPGRP 0x540F
#define LX_TIOCSPGRP 0x5410
#define LX_TIOCGWINSZ 0x5413
#define LX_FIONREAD 0x541B
#define LX_FIONBIO 0x5421

#define LX_AT_NULL 0
#define LX_AT_PHDR 3
#define LX_AT_PHENT 4
#define LX_AT_PHNUM 5
#define LX_AT_PAGESZ 6
#define LX_AT_BASE 7
#define LX_AT_FLAGS 8
#define LX_AT_ENTRY 9
#define LX_AT_UID 11
#define LX_AT_EUID 12
#define LX_AT_GID 13
#define LX_AT_EGID 14
#define LX_AT_PLATFORM 15
#define LX_AT_HWCAP 16
#define LX_AT_CLKTCK 17
#define LX_AT_SECURE 23
#define LX_AT_RANDOM 25
#define LX_AT_HWCAP2 26
#define LX_AT_EXECFN 31
#define LX_AT_MINSIGSTKSZ 51

#define LX_WNOHANG 1

struct lx_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct lx_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct lx_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct lx_timespec st_atim;
    struct lx_timespec st_mtim;
    struct lx_timespec st_ctim;
    int64_t unused[3];
};

struct lx_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t reserved;
};

struct lx_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct lx_statx_timestamp stx_atime;
    struct lx_statx_timestamp stx_btime;
    struct lx_statx_timestamp stx_ctime;
    struct lx_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t spare2[13];
};

struct lx_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} __attribute__((packed));

struct lx_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct lx_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct lx_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct lx_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct lx_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct lx_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char reserved[4];
};

struct lx_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};

#define LX_USER_TOP 0x00007FFFFFFFF000ULL

#endif
