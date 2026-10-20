// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/syscall.h"

#include "components/drivers.h"
#include "components/GDT/gdt.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/pmm.h"
#include "drivers/Timer/timer.h"
#include "kernel/kernel_info.h"
#include "kernel/proc/futex.h"
#include "kernel/proc/linux_abi.h"
#include "kernel/proc/process.h"
#include "kernel/proc/vfs.h"
#include "kernel/scheduler/scheduler.h"

#include <msr.h>
#include <string.h>

static volatile uint64_t g_syscalls = 0;
static volatile int g_strace = 0;

void syscall_set_trace(int on) { g_strace = on; }
int syscall_get_trace(void) { return g_strace; }
static uint8_t g_unknown_warned[512];

uint64_t syscall_count(void) { return g_syscalls; }

__attribute__((naked)) void syscall_entry(void) {
    asm volatile(
        "swapgs\n\t"
        "mov %rsp, %gs:8\n\t"
        "mov %gs:16, %rsp\n\t"
        "pushq $0x23\n\t"
        "pushq %gs:8\n\t"
        "pushq %r11\n\t"
        "pushq $0x2B\n\t"
        "pushq %rcx\n\t"
        "swapgs\n\t"
        "pushq $0\n\t"
        "pushq $0x80\n\t"
        "push %rax\n\t"
        "push %rbx\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %rbp\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rsp, %rdi\n\t"
        "mov %rsp, %rbx\n\t"
        "sub fpu_save_size(%rip), %rsp\n\t"
        "and $-64, %rsp\n\t"
        "cmpb $0, fpu_use_xsave(%rip)\n\t"
        "je 1f\n\t"
        "xor %eax, %eax\n\t"
        "mov %rax, 512(%rsp)\n\t"
        "mov %rax, 520(%rsp)\n\t"
        "mov %rax, 528(%rsp)\n\t"
        "mov %rax, 536(%rsp)\n\t"
        "mov %rax, 544(%rsp)\n\t"
        "mov %rax, 552(%rsp)\n\t"
        "mov %rax, 560(%rsp)\n\t"
        "mov %rax, 568(%rsp)\n\t"
        "mov $-1, %eax\n\t"
        "mov $-1, %edx\n\t"
        "xsave64 (%rsp)\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "fxsave (%rsp)\n"
        "2:\n\t"
        "mov %rsp, %rsi\n\t"
        "call syscall_handler\n\t"
        "cli\n\t"
        "cmpb $0, fpu_use_xsave(%rip)\n\t"
        "je 3f\n\t"
        "mov $-1, %eax\n\t"
        "mov $-1, %edx\n\t"
        "xrstor64 (%rsp)\n\t"
        "jmp 4f\n"
        "3:\n\t"
        "fxrstor (%rsp)\n"
        "4:\n\t"
        "mov %rbx, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rbp\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        "add $16, %rsp\n\t"
        "iretq\n\t");
}


static uint64_t tsc_tpm(void) {
    static uint64_t tpm;
    if (!tpm) {
        struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
        if (tsc && tsc->get_tsc_ticks_per_ms) tpm = tsc->get_tsc_ticks_per_ms();
    }
    return tpm;
}

static uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t sys_monotonic_ns(void) {
    uint64_t tpm = tsc_tpm();
    if (!tpm) return scheduler_ticks() * 1000000ULL;
    uint64_t t = rdtsc64();
    return (t / tpm) * 1000000ULL + ((t % tpm) * 1000000ULL) / tpm;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t sys_realtime_ns(void) {
    static uint64_t epoch_at_boot_ns;
    if (!epoch_at_boot_ns) {
        struct rtc_driver *rtc = get_self_driver(TIMER_DRIVER, RTC_TIMER);
        struct system_time *t = (rtc && rtc->get_rtc_time) ? rtc->get_rtc_time() : NULL;
        uint64_t secs = 1767225600ULL;
        if (t && t->year >= 1970) {
            secs = (uint64_t)days_from_civil(t->year, t->month, t->day) * 86400ULL + (uint64_t)t->hours * 3600ULL +
                   (uint64_t)t->minutes * 60ULL + t->seconds;
        }
        uint64_t now = sys_monotonic_ns();
        epoch_at_boot_ns = secs * 1000000000ULL - now;
    }
    return epoch_at_boot_ns + sys_monotonic_ns();
}

static uint64_t ts_to_ms_ceil(const struct lx_timespec *ts) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0) return 0;
    return (uint64_t)ts->tv_sec * 1000ULL + ((uint64_t)ts->tv_nsec + 999999ULL) / 1000000ULL;
}

static long do_sleep_ns(uint64_t ns) {
    uint64_t end = sys_monotonic_ns() + ns;
    for (;;) {
        uint64_t now = sys_monotonic_ns();
        if (now >= end) return 0;
        struct task *t = current_task();
        if (t && t->kill_pending) return -LX_EINTR;
        uint64_t left_ms = (end - now) / 1000000ULL;
        if (left_ms == 0) scheduler_yield();
        else scheduler_sleep_ms(left_ms > 100 ? 100 : left_ms);
    }
}


static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    struct process *p = proc_current();
    if (!len) return -LX_EINVAL;
    if (off & (PAGE_SIZE - 1)) return -LX_EINVAL;
    uint32_t type = flags & LX_MAP_TYPE;
    if (type != LX_MAP_PRIVATE && type != LX_MAP_SHARED && type != LX_MAP_SHARED_VALIDATE) return -LX_EINVAL;

    struct ufile *f = NULL;
    uint64_t file_bytes = 0;
    if (!(flags & LX_MAP_ANONYMOUS)) {
        f = fd_get(p, (int)fd);
        if (!f) return -LX_EBADF;
        if (f->ops->mmap) {
            int64_t r = f->ops->mmap(f, addr, len, (uint32_t)prot, (uint32_t)flags, off);
            ufile_put(f);
            return r;
        }
        if (!f->ops->pread) {
            ufile_put(f);
            return -LX_ENODEV;
        }
        if ((f->mode & LX_S_IFMT) == LX_S_IFREG) {
            struct lx_stat st;
            if (f->ops->stat && f->ops->stat(f, &st) == 0)
                file_bytes = st.st_size > (int64_t)off ? (uint64_t)st.st_size - off : 0;
        }
        if (file_bytes > len) file_bytes = len;
    }

    uint64_t out = 0;
    int r = mm_map(&p->mm, addr, len, (uint32_t)prot, (uint32_t)flags, f, off, file_bytes, &out);
    if (f) ufile_put(f);
    return r ? r : (long)out;
}

static long sys_brk(uint64_t want) {
    struct process *p = proc_current();
    struct mm *mm = &p->mm;
    if (!want || want < mm->brk_start) return (long)mm->brk;
    uint64_t old_end = (mm->brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t new_end = (want + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (new_end > old_end) {
        uint64_t got;
        int r = mm_map(mm, old_end, new_end - old_end, LX_PROT_READ | LX_PROT_WRITE,
                       LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED_NOREPLACE, NULL, 0, 0, &got);
        if (r) return (long)mm->brk;
    } else if (new_end < old_end) {
        mm_unmap(mm, new_end, old_end - new_end);
    }
    mm->brk = want;
    return (long)mm->brk;
}


static long sys_uname(uint64_t buf) {
    struct lx_utsname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "Linux");
    strcpy(u.nodename, "luos");
    strcpy(u.release, "6.12.0-luos-" KERNEL_VERSION);
    strcpy(u.version, "#1 SMP LuOS " KERNEL_VERSION);
    strcpy(u.machine, "x86_64");
    strcpy(u.domainname, "(none)");
    return copy_to_user(buf, &u, sizeof(u));
}

static long sys_clock_gettime(uint64_t clk, uint64_t tsp) {
    uint64_t ns;
    switch (clk) {
        case LX_CLOCK_REALTIME:
        case LX_CLOCK_REALTIME_COARSE:
            ns = sys_realtime_ns();
            break;
        case LX_CLOCK_MONOTONIC:
        case LX_CLOCK_MONOTONIC_RAW:
        case LX_CLOCK_MONOTONIC_COARSE:
        case LX_CLOCK_BOOTTIME:
            ns = sys_monotonic_ns();
            break;
        case LX_CLOCK_PROCESS_CPUTIME_ID:
        case LX_CLOCK_THREAD_CPUTIME_ID: {
            struct task *t = current_task();
            ns = t ? t->run_ticks * 1000000ULL : 0;
            break;
        }
        default:
            return -LX_EINVAL;
    }
    struct lx_timespec ts = { (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL) };
    return copy_to_user(tsp, &ts, sizeof(ts));
}

static long sys_clock_nanosleep(uint64_t clk, uint64_t flags, uint64_t req, uint64_t rem) {
    (void)rem;
    struct lx_timespec ts;
    if (copy_from_user(&ts, req, sizeof(ts))) return -LX_EFAULT;
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL || ts.tv_sec < 0) return -LX_EINVAL;
    uint64_t want = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    if (flags & LX_TIMER_ABSTIME) {
        uint64_t now = clk == LX_CLOCK_REALTIME ? sys_realtime_ns() : sys_monotonic_ns();
        want = want > now ? want - now : 0;
    }
    return do_sleep_ns(want);
}

static long sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    struct process *p = proc_current();
    int cmd = (int)(op & LX_FUTEX_CMD_MASK);
    switch (cmd) {
        case LX_FUTEX_WAIT:
        case LX_FUTEX_WAIT_BITSET: {
            uint64_t ms = 0;
            if (timeout) {
                struct lx_timespec ts;
                if (copy_from_user(&ts, timeout, sizeof(ts))) return -LX_EFAULT;
                if (cmd == LX_FUTEX_WAIT_BITSET) {
                    uint64_t abs_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
                    uint64_t now = (op & LX_FUTEX_CLOCK_REALTIME) ? sys_realtime_ns() : sys_monotonic_ns();
                    if (abs_ns <= now) return -LX_ETIMEDOUT;
                    ms = (abs_ns - now + 999999ULL) / 1000000ULL;
                } else {
                    ms = ts_to_ms_ceil(&ts);
                    if (!ms) return -LX_ETIMEDOUT;
                }
            }
            uint32_t bits = cmd == LX_FUTEX_WAIT_BITSET ? (uint32_t)val3 : 0xFFFFFFFFu;
            return futex_wait(p, uaddr, (uint32_t)val, ms, bits);
        }
        case LX_FUTEX_WAKE:
            return futex_wake(p, uaddr, (int)val, 0xFFFFFFFFu);
        case LX_FUTEX_WAKE_BITSET:
            return futex_wake(p, uaddr, (int)val, (uint32_t)val3);
        case LX_FUTEX_REQUEUE:
            return futex_requeue(p, uaddr, (int)val, (int)timeout, uaddr2, 0, 0);
        case LX_FUTEX_CMP_REQUEUE:
            return futex_requeue(p, uaddr, (int)val, (int)timeout, uaddr2, 1, (uint32_t)val3);
        case LX_FUTEX_WAKE_OP:
            return futex_wake(p, uaddr, (int)val, 0xFFFFFFFFu) + futex_wake(p, uaddr2, (int)timeout, 0xFFFFFFFFu);
        default:
            return -LX_ENOSYS;
    }
}

static long sys_arch_prctl(uint64_t code, uint64_t addr) {
    struct task *t = current_task();
    switch (code) {
        case LX_ARCH_SET_FS:
            if (addr >= USER_LIMIT) return -LX_EPERM;
            t->fs_base = addr;
            wrmsr(MSR_IA32_FS_BASE, addr);
            return 0;
        case LX_ARCH_SET_GS:
            if (addr >= USER_LIMIT) return -LX_EPERM;
            t->gs_base = addr;
            wrmsr(MSR_IA32_GS_BASE, addr);
            return 0;
        case LX_ARCH_GET_FS:
            return copy_to_user(addr, &t->fs_base, sizeof(uint64_t));
        case LX_ARCH_GET_GS:
            return copy_to_user(addr, &t->gs_base, sizeof(uint64_t));
        default:
            return -LX_EINVAL;
    }
}

static long sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size) {
    struct process *p = proc_current();
    if (sig == 0 || sig >= LX_NSIG || size != 8) return -LX_EINVAL;
    if (oact) {
        uint64_t k[4] = { p->sig_handlers[sig], 0, 0, 0 };
        if (copy_to_user(oact, k, sizeof(k))) return -LX_EFAULT;
    }
    if (act) {
        uint64_t h;
        if (copy_from_user(&h, act, sizeof(h))) return -LX_EFAULT;
        if (sig == LX_SIGKILL || sig == 19) return -LX_EINVAL;
        p->sig_handlers[sig] = h;
    }
    return 0;
}

static long sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t size) {
    struct process *p = proc_current();
    if (size != 8) return -LX_EINVAL;
    if (oset && copy_to_user(oset, &p->sig_mask, 8)) return -LX_EFAULT;
    if (set) {
        uint64_t s;
        if (copy_from_user(&s, set, 8)) return -LX_EFAULT;
        if (how == 0) p->sig_mask |= s;
        else if (how == 1) p->sig_mask &= ~s;
        else if (how == 2) p->sig_mask = s;
        else return -LX_EINVAL;
    }
    return 0;
}

static long sys_kill_thread(int64_t tid, int64_t sig) {
    struct process *p = proc_current();
    if (sig == 0) return 0;
    if (sig < 0 || sig >= LX_NSIG) return -LX_EINVAL;
    (void)tid;
    uint64_t h = p->sig_handlers[sig];
    if (h == 1) return 0;
    if (sig == LX_SIGCHLD || sig == 23 || sig == 28) return 0;
    LOG_WARNING("pid %d (%s) terminated by signal %lld", p->pid, p->name, (long long)sig);
    proc_exit_group(PROC_EXIT_SIGNAL | (int)sig);
}

static long sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags) {
    (void)flags;
    uint8_t tmp[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t k = len - done > sizeof(tmp) ? sizeof(tmp) : len - done;
        vfs_random_bytes(tmp, k);
        if (copy_to_user(buf + done, tmp, k)) return done ? (long)done : -LX_EFAULT;
        done += k;
    }
    return (long)len;
}

static long sys_sysinfo(uint64_t buf) {
    struct lx_sysinfo si;
    memset(&si, 0, sizeof(si));
    si.uptime = (int64_t)(sys_monotonic_ns() / 1000000000ULL);
    si.totalram = pmm_total_pages() * PAGE_SIZE;
    si.freeram = pmm_free_page_count() * PAGE_SIZE;
    si.procs = (uint16_t)proc_count();
    si.mem_unit = 1;
    return copy_to_user(buf, &si, sizeof(si));
}

static long sys_prlimit(uint64_t pid, uint64_t res, uint64_t newl, uint64_t oldl) {
    (void)pid;
    (void)newl;
    if (oldl) {
        struct lx_rlimit l = { ~0ULL, ~0ULL };
        if (res == 3) l.rlim_cur = l.rlim_max = USER_STACK_SIZE;
        if (res == 7) l.rlim_cur = l.rlim_max = PROC_MAX_FDS;
        if (copy_to_user(oldl, &l, sizeof(l))) return -LX_EFAULT;
    }
    return 0;
}

static long sys_sched_getaffinity(uint64_t pid, uint64_t len, uint64_t mask) {
    (void)pid;
    if (len < 8) return -LX_EINVAL;
    uint8_t m[128];
    memset(m, 0, sizeof(m));
    int cpus = scheduler_core_count();
    for (int i = 0; i < cpus && i < 1024; i++) m[i / 8] |= (uint8_t)(1u << (i % 8));
    uint64_t n = len > sizeof(m) ? sizeof(m) : len;
    n = (n + 7) & ~7ULL;
    if (copy_to_user(mask, m, n)) return -LX_EFAULT;
    return (long)n;
}

static long sys_getrusage(uint64_t who, uint64_t buf) {
    (void)who;
    uint8_t ru[144];
    memset(ru, 0, sizeof(ru));
    struct task *t = current_task();
    uint64_t us = t ? t->run_ticks * 1000ULL : 0;
    ((int64_t *)ru)[0] = (int64_t)(us / 1000000ULL);
    ((int64_t *)ru)[1] = (int64_t)(us % 1000000ULL);
    return copy_to_user(buf, ru, sizeof(ru));
}


static long syscall_dispatch(struct registers *r) {
    uint64_t nr = r->rax;
    uint64_t a0 = r->rdi, a1 = r->rsi, a2 = r->rdx, a3 = r->r10, a4 = r->r8, a5 = r->r9;
    struct process *p = proc_current();
    struct task *t = current_task();

    switch (nr) {
        case 9: return sys_mmap(a0, a1, a2, a3, (int64_t)a4, a5);
        case 10: return mm_protect(&p->mm, a0, a1, (uint32_t)a2);
        case 11: return mm_unmap(&p->mm, a0, a1);
        case 12: return sys_brk(a0);
        case 13: return sys_rt_sigaction(a0, a1, a2, a3);
        case 14: return sys_rt_sigprocmask(a0, a1, a2, a3);
        case 15: return -LX_ENOSYS;
        case 24: scheduler_yield(); return 0;
        case 25: return -LX_ENOMEM;
        case 26: return mm_sync(&proc_current()->mm, a0, a1);
        case 27: return -LX_ENOSYS;
        case 28:
            if (a2 == LX_MADV_DONTNEED) return mm_dontneed(&p->mm, a0, a1);
            return 0;
        case 34: return do_sleep_ns(1000000000ULL * 3600);
        case 35: {
            struct lx_timespec ts;
            if (copy_from_user(&ts, a0, sizeof(ts))) return -LX_EFAULT;
            if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL || ts.tv_sec < 0) return -LX_EINVAL;
            return do_sleep_ns((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
        }
        case 39: return p->pid;
        case 56: return proc_clone_thread(r, a0, a1, a2, a3, a4);
        case 57: case 58: return -LX_ENOSYS;
        case 59: return -LX_ENOSYS;
        case 60: proc_thread_exit((int)(a0 & 0xFF));
        case 61: return -LX_ECHILD;
        case 62: return (int64_t)a0 == p->pid || (int64_t)a0 == 0 ? sys_kill_thread(t->tid, (int64_t)a1) : -LX_ESRCH;
        case 63: return sys_uname(a0);
        case 96: {
            uint64_t ns = sys_realtime_ns();
            struct lx_timeval tv = { (int64_t)(ns / 1000000000ULL), (int64_t)((ns % 1000000000ULL) / 1000ULL) };
            return a0 ? copy_to_user(a0, &tv, sizeof(tv)) : 0;
        }
        case 97: {
            struct lx_rlimit l = { ~0ULL, ~0ULL };
            if (a0 == 3) l.rlim_cur = l.rlim_max = USER_STACK_SIZE;
            if (a0 == 7) l.rlim_cur = l.rlim_max = PROC_MAX_FDS;
            return copy_to_user(a1, &l, sizeof(l));
        }
        case 98: return sys_getrusage(a0, a1);
        case 99: return sys_sysinfo(a0);
        case 102: case 104: case 107: case 108: return 0;
        case 105: case 106: case 113: case 114: case 117: case 119: return 0;
        case 109: case 112: return p->pid;
        case 110: return p->ppid;
        case 111: return p->pid;
        case 121: case 124: return p->pid;
        case 118: case 120: {
            uint32_t z[3] = { 0, 0, 0 };
            if (a0 && copy_to_user(a0, z, 4)) return -LX_EFAULT;
            if (a1 && copy_to_user(a1, z, 4)) return -LX_EFAULT;
            if (a2 && copy_to_user(a2, z, 4)) return -LX_EFAULT;
            return 0;
        }
        case 131: return 0;
        case 140: return 0;
        case 141: return 0;
        case 142: case 143: return 0;
        case 144: return 0;
        case 145: return 0;
        case 146: case 147: return 0;
        case 188: case 189: case 190: case 191: case 192: case 193:
        case 194: case 195: case 196: case 197: case 198: case 199: return -LX_EOPNOTSUPP;
        case 41: return -LX_EAFNOSUPPORT;
        case 303: case 304: return -LX_EOPNOTSUPP;
        case 221: return 0;
        case 157: return 0;
        case 158: return sys_arch_prctl(a0, a1);
        case 186: return t->tid;
        case 200: return sys_kill_thread((int64_t)a0, (int64_t)a1);
        case 201: {
            uint64_t s = sys_realtime_ns() / 1000000000ULL;
            if (a0 && copy_to_user(a0, &s, 8)) return -LX_EFAULT;
            return (long)s;
        }
        case 202: return sys_futex(a0, a1, a2, a3, a4, a5);
        case 203: return 0;
        case 204: return sys_sched_getaffinity(a0, a1, a2);
        case 218: t->clear_child_tid = a0; return t->tid;
        case 228: return sys_clock_gettime(a0, a1);
        case 229: {
            struct lx_timespec ts = { 0, 1 };
            return a1 ? copy_to_user(a1, &ts, sizeof(ts)) : 0;
        }
        case 230: return sys_clock_nanosleep(a0, a1, a2, a3);
        case 231: proc_exit_group((int)(a0 & 0xFF));
        case 234: return (int64_t)a0 == p->pid ? sys_kill_thread((int64_t)a1, (int64_t)a2) : -LX_ESRCH;
        case 273: return 0;
        case 274: return -LX_ENOSYS;
        case 302: return sys_prlimit(a0, a1, a2, a3);
        case 309: {
            uint32_t cpu = (uint32_t)current_core(), node = 0;
            if (a0 && copy_to_user(a0, &cpu, 4)) return -LX_EFAULT;
            if (a1 && copy_to_user(a1, &node, 4)) return -LX_EFAULT;
            return 0;
        }
        case 312: {
            if ((int64_t)a0 != p->pid || (int64_t)a1 != p->pid) return -LX_EPERM;
            if (a2 != 0) return -LX_EINVAL;
            struct ufile *f1 = fd_get(p, (int)a3);
            struct ufile *f2 = fd_get(p, (int)a4);
            int64_t r = (!f1 || !f2) ? -LX_EBADF : f1 == f2 ? 0 : (uintptr_t)f1 < (uintptr_t)f2 ? 1 : 2;
            if (f1) ufile_put(f1);
            if (f2) ufile_put(f2);
            return r;
        }
        case 318: return sys_getrandom(a0, a1, a2);
        case 324: return 0;
        case 334: return -LX_ENOSYS;
        case 435: return -LX_ENOSYS;
        default: break;
    }

    int handled = 0;
    long rv = sys_fs_dispatch(r, nr, a0, a1, a2, a3, a4, a5, &handled);
    if (handled) return rv;

    if (nr < sizeof(g_unknown_warned) && !g_unknown_warned[nr]) {
        g_unknown_warned[nr] = 1;
        LOG_WARNING("pid %d (%s): unimplemented syscall %llu (args 0x%llx 0x%llx 0x%llx)", p->pid, p->name,
                    (unsigned long long)nr, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2);
    }
    return -LX_ENOSYS;
}

void syscall_handler(struct registers *regs, void *fx) {
    (void)fx;
    asm volatile("sti");
    __atomic_fetch_add(&g_syscalls, 1, __ATOMIC_RELAXED);

    struct task *t = current_task();
    if (!t || !t->proc) {
        regs->rax = (uint64_t)-LX_ENOSYS;
        return;
    }
    uint64_t nr = regs->rax, a0 = regs->rdi, a1 = regs->rsi, a2 = regs->rdx;
    long rv = syscall_dispatch(regs);
    regs->rax = (uint64_t)rv;
    if (g_strace)
        LOG_INFO("[%u] syscall %llu(0x%llx, 0x%llx, 0x%llx) = %lld", t->tid, (unsigned long long)nr,
                 (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (long long)rv);
    proc_check_pending();
}
