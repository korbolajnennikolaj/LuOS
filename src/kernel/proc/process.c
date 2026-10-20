// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/process.h"

#include "components/GDT/gdt.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/vmm.h"
#include "kernel/proc/elf.h"
#include "kernel/proc/futex.h"
#include "kernel/proc/vfs.h"
#include "kernel/rootfs.h"
#include "kernel/scheduler/scheduler.h"

#include <msr.h>
#include <string.h>

#define PROC_TABLE_MAX 64
#define USER_KSTACK_SIZE (64 * 1024)

static struct process *proc_table[PROC_TABLE_MAX];
static spinlock_t proc_table_lock = SPINLOCK_INIT;

struct spawn_ctx {
    struct process *proc;
    char path[PROC_PATH_MAX];
    int argc;
    int envc;
    char **argv;
    char **envp;
};

struct process *proc_current(void) {
    struct task *t = current_task();
    return t ? (struct process *)t->proc : NULL;
}

int proc_count(void) {
    int n = 0;
    uint64_t f = spin_lock_irqsave(&proc_table_lock);
    for (int i = 0; i < PROC_TABLE_MAX; i++) n += proc_table[i] ? 1 : 0;
    spin_unlock_irqrestore(&proc_table_lock, f);
    return n;
}

struct process *proc_at(int index) {
    int n = 0;
    struct process *r = NULL;
    uint64_t f = spin_lock_irqsave(&proc_table_lock);
    for (int i = 0; i < PROC_TABLE_MAX; i++) {
        if (!proc_table[i]) continue;
        if (n++ == index) { r = proc_table[i]; break; }
    }
    spin_unlock_irqrestore(&proc_table_lock, f);
    return r;
}

static void proc_table_add(struct process *p) {
    uint64_t f = spin_lock_irqsave(&proc_table_lock);
    for (int i = 0; i < PROC_TABLE_MAX; i++)
        if (!proc_table[i]) { proc_table[i] = p; break; }
    spin_unlock_irqrestore(&proc_table_lock, f);
}

static void proc_table_remove(struct process *p) {
    uint64_t f = spin_lock_irqsave(&proc_table_lock);
    for (int i = 0; i < PROC_TABLE_MAX; i++)
        if (proc_table[i] == p) proc_table[i] = NULL;
    spin_unlock_irqrestore(&proc_table_lock, f);
}

void proc_put(struct process *p) {
    if (!p) return;
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        proc_table_remove(p);
        kfree(p);
    }
}

__attribute__((noreturn)) static void enter_user(uint64_t rip, uint64_t rsp) {
    asm volatile(
        "mov %0, %%rcx\n\t"
        "mov %1, %%rdx\n\t"
        "pushq %2\n\t"
        "pushq %%rdx\n\t"
        "pushq $0x202\n\t"
        "pushq %3\n\t"
        "pushq %%rcx\n\t"
        "xor %%eax, %%eax\n\t"
        "xor %%ebx, %%ebx\n\t"
        "xor %%ecx, %%ecx\n\t"
        "xor %%edx, %%edx\n\t"
        "xor %%esi, %%esi\n\t"
        "xor %%edi, %%edi\n\t"
        "xor %%ebp, %%ebp\n\t"
        "xor %%r8d, %%r8d\n\t"
        "xor %%r9d, %%r9d\n\t"
        "xor %%r10d, %%r10d\n\t"
        "xor %%r11d, %%r11d\n\t"
        "xor %%r12d, %%r12d\n\t"
        "xor %%r13d, %%r13d\n\t"
        "xor %%r14d, %%r14d\n\t"
        "xor %%r15d, %%r15d\n\t"
        "iretq\n\t"
        :
        : "r"(rip), "r"(rsp), "i"((uint64_t)USER_SS), "i"((uint64_t)USER_CS)
        : "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) static void return_to_user(struct registers *r) {
    asm volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "add $16, %%rsp\n\t"
        "iretq\n\t"
        :
        : "r"(r)
        : "memory");
    __builtin_unreachable();
}

static void spawn_ctx_free(struct spawn_ctx *c) {
    if (!c) return;
    for (int i = 0; i < c->argc; i++) kfree(c->argv[i]);
    for (int i = 0; i < c->envc; i++) kfree(c->envp[i]);
    kfree(c->argv);
    kfree(c->envp);
    kfree(c);
}

static void user_main(void *arg) {
    struct spawn_ctx *c = arg;
    struct process *p = c->proc;
    struct task *t = current_task();
    p->pid = (int)t->tid;

    uint64_t entry = 0, sp = 0;
    int r = elf_exec(p, c->path, c->argc, (const char *const *)c->argv, c->envc, (const char *const *)c->envp,
                     &entry, &sp);
    spawn_ctx_free(c);

    p->exec_error = r;
    __atomic_store_n(&p->exec_ready, 1, __ATOMIC_RELEASE);
    scheduler_wake_all((void *)&p->exec_ready);

    if (r < 0) proc_thread_exit(127);

    LOG_DEBUG("pid %d (%s): entering user mode at 0x%llx, stack 0x%llx", p->pid, p->name,
              (unsigned long long)entry, (unsigned long long)sp);
    wrmsr(MSR_IA32_FS_BASE, 0);
    t->fs_base = 0;
    enter_user(entry, sp);
}

static char *kstrdup_n(const char *s) {
    size_t n = strlen(s);
    char *d = kmalloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

int proc_spawn(const char *path, int argc, const char *const *argv, int envc, const char *const *envp,
               struct process **out) {
    if (out) *out = NULL;
    if (!path || !path[0]) return -LX_ENOENT;
    if (argc > PROC_MAX_ARGS || envc > PROC_MAX_ARGS) return -LX_E2BIG;

    struct process *p = kmalloc(sizeof(*p));
    if (!p) return -LX_ENOMEM;
    memset(p, 0, sizeof(*p));
    if (mm_init(&p->mm) != 0) {
        kfree(p);
        return -LX_ENOMEM;
    }
    spin_lock_init(&p->fd_lock);

    const char *base = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') base = s + 1;
    strncpy(p->name, base, sizeof(p->name) - 1);
    strncpy(p->exe, path, sizeof(p->exe) - 1);
    strncpy(p->cwd, rootfs_cwd(), sizeof(p->cwd) - 1);
    if (!p->cwd[0]) strcpy(p->cwd, "/");

    struct ufile *con = vfs_console_open();
    if (!con) {
        mm_destroy(&p->mm);
        kfree(p);
        return -LX_ENOMEM;
    }
    fd_install(p, con, 0, false);
    fd_install(p, ufile_get(con), 1, false);
    fd_install(p, ufile_get(con), 2, false);

    struct spawn_ctx *c = kmalloc(sizeof(*c));
    if (!c) {
        fd_close_all(p);
        mm_destroy(&p->mm);
        kfree(p);
        return -LX_ENOMEM;
    }
    memset(c, 0, sizeof(*c));
    c->proc = p;
    strncpy(c->path, path, sizeof(c->path) - 1);
    c->argv = kmalloc(sizeof(char *) * (size_t)(argc + 1));
    c->envp = kmalloc(sizeof(char *) * (size_t)(envc + 1));
    if (!c->argv || !c->envp) {
        spawn_ctx_free(c);
        fd_close_all(p);
        mm_destroy(&p->mm);
        kfree(p);
        return -LX_ENOMEM;
    }
    for (int i = 0; i < argc; i++) {
        c->argv[i] = kstrdup_n(argv[i]);
        if (c->argv[i]) c->argc++;
    }
    for (int i = 0; i < envc; i++) {
        c->envp[i] = kstrdup_n(envp[i]);
        if (c->envp[i]) c->envc++;
    }

    p->threads = 1;
    p->refs = 2;
    p->ppid = 0;
    proc_table_add(p);

    struct task *t = task_create_user(p->name, user_main, c, PRIO_DEFAULT, p, p->mm.pml4, USER_KSTACK_SIZE, 1);
    if (!t) {
        spawn_ctx_free(c);
        fd_close_all(p);
        mm_destroy(&p->mm);
        proc_table_remove(p);
        kfree(p);
        return -LX_ENOMEM;
    }

    while (!__atomic_load_n(&p->exec_ready, __ATOMIC_ACQUIRE))
        scheduler_wait((void *)&p->exec_ready, &p->exec_ready, 50);

    if (p->exec_error < 0) {
        int err = p->exec_error;
        int status;
        proc_wait(p, &status);
        proc_put(p);
        return err;
    }

    if (out) *out = p;
    else proc_put(p);
    return p->pid;
}

int proc_wait(struct process *p, int *status) {
    while (!__atomic_load_n(&p->done, __ATOMIC_ACQUIRE))
        scheduler_wait((void *)p, &p->done, 100);
    if (status) *status = p->exit_code;
    return p->pid;
}

static void proc_teardown(struct process *p) {
    fd_close_all(p);
    mm_destroy(&p->mm);
    LOG_DEBUG("pid %d (%s) exited with %d", p->pid, p->name, p->exit_code);
    __atomic_store_n(&p->done, 1, __ATOMIC_RELEASE);
    scheduler_wake_all((void *)p);
    proc_put(p);
}

void proc_thread_exit(int code) {
    struct task *t = current_task();
    struct process *p = (struct process *)t->proc;

    if (p && t->clear_child_tid) {
        int zero = 0;
        if (copy_to_user(t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake(p, t->clear_child_tid, 1, 0xFFFFFFFFu);
        t->clear_child_tid = 0;
    }

    asm volatile("cli");
    t->cr3 = 0;
    vmm_switch_address_space(vmm_kernel_pml4());
    asm volatile("sti");

    if (p) {
        if (!p->exiting) p->exit_code = code;
        if (__atomic_sub_fetch(&p->threads, 1, __ATOMIC_ACQ_REL) == 0) proc_teardown(p);
    }
    t->proc = NULL;
    task_exit();
}

static int match_proc_other(struct task *t, void *ctx) {
    return t->proc == ctx && t != current_task();
}

static void mark_kill(struct task *t, void *ctx) {
    if (t->proc == ctx && t != current_task()) t->kill_pending = 1;
}

void proc_kill(struct process *p, int code) {
    if (__atomic_exchange_n(&p->exiting, 1, __ATOMIC_ACQ_REL) == 0) p->exit_code = code;
    scheduler_for_each_task(mark_kill, p);
    scheduler_wake_matching(match_proc_other, p);
}

void proc_exit_group(int code) {
    struct process *p = proc_current();
    if (p) proc_kill(p, code);
    proc_thread_exit(code);
}

void proc_check_pending(void) {
    struct task *t = current_task();
    if (!t || !t->proc) return;
    struct process *p = (struct process *)t->proc;
    if (t->kill_pending || p->exiting) {
        asm volatile("sti");
        proc_thread_exit(p->exit_code);
    }
}


struct clone_ctx {
    struct registers regs;
};

static void clone_start(void *arg) {
    struct clone_ctx *c = arg;
    struct registers r = c->regs;
    kfree(c);
    struct task *t = current_task();
    asm volatile("cli");
    wrmsr(MSR_IA32_FS_BASE, t->fs_base);
    return_to_user(&r);
}

long proc_clone_thread(struct registers *regs, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid,
                       uint64_t tls) {
    struct process *p = proc_current();
    if (!p) return -LX_EINVAL;
    if ((flags & (LX_CLONE_VM | LX_CLONE_THREAD)) != (LX_CLONE_VM | LX_CLONE_THREAD)) return -LX_ENOSYS;

    struct clone_ctx *c = kmalloc(sizeof(*c));
    if (!c) return -LX_ENOMEM;
    c->regs = *regs;
    c->regs.rax = 0;
    if (newsp) c->regs.rsp = newsp;
    c->regs.cs = USER_CS;
    c->regs.ss = USER_SS;
    c->regs.rflags = (regs->rflags & 0xCD5) | 0x202;

    __atomic_add_fetch(&p->threads, 1, __ATOMIC_ACQ_REL);
    struct task *t = task_create_user(p->name, clone_start, c, PRIO_DEFAULT, p, p->mm.pml4, USER_KSTACK_SIZE, 0);
    if (!t) {
        __atomic_sub_fetch(&p->threads, 1, __ATOMIC_ACQ_REL);
        kfree(c);
        return -LX_EAGAIN;
    }

    int tid = (int)t->tid;
    t->fs_base = (flags & LX_CLONE_SETTLS) ? tls : rdmsr(MSR_IA32_FS_BASE);
    t->gs_base = rdmsr(MSR_IA32_GS_BASE);
    if (flags & LX_CLONE_CHILD_CLEARTID) t->clear_child_tid = ctid;
    if (flags & LX_CLONE_PARENT_SETTID) copy_to_user(ptid, &tid, sizeof(tid));
    if (flags & LX_CLONE_CHILD_SETTID) copy_to_user(ctid, &tid, sizeof(tid));
    task_start(t);
    return tid;
}
