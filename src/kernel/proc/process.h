// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef PROCESS_H
#define PROCESS_H

#include "components/Interruptions/isr.h"
#include "kernel/proc/linux_abi.h"
#include "kernel/scheduler/spinlock.h"
#include "kernel/scheduler/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROC_MAX_FDS 256
#define PROC_PATH_MAX 256
#define PROC_MAX_ARGS 64

#define USER_STACK_TOP 0x00007FFFFFFFE000ULL
#define USER_STACK_SIZE (8ULL * 1024 * 1024)
#define USER_MMAP_TOP 0x00007F0000000000ULL
#define USER_MMAP_BOTTOM 0x0000100000000000ULL
#define USER_PIE_BASE 0x0000555555554000ULL
#define USER_LIMIT 0x0000800000000000ULL

struct ufile;

struct kmutex {
    spinlock_t lock;
};

static inline void kmutex_init(struct kmutex *m) { spin_lock_init(&m->lock); }
void kmutex_lock(struct kmutex *m);
static inline void kmutex_unlock(struct kmutex *m) { spin_unlock(&m->lock); }

enum vma_kind { VMA_ANON = 0, VMA_FILE = 1, VMA_DEVICE = 2, VMA_SHARED = 3 };

struct vma {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
    uint8_t kind;
    struct ufile *file;
    uint64_t file_off;
    uint64_t file_bytes;
    void *dev_ctx;
};

struct mm_device_hooks {
    int (*fault)(void *ctx, void *proc, uint64_t addr, int write);
    void *(*split)(void *ctx, uint64_t old_start, uint64_t new_start, uint64_t new_end);
    void (*close)(void *ctx);
};
extern const struct mm_device_hooks *mm_device_hooks;
struct mm;
int mm_map_device(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, void *dev_ctx,
                  uint64_t *out_addr);
int mm_reserve_range(struct mm *mm, uint64_t addr, uint64_t len, uint32_t flags, uint64_t *out_addr);

struct mm {
    uint64_t pml4;
    struct kmutex lock;
    struct vma *vmas;
    int nvma;
    int cap;
    uint64_t brk_start;
    uint64_t brk;
    uint64_t mmap_hint;
    uint64_t resident_pages;
};

struct process {
    int pid;
    int ppid;
    char name[32];
    char exe[PROC_PATH_MAX];
    char cwd[PROC_PATH_MAX];

    struct mm mm;

    spinlock_t fd_lock;
    struct ufile *fds[PROC_MAX_FDS];
    uint8_t fd_cloexec[PROC_MAX_FDS];

    uint64_t sig_handlers[LX_NSIG];
    uint64_t sig_mask;

    volatile int threads;
    volatile int exiting;
    int exit_code;
    volatile int done;
    volatile int refs;

    volatile int exec_ready;
    int exec_error;

    uint64_t start_ms;
};

struct process *proc_current(void);

int proc_spawn(const char *path, int argc, const char *const *argv, int envc, const char *const *envp,
               struct process **out);
int proc_wait(struct process *p, int *status);
void proc_put(struct process *p);

void proc_thread_exit(int code) __attribute__((noreturn));
void proc_exit_group(int code) __attribute__((noreturn));

#define PROC_EXIT_SIGNAL 0x10000
void proc_kill(struct process *p, int code);
void proc_check_pending(void);

int proc_count(void);
struct process *proc_at(int index);

int mm_init(struct mm *mm);
void mm_destroy(struct mm *mm);
int mm_map(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, struct ufile *file,
           uint64_t off, uint64_t file_bytes, uint64_t *out_addr);
int mm_unmap(struct mm *mm, uint64_t addr, uint64_t len);
int mm_protect(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot);
int mm_dontneed(struct mm *mm, uint64_t addr, uint64_t len);
int mm_sync(struct mm *mm, uint64_t addr, uint64_t len);
uint64_t mm_find_free(struct mm *mm, uint64_t len, uint64_t align);
int mm_handle_fault(struct mm *mm, uint64_t addr, uint64_t err);

int user_fault(struct registers *regs, uint64_t addr);
void user_exception(struct registers *regs);

int copy_from_user(void *dst, uint64_t src, size_t n);
int copy_to_user(uint64_t dst, const void *src, size_t n);
int clear_user(uint64_t dst, size_t n);
long strncpy_from_user(char *dst, uint64_t src, size_t cap);
bool user_range_ok(uint64_t addr, size_t n);

#endif
