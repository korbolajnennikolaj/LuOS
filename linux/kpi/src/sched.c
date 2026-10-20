// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"


static struct signal_struct lkpi_signal;
static struct sighand_struct lkpi_sighand = { .siglock = __SPIN_LOCK_UNLOCKED(lkpi_sighand.siglock) };
static struct task_struct lkpi_boot_task = {
        .comm = "boot",
        .signal = &lkpi_signal,
        .sighand = &lkpi_sighand,
        .usage = REFCOUNT_INIT(1),
};

static void task_struct_init(struct task_struct *ts, void *lt)
{
        ts->luos_task = lt;
        ts->pid = lt ? luos_task_tid(lt) : 0;
        ts->tgid = ts->pid;
        strscpy(ts->comm, lt ? luos_task_name(lt) : "?", sizeof(ts->comm));
        ts->signal = &lkpi_signal;
        ts->sighand = &lkpi_sighand;
        ts->group_leader = ts;
        ts->prio = 120;
        INIT_LIST_HEAD(&ts->lkpi_node);
        refcount_set(&ts->usage, 1);
        if (lt && !luos_task_is_user(lt)) ts->flags |= PF_KTHREAD;
}

struct task_struct *lkpi_current(void)
{
        void *lt = luos_current_task();
        if (unlikely(!lt)) return &lkpi_boot_task;
        void **slot = luos_task_lx_slot(lt);
        struct task_struct *ts = *slot;
        if (likely(ts)) return ts;
        ts = luos_heap_alloc(sizeof(*ts), 16);
        if (!ts) luos_panic("LinuxKPI: out of memory for task_struct");
        memset(ts, 0, sizeof(*ts));
        task_struct_init(ts, lt);
        *slot = ts;
        return ts;
}

void put_task_struct(struct task_struct *t)
{
        if (refcount_dec_and_test(&t->usage) && t != &lkpi_boot_task) luos_heap_free(t);
}

struct task_struct *pid_task(struct pid *pid, int type)
{
        struct task_struct *cur = current;
        pid_t nr = pid_nr(pid);
        if (nr == cur->pid || nr == cur->tgid) return cur;
        return NULL;
}

struct task_struct *get_pid_task(struct pid *pid, int type)
{
        struct task_struct *t = pid_task(pid, type);
        if (t) get_task_struct(t);
        return t;
}

int signal_pending(struct task_struct *p)
{
        return p && p->luos_task ? luos_task_kill_pending(p->luos_task) : 0;
}

int fatal_signal_pending(struct task_struct *p) { return signal_pending(p); }

int wake_up_state(struct task_struct *p, unsigned int state)
{
        if (!(READ_ONCE(p->__state) & state)) return 0;
        WRITE_ONCE(p->wake_flag, 1);
        smp_mb();
        WRITE_ONCE(p->__state, TASK_RUNNING);
        luos_wake_all(p);
        return 1;
}

int wake_up_process(struct task_struct *p) { return wake_up_state(p, TASK_NORMAL); }

static void lkpi_block(struct task_struct *ts, unsigned long ms)
{
        if (luos_wait(ts, &ts->wake_flag, ms) < 0 && !READ_ONCE(ts->wake_flag) && luos_task_kill_pending(ts->luos_task) &&
            !(READ_ONCE(ts->__state) & TASK_INTERRUPTIBLE))
                luos_yield();
}

void schedule(void)
{
        struct task_struct *ts = current;
        if (READ_ONCE(ts->__state) == TASK_RUNNING) {
                luos_yield();
                return;
        }
        lkpi_block(ts, 0);
        WRITE_ONCE(ts->__state, TASK_RUNNING);
}

long schedule_timeout(long timeout)
{
        struct task_struct *ts = current;
        if (timeout == MAX_SCHEDULE_TIMEOUT) {
                schedule();
                return timeout;
        }
        if (timeout <= 0) {
                __set_current_state(TASK_RUNNING);
                return 0;
        }
        unsigned long expire = jiffies + timeout;
        if (READ_ONCE(ts->__state) != TASK_RUNNING) lkpi_block(ts, timeout);
        else luos_yield();
        WRITE_ONCE(ts->__state, TASK_RUNNING);
        long left = (long)(expire - jiffies);
        return left < 0 ? 0 : left;
}

long schedule_timeout_interruptible(long timeout)
{
        __set_current_state(TASK_INTERRUPTIBLE);
        return schedule_timeout(timeout);
}

long schedule_timeout_killable(long timeout)
{
        __set_current_state(TASK_KILLABLE);
        return schedule_timeout(timeout);
}

long schedule_timeout_uninterruptible(long timeout)
{
        __set_current_state(TASK_UNINTERRUPTIBLE);
        return schedule_timeout(timeout);
}

long schedule_timeout_idle(long timeout)
{
        __set_current_state(TASK_IDLE);
        return schedule_timeout(timeout);
}

int schedule_hrtimeout_range(ktime_t *expires, u64 delta, const enum hrtimer_mode mode)
{
        if (!expires) {
                schedule();
                return -EINTR;
        }
        s64 ns = *expires;
        if (!(mode & HRTIMER_MODE_REL)) ns -= ktime_get();
        if (ns <= 0) {
                __set_current_state(TASK_RUNNING);
                return 0;
        }
        if (ns <= NSEC_PER_MSEC / 2) {
                __set_current_state(TASK_RUNNING);
                luos_udelay((unsigned long)ns / 1000 + 1);
                return 0;
        }
        return schedule_timeout(nsecs_to_jiffies(ns)) ? -EINTR : 0;
}

int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode) { return schedule_hrtimeout_range(expires, 0, mode); }

void yield(void) { luos_yield(); }
int _cond_resched(void) { return 0; }


unsigned long lkpi_jiffies(void) { return (unsigned long)(luos_monotonic_ns() / NSEC_PER_MSEC); }
u64 lkpi_ktime_ns(void) { return luos_monotonic_ns(); }
u64 lkpi_realtime_ns(void) { return luos_realtime_ns(); }
void lkpi_udelay(unsigned long usecs) { luos_udelay(usecs); }

void msleep(unsigned int msecs)
{
        u64 end = luos_monotonic_ns() + (u64)msecs * NSEC_PER_MSEC;
        for (;;) {
                u64 now = luos_monotonic_ns();
                if (now >= end) break;
                u64 left = (end - now + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
                luos_sleep_ms(left);
                if (luos_monotonic_ns() < end && luos_task_kill_pending(luos_current_task())) luos_yield();
        }
}

unsigned long msleep_interruptible(unsigned int msecs)
{
        u64 end = luos_monotonic_ns() + (u64)msecs * NSEC_PER_MSEC;
        luos_sleep_ms(msecs);
        u64 now = luos_monotonic_ns();
        return now >= end ? 0 : (unsigned long)((end - now) / NSEC_PER_MSEC);
}

void usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
        if (min < 1000) luos_udelay(min);
        else msleep((min + 999) / 1000);
}

void fsleep(unsigned long usecs)
{
        if (usecs <= 20) luos_udelay(usecs);
        else usleep_range(usecs, usecs * 2);
}


void __init_waitqueue_head(struct wait_queue_head *wq_head, const char *name, struct lock_class_key *key)
{
        spin_lock_init(&wq_head->lock);
        INIT_LIST_HEAD(&wq_head->head);
}

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key)
{
        return wake_up_state(wq_entry->private, mode);
}

int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
        int ret = default_wake_function(wq_entry, mode, sync, key);
        if (ret) list_del_init(&wq_entry->entry);
        return ret;
}

int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
        smp_mb();
        wq_entry->flags |= WQ_FLAG_WOKEN;
        return default_wake_function(wq_entry, mode, sync, key);
}

static void __wake_up_common(struct wait_queue_head *wq_head, unsigned int mode, int nr_exclusive, int wake_flags,
                             void *key)
{
        struct wait_queue_entry *curr, *next;
        list_for_each_entry_safe(curr, next, &wq_head->head, entry) {
                unsigned flags = curr->flags;
                int ret = curr->func(curr, mode, wake_flags, key);
                if (ret < 0) break;
                if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive) break;
        }
}

void __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr, void *key)
{
        unsigned long flags;
        spin_lock_irqsave(&wq_head->lock, flags);
        __wake_up_common(wq_head, mode, nr, 0, key);
        spin_unlock_irqrestore(&wq_head->lock, flags);
}

void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr)
{
        __wake_up_common(wq_head, mode, nr, 0, NULL);
}

void __wake_up_locked_key(struct wait_queue_head *wq_head, unsigned int mode, void *key)
{
        __wake_up_common(wq_head, mode, 1, 0, key);
}

void add_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
        unsigned long flags;
        wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
        spin_lock_irqsave(&wq_head->lock, flags);
        __add_wait_queue(wq_head, wq_entry);
        spin_unlock_irqrestore(&wq_head->lock, flags);
}

void add_wait_queue_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
        unsigned long flags;
        wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
        spin_lock_irqsave(&wq_head->lock, flags);
        __add_wait_queue_entry_tail(wq_head, wq_entry);
        spin_unlock_irqrestore(&wq_head->lock, flags);
}

void remove_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
        unsigned long flags;
        spin_lock_irqsave(&wq_head->lock, flags);
        __remove_wait_queue(wq_head, wq_entry);
        spin_unlock_irqrestore(&wq_head->lock, flags);
}

void prepare_to_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
        unsigned long flags;
        wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
        spin_lock_irqsave(&wq_head->lock, flags);
        if (list_empty(&wq_entry->entry)) __add_wait_queue(wq_head, wq_entry);
        set_current_state(state);
        spin_unlock_irqrestore(&wq_head->lock, flags);
}

bool prepare_to_wait_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
        unsigned long flags;
        bool was_empty = false;
        wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
        spin_lock_irqsave(&wq_head->lock, flags);
        if (list_empty(&wq_entry->entry)) {
                was_empty = list_empty(&wq_head->head);
                __add_wait_queue_entry_tail(wq_head, wq_entry);
        }
        set_current_state(state);
        spin_unlock_irqrestore(&wq_head->lock, flags);
        return was_empty;
}

long prepare_to_wait_event(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
        unsigned long flags;
        long ret = 0;
        spin_lock_irqsave(&wq_head->lock, flags);
        if (signal_pending_state(state, current)) {
                list_del_init(&wq_entry->entry);
                ret = -ERESTARTSYS;
        } else {
                if (list_empty(&wq_entry->entry)) {
                        if (wq_entry->flags & WQ_FLAG_EXCLUSIVE) __add_wait_queue_entry_tail(wq_head, wq_entry);
                        else __add_wait_queue(wq_head, wq_entry);
                }
                set_current_state(state);
        }
        spin_unlock_irqrestore(&wq_head->lock, flags);
        return ret;
}

void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
        unsigned long flags;
        __set_current_state(TASK_RUNNING);
        if (!list_empty_careful(&wq_entry->entry)) {
                spin_lock_irqsave(&wq_head->lock, flags);
                list_del_init(&wq_entry->entry);
                spin_unlock_irqrestore(&wq_head->lock, flags);
        }
}

long wait_woken(struct wait_queue_entry *wq_entry, unsigned mode, long timeout)
{
        set_current_state(mode);
        if (!(wq_entry->flags & WQ_FLAG_WOKEN)) timeout = schedule_timeout(timeout);
        __set_current_state(TASK_RUNNING);
        smp_mb();
        wq_entry->flags &= ~WQ_FLAG_WOKEN;
        return timeout;
}


static DEFINE_RAW_SPINLOCK(timer_lock);
static LIST_HEAD(timer_list_head);
static LIST_HEAD(hrtimer_list_head);
static struct task_struct *timer_task;
static struct timer_list *timer_running;
static struct hrtimer *hrtimer_running;

static void timer_kick(void)
{
        if (timer_task) wake_up_process(timer_task);
}

void timer_setup(struct timer_list *timer, void (*func)(struct timer_list *), unsigned int flags)
{
        INIT_LIST_HEAD(&timer->entry);
        timer->function = func;
        timer->flags = flags;
        timer->pending = false;
        timer->expires = 0;
}

static int __timer_delete_locked(struct timer_list *timer)
{
        if (!timer->pending) return 0;
        list_del_init(&timer->entry);
        timer->pending = false;
        return 1;
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
        unsigned long flags;
        int was;
        raw_spin_lock_irqsave(&timer_lock, flags);
        if (!timer->entry.next) INIT_LIST_HEAD(&timer->entry);
        was = __timer_delete_locked(timer);
        timer->expires = expires;
        timer->pending = true;
        list_add_tail(&timer->entry, &timer_list_head);
        raw_spin_unlock_irqrestore(&timer_lock, flags);
        timer_kick();
        return was;
}

void add_timer(struct timer_list *timer) { mod_timer(timer, timer->expires); }

int timer_delete(struct timer_list *timer)
{
        unsigned long flags;
        int was;
        raw_spin_lock_irqsave(&timer_lock, flags);
        was = timer->entry.next ? __timer_delete_locked(timer) : 0;
        raw_spin_unlock_irqrestore(&timer_lock, flags);
        return was;
}

int timer_delete_sync(struct timer_list *timer)
{
        int was = timer_delete(timer);
        while (READ_ONCE(timer_running) == timer && current != timer_task) luos_yield();
        return was;
}

int timer_shutdown_sync(struct timer_list *timer) { return timer_delete_sync(timer); }

void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*function)(struct hrtimer *), clockid_t clock_id,
                   enum hrtimer_mode mode)
{
        INIT_LIST_HEAD(&timer->entry);
        timer->function = function;
        timer->pending = false;
        timer->running = false;
        timer->expires = 0;
}

void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 range_ns, const enum hrtimer_mode mode)
{
        unsigned long flags;
        if (mode & HRTIMER_MODE_REL) tim += ktime_get();
        raw_spin_lock_irqsave(&timer_lock, flags);
        if (!timer->entry.next) INIT_LIST_HEAD(&timer->entry);
        if (timer->pending) list_del_init(&timer->entry);
        timer->expires = tim;
        timer->pending = true;
        list_add_tail(&timer->entry, &hrtimer_list_head);
        raw_spin_unlock_irqrestore(&timer_lock, flags);
        timer_kick();
}

int hrtimer_try_to_cancel(struct hrtimer *timer)
{
        unsigned long flags;
        int ret = 0;
        raw_spin_lock_irqsave(&timer_lock, flags);
        if (READ_ONCE(hrtimer_running) == timer) {
                ret = -1;
        } else if (timer->pending) {
                list_del_init(&timer->entry);
                timer->pending = false;
                ret = 1;
        }
        raw_spin_unlock_irqrestore(&timer_lock, flags);
        return ret;
}

int hrtimer_cancel(struct hrtimer *timer)
{
        for (;;) {
                int ret = hrtimer_try_to_cancel(timer);
                if (ret >= 0) return ret;
                luos_yield();
        }
}

u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
        u64 orun = 1;
        ktime_t delta = now - timer->expires;
        if (delta < 0) return 0;
        if (interval <= 0) interval = 1;
        if (delta >= interval) {
                s64 incr = interval;
                orun = delta / incr;
                timer->expires += incr * orun;
                if (timer->expires > now) return orun;
                orun++;
        }
        timer->expires += interval;
        return orun;
}

static int timer_thread_fn(void *arg)
{
        for (;;) {
                unsigned long flags;
                struct timer_list *t, *tfire = NULL;
                struct hrtimer *h, *hfire = NULL;
                s64 next_ns = S64_MAX;

                set_current_state(TASK_UNINTERRUPTIBLE);
                raw_spin_lock_irqsave(&timer_lock, flags);
                unsigned long now_j = jiffies;
                s64 now_ns = ktime_get();
                list_for_each_entry(t, &timer_list_head, entry) {
                        if (time_after_eq(now_j, t->expires)) {
                                tfire = t;
                                break;
                        }
                        s64 d = (s64)(t->expires - now_j) * NSEC_PER_MSEC;
                        if (d < next_ns) next_ns = d;
                }
                if (tfire) {
                        list_del_init(&tfire->entry);
                        tfire->pending = false;
                        WRITE_ONCE(timer_running, tfire);
                } else {
                        list_for_each_entry(h, &hrtimer_list_head, entry) {
                                if (h->expires <= now_ns) {
                                        hfire = h;
                                        break;
                                }
                                if (h->expires - now_ns < next_ns) next_ns = h->expires - now_ns;
                        }
                        if (hfire) {
                                list_del_init(&hfire->entry);
                                hfire->pending = false;
                                hfire->running = true;
                                WRITE_ONCE(hrtimer_running, hfire);
                        }
                }
                raw_spin_unlock_irqrestore(&timer_lock, flags);

                if (tfire) {
                        __set_current_state(TASK_RUNNING);
                        tfire->function(tfire);
                        WRITE_ONCE(timer_running, NULL);
                        continue;
                }
                if (hfire) {
                        __set_current_state(TASK_RUNNING);
                        enum hrtimer_restart r = hfire->function ? hfire->function(hfire) : HRTIMER_NORESTART;
                        raw_spin_lock_irqsave(&timer_lock, flags);
                        hfire->running = false;
                        if (r == HRTIMER_RESTART && !hfire->pending) {
                                hfire->pending = true;
                                list_add_tail(&hfire->entry, &hrtimer_list_head);
                        }
                        WRITE_ONCE(hrtimer_running, NULL);
                        raw_spin_unlock_irqrestore(&timer_lock, flags);
                        continue;
                }
                if (next_ns == S64_MAX) schedule();
                else schedule_timeout(max_t(long, 1, (long)((next_ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC)));
        }
        return 0;
}


#define WORK_DISABLED_BIT 1
#define LKPI_WQ_MAX_WORKERS 8

struct workqueue_struct {
        char name[32];
        unsigned int flags;
        raw_spinlock_t lock;
        struct list_head pending;
        struct task_struct *workers[LKPI_WQ_MAX_WORKERS];
        struct work_struct *running[LKPI_WQ_MAX_WORKERS];
        int nr_workers;
        int idle;
        bool stopping;
        wait_queue_head_t done_wq;
        atomic_t alive;
};

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_freezable_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_percpu_wq;
struct workqueue_struct *system_dfl_wq;
struct workqueue_struct *lkpi_irq_wq;

struct wq_worker_arg {
        struct workqueue_struct *wq;
        int index;
};

static int wq_worker_fn(void *data)
{
        struct wq_worker_arg *a = data;
        struct workqueue_struct *wq = a->wq;
        int index = a->index;
        kfree(a);
        current->flags |= PF_WQ_WORKER;

        for (;;) {
                unsigned long flags;
                struct work_struct *work = NULL;

                set_current_state(TASK_UNINTERRUPTIBLE);
                raw_spin_lock_irqsave(&wq->lock, flags);
                if (!list_empty(&wq->pending)) {
                        work = list_first_entry(&wq->pending, struct work_struct, entry);
                        list_del_init(&work->entry);
                        clear_bit(WORK_STRUCT_PENDING_BIT, (unsigned long *)&work->data.counter);
                        wq->running[index] = work;
                } else if (wq->stopping) {
                        raw_spin_unlock_irqrestore(&wq->lock, flags);
                        __set_current_state(TASK_RUNNING);
                        break;
                } else {
                        wq->idle++;
                }
                raw_spin_unlock_irqrestore(&wq->lock, flags);

                if (!work) {
                        schedule();
                        raw_spin_lock_irqsave(&wq->lock, flags);
                        wq->idle--;
                        raw_spin_unlock_irqrestore(&wq->lock, flags);
                        continue;
                }
                __set_current_state(TASK_RUNNING);
                current->lkpi_current_work = work;
                work->func(work);
                current->lkpi_current_work = NULL;
                raw_spin_lock_irqsave(&wq->lock, flags);
                wq->running[index] = NULL;
                raw_spin_unlock_irqrestore(&wq->lock, flags);
                wake_up_all(&wq->done_wq);
        }
        if (atomic_dec_and_test(&wq->alive)) wake_up_all(&wq->done_wq);
        return 0;
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
        va_list ap;
        struct workqueue_struct *wq = kzalloc(sizeof(*wq), GFP_KERNEL);
        if (!wq) return NULL;
        va_start(ap, max_active);
        vsnprintf(wq->name, sizeof(wq->name), fmt, ap);
        va_end(ap);
        wq->flags = flags;
        raw_spin_lock_init(&wq->lock);
        INIT_LIST_HEAD(&wq->pending);
        init_waitqueue_head(&wq->done_wq);

        int n = (flags & __WQ_ORDERED) || max_active == 1 ? 1 : 4;
        if (n > LKPI_WQ_MAX_WORKERS) n = LKPI_WQ_MAX_WORKERS;
        atomic_set(&wq->alive, n);
        for (int i = 0; i < n; i++) {
                struct wq_worker_arg *a = kmalloc(sizeof(*a), GFP_KERNEL);
                struct task_struct *t;
                if (!a) break;
                a->wq = wq;
                a->index = i;
                t = kthread_run(wq_worker_fn, a, "kworker/%s", wq->name);
                if (IS_ERR(t)) {
                        kfree(a);
                        break;
                }
                wq->workers[i] = t;
                wq->nr_workers++;
        }
        if (!wq->nr_workers) {
                kfree(wq);
                return NULL;
        }
        atomic_set(&wq->alive, wq->nr_workers);
        return wq;
}

static void wq_kick(struct workqueue_struct *wq)
{
        for (int i = 0; i < wq->nr_workers; i++)
                if (wake_up_process(wq->workers[i])) break;
}

static void __queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
        unsigned long flags;
        if (!wq) wq = system_wq;
        raw_spin_lock_irqsave(&wq->lock, flags);
        work->wq = wq;
        list_add_tail(&work->entry, &wq->pending);
        raw_spin_unlock_irqrestore(&wq->lock, flags);
        wq_kick(wq);
}

bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
        unsigned long *bits = (unsigned long *)&work->data.counter;
        if (test_bit(WORK_DISABLED_BIT, bits)) return false;
        if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, bits)) return false;
        __queue_work(wq, work);
        return true;
}

void lkpi_delayed_work_timer_fn(struct timer_list *t)
{
        struct delayed_work *dwork = container_of(t, struct delayed_work, timer);
        __queue_work(dwork->wq, &dwork->work);
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
        unsigned long *bits = (unsigned long *)&dwork->work.data.counter;
        if (test_bit(WORK_DISABLED_BIT, bits)) return false;
        if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, bits)) return false;
        if (!wq) wq = system_wq;
        dwork->wq = wq;
        if (!delay) {
                __queue_work(wq, &dwork->work);
        } else {
                if (dwork->timer.function != lkpi_delayed_work_timer_fn) timer_setup(&dwork->timer, lkpi_delayed_work_timer_fn, 0);
                mod_timer(&dwork->timer, jiffies + delay);
        }
        return true;
}

static bool work_unqueue(struct work_struct *work)
{
        struct workqueue_struct *wq = READ_ONCE(work->wq);
        unsigned long flags;
        bool was = false;
        if (!wq) return test_and_clear_bit(WORK_STRUCT_PENDING_BIT, (unsigned long *)&work->data.counter);
        raw_spin_lock_irqsave(&wq->lock, flags);
        if (!list_empty(&work->entry)) {
                list_del_init(&work->entry);
                was = true;
        }
        if (test_and_clear_bit(WORK_STRUCT_PENDING_BIT, (unsigned long *)&work->data.counter)) was = true;
        raw_spin_unlock_irqrestore(&wq->lock, flags);
        return was;
}

static bool work_running(struct work_struct *work)
{
        struct workqueue_struct *wq = READ_ONCE(work->wq);
        unsigned long flags;
        bool busy = false;
        if (!wq) return false;
        raw_spin_lock_irqsave(&wq->lock, flags);
        for (int i = 0; i < wq->nr_workers; i++)
                if (wq->running[i] == work) busy = true;
        raw_spin_unlock_irqrestore(&wq->lock, flags);
        return busy;
}

static bool work_busy_now(struct work_struct *work)
{
        return work_pending(work) || work_running(work);
}

bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
        bool was = timer_delete(&dwork->timer);
        if (work_unqueue(&dwork->work)) was = true;
        queue_delayed_work_on(cpu, wq, dwork, delay);
        return was;
}

bool flush_work(struct work_struct *work)
{
        struct workqueue_struct *wq = READ_ONCE(work->wq);
        if (!wq) return false;
        if (current->lkpi_current_work == work) return false;
        bool was = work_busy_now(work);
        wait_event(wq->done_wq, !work_busy_now(work));
        return was;
}

bool cancel_work(struct work_struct *work) { return work_unqueue(work); }

bool cancel_work_sync(struct work_struct *work)
{
        bool was = work_unqueue(work);
        struct workqueue_struct *wq = READ_ONCE(work->wq);
        if (wq && current->lkpi_current_work != work) wait_event(wq->done_wq, !work_running(work));
        return was;
}

bool flush_delayed_work(struct delayed_work *dwork)
{
        if (timer_delete_sync(&dwork->timer)) __queue_work(dwork->wq, &dwork->work);
        return flush_work(&dwork->work);
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
        bool was = timer_delete(&dwork->timer);
        if (work_unqueue(&dwork->work)) was = true;
        return was;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
        bool was = timer_delete_sync(&dwork->timer);
        if (cancel_work_sync(&dwork->work)) was = true;
        return was;
}

bool disable_work_sync(struct work_struct *work)
{
        set_bit(WORK_DISABLED_BIT, (unsigned long *)&work->data.counter);
        return cancel_work_sync(work);
}

bool disable_delayed_work_sync(struct delayed_work *dwork)
{
        set_bit(WORK_DISABLED_BIT, (unsigned long *)&dwork->work.data.counter);
        return cancel_delayed_work_sync(dwork);
}

bool enable_work(struct work_struct *work)
{
        clear_bit(WORK_DISABLED_BIT, (unsigned long *)&work->data.counter);
        return true;
}

static bool wq_idle(struct workqueue_struct *wq)
{
        unsigned long flags;
        bool idle;
        raw_spin_lock_irqsave(&wq->lock, flags);
        idle = list_empty(&wq->pending);
        for (int i = 0; i < wq->nr_workers && idle; i++)
                if (wq->running[i]) idle = false;
        raw_spin_unlock_irqrestore(&wq->lock, flags);
        return idle;
}

void flush_workqueue(struct workqueue_struct *wq)
{
        if (!wq) return;
        for (int i = 0; i < wq->nr_workers; i++)
                if (wq->workers[i] == current) return;
        wait_event(wq->done_wq, wq_idle(wq));
}

void drain_workqueue(struct workqueue_struct *wq) { flush_workqueue(wq); }

void destroy_workqueue(struct workqueue_struct *wq)
{
        unsigned long flags;
        if (!wq) return;
        flush_workqueue(wq);
        raw_spin_lock_irqsave(&wq->lock, flags);
        wq->stopping = true;
        raw_spin_unlock_irqrestore(&wq->lock, flags);
        for (int i = 0; i < wq->nr_workers; i++) wake_up_process(wq->workers[i]);
        wait_event(wq->done_wq, atomic_read(&wq->alive) == 0);
        kfree(wq);
}

bool current_is_workqueue_rescuer(void) { return false; }
bool workqueue_congested(int cpu, struct workqueue_struct *wq) { return false; }
struct work_struct *current_work(void) { return current->lkpi_current_work; }


struct lkpi_kthread {
        int (*threadfn)(void *data);
        void *data;
        struct task_struct *task;
        int should_stop;
        int should_park;
        int parked;
        int result;
        struct completion exited;
        struct completion parked_done;
};

static struct lkpi_kthread *to_kthread(struct task_struct *t) { return t->kthread; }

static void kthread_entry(void *arg)
{
        struct lkpi_kthread *k = arg;
        struct task_struct *ts = k->task;
        *luos_task_lx_slot(luos_current_task()) = ts;
        ts->luos_task = luos_current_task();

        while (!READ_ONCE(ts->wake_flag)) luos_wait(ts, &ts->wake_flag, 0);
        WRITE_ONCE(ts->__state, TASK_RUNNING);

        int ret = -EINTR;
        if (!READ_ONCE(k->should_stop)) ret = k->threadfn(k->data);
        k->result = ret;
        complete(&k->exited);
        luos_task_exit();
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data), void *data, int node, const char namefmt[], ...)
{
        va_list ap;
        struct lkpi_kthread *k = kzalloc(sizeof(*k), GFP_KERNEL);
        struct task_struct *ts = kzalloc(sizeof(*ts), GFP_KERNEL);
        if (!k || !ts) {
                kfree(k);
                kfree(ts);
                return ERR_PTR(-ENOMEM);
        }
        task_struct_init(ts, NULL);
        ts->flags |= PF_KTHREAD;
        refcount_set(&ts->usage, 2);
        va_start(ap, namefmt);
        vsnprintf(ts->comm, sizeof(ts->comm), namefmt, ap);
        va_end(ap);
        ts->__state = TASK_UNINTERRUPTIBLE;
        ts->wake_flag = 0;
        ts->kthread = k;
        k->threadfn = threadfn;
        k->data = data;
        k->task = ts;
        init_completion(&k->exited);
        init_completion(&k->parked_done);

        void *lt = luos_task_spawn(ts->comm, kthread_entry, k);
        if (!lt) {
                kfree(k);
                kfree(ts);
                return ERR_PTR(-ENOMEM);
        }
        ts->pid = luos_task_tid(lt);
        ts->tgid = ts->pid;
        return ts;
}

bool kthread_should_stop(void)
{
        struct lkpi_kthread *k = to_kthread(current);
        return k && READ_ONCE(k->should_stop);
}

bool kthread_should_park(void)
{
        struct lkpi_kthread *k = to_kthread(current);
        return k && READ_ONCE(k->should_park);
}

void kthread_parkme(void)
{
        struct lkpi_kthread *k = to_kthread(current);
        if (!k) return;
        for (;;) {
                set_current_state(TASK_PARKED);
                if (!READ_ONCE(k->should_park)) break;
                if (!k->parked) {
                        k->parked = 1;
                        complete(&k->parked_done);
                }
                schedule();
        }
        k->parked = 0;
        __set_current_state(TASK_RUNNING);
}

int kthread_park(struct task_struct *t)
{
        struct lkpi_kthread *k = to_kthread(t);
        if (!k) return -EINVAL;
        WRITE_ONCE(k->should_park, 1);
        if (t != current) {
                wake_up_process(t);
                wait_for_completion(&k->parked_done);
        }
        return 0;
}

void kthread_unpark(struct task_struct *t)
{
        struct lkpi_kthread *k = to_kthread(t);
        if (!k) return;
        WRITE_ONCE(k->should_park, 0);
        reinit_completion(&k->parked_done);
        wake_up_state(t, TASK_PARKED);
}

int kthread_stop(struct task_struct *t)
{
        struct lkpi_kthread *k = to_kthread(t);
        if (!k) return -EINVAL;
        WRITE_ONCE(k->should_stop, 1);
        kthread_unpark(t);
        wake_up_state(t, TASK_NORMAL | TASK_PARKED);
        WRITE_ONCE(t->wake_flag, 1);
        luos_wake_all(t);
        wait_for_completion(&k->exited);
        return k->result;
}

int kthread_stop_put(struct task_struct *t)
{
        int ret = kthread_stop(t);
        put_task_struct(t);
        return ret;
}

void kthread_bind(struct task_struct *k, unsigned int cpu) { }

void *kthread_data(struct task_struct *t)
{
        struct lkpi_kthread *k = to_kthread(t);
        return k ? k->data : NULL;
}


static bool kworker_idle(struct kthread_worker *worker, struct kthread_work *work)
{
        unsigned long flags;
        bool idle;
        raw_spin_lock_irqsave(&worker->lock, flags);
        if (work) idle = list_empty(&work->node) && worker->current_work != work;
        else idle = list_empty(&worker->work_list) && !worker->current_work;
        raw_spin_unlock_irqrestore(&worker->lock, flags);
        return idle;
}

static int kthread_worker_fn(void *data)
{
        struct kthread_worker *worker = data;
        for (;;) {
                unsigned long flags;
                struct kthread_work *work = NULL;

                set_current_state(TASK_INTERRUPTIBLE);
                if (kthread_should_stop()) {
                        __set_current_state(TASK_RUNNING);
                        break;
                }
                raw_spin_lock_irqsave(&worker->lock, flags);
                if (!list_empty(&worker->work_list)) {
                        work = list_first_entry(&worker->work_list, struct kthread_work, node);
                        list_del_init(&work->node);
                }
                worker->current_work = work;
                raw_spin_unlock_irqrestore(&worker->lock, flags);

                if (!work) {
                        schedule();
                        continue;
                }
                __set_current_state(TASK_RUNNING);
                work->func(work);
                raw_spin_lock_irqsave(&worker->lock, flags);
                worker->current_work = NULL;
                raw_spin_unlock_irqrestore(&worker->lock, flags);
                wake_up_all(&worker->wait);
        }
        return 0;
}

struct kthread_worker *kthread_create_worker_on_node(unsigned int flags, int node, const char namefmt[], ...)
{
        va_list ap;
        char name[TASK_COMM_LEN];
        struct kthread_worker *worker = kzalloc(sizeof(*worker), GFP_KERNEL);
        if (!worker) return ERR_PTR(-ENOMEM);
        raw_spin_lock_init(&worker->lock);
        INIT_LIST_HEAD(&worker->work_list);
        INIT_LIST_HEAD(&worker->delayed_work_list);
        init_waitqueue_head(&worker->wait);
        worker->flags = flags;
        va_start(ap, namefmt);
        vsnprintf(name, sizeof(name), namefmt, ap);
        va_end(ap);
        struct task_struct *t = kthread_run(kthread_worker_fn, worker, "%s", name);
        if (IS_ERR(t)) {
                kfree(worker);
                return ERR_CAST(t);
        }
        worker->task = t;
        return worker;
}

static void kthread_insert_work(struct kthread_worker *worker, struct kthread_work *work)
{
        list_add_tail(&work->node, &worker->work_list);
        work->worker = worker;
        if (worker->task) wake_up_process(worker->task);
}

bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
        unsigned long flags;
        bool ret = false;
        raw_spin_lock_irqsave(&worker->lock, flags);
        if (list_empty(&work->node) && !work->canceling) {
                kthread_insert_work(worker, work);
                ret = true;
        }
        raw_spin_unlock_irqrestore(&worker->lock, flags);
        return ret;
}

void lkpi_kthread_delayed_work_timer_fn(struct timer_list *t)
{
        struct kthread_delayed_work *dwork = container_of(t, struct kthread_delayed_work, timer);
        struct kthread_work *work = &dwork->work;
        struct kthread_worker *worker = work->worker;
        unsigned long flags;
        if (!worker) return;
        raw_spin_lock_irqsave(&worker->lock, flags);
        list_del_init(&work->node);
        if (!work->canceling) kthread_insert_work(worker, work);
        raw_spin_unlock_irqrestore(&worker->lock, flags);
}

bool kthread_queue_delayed_work(struct kthread_worker *worker, struct kthread_delayed_work *dwork, unsigned long delay)
{
        struct kthread_work *work = &dwork->work;
        unsigned long flags;
        bool ret = false;
        raw_spin_lock_irqsave(&worker->lock, flags);
        if (list_empty(&work->node) && !work->canceling) {
                if (!delay) {
                        kthread_insert_work(worker, work);
                } else {
                        work->worker = worker;
                        list_add(&work->node, &worker->delayed_work_list);
                        mod_timer(&dwork->timer, jiffies + delay);
                }
                ret = true;
        }
        raw_spin_unlock_irqrestore(&worker->lock, flags);
        return ret;
}

bool kthread_mod_delayed_work(struct kthread_worker *worker, struct kthread_delayed_work *dwork, unsigned long delay)
{
        struct kthread_work *work = &dwork->work;
        unsigned long flags;
        bool was;
        timer_delete(&dwork->timer);
        raw_spin_lock_irqsave(&worker->lock, flags);
        was = !list_empty(&work->node);
        list_del_init(&work->node);
        raw_spin_unlock_irqrestore(&worker->lock, flags);
        kthread_queue_delayed_work(worker, dwork, delay);
        return was;
}

void kthread_flush_work(struct kthread_work *work)
{
        struct kthread_worker *worker = work->worker;
        if (!worker || worker->task == current) return;
        wait_event(worker->wait, kworker_idle(worker, work));
}

void kthread_flush_worker(struct kthread_worker *worker)
{
        if (worker->task == current) return;
        wait_event(worker->wait, kworker_idle(worker, NULL));
}

bool kthread_cancel_work_sync(struct kthread_work *work)
{
        struct kthread_worker *worker = work->worker;
        unsigned long flags;
        bool was;
        if (!worker) return false;
        raw_spin_lock_irqsave(&worker->lock, flags);
        was = !list_empty(&work->node);
        list_del_init(&work->node);
        work->canceling++;
        raw_spin_unlock_irqrestore(&worker->lock, flags);
        if (worker->task != current) wait_event(worker->wait, kworker_idle(worker, work));
        raw_spin_lock_irqsave(&worker->lock, flags);
        work->canceling--;
        raw_spin_unlock_irqrestore(&worker->lock, flags);
        return was;
}

bool kthread_cancel_delayed_work_sync(struct kthread_delayed_work *dwork)
{
        struct kthread_work *work = &dwork->work;
        bool was = false;
        if (!work->worker) return false;
        work->canceling++;
        if (timer_delete_sync(&dwork->timer)) was = true;
        work->canceling--;
        if (kthread_cancel_work_sync(work)) was = true;
        return was;
}

void kthread_destroy_worker(struct kthread_worker *worker)
{
        kthread_flush_worker(worker);
        kthread_stop(worker->task);
        kfree(worker);
}



#define WAIT_TABLE_BITS 8
#define WAIT_TABLE_SIZE (1 << WAIT_TABLE_BITS)
static struct wait_queue_head bit_wait_table[WAIT_TABLE_SIZE];

struct wait_queue_head *bit_waitqueue(unsigned long *word, int bit)
{
        unsigned long val = (unsigned long)word << 6 | bit;
        return bit_wait_table + hash_long(val, WAIT_TABLE_BITS);
}

struct wait_queue_head *__var_waitqueue(void *p) { return bit_wait_table + hash_ptr(p, WAIT_TABLE_BITS); }

int wake_bit_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *arg)
{
        struct wait_bit_key *key = arg;
        struct wait_bit_queue_entry *wait_bit = container_of(wq_entry, struct wait_bit_queue_entry, wq_entry);
        if (wait_bit->key.flags != key->flags || wait_bit->key.bit_nr != key->bit_nr ||
            test_bit(key->bit_nr, key->flags))
                return 0;
        return autoremove_wake_function(wq_entry, mode, sync, key);
}

int __wait_on_bit(struct wait_queue_head *wq_head, struct wait_bit_queue_entry *wbq_entry, wait_bit_action_f *action,
                  unsigned int mode)
{
        int ret = 0;
        do {
                prepare_to_wait(wq_head, &wbq_entry->wq_entry, mode);
                if (test_bit(wbq_entry->key.bit_nr, wbq_entry->key.flags)) ret = (*action)(&wbq_entry->key, mode);
        } while (test_bit_acquire(wbq_entry->key.bit_nr, wbq_entry->key.flags) && !ret);
        finish_wait(wq_head, &wbq_entry->wq_entry);
        return ret;
}

int out_of_line_wait_on_bit(unsigned long *word, int bit, wait_bit_action_f *action, unsigned int mode)
{
        DEFINE_WAIT_BIT(wq_entry, word, bit);
        return __wait_on_bit(bit_waitqueue(word, bit), &wq_entry, action, mode);
}

int out_of_line_wait_on_bit_timeout(unsigned long *word, int bit, wait_bit_action_f *action, unsigned int mode,
                                    unsigned long timeout)
{
        DEFINE_WAIT_BIT(wq_entry, word, bit);
        wq_entry.key.timeout = jiffies + timeout;
        return __wait_on_bit(bit_waitqueue(word, bit), &wq_entry, action, mode);
}

int __wait_on_bit_lock(struct wait_queue_head *wq_head, struct wait_bit_queue_entry *wbq_entry,
                       wait_bit_action_f *action, unsigned int mode)
{
        int ret = 0;
        for (;;) {
                prepare_to_wait_exclusive(wq_head, &wbq_entry->wq_entry, mode);
                if (test_bit(wbq_entry->key.bit_nr, wbq_entry->key.flags)) {
                        ret = action(&wbq_entry->key, mode);
                        if (ret) break;
                }
                if (!test_and_set_bit(wbq_entry->key.bit_nr, wbq_entry->key.flags)) break;
        }
        finish_wait(wq_head, &wbq_entry->wq_entry);
        return ret;
}

int out_of_line_wait_on_bit_lock(unsigned long *word, int bit, wait_bit_action_f *action, unsigned int mode)
{
        DEFINE_WAIT_BIT(wq_entry, word, bit);
        return __wait_on_bit_lock(bit_waitqueue(word, bit), &wq_entry, action, mode);
}

void __wake_up_bit(struct wait_queue_head *wq_head, unsigned long *word, int bit)
{
        struct wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);
        if (waitqueue_active(wq_head)) __wake_up(wq_head, TASK_NORMAL, 1, &key);
}

void wake_up_bit(unsigned long *word, int bit)
{
        smp_mb();
        __wake_up_bit(bit_waitqueue(word, bit), word, bit);
}

static int var_wake_function(struct wait_queue_entry *wq_entry, unsigned int mode, int sync, void *arg)
{
        struct wait_bit_key *key = arg;
        struct wait_bit_queue_entry *wbq_entry = container_of(wq_entry, struct wait_bit_queue_entry, wq_entry);
        if (wbq_entry->key.flags != key->flags || wbq_entry->key.bit_nr != key->bit_nr) return 0;
        return autoremove_wake_function(wq_entry, mode, sync, key);
}

void init_wait_var_entry(struct wait_bit_queue_entry *wbq_entry, void *var, int flags)
{
        *wbq_entry = (struct wait_bit_queue_entry){
                .key = { .flags = (var), .bit_nr = -1, },
                .wq_entry = {
                        .flags = flags,
                        .private = current,
                        .func = var_wake_function,
                        .entry = LIST_HEAD_INIT(wbq_entry->wq_entry.entry),
                },
        };
}

void wake_up_var(void *var)
{
        smp_mb();
        __wake_up_bit(__var_waitqueue(var), var, -1);
}

int bit_wait(struct wait_bit_key *word, int mode)
{
        schedule();
        if (signal_pending_state(mode, current)) return -EINTR;
        return 0;
}

int bit_wait_io(struct wait_bit_key *word, int mode) { return bit_wait(word, mode); }

int bit_wait_timeout(struct wait_bit_key *word, int mode)
{
        unsigned long now = jiffies;
        if (time_after_eq(now, word->timeout)) return -EAGAIN;
        schedule_timeout(word->timeout - now);
        if (signal_pending_state(mode, current)) return -EINTR;
        return 0;
}

void lkpi_sched_init(void)
{
        for (int i = 0; i < WAIT_TABLE_SIZE; i++) init_waitqueue_head(&bit_wait_table[i]);
        timer_task = kthread_run(timer_thread_fn, NULL, "lkpi-timers");
        system_wq = alloc_workqueue("events", 0, 0);
        system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
        system_long_wq = alloc_workqueue("events_long", 0, 0);
        system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND, 0);
        system_freezable_wq = system_wq;
        system_power_efficient_wq = system_wq;
        system_percpu_wq = system_wq;
        system_dfl_wq = system_unbound_wq;
        lkpi_irq_wq = alloc_workqueue("lkpi_irq", WQ_HIGHPRI, 0);
}
