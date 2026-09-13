#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "kernel/scheduler/task.h"
#include "components/Interruptions/isr.h"

#include <stdint.h>

#define MAX_CORES 64
#define MAX_TASKS 512
#define SCHEDULER_YIELD_VECTOR 0x41

void scheduler_init(void);
void scheduler_start(void);

struct task *task_create(const char *name, void (*entry)(void *), void *arg, int priority);
struct task *task_create_on_core(const char *name, void (*entry)(void *), void *arg, int priority, int core);
struct task *task_create_balanced(const char *name, void (*entry)(void *), void *arg, int priority);
int scheduler_least_loaded_core(void);
void task_exit(void);

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

#endif
