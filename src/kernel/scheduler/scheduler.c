#include "kernel/scheduler/scheduler.h"

#include "kernel/scheduler/spinlock.h"
#include "drivers/Timer/apic_driver.h"
#include "components/Memory/heap.h"
#include "components/Interruptions/isr.h"
#include "components/panic.h"
#include "components/drivers.h"
#include "components/logger.h"
#include "drivers/Timer/timer.h"

#include <string.h>
#include <stddef.h>

#define SCHED_PENALTY_MAX 3
#define SCHED_STARVE_MS 100
#define SCHED_AGING_MS 25
#define SCHED_BALANCE_MS 20
#define SCHED_STACK_MAGIC 0x4C754F5353544B21ull

enum sched_reason {
    SCHED_REASON_EXPIRED = 0,
    SCHED_REASON_YIELD = 1,
    SCHED_REASON_PREEMPT = 2,
};

struct rq {
    spinlock_t lock;
    struct task *head[PRIO_LEVELS];
    struct task *tail[PRIO_LEVELS];
    uint32_t bitmap;
    volatile int nr_queued;

    struct task *volatile current;
    struct task *idle;
    struct task *prev;
    void *isr_fx;

    volatile int need_resched;
    volatile int online;

    uint64_t local_ticks;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint64_t steals;
    uint64_t switches;
} __attribute__((aligned(64)));

static struct rq g_rq[MAX_CORES];

static struct task *g_tasks[MAX_TASKS];
static int g_task_count = 0;
static spinlock_t g_tasks_lock = SPINLOCK_INIT;

static struct task *g_sleep_head = NULL;
static spinlock_t g_sleep_lock = SPINLOCK_INIT;

static uint32_t g_next_tid = 1;
static volatile uint64_t g_ticks = 0;
static uint64_t (*g_clock_ms)(void) = NULL;

static uint8_t g_lapic_to_core[256];
static int g_core_count = 1;

static uint8_t g_fx_template[TASK_FX_SIZE] __attribute__((aligned(16)));

static void scheduler_yield_isr(struct registers *regs);

static inline uint64_t irq_save_local(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore_local(uint64_t flags) {
    if (flags & (1u << 9)) asm volatile("sti" ::: "memory");
}

static inline int clamp_level(int p) {
    if (p < 0) return 0;
    if (p > PRIO_IDLE - 1) return PRIO_IDLE - 1;
    return p;
}

static inline int level_cap(const struct task *t) {
    return clamp_level(t->base_priority + SCHED_PENALTY_MAX);
}

static inline int sched_slice_for(int prio) {
    return 4 + 2 * clamp_level(prio);
}

static inline int rq_nr_running(const struct rq *rq) {
    const struct task *c = rq->current;
    return rq->nr_queued + ((c && !c->is_idle && c->state == TASK_RUNNING) ? 1 : 0);
}

void scheduler_register_core(uint8_t lapic_id, int logical_id) {
    g_lapic_to_core[lapic_id] = (uint8_t)logical_id;
    if (logical_id + 1 > g_core_count) g_core_count = logical_id + 1;
}

int scheduler_core_count(void) {
    return g_core_count;
}

int current_core(void) {
    return g_lapic_to_core[apic_get_lapic_id()];
}

struct task *current_task(void) {
    uint64_t f = irq_save_local();
    struct task *t = g_rq[current_core()].current;
    irq_restore_local(f);
    return t;
}

uint64_t scheduler_ticks(void) {
    return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
}

int scheduler_current_stack(uintptr_t *lo, uintptr_t *hi) {
    struct task *t = current_task();
    if (!t || !t->kstack || !t->stack_size) return 0;
    if (lo) *lo = (uintptr_t)t->kstack;
    if (hi) *hi = (uintptr_t)t->kstack + t->stack_size;
    return 1;
}

void *scheduler_set_isr_fx(void *fx) {
    struct rq *rq = &g_rq[current_core()];
    void *old = rq->isr_fx;
    rq->isr_fx = fx;
    return old;
}

static uint16_t read_cs(void) {
    uint16_t v;
    asm volatile("mov %%cs, %0" : "=r"(v));
    return v;
}

static uint16_t read_ss(void) {
    uint16_t v;
    asm volatile("mov %%ss, %0" : "=r"(v));
    return v;
}

static void rq_enqueue(struct rq *rq, struct task *t, int at_head) {
    int p = t->priority;
    t->state = TASK_READY;
    t->enqueue_tick = g_ticks;
    t->on_rq = 1;
    if (at_head && rq->head[p]) {
        t->next = rq->head[p];
        rq->head[p] = t;
    } else {
        t->next = NULL;
        if (rq->tail[p]) rq->tail[p]->next = t;
        else rq->head[p] = t;
        rq->tail[p] = t;
    }
    rq->bitmap |= (1u << p);
    rq->nr_queued++;
}

static void rq_unlink(struct rq *rq, int p, struct task *prev, struct task *t) {
    if (prev) prev->next = t->next;
    else rq->head[p] = t->next;
    if (rq->tail[p] == t) rq->tail[p] = prev;
    if (!rq->head[p]) rq->bitmap &= ~(1u << p);
    t->next = NULL;
    t->on_rq = 0;
    rq->nr_queued--;
}

static struct task *rq_pick(struct rq *rq) {
    if (!rq->bitmap) return NULL;
    int p = __builtin_ctz(rq->bitmap);
    struct task *t = rq->head[p];
    rq_unlink(rq, p, NULL, t);
    return t;
}

static struct task *rq_take_migratable(struct rq *rq) {
    uint32_t bm = rq->bitmap;
    while (bm) {
        int p = __builtin_ctz(bm);
        bm &= bm - 1;
        struct task *prev = NULL;
        for (struct task *t = rq->head[p]; t; prev = t, t = t->next) {
            if (t->affinity != TASK_ANY_CORE) continue;
            if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) continue;
            rq_unlink(rq, p, prev, t);
            return t;
        }
    }
    return NULL;
}

static void finish_prev(struct rq *rq) {
    struct task *p = rq->prev;
    if (p && p != rq->current) {
        __atomic_store_n(&p->on_cpu, 0, __ATOMIC_RELEASE);
        rq->prev = NULL;
    }
}

static int least_loaded_online(int prefer) {
    int best = -1, best_load = 0x7FFFFFFF;
    for (int c = 0; c < g_core_count; c++) {
        if (!g_rq[c].online) continue;
        int load = rq_nr_running(&g_rq[c]);
        if (load < best_load || (load == best_load && c == prefer)) {
            best_load = load;
            best = c;
        }
    }
    return best < 0 ? 0 : best;
}

static int select_core(struct task *t) {
    if (t->affinity != TASK_ANY_CORE) return t->affinity;
    if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) return t->core;

    int prev = t->core;
    if (prev < 0 || prev >= g_core_count || !g_rq[prev].online) prev = 0;
    int best = least_loaded_online(prev);
    if (rq_nr_running(&g_rq[prev]) <= rq_nr_running(&g_rq[best]) + 1)
        return prev;
    return best;
}

static void wake_task(struct task *t) {
    int core = select_core(t);
    struct rq *rq = &g_rq[core];

    uint64_t f = spin_lock_irqsave(&rq->lock);
    if (!t->on_rq && t->state != TASK_ZOMBIE && t->state != TASK_RUNNING) {
        t->core = core;
        rq_enqueue(rq, t, 0);
        struct task *cur = rq->current;
        if (!cur || cur->is_idle || t->priority < cur->priority)
            rq->need_resched = 1;
    }
    spin_unlock_irqrestore(&rq->lock, f);
}

static void idle_entry(void *arg) {
    (void)arg;
    for (;;)
        asm volatile("sti; hlt" ::: "memory");
}

static void task_init_context(struct task *t) {
    uint64_t stack_top = (uint64_t)t->kstack + t->stack_size;
    stack_top &= ~0xFULL;
    stack_top -= 8;
    *(uint64_t *)stack_top = 0;

    extern void task_trampoline(void);

    memset(&t->regs, 0, sizeof(t->regs));
    t->regs.rip = (uint64_t)task_trampoline;
    t->regs.rsp = stack_top;
    t->regs.rbp = 0;
    t->regs.cs = read_cs();
    t->regs.ss = read_ss();
    t->regs.rflags = 0x202;

    t->fx = (uint8_t *)(((uintptr_t)t->fx_raw + 15) & ~(uintptr_t)15);
    memcpy(t->fx, g_fx_template, TASK_FX_SIZE);

    *(uint64_t *)t->kstack = SCHED_STACK_MAGIC;
}

static struct task *task_alloc(const char *name, int priority, uint32_t stack_size) {
    struct task *t = kmalloc(sizeof(struct task));
    if (!t) return NULL;
    memset(t, 0, sizeof(struct task));

    t->tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_SEQ_CST);
    strncpy(t->name, name ? name : "task", TASK_NAME_MAX - 1);
    t->name[TASK_NAME_MAX - 1] = 0;
    if (priority < 0) priority = 0;
    if (priority > PRIO_IDLE) priority = PRIO_IDLE;
    t->base_priority = priority;
    t->priority = priority;
    t->time_slice = sched_slice_for(priority);
    t->affinity = TASK_ANY_CORE;
    t->core = 0;

    if (stack_size) {
        if (stack_size < TASK_STACK_MIN) stack_size = TASK_STACK_MIN;
        stack_size = (stack_size + 15u) & ~15u;
        t->kstack = kmalloc(stack_size);
        if (!t->kstack) { kfree(t); return NULL; }
        t->stack_size = stack_size;
        task_init_context(t);
    } else {
        t->fx = (uint8_t *)(((uintptr_t)t->fx_raw + 15) & ~(uintptr_t)15);
        memcpy(t->fx, g_fx_template, TASK_FX_SIZE);
    }
    return t;
}

static void reap_zombies(void) {
    struct task *dead[16];
    int nd = 0;

    uint64_t f = spin_lock_irqsave(&g_tasks_lock);
    for (int i = 0; i < g_task_count && nd < 16; ) {
        struct task *t = g_tasks[i];
        if (t->state == TASK_ZOMBIE && !t->on_rq &&
            !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
            dead[nd++] = t;
            g_tasks[i] = g_tasks[--g_task_count];
            g_tasks[g_task_count] = NULL;
            continue;
        }
        i++;
    }
    spin_unlock_irqrestore(&g_tasks_lock, f);

    for (int i = 0; i < nd; i++) {
        LOG_DEBUG("reaped task '%s' tid %u", dead[i]->name, dead[i]->tid);
        if (dead[i]->kstack) kfree(dead[i]->kstack);
        kfree(dead[i]);
    }
}

static int register_task(struct task *t) {
    uint64_t flags = spin_lock_irqsave(&g_tasks_lock);
    if (g_task_count >= MAX_TASKS) {
        spin_unlock_irqrestore(&g_tasks_lock, flags);
        return -1;
    }
    g_tasks[g_task_count++] = t;
    spin_unlock_irqrestore(&g_tasks_lock, flags);
    return 0;
}

void scheduler_init(void) {
    for (int c = 0; c < MAX_CORES; c++) {
        memset(&g_rq[c], 0, sizeof(struct rq));
        spin_lock_init(&g_rq[c].lock);
    }
    spin_lock_init(&g_tasks_lock);
    spin_lock_init(&g_sleep_lock);

    memset(g_fx_template, 0, sizeof(g_fx_template));
    *(uint16_t *)&g_fx_template[0] = 0x037F;
    *(uint32_t *)&g_fx_template[24] = 0x1F80;
    *(uint32_t *)&g_fx_template[28] = 0xFFFF;

    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (tsc && tsc->get_tsc_uptime_ms && tsc->get_tsc_uptime_ms() != 0) {
        g_clock_ms = tsc->get_tsc_uptime_ms;
        g_ticks = g_clock_ms();
    } else {
        LOG_WARNING("TSC unavailable, scheduler clock falls back to tick counting");
    }

    scheduler_register_core(apic_get_lapic_id(), 0);
    irq_register_handler(SCHEDULER_YIELD_VECTOR, scheduler_yield_isr);
    LOG_DEBUG("scheduler initialized, BSP LAPIC %u, max %d tasks", (unsigned)apic_get_lapic_id(), MAX_TASKS);
}

void scheduler_start(void) {
    int core = current_core();
    struct rq *rq = &g_rq[core];

    struct task *main_task = task_alloc(core == 0 ? "main" : "ap-main", PRIO_DEFAULT, 0);
    main_task->state = TASK_RUNNING;
    main_task->core = core;
    main_task->affinity = core;
    main_task->on_cpu = 1;
    register_task(main_task);

    struct task *idle = task_alloc("idle", PRIO_IDLE, 32 * 1024);
    idle->entry = idle_entry;
    idle->core = core;
    idle->affinity = core;
    idle->is_idle = 1;
    idle->state = TASK_READY;
    register_task(idle);

    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq->idle = idle;
    rq->current = main_task;
    rq->online = 1;
    spin_unlock_irqrestore(&rq->lock, f);

    LOG_DEBUG("core %d run queue online, main tid %u, idle tid %u", core, main_task->tid, idle->tid);
}

struct task *task_create_ex(const char *name, void (*entry)(void *), void *arg,
                            int priority, int core, uint32_t stack_size) {
    reap_zombies();

    if (!stack_size) stack_size = TASK_STACK_SIZE;
    struct task *t = task_alloc(name, priority, stack_size);
    if (!t) {
        LOG_ERROR("cannot allocate task '%s' with %u byte stack", name ? name : "task", stack_size);
        return NULL;
    }
    t->entry = entry;
    t->arg = arg;

    if (core >= 0 && core < MAX_CORES) {
        if (!g_rq[core].online) core = 0;
        t->affinity = core;
        t->core = core;
    } else {
        t->affinity = TASK_ANY_CORE;
        t->core = least_loaded_online(current_core());
    }

    if (register_task(t) != 0) {
        LOG_ERROR("task table full (%d), '%s' not created", MAX_TASKS, t->name);
        kfree(t->kstack);
        kfree(t);
        return NULL;
    }

    struct rq *rq = &g_rq[t->core];
    uint64_t flags = spin_lock_irqsave(&rq->lock);
    rq_enqueue(rq, t, 0);
    struct task *cur = rq->current;
    if (!cur || cur->is_idle || t->priority < cur->priority)
        rq->need_resched = 1;
    spin_unlock_irqrestore(&rq->lock, flags);

    LOG_DEBUG("task '%s' tid %u created on core %d, priority %d, stack %u",
              t->name, t->tid, t->core, t->priority, t->stack_size);
    return t;
}

struct task *task_create_on_core(const char *name, void (*entry)(void *), void *arg, int priority, int core) {
    return task_create_ex(name, entry, arg, priority, core, 0);
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg, int priority) {
    return task_create_ex(name, entry, arg, priority, TASK_ANY_CORE, 0);
}

int scheduler_least_loaded_core(void) {
    return least_loaded_online(current_core());
}

struct task *task_create_balanced(const char *name, void (*entry)(void *), void *arg, int priority) {
    return task_create_ex(name, entry, arg, priority, TASK_ANY_CORE, 0);
}

void task_trampoline(void) {
    struct task *t = current_task();
    t->entry(t->arg);
    task_exit();
}

void task_exit(void) {
    struct task *self = current_task();
    if (self && !self->is_idle)
        LOG_DEBUG("task '%s' tid %u exiting on core %d", self->name, self->tid, current_core());

    asm volatile("cli");
    struct task *t = current_task();
    if (t) t->state = TASK_ZOMBIE;
    asm volatile("int $0x41" ::: "memory");
    for (;;)
        asm volatile("sti; hlt");
}

void scheduler_idle_loop(void) {
    task_exit();
}

static void stack_check(struct task *t, struct registers *regs) {
    if (!t->kstack || t->is_idle) return;
    if (*(volatile uint64_t *)t->kstack == SCHED_STACK_MAGIC) return;

    static cpu_regs_t cr;
    cr.rax = regs->rax; cr.rbx = regs->rbx; cr.rcx = regs->rcx; cr.rdx = regs->rdx;
    cr.rsi = regs->rsi; cr.rdi = regs->rdi; cr.rbp = regs->rbp; cr.rsp = regs->rsp;
    cr.r8 = regs->r8; cr.r9 = regs->r9; cr.r10 = regs->r10; cr.r11 = regs->r11;
    cr.r12 = regs->r12; cr.r13 = regs->r13; cr.r14 = regs->r14; cr.r15 = regs->r15;
    cr.rip = regs->rip; cr.rflags = regs->rflags; cr.cr2 = 0; cr.cr3 = 0;
    struct panic_info info = { PANIC_CODE_STACK_OVERFLOW,
                               "Kernel task stack overflow (canary destroyed)", &cr };
    panic(&info);
}

static void adjust_priority(struct task *t, enum sched_reason reason) {
    if (t->on_rq) return;
    if (t->boosted) {
        t->boosted = 0;
        t->priority = t->base_priority;
        return;
    }
    if (t->state == TASK_RUNNING) {
        if (reason == SCHED_REASON_EXPIRED) {
            if (t->priority < level_cap(t)) t->priority++;
        }
    } else if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED) {
        if (t->priority > t->base_priority) t->priority--;
    }
}

static struct task *steal_for(int core) {
    int busiest = -1, most = 0;
    for (int c = 0; c < g_core_count; c++) {
        if (c == core || !g_rq[c].online) continue;
        int q = g_rq[c].nr_queued;
        if (q > most) { most = q; busiest = c; }
    }
    if (busiest < 0) return NULL;

    struct rq *src = &g_rq[busiest];
    if (!spin_trylock(&src->lock)) return NULL;
    struct task *t = rq_take_migratable(src);
    spin_unlock(&src->lock);

    if (t) {
        t->core = core;
        g_rq[core].steals++;
    }
    return t;
}

static void reschedule(int core, struct registers *regs, enum sched_reason reason) {
    struct rq *rq = &g_rq[core];
    if (!rq->online) return;

    spin_lock(&rq->lock);
    finish_prev(rq);
    rq->need_resched = 0;

    struct task *cur = rq->current;

    if (cur && !cur->is_idle) {
        adjust_priority(cur, reason);
        if (cur->state == TASK_RUNNING)
            rq_enqueue(rq, cur, 0);
    }

    struct task *next = rq_pick(rq);
    if (!next && g_core_count > 1) next = steal_for(core);
    if (!next) next = rq->idle;

    if (next == cur) {
        cur->state = TASK_RUNNING;
        cur->time_slice = sched_slice_for(cur->priority);
        spin_unlock(&rq->lock);
        return;
    }

    if (cur) {
        stack_check(cur, regs);
        cur->regs = *regs;
        if (rq->isr_fx && cur->fx) memcpy(cur->fx, rq->isr_fx, TASK_FX_SIZE);
        if (cur->is_idle) cur->state = TASK_READY;
        rq->prev = cur;
    }

    __atomic_store_n(&next->on_cpu, 1, __ATOMIC_RELEASE);
    next->state = TASK_RUNNING;
    next->core = core;
    next->time_slice = sched_slice_for(next->priority);
    next->switches++;
    next->last_ran_tick = g_ticks;
    rq->current = next;
    rq->switches++;

    *regs = next->regs;
    if (rq->isr_fx && next->fx) memcpy(rq->isr_fx, next->fx, TASK_FX_SIZE);

    spin_unlock(&rq->lock);
}

static void wake_expired_sleepers(uint64_t now) {
    struct task *h = __atomic_load_n(&g_sleep_head, __ATOMIC_ACQUIRE);
    if (!h || h->wake_tick > now) return;
    if (!spin_trylock(&g_sleep_lock)) return;

    struct task *ready = NULL;
    while (g_sleep_head && g_sleep_head->wake_tick <= now) {
        struct task *t = g_sleep_head;
        g_sleep_head = t->sleep_next;
        t->sleep_next = ready;
        ready = t;
    }
    spin_unlock(&g_sleep_lock);

    while (ready) {
        struct task *t = ready;
        ready = t->sleep_next;
        t->sleep_next = NULL;
        if (t->state == TASK_SLEEPING) wake_task(t);
    }
}

static void age_queue(struct rq *rq, uint64_t now) {
    for (int p = 1; p < PRIO_LEVELS; p++) {
        if (!(rq->bitmap & (1u << p))) continue;
        struct task *prev = NULL, *t = rq->head[p];
        while (t) {
            struct task *nx = t->next;
            if ((int64_t)(now - t->enqueue_tick) >= (int64_t)SCHED_STARVE_MS) {
                uint64_t since = t->enqueue_tick;
                rq_unlink(rq, p, prev, t);
                t->priority = p - 1;
                t->boosted = 1;
                rq_enqueue(rq, t, 0);
                t->enqueue_tick = since;
                struct task *cur = rq->current;
                if (cur && !cur->is_idle && t->priority < cur->priority)
                    rq->need_resched = 1;
            } else {
                prev = t;
            }
            t = nx;
        }
    }
}

static void balance(int core) {
    struct rq *rq = &g_rq[core];
    int mine = rq_nr_running(rq);
    int busiest = -1, most = mine + 1;
    for (int c = 0; c < g_core_count; c++) {
        if (c == core || !g_rq[c].online) continue;
        int n = rq_nr_running(&g_rq[c]);
        if (n > most && g_rq[c].nr_queued > 0) { most = n; busiest = c; }
    }
    if (busiest < 0) return;

    struct rq *src = &g_rq[busiest];
    if (!spin_trylock(&src->lock)) return;
    struct task *t = rq_take_migratable(src);
    spin_unlock(&src->lock);
    if (!t) return;

    t->core = core;
    rq->steals++;
    rq_enqueue(rq, t, 0);
    struct task *cur = rq->current;
    if (!cur || cur->is_idle || t->priority < cur->priority)
        rq->need_resched = 1;
}

static int others_have_work(int core) {
    for (int c = 0; c < g_core_count; c++)
        if (c != core && g_rq[c].online && g_rq[c].nr_queued > 0) return 1;
    return 0;
}

void scheduler_tick(struct registers *regs) {
    int core = current_core();
    struct rq *rq = &g_rq[core];
    if (!rq->online) return;

    if (core == 0) {
        if (g_clock_ms) {
            uint64_t t = g_clock_ms();
            if (t > g_ticks) __atomic_store_n(&g_ticks, t, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&g_ticks, 1, __ATOMIC_RELAXED);
        }
    }
    uint64_t now = g_ticks;
    rq->local_ticks++;

    wake_expired_sleepers(now);

    spin_lock(&rq->lock);
    finish_prev(rq);

    struct task *cur = rq->current;
    int cur_is_idle = (!cur || cur->is_idle);
    if (cur_is_idle) rq->idle_ticks++;
    else { rq->busy_ticks++; cur->run_ticks++; }

    if ((rq->local_ticks % SCHED_AGING_MS) == 0) age_queue(rq, now);
    if (g_core_count > 1 && (rq->local_ticks % SCHED_BALANCE_MS) == (uint64_t)(core % SCHED_BALANCE_MS))
        balance(core);

    int expired = 0;
    if (!cur_is_idle && cur->state == TASK_RUNNING) {
        if (--cur->time_slice <= 0) expired = 1;
    }
    int want = rq->need_resched || expired ||
               (cur && cur->state != TASK_RUNNING) ||
               (cur_is_idle && rq->nr_queued > 0);
    spin_unlock(&rq->lock);

    if (!want && cur_is_idle && g_core_count > 1 && others_have_work(core))
        want = 1;

    if (want)
        reschedule(core, regs, expired ? SCHED_REASON_EXPIRED : SCHED_REASON_PREEMPT);
}

static void scheduler_yield_isr(struct registers *regs) {
    reschedule(current_core(), regs, SCHED_REASON_YIELD);
}

void scheduler_yield(void) {
    asm volatile("int $0x41" ::: "memory");
}

void scheduler_sleep_ms(uint64_t ms) {
    struct task *t = current_task();
    if (!t || t->is_idle || !g_rq[t->core].online) {
        for (volatile uint64_t i = 0; i < ms * 100000ull; i++) asm volatile("pause");
        return;
    }
    if (ms == 0) { scheduler_yield(); return; }

    uint64_t f = irq_save_local();

    spin_lock(&g_sleep_lock);
    t->wake_tick = g_ticks + ms;
    t->state = TASK_SLEEPING;
    struct task **pp = &g_sleep_head;
    while (*pp && (*pp)->wake_tick <= t->wake_tick) pp = &(*pp)->sleep_next;
    t->sleep_next = *pp;
    __atomic_store_n(pp, t, __ATOMIC_RELEASE);
    spin_unlock(&g_sleep_lock);

    asm volatile("int $0x41" ::: "memory");

    irq_restore_local(f);
}

void scheduler_block(void *channel) {
    struct task *t = current_task();
    if (!t) return;
    uint64_t f = irq_save_local();
    spin_lock(&g_tasks_lock);
    t->wait_channel = channel;
    t->state = TASK_BLOCKED;
    spin_unlock(&g_tasks_lock);
    asm volatile("int $0x41" ::: "memory");
    irq_restore_local(f);
}

static void wake_channel(void *channel, int all) {
    struct task *list[32];
    int n = 0;

    uint64_t flags = spin_lock_irqsave(&g_tasks_lock);
    for (int i = 0; i < g_task_count && n < 32; i++) {
        struct task *t = g_tasks[i];
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            t->wait_channel = 0;
            list[n++] = t;
            if (!all) break;
        }
    }
    spin_unlock_irqrestore(&g_tasks_lock, flags);

    for (int i = 0; i < n; i++) wake_task(list[i]);
}

void scheduler_wake(void *channel) {
    wake_channel(channel, 0);
}

void scheduler_wake_all(void *channel) {
    wake_channel(channel, 1);
}

int scheduler_snapshot(struct sched_task_info *out, int max) {
    int n = 0;
    uint64_t f = spin_lock_irqsave(&g_tasks_lock);
    for (int i = 0; i < g_task_count && n < max; i++) {
        struct task *t = g_tasks[i];
        if (t->state == TASK_ZOMBIE) continue;
        struct sched_task_info *o = &out[n++];
        o->tid = t->tid;
        memcpy(o->name, t->name, TASK_NAME_MAX);
        o->state = t->is_idle ? -1 : (int)t->state;
        o->base_priority = t->base_priority;
        o->priority = t->priority;
        o->core = t->core;
        o->affinity = t->affinity;
        o->run_ticks = t->run_ticks;
        o->switches = t->switches;
        o->stack_size = t->stack_size;
    }
    spin_unlock_irqrestore(&g_tasks_lock, f);
    return n;
}

int scheduler_core_info(int core, struct sched_core_info *out) {
    if (core < 0 || core >= g_core_count || !g_rq[core].online || !out) return -1;
    struct rq *rq = &g_rq[core];
    out->nr_running = rq_nr_running(rq);
    out->busy_ticks = rq->busy_ticks;
    out->idle_ticks = rq->idle_ticks;
    out->steals = rq->steals;
    out->switches = rq->switches;
    return 0;
}
