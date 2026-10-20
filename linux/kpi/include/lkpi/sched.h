// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_SCHED_H
#define _LKPI_SCHED_H

#include <lkpi/sync.h>


#define TASK_RUNNING 0x00000000
#define TASK_INTERRUPTIBLE 0x00000001
#define TASK_UNINTERRUPTIBLE 0x00000002
#define __TASK_STOPPED 0x00000004
#define __TASK_TRACED 0x00000008
#define TASK_PARKED 0x00000040
#define TASK_DEAD 0x00000080
#define TASK_WAKEKILL 0x00000100
#define TASK_WAKING 0x00000200
#define TASK_NOLOAD 0x00000400
#define TASK_NEW 0x00000800
#define TASK_FREEZABLE 0x00002000
#define TASK_KILLABLE (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_IDLE (TASK_UNINTERRUPTIBLE | TASK_NOLOAD)
#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

#define PF_EXITING 0x00000004
#define SIGKILL 9
#define PF_KTHREAD 0x00200000
#define PF_MEMALLOC 0x00000800
#define PF_MEMALLOC_NOFS 0x00040000
#define PF_MEMALLOC_NOIO 0x00080000
#define PF_NOFREEZE 0x00008000
#define PF_WQ_WORKER 0x00000020

#define MAX_SCHEDULE_TIMEOUT LONG_MAX
#define MAX_RT_PRIO 100

struct mm_struct;
struct files_struct;
struct signal_struct { int dummy; };
struct sighand_struct { spinlock_t siglock; };

typedef int pid_t;
struct pid;

struct task_struct {
        volatile unsigned int __state;
        volatile int wake_flag;
        unsigned int flags;
        pid_t pid;
        pid_t tgid;
        char comm[TASK_COMM_LEN];
        struct mm_struct *mm;
        struct mm_struct *active_mm;
        void *luos_task;
        void *kthread;
        struct signal_struct *signal;
        struct sighand_struct *sighand;
        int prio;
        struct task_struct *group_leader;
        struct list_head lkpi_node;
        refcount_t usage;
        struct work_struct *lkpi_current_work;
        int exit_code;
        unsigned int uaccess_kds;
};

struct task_struct *lkpi_current(void);
#define current lkpi_current()
#define get_current() lkpi_current()

#define set_current_state(state_value) do { WRITE_ONCE(current->wake_flag, 0); smp_store_mb(current->__state, (state_value)); } while (0)
#define __set_current_state(state_value) set_current_state(state_value)
#define set_special_state(state_value) set_current_state(state_value)
#define get_current_state() READ_ONCE(current->__state)
#define task_is_running(task) (READ_ONCE((task)->__state) == TASK_RUNNING)

void schedule(void);
long schedule_timeout(long timeout);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_killable(long timeout);
long schedule_timeout_uninterruptible(long timeout);
long schedule_timeout_idle(long timeout);
#define io_schedule() schedule()
#define io_schedule_timeout(t) schedule_timeout(t)
void yield(void);
int wake_up_process(struct task_struct *tsk);
int wake_up_state(struct task_struct *tsk, unsigned int state);
int _cond_resched(void);
#define cond_resched() _cond_resched()
#define cond_resched_lock(lock) ({ spin_unlock(lock); cond_resched(); spin_lock(lock); 0; })
#define need_resched() 0
#define might_sleep() do { } while (0)
#define might_sleep_if(cond) do { } while (0)
#define cant_sleep() do { } while (0)
#define sched_annotate_sleep() do { } while (0)
int signal_pending(struct task_struct *p);
int fatal_signal_pending(struct task_struct *p);
static inline int signal_pending_state(unsigned int state, struct task_struct *p)
{
        if (!(state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL))) return 0;
        if (!signal_pending(p)) return 0;
        return (state & TASK_INTERRUPTIBLE) || fatal_signal_pending(p);
}
static inline void get_task_struct(struct task_struct *t) { refcount_inc(&t->usage); }
void put_task_struct(struct task_struct *t);
static inline pid_t task_pid_nr(struct task_struct *t) { return t->pid; }
static inline pid_t task_tgid_nr(struct task_struct *t) { return t->tgid; }
#define task_pid_vnr task_pid_nr
#define task_tgid_vnr task_tgid_nr
static inline struct pid *task_pid(struct task_struct *t) { return (struct pid *)(unsigned long)t->pid; }
static inline struct pid *task_tgid(struct task_struct *t) { return (struct pid *)(unsigned long)t->tgid; }
static inline struct pid *get_pid(struct pid *pid) { return pid; }
static inline void put_pid(struct pid *pid) { }
static inline pid_t pid_nr(struct pid *pid) { return (pid_t)(unsigned long)pid; }
static inline pid_t pid_vnr(struct pid *pid) { return pid_nr(pid); }
struct task_struct *pid_task(struct pid *pid, int type);
struct task_struct *get_pid_task(struct pid *pid, int type);
enum pid_type { PIDTYPE_PID, PIDTYPE_TGID, PIDTYPE_PGID, PIDTYPE_SID, PIDTYPE_MAX };
static inline const char *get_task_comm_ptr(struct task_struct *t) { return t->comm; }
#define get_task_comm(buf, tsk) strscpy(buf, (tsk)->comm, sizeof(buf))
#define __get_task_comm(buf, len, tsk) strscpy(buf, (tsk)->comm, len)
#define set_task_comm(tsk, from) strscpy((tsk)->comm, from, sizeof((tsk)->comm))
static inline bool same_thread_group(struct task_struct *a, struct task_struct *b) { return a->tgid == b->tgid; }
static inline bool is_idle_task(const struct task_struct *p) { return false; }
#define sched_set_fifo(t) do { } while (0)
#define sched_set_fifo_low(t) do { } while (0)
#define sched_set_normal(t, n) do { } while (0)
#define set_user_nice(t, n) do { } while (0)
#define task_nice(t) 0
#define capable(cap) true
#define ns_capable(ns, cap) true
#define perfmon_capable() true
#define CAP_SYS_ADMIN 21
#define CAP_SYS_NICE 23
#define CAP_PERFMON 38
static inline unsigned int memalloc_noreclaim_save(void) { return 0; }
static inline void memalloc_noreclaim_restore(unsigned int flags) { }
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int flags) { }
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int flags) { }
static inline gfp_t current_gfp_context(gfp_t flags) { return flags; }
#define freezing(t) false
#define try_to_freeze() false
#define set_freezable() do { } while (0)
#define freezable_schedule() schedule()
static inline bool kthread_freezable_should_stop(bool *was_frozen) { return false; }
#define send_sig(sig, p, priv) 0
#define force_sig(sig) do { } while (0)
#define rlimit(r) (~0UL)
#define RLIMIT_NOFILE 7
#define nr_threads 1
#define in_compat_syscall() false
#define is_compat_task() false
#define untagged_addr(addr) (addr)


unsigned long lkpi_jiffies(void);
#define jiffies lkpi_jiffies()
#define jiffies_64 ((u64)lkpi_jiffies())
static inline u64 get_jiffies_64(void) { return lkpi_jiffies(); }
#define INITIAL_JIFFIES 0UL
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define time_in_range(a, b, c) (time_after_eq(a, b) && time_before_eq(a, c))
#define time_after64 time_after
#define time_before64 time_before
#define time_after_eq64 time_after_eq
#define time_before_eq64 time_before_eq
#define time_is_before_jiffies(a) time_after(jiffies, a)
#define time_is_after_jiffies(a) time_before(jiffies, a)
#define time_is_before_eq_jiffies(a) time_after_eq(jiffies, a)
#define time_is_after_eq_jiffies(a) time_before_eq(jiffies, a)
static inline unsigned int jiffies_to_msecs(const unsigned long j) { return (unsigned int)j; }
static inline unsigned int jiffies_to_usecs(const unsigned long j) { return (unsigned int)(j * 1000); }
static inline u64 jiffies_to_nsecs(const unsigned long j) { return (u64)j * 1000000ULL; }
static inline unsigned long msecs_to_jiffies(const unsigned int m) { return m; }
static inline unsigned long usecs_to_jiffies(const unsigned int u) { return (u + 999) / 1000; }
static inline unsigned long nsecs_to_jiffies(u64 n) { return (unsigned long)((n + 999999) / 1000000); }
static inline u64 nsecs_to_jiffies64(u64 n) { return (n + 999999) / 1000000; }
static inline u64 jiffies64_to_nsecs(u64 j) { return j * 1000000ULL; }
static inline u64 jiffies64_to_msecs(u64 j) { return j; }
#define secs_to_jiffies(s) ((unsigned long)(s) * HZ)
#define round_jiffies_up(j) (j)
#define round_jiffies_up_relative(j) (j)
#define round_jiffies(j) (j)
#define round_jiffies_relative(j) (j)
#define jiffies_delta_to_msecs(d) jiffies_to_msecs(d)

#define NSEC_PER_USEC 1000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_SEC 1000000000L
#define USEC_PER_MSEC 1000L
#define USEC_PER_SEC 1000000L
#define MSEC_PER_SEC 1000L
#define FSEC_PER_SEC 1000000000000000LL
#define KTIME_MAX ((s64)~((u64)1 << 63))
#define KTIME_SEC_MAX (KTIME_MAX / NSEC_PER_SEC)

typedef s64 ktime_t;
struct timespec64 { time64_t tv_sec; long tv_nsec; };
struct timeval_lkpi { long tv_sec; long tv_usec; };
u64 lkpi_ktime_ns(void);
u64 lkpi_realtime_ns(void);
static inline ktime_t ktime_get(void) { return (ktime_t)lkpi_ktime_ns(); }
static inline ktime_t ktime_get_raw(void) { return ktime_get(); }
static inline ktime_t ktime_get_boottime(void) { return ktime_get(); }
static inline ktime_t ktime_get_real(void) { return (ktime_t)lkpi_realtime_ns(); }
static inline ktime_t ktime_get_coarse(void) { return ktime_get(); }
static inline u64 ktime_get_ns(void) { return lkpi_ktime_ns(); }
static inline u64 ktime_get_raw_ns(void) { return lkpi_ktime_ns(); }
static inline u64 ktime_get_mono_fast_ns(void) { return lkpi_ktime_ns(); }
static inline u64 ktime_get_boottime_ns(void) { return lkpi_ktime_ns(); }
static inline u64 ktime_get_real_ns(void) { return lkpi_realtime_ns(); }
static inline u64 ktime_get_coarse_ns(void) { return lkpi_ktime_ns(); }
static inline u64 local_clock(void) { return lkpi_ktime_ns(); }
static inline u64 sched_clock(void) { return lkpi_ktime_ns(); }
static inline time64_t ktime_get_seconds(void) { return lkpi_ktime_ns() / NSEC_PER_SEC; }
static inline time64_t ktime_get_real_seconds(void) { return lkpi_realtime_ns() / NSEC_PER_SEC; }
static inline time64_t ktime_get_boottime_seconds(void) { return ktime_get_seconds(); }
static inline ktime_t ns_to_ktime(u64 ns) { return (ktime_t)ns; }
static inline ktime_t ms_to_ktime(u64 ms) { return (ktime_t)(ms * NSEC_PER_MSEC); }
static inline ktime_t us_to_ktime(u64 us) { return (ktime_t)(us * NSEC_PER_USEC); }
static inline s64 ktime_to_ns(const ktime_t kt) { return kt; }
static inline s64 ktime_to_us(const ktime_t kt) { return kt / NSEC_PER_USEC; }
static inline s64 ktime_to_ms(const ktime_t kt) { return kt / NSEC_PER_MSEC; }
static inline s64 ktime_us_delta(const ktime_t later, const ktime_t earlier) { return (later - earlier) / NSEC_PER_USEC; }
static inline s64 ktime_ms_delta(const ktime_t later, const ktime_t earlier) { return (later - earlier) / NSEC_PER_MSEC; }
#define ktime_add(a, b) ((a) + (b))
#define ktime_sub(a, b) ((a) - (b))
#define ktime_add_ns(kt, nsval) ((kt) + (nsval))
#define ktime_sub_ns(kt, nsval) ((kt) - (nsval))
#define ktime_add_us(kt, usec) ((kt) + (s64)(usec) * NSEC_PER_USEC)
#define ktime_sub_us(kt, usec) ((kt) - (s64)(usec) * NSEC_PER_USEC)
#define ktime_add_ms(kt, msec) ((kt) + (s64)(msec) * NSEC_PER_MSEC)
#define ktime_set(secs, nsecs) ((ktime_t)(secs) * NSEC_PER_SEC + (nsecs))
#define ktime_compare(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define ktime_after(a, b) ((a) > (b))
#define ktime_before(a, b) ((a) < (b))
#define ktime_divns(kt, div) ((kt) / (div))
#define ktime_to_timespec64(kt) ns_to_timespec64(kt)
#define ktime_mono_to_real(kt) ((kt) + (ktime_get_real() - ktime_get()))
static inline struct timespec64 ns_to_timespec64(s64 nsec)
{
        struct timespec64 ts = { nsec / NSEC_PER_SEC, nsec % NSEC_PER_SEC };
        if (ts.tv_nsec < 0) { ts.tv_sec--; ts.tv_nsec += NSEC_PER_SEC; }
        return ts;
}
static inline s64 timespec64_to_ns(const struct timespec64 *ts) { return ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec; }
static inline ktime_t timespec64_to_ktime(struct timespec64 ts) { return ktime_set(ts.tv_sec, ts.tv_nsec); }
static inline void ktime_get_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(ktime_get()); }
static inline void ktime_get_real_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(ktime_get_real()); }
static inline void ktime_get_raw_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(ktime_get()); }
static inline struct timespec64 timespec64_sub(struct timespec64 a, struct timespec64 b)
{
        return ns_to_timespec64(timespec64_to_ns(&a) - timespec64_to_ns(&b));
}
#define ktime_to_timespec64_cond(kt, ts) ({ *(ts) = ns_to_timespec64(kt); true; })
static inline u32 ktime_to_ms32(ktime_t kt) { return (u32)(kt / NSEC_PER_MSEC); }

void msleep(unsigned int msecs);
unsigned long msleep_interruptible(unsigned int msecs);
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state);
#define usleep_range(min, max) usleep_range_state(min, max, TASK_UNINTERRUPTIBLE)
#define usleep_range_idle(min, max) usleep_range_state(min, max, TASK_IDLE)
void fsleep(unsigned long usecs);
void lkpi_udelay(unsigned long usecs);
#define udelay(n) lkpi_udelay(n)
#define ndelay(n) lkpi_udelay(((n) + 999) / 1000)
#define mdelay(n) lkpi_udelay((unsigned long)(n) * 1000)
static inline void ssleep(unsigned int seconds) { msleep(seconds * 1000); }


struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN 0x02
#define WQ_FLAG_PRIORITY 0x10

struct wait_queue_entry {
        unsigned int flags;
        void *private;
        wait_queue_func_t func;
        struct list_head entry;
};
typedef struct wait_queue_entry wait_queue_entry_t;

struct wait_queue_head {
        spinlock_t lock;
        struct list_head head;
};
typedef struct wait_queue_head wait_queue_head_t;

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key);
int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key);

#define __WAITQUEUE_INITIALIZER(name, tsk) { .private = tsk, .func = default_wake_function, .entry = { NULL, NULL } }
#define DECLARE_WAITQUEUE(name, tsk) struct wait_queue_entry name = __WAITQUEUE_INITIALIZER(name, tsk)
#define __WAIT_QUEUE_HEAD_INITIALIZER(name) { .lock = __SPIN_LOCK_UNLOCKED(name.lock), .head = LIST_HEAD_INIT(name.head) }
#define DECLARE_WAIT_QUEUE_HEAD(name) struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)
#define DECLARE_WAIT_QUEUE_HEAD_ONSTACK(name) DECLARE_WAIT_QUEUE_HEAD(name)
void __init_waitqueue_head(struct wait_queue_head *wq_head, const char *name, struct lock_class_key *key);
#define init_waitqueue_head(wq_head) __init_waitqueue_head((wq_head), #wq_head, NULL)
static inline void init_waitqueue_entry(struct wait_queue_entry *wq_entry, struct task_struct *p)
{
        wq_entry->flags = 0;
        wq_entry->private = p;
        wq_entry->func = default_wake_function;
}
static inline void init_waitqueue_func_entry(struct wait_queue_entry *wq_entry, wait_queue_func_t func)
{
        wq_entry->flags = 0;
        wq_entry->private = NULL;
        wq_entry->func = func;
}
#define DEFINE_WAIT_FUNC(name, function) struct wait_queue_entry name = { \
        .private = current, .func = function, .entry = LIST_HEAD_INIT((name).entry) }
#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, autoremove_wake_function)
#define init_wait(wait) do { (wait)->private = current; (wait)->func = autoremove_wake_function; \
        INIT_LIST_HEAD(&(wait)->entry); (wait)->flags = 0; } while (0)
static inline int waitqueue_active(struct wait_queue_head *wq_head) { return !list_empty(&wq_head->head); }
static inline bool wq_has_sleeper(struct wait_queue_head *wq_head) { smp_mb(); return waitqueue_active(wq_head); }
static inline void __add_wait_queue(struct wait_queue_head *h, struct wait_queue_entry *e) { list_add(&e->entry, &h->head); }
static inline void __add_wait_queue_entry_tail(struct wait_queue_head *h, struct wait_queue_entry *e) { list_add_tail(&e->entry, &h->head); }
static inline void __remove_wait_queue(struct wait_queue_head *h, struct wait_queue_entry *e) { list_del(&e->entry); }
void add_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry);
void add_wait_queue_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry);
void remove_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry);
void prepare_to_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state);
bool prepare_to_wait_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state);
long prepare_to_wait_event(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state);
void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry);
long wait_woken(struct wait_queue_entry *wq_entry, unsigned mode, long timeout);
void __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr, void *key);
void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr);
void __wake_up_locked_key(struct wait_queue_head *wq_head, unsigned int mode, void *key);
#define wake_up(x) __wake_up(x, TASK_NORMAL, 1, NULL)
#define wake_up_nr(x, nr) __wake_up(x, TASK_NORMAL, nr, NULL)
#define wake_up_all(x) __wake_up(x, TASK_NORMAL, 0, NULL)
#define wake_up_locked(x) __wake_up_locked((x), TASK_NORMAL, 1)
#define wake_up_all_locked(x) __wake_up_locked((x), TASK_NORMAL, 0)
#define wake_up_locked_poll(x, m) __wake_up_locked_key((x), TASK_NORMAL, (void *)(unsigned long)(m))
#define wake_up_interruptible(x) __wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_interruptible_nr(x, nr) __wake_up(x, TASK_INTERRUPTIBLE, nr, NULL)
#define wake_up_interruptible_all(x) __wake_up(x, TASK_INTERRUPTIBLE, 0, NULL)
#define wake_up_interruptible_sync(x) __wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_poll(x, m) __wake_up(x, TASK_NORMAL, 1, (void *)(unsigned long)(m))
#define wake_up_interruptible_poll(x, m) __wake_up(x, TASK_INTERRUPTIBLE, 1, (void *)(unsigned long)(m))
#define wake_up_interruptible_sync_poll(x, m) __wake_up(x, TASK_INTERRUPTIBLE, 1, (void *)(unsigned long)(m))
#define wake_up_all_poll(x, m) __wake_up(x, TASK_NORMAL, 0, (void *)(unsigned long)(m))

#define ___wait_is_interruptible(state) \
        (!__builtin_constant_p(state) || (state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)))
#define ___wait_event(wq_head, condition, state, exclusive, ret, cmd) ({ \
        __label__ __out; \
        struct wait_queue_entry __wq_entry; \
        long __ret = ret; \
        init_wait(&__wq_entry); \
        if (exclusive) __wq_entry.flags = WQ_FLAG_EXCLUSIVE; \
        for (;;) { \
                long __int = prepare_to_wait_event(&wq_head, &__wq_entry, state); \
                if (condition) break; \
                if (___wait_is_interruptible(state) && __int) { __ret = __int; goto __out; } \
                cmd; \
        } \
        finish_wait(&wq_head, &__wq_entry); \
__out:  __ret; })
#define ___wait_cond_timeout(condition) ({ bool __cond = (condition); if (__cond && !__ret) __ret = 1; __cond || !__ret; })
#define wait_event(wq_head, condition) do { if (condition) break; \
        (void)___wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, 0, schedule()); } while (0)
#define wait_event_idle(wq_head, condition) wait_event(wq_head, condition)
#define wait_event_freezable(wq_head, condition) wait_event_interruptible(wq_head, condition)
#define wait_event_interruptible(wq_head, condition) ({ int __ret = 0; if (!(condition)) { \
        __ret = ___wait_event(wq_head, condition, TASK_INTERRUPTIBLE, 0, 0, schedule()); } __ret; })
#define wait_event_killable(wq_head, condition) ({ int __ret = 0; if (!(condition)) { \
        __ret = ___wait_event(wq_head, condition, TASK_KILLABLE, 0, 0, schedule()); } __ret; })
#define wait_event_timeout(wq_head, condition, timeout) ({ long __ret = timeout; \
        if (!___wait_cond_timeout(condition)) \
                __ret = ___wait_event(wq_head, ___wait_cond_timeout(condition), TASK_UNINTERRUPTIBLE, 0, timeout, \
                                      __ret = schedule_timeout(__ret)); \
        __ret; })
#define wait_event_idle_timeout wait_event_timeout
#define wait_event_interruptible_timeout(wq_head, condition, timeout) ({ long __ret = timeout; \
        if (!___wait_cond_timeout(condition)) \
                __ret = ___wait_event(wq_head, ___wait_cond_timeout(condition), TASK_INTERRUPTIBLE, 0, timeout, \
                                      __ret = schedule_timeout(__ret)); \
        __ret; })
#define wait_event_killable_timeout(wq_head, condition, timeout) ({ long __ret = timeout; \
        if (!___wait_cond_timeout(condition)) \
                __ret = ___wait_event(wq_head, ___wait_cond_timeout(condition), TASK_KILLABLE, 0, timeout, \
                                      __ret = schedule_timeout(__ret)); \
        __ret; })
#define wait_event_interruptible_exclusive(wq, condition) ({ int __ret = 0; if (!(condition)) { \
        __ret = ___wait_event(wq, condition, TASK_INTERRUPTIBLE, 1, 0, schedule()); } __ret; })
#define wait_event_lock_irq(wq_head, condition, lock) do { if (condition) break; \
        (void)___wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, 0, \
                            spin_unlock_irq(&lock); schedule(); spin_lock_irq(&lock)); } while (0)
#define wait_event_interruptible_lock_irq(wq_head, condition, lock) ({ int __ret = 0; if (!(condition)) { \
        __ret = ___wait_event(wq_head, condition, TASK_INTERRUPTIBLE, 0, 0, \
                              spin_unlock_irq(&lock); schedule(); spin_lock_irq(&lock)); } __ret; })
#define wait_event_interruptible_locked(wq, condition) ({ int __ret = 0; if (!(condition)) { \
        __ret = ___wait_event(wq, condition, TASK_INTERRUPTIBLE, 0, 0, \
                              spin_unlock(&(wq).lock); schedule(); spin_lock(&(wq).lock)); } __ret; })

#include <linux/wait_bit.h>


struct timer_list {
        struct list_head entry;
        unsigned long expires;
        void (*function)(struct timer_list *);
        u32 flags;
        bool pending;
};
#define TIMER_DEFERRABLE 0x00080000
#define TIMER_PINNED 0x00100000
#define TIMER_IRQSAFE 0x00200000
#define __TIMER_INITIALIZER(_function, _flags) { .entry = { NULL, NULL }, .function = (_function), .flags = (_flags) }
#define DEFINE_TIMER(_name, _function) struct timer_list _name = __TIMER_INITIALIZER(_function, 0)
void timer_setup(struct timer_list *timer, void (*func)(struct timer_list *), unsigned int flags);
#define timer_setup_on_stack timer_setup
#define destroy_timer_on_stack(t) do { } while (0)
int mod_timer(struct timer_list *timer, unsigned long expires);
void add_timer(struct timer_list *timer);
int timer_delete(struct timer_list *timer);
int timer_delete_sync(struct timer_list *timer);
int timer_shutdown_sync(struct timer_list *timer);
#define del_timer(t) timer_delete(t)
#define del_timer_sync(t) timer_delete_sync(t)
#define try_to_del_timer_sync(t) timer_delete(t)
static inline int timer_pending(const struct timer_list *timer) { return READ_ONCE(timer->pending); }
#define from_timer(var, callback_timer, timer_fieldname) container_of(callback_timer, typeof(*var), timer_fieldname)
#define timer_container_of(var, callback_timer, timer_fieldname) container_of(callback_timer, typeof(*var), timer_fieldname)

enum hrtimer_mode {
        HRTIMER_MODE_ABS = 0x00,
        HRTIMER_MODE_REL = 0x01,
        HRTIMER_MODE_PINNED = 0x02,
        HRTIMER_MODE_SOFT = 0x04,
        HRTIMER_MODE_HARD = 0x08,
        HRTIMER_MODE_ABS_PINNED = 0x02,
        HRTIMER_MODE_REL_PINNED = 0x03,
        HRTIMER_MODE_ABS_SOFT = 0x04,
        HRTIMER_MODE_REL_SOFT = 0x05,
        HRTIMER_MODE_ABS_HARD = 0x08,
        HRTIMER_MODE_REL_HARD = 0x09,
};
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
struct timerqueue_node_lkpi {
        ktime_t expires;
};
struct hrtimer {
        struct list_head entry;
        union {
                ktime_t expires;
                struct timerqueue_node_lkpi node;
        };
        enum hrtimer_restart (*function)(struct hrtimer *);
        bool pending;
        bool running;
};
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7
void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*function)(struct hrtimer *), clockid_t clock_id,
                   enum hrtimer_mode mode);
#define hrtimer_init(timer, clock, mode) hrtimer_setup(timer, NULL, clock, mode)
void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 range_ns, const enum hrtimer_mode mode);
static inline void hrtimer_start(struct hrtimer *timer, ktime_t tim, const enum hrtimer_mode mode)
{
        hrtimer_start_range_ns(timer, tim, 0, mode);
}
int hrtimer_cancel(struct hrtimer *timer);
int hrtimer_try_to_cancel(struct hrtimer *timer);
static inline bool hrtimer_active(const struct hrtimer *timer) { return READ_ONCE(timer->pending) || READ_ONCE(timer->running); }
static inline bool hrtimer_is_queued(const struct hrtimer *timer) { return READ_ONCE(timer->pending); }
static inline bool hrtimer_callback_running(const struct hrtimer *timer) { return READ_ONCE(timer->running); }
static inline void hrtimer_set_expires(struct hrtimer *timer, ktime_t time) { timer->expires = time; }
static inline void hrtimer_set_expires_ns(struct hrtimer *timer, s64 ns) { timer->expires = ns; }
static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer) { return timer->expires; }
static inline s64 hrtimer_get_expires_ns(const struct hrtimer *timer) { return timer->expires; }
static inline ktime_t hrtimer_expires_remaining(const struct hrtimer *timer) { return timer->expires - ktime_get(); }
static inline ktime_t hrtimer_get_remaining(const struct hrtimer *timer) { return hrtimer_expires_remaining(timer); }
static inline void hrtimer_add_expires_ns(struct hrtimer *timer, u64 ns) { timer->expires += ns; }
static inline void hrtimer_add_expires(struct hrtimer *timer, ktime_t t) { timer->expires += t; }
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);
static inline u64 hrtimer_forward_now(struct hrtimer *timer, ktime_t interval) { return hrtimer_forward(timer, ktime_get(), interval); }
static inline ktime_t hrtimer_cb_get_time(const struct hrtimer *timer) { return ktime_get(); }
static inline void hrtimer_restart(struct hrtimer *timer) { hrtimer_start(timer, timer->expires, HRTIMER_MODE_ABS); }
int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode);
int schedule_hrtimeout_range(ktime_t *expires, u64 delta, const enum hrtimer_mode mode);


struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct workqueue_struct;

struct work_struct {
        atomic_long_t data;
        struct list_head entry;
        work_func_t func;
        struct workqueue_struct *wq;
};
#define WORK_STRUCT_PENDING_BIT 0
#define WORK_DATA_INIT() ATOMIC_LONG_INIT(0)
#define __WORK_INITIALIZER(n, f) { .data = WORK_DATA_INIT(), .entry = { &(n).entry, &(n).entry }, .func = (f) }
#define DECLARE_WORK(n, f) struct work_struct n = __WORK_INITIALIZER(n, f)
#define INIT_WORK(_work, _func) do { atomic_long_set(&(_work)->data, 0); INIT_LIST_HEAD(&(_work)->entry); \
        (_work)->func = (_func); (_work)->wq = NULL; } while (0)
#define INIT_WORK_ONSTACK INIT_WORK
#define destroy_work_on_stack(w) do { } while (0)
struct delayed_work {
        struct work_struct work;
        struct timer_list timer;
        struct workqueue_struct *wq;
        int cpu;
};
void lkpi_delayed_work_timer_fn(struct timer_list *t);
#define __DELAYED_WORK_INITIALIZER(n, f, tflags) { .work = __WORK_INITIALIZER((n).work, (f)), \
        .timer = __TIMER_INITIALIZER(lkpi_delayed_work_timer_fn, (tflags)) }
#define DECLARE_DELAYED_WORK(n, f) struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f, 0)
#define DECLARE_DEFERRABLE_WORK(n, f) DECLARE_DELAYED_WORK(n, f)
#define INIT_DELAYED_WORK(_work, _func) do { INIT_WORK(&(_work)->work, (_func)); \
        timer_setup(&(_work)->timer, lkpi_delayed_work_timer_fn, 0); } while (0)
#define INIT_DEFERRABLE_WORK INIT_DELAYED_WORK
#define INIT_DELAYED_WORK_ONSTACK INIT_DELAYED_WORK
#define destroy_delayed_work_on_stack(w) do { } while (0)
struct rcu_work { struct work_struct work; struct callback_head rcu; struct workqueue_struct *wq; };
static inline struct delayed_work *to_delayed_work(struct work_struct *work) { return container_of(work, struct delayed_work, work); }
#define work_pending(work) test_bit(WORK_STRUCT_PENDING_BIT, (unsigned long *)&(work)->data.counter)
#define delayed_work_pending(w) work_pending(&(w)->work)

enum {
        WQ_UNBOUND = 1 << 1,
        WQ_FREEZABLE = 1 << 2,
        WQ_MEM_RECLAIM = 1 << 3,
        WQ_HIGHPRI = 1 << 4,
        WQ_CPU_INTENSIVE = 1 << 5,
        WQ_SYSFS = 1 << 6,
        WQ_POWER_EFFICIENT = 1 << 7,
        WQ_PERCPU = 1 << 8,
        __WQ_ORDERED = 1 << 17,
        WQ_MAX_ACTIVE = 512,
        WQ_DFL_ACTIVE = WQ_MAX_ACTIVE / 2,
};
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
extern struct workqueue_struct *system_freezable_wq;
extern struct workqueue_struct *system_power_efficient_wq;
extern struct workqueue_struct *system_percpu_wq;
extern struct workqueue_struct *system_dfl_wq;
__printf(1, 4) struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, args...) alloc_workqueue(fmt, __WQ_ORDERED | (flags), 1, ##args)
#define create_workqueue(name) alloc_workqueue("%s", WQ_MEM_RECLAIM, 1, (name))
#define create_singlethread_workqueue(name) alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM, name)
#define create_freezable_workqueue(name) alloc_workqueue("%s", WQ_FREEZABLE | WQ_UNBOUND | WQ_MEM_RECLAIM, 1, (name))
void destroy_workqueue(struct workqueue_struct *wq);
bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *work, unsigned long delay);
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);
void flush_workqueue(struct workqueue_struct *wq);
void drain_workqueue(struct workqueue_struct *wq);
bool flush_work(struct work_struct *work);
bool cancel_work(struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
bool flush_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
bool disable_work_sync(struct work_struct *work);
bool disable_delayed_work_sync(struct delayed_work *dwork);
bool enable_work(struct work_struct *work);
bool current_is_workqueue_rescuer(void);
struct work_struct *current_work(void);
bool workqueue_congested(int cpu, struct workqueue_struct *wq);
#define WORK_CPU_UNBOUND NR_CPUS
#define queue_work(wq, work) queue_work_on(WORK_CPU_UNBOUND, wq, work)
#define queue_delayed_work(wq, dwork, delay) queue_delayed_work_on(WORK_CPU_UNBOUND, wq, dwork, delay)
#define mod_delayed_work(wq, dwork, delay) mod_delayed_work_on(WORK_CPU_UNBOUND, wq, dwork, delay)
#define schedule_work(work) queue_work(system_wq, work)
#define schedule_work_on(cpu, work) queue_work(system_wq, work)
#define schedule_delayed_work(dwork, delay) queue_delayed_work(system_wq, dwork, delay)
#define schedule_delayed_work_on(cpu, dwork, delay) queue_delayed_work(system_wq, dwork, delay)
#define flush_scheduled_work() flush_workqueue(system_wq)
#define queue_rcu_work(wq, rwork) ({ synchronize_rcu(); queue_work(wq, &(rwork)->work); })
#define INIT_RCU_WORK(_work, _func) INIT_WORK(&(_work)->work, (_func))
#define to_rcu_work(w) container_of(w, struct rcu_work, work)
#define workqueue_set_max_active(wq, n) do { } while (0)
#define set_workqueue_lockdep_map(wq, m) do { } while (0)


__printf(4, 5) struct task_struct *kthread_create_on_node(int (*threadfn)(void *data), void *data, int node,
                                                        const char namefmt[], ...);
#define kthread_create(threadfn, data, namefmt, arg...) kthread_create_on_node(threadfn, data, NUMA_NO_NODE, namefmt, ##arg)
#define kthread_run(threadfn, data, namefmt, ...) ({ \
        struct task_struct *__k = kthread_create(threadfn, data, namefmt, ##__VA_ARGS__); \
        if (!IS_ERR(__k)) { wake_up_process(__k); } __k; })
int kthread_stop(struct task_struct *k);
int kthread_stop_put(struct task_struct *k);
bool kthread_should_stop(void);
bool kthread_should_park(void);
int kthread_park(struct task_struct *k);
void kthread_unpark(struct task_struct *k);
void kthread_parkme(void);
void kthread_bind(struct task_struct *k, unsigned int cpu);
void *kthread_data(struct task_struct *k);
#define kthread_set_per_cpu(k, cpu) do { } while (0)

struct kthread_work;
typedef void (*kthread_work_func_t)(struct kthread_work *work);
struct kthread_worker {
        unsigned int flags;
        raw_spinlock_t lock;
        struct list_head work_list;
        struct list_head delayed_work_list;
        struct task_struct *task;
        struct kthread_work *current_work;
        wait_queue_head_t wait;
};
struct kthread_work {
        struct list_head node;
        kthread_work_func_t func;
        struct kthread_worker *worker;
        int canceling;
};
struct kthread_delayed_work {
        struct kthread_work work;
        struct timer_list timer;
};
#define KTHREAD_WORK_INIT(work, fn) { .node = LIST_HEAD_INIT((work).node), .func = (fn) }
#define DEFINE_KTHREAD_WORK(work, fn) struct kthread_work work = KTHREAD_WORK_INIT(work, fn)
#define kthread_init_work(work, fn) do { memset((work), 0, sizeof(struct kthread_work)); \
        INIT_LIST_HEAD(&(work)->node); (work)->func = (fn); } while (0)
void lkpi_kthread_delayed_work_timer_fn(struct timer_list *t);
#define kthread_init_delayed_work(dwork, fn) do { kthread_init_work(&(dwork)->work, (fn)); \
        timer_setup(&(dwork)->timer, lkpi_kthread_delayed_work_timer_fn, 0); } while (0)
__printf(3, 4) struct kthread_worker *kthread_create_worker_on_node(unsigned int flags, int node, const char namefmt[], ...);
#define kthread_create_worker(flags, namefmt, ...) kthread_create_worker_on_node(flags, NUMA_NO_NODE, namefmt, ##__VA_ARGS__)
#define kthread_run_worker(flags, namefmt, ...) kthread_create_worker(flags, namefmt, ##__VA_ARGS__)
void kthread_destroy_worker(struct kthread_worker *worker);
bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work);
bool kthread_queue_delayed_work(struct kthread_worker *worker, struct kthread_delayed_work *dwork, unsigned long delay);
bool kthread_mod_delayed_work(struct kthread_worker *worker, struct kthread_delayed_work *dwork, unsigned long delay);
void kthread_flush_work(struct kthread_work *work);
void kthread_flush_worker(struct kthread_worker *worker);
bool kthread_cancel_work_sync(struct kthread_work *work);
bool kthread_cancel_delayed_work_sync(struct kthread_delayed_work *work);
static inline bool kthread_work_pending(const struct kthread_work *work) { return !list_empty(&work->node); }

#endif
