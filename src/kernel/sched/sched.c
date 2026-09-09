#include "kernel/sched/sched.h"

#include "kernel/sched/spinlock.h"
#include "drivers/Timer/apic_driver.h"
#include "components/Memory/heap.h"
#include "components/Interruptions/isr.h"

#include <string.h>
#include <stddef.h>

struct rq {
    struct task *head[PRIO_LEVELS];
    struct task *tail[PRIO_LEVELS];
    uint32_t bitmap;
    struct task *current;
    struct task *idle;
};

static struct rq g_rq[MAX_CORES];
static struct task *g_tasks[MAX_TASKS];
static int g_task_count = 0;
static uint32_t g_next_tid = 1;
static volatile uint64_t g_ticks = 0;
static spinlock_t g_tasks_lock = SPINLOCK_INIT;
static spinlock_t g_rq_lock = SPINLOCK_INIT;

static uint8_t g_lapic_to_core[256];
static int g_core_count = 1;

void sched_register_core(uint8_t lapic_id, int logical_id) {
    g_lapic_to_core[lapic_id] = (uint8_t)logical_id;
    if (logical_id + 1 > g_core_count) g_core_count = logical_id + 1;
}

int sched_core_count(void) {
    return g_core_count;
}

static void sched_yield_isr(struct registers *regs);

int current_core(void) {
    return g_lapic_to_core[apic_get_lapic_id()];
}

struct task *current_task(void) {
    return g_rq[current_core()].current;
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

static void enqueue(int core, struct task *t) {
    struct rq *rq = &g_rq[core];
    t->state = TASK_READY;
    t->next = 0;
    if (rq->tail[t->priority])
        rq->tail[t->priority]->next = t;
    else
        rq->head[t->priority] = t;
    rq->tail[t->priority] = t;
    rq->bitmap |= (1u << t->priority);
}

static struct task *dequeue(int core) {
    struct rq *rq = &g_rq[core];
    if (!rq->bitmap)
        return 0;
    int prio = __builtin_ctz(rq->bitmap);
    struct task *t = rq->head[prio];
    rq->head[prio] = t->next;
    if (!rq->head[prio]) {
        rq->tail[prio] = 0;
        rq->bitmap &= ~(1u << prio);
    }
    return t;
}

static void register_task(struct task *t) {
    spin_lock(&g_tasks_lock);
    g_tasks[g_task_count++] = t;
    spin_unlock(&g_tasks_lock);
}

static void idle_entry(void *arg) {
    (void)arg;
    for (;;)
        asm volatile("hlt");
}

void sched_init(void) {
    for (int c = 0; c < MAX_CORES; c++) {
        memset(&g_rq[c], 0, sizeof(struct rq));
    }
    spin_lock_init(&g_tasks_lock);
    spin_lock_init(&g_rq_lock);
    sched_register_core(apic_get_lapic_id(), 0);
    irq_register_handler(SCHED_YIELD_VECTOR, sched_yield_isr);
}

void sched_start(void) {
    int core = current_core();

    struct task *main_task = kmalloc(sizeof(struct task));
    memset(main_task, 0, sizeof(struct task));
    main_task->tid = g_next_tid++;
    strncpy(main_task->name, "main", TASK_NAME_MAX);
    main_task->state = TASK_RUNNING;
    main_task->priority = PRIO_DEFAULT;
    main_task->time_slice = TIME_SLICE_TICKS;
    g_rq[core].current = main_task;
    register_task(main_task);

    struct task *idle = task_create("idle", idle_entry, 0, PRIO_IDLE);
    g_rq[core].idle = idle;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg, int priority) {
    struct task *t = kmalloc(sizeof(struct task));
    memset(t, 0, sizeof(struct task));

    t->tid = g_next_tid++;
    strncpy(t->name, name, TASK_NAME_MAX);
    t->priority = priority;
    t->time_slice = TIME_SLICE_TICKS;
    t->entry = entry;
    t->arg = arg;
    t->kstack = kmalloc(TASK_STACK_SIZE);

    uint64_t stack_top = (uint64_t)t->kstack + TASK_STACK_SIZE;
    stack_top &= ~0xFULL;

    extern void task_trampoline(void);

    t->regs.rip = (uint64_t)task_trampoline;
    t->regs.rsp = stack_top;
    t->regs.cs = read_cs();
    t->regs.ss = read_ss();
    t->regs.rflags = 0x202;

    register_task(t);
    enqueue(current_core(), t);

    return t;
}

void task_trampoline(void) {
    struct task *t = current_task();
    t->entry(t->arg);
    task_exit();
}

void task_exit(void) {
    struct task *t = current_task();
    t->state = TASK_ZOMBIE;
    sched_yield();
    for (;;)
        asm volatile("hlt");
}

static void reschedule(int core, struct registers *regs) {
    struct rq *rq = &g_rq[core];

    spin_lock(&g_rq_lock);

    struct task *cur = rq->current;
    struct task *next = dequeue(core);

    if (!next) {
        if (cur && cur->state == TASK_RUNNING) {
            spin_unlock(&g_rq_lock);
            return;
        }
        next = rq->idle;
    }

    if (cur == next) {
        spin_unlock(&g_rq_lock);
        return;
    }

    if (cur) {
        cur->regs = *regs;
        if (cur->state == TASK_RUNNING) {
            cur->time_slice = TIME_SLICE_TICKS;
            enqueue(core, cur);
        }
    }

    next->state = TASK_RUNNING;
    next->time_slice = TIME_SLICE_TICKS;
    rq->current = next;
    *regs = next->regs;

    spin_unlock(&g_rq_lock);
}

void sched_tick(struct registers *regs) {
    g_ticks++;

    spin_lock(&g_tasks_lock);
    for (int i = 0; i < g_task_count; i++) {
        struct task *t = g_tasks[i];
        if (t->state == TASK_SLEEPING && g_ticks >= t->wake_tick) {
            spin_lock(&g_rq_lock);
            enqueue(current_core(), t);
            spin_unlock(&g_rq_lock);
        }
    }
    spin_unlock(&g_tasks_lock);

    int core = current_core();
    struct task *cur = g_rq[core].current;

    if (cur && cur->state == TASK_RUNNING) {
        cur->time_slice--;
        if (cur->time_slice > 0)
            return;
    }

    reschedule(core, regs);
}

static void sched_yield_isr(struct registers *regs) {
    reschedule(current_core(), regs);
}

void sched_yield(void) {
    asm volatile("int $0x41");
}

void sched_sleep_ms(uint64_t ms) {
    struct task *t = current_task();
    t->wake_tick = g_ticks + ms;
    t->state = TASK_SLEEPING;
    sched_yield();
}

void sched_block(void *channel) {
    struct task *t = current_task();
    t->wait_channel = channel;
    t->state = TASK_BLOCKED;
    sched_yield();
}

void sched_wake(void *channel) {
    spin_lock(&g_tasks_lock);
    for (int i = 0; i < g_task_count; i++) {
        struct task *t = g_tasks[i];
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            t->wait_channel = 0;
            spin_lock(&g_rq_lock);
            enqueue(current_core(), t);
            spin_unlock(&g_rq_lock);
            break;
        }
    }
    spin_unlock(&g_tasks_lock);
}

void sched_wake_all(void *channel) {
    spin_lock(&g_tasks_lock);
    for (int i = 0; i < g_task_count; i++) {
        struct task *t = g_tasks[i];
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            t->wait_channel = 0;
            spin_lock(&g_rq_lock);
            enqueue(current_core(), t);
            spin_unlock(&g_rq_lock);
        }
    }
    spin_unlock(&g_tasks_lock);
}
