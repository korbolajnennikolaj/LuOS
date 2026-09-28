#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "kernel/scheduler/task.h"
#include "components/Interruptions/isr.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_CORES 64
#define MAX_TASKS 512
#define SCHEDULER_YIELD_VECTOR 0x41

void scheduler_init(void);
void scheduler_start(void);

void scheduler_idle_loop(void) __attribute__((noreturn));

struct task *task_create(const char *name, void (*entry)(void *), void *arg, int priority);
struct task *task_create_on_core(const char *name, void (*entry)(void *), void *arg, int priority, int core);
struct task *task_create_balanced(const char *name, void (*entry)(void *), void *arg, int priority);
struct task *task_create_ex(const char *name, void (*entry)(void *), void *arg,
                            int priority, int core, uint32_t stack_size);
int scheduler_least_loaded_core(void);
void task_exit(void) __attribute__((noreturn));

void scheduler_tick(struct registers *regs);
void scheduler_yield(void);
void scheduler_sleep_ms(uint64_t ms);

void scheduler_block(void *channel);
void scheduler_wake(void *channel);
void scheduler_wake_all(void *channel);

struct task *current_task(void);
int current_core(void);
void scheduler_register_core(uint8_t lapic_id, int logical_id);
int scheduler_core_count(void);

uint64_t scheduler_ticks(void);

int scheduler_current_stack(uintptr_t *lo, uintptr_t *hi);

void *scheduler_set_isr_fx(void *fx);

struct sched_task_info {
    uint32_t tid;
    char name[TASK_NAME_MAX];
    int state;
    int base_priority;
    int priority;
    int core;
    int affinity;
    uint64_t run_ticks;
    uint64_t switches;
    uint32_t stack_size;
};

struct sched_core_info {
    int nr_running;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint64_t steals;
    uint64_t switches;
};

int scheduler_snapshot(struct sched_task_info *out, int max);
int scheduler_core_info(int core, struct sched_core_info *out);

#endif
