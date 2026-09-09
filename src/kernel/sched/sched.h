#ifndef SCHED_H
#define SCHED_H

#include "kernel/sched/task.h"
#include "components/Interruptions/isr.h"

#include <stdint.h>

#define MAX_CORES 4
#define MAX_TASKS 128
#define SCHED_YIELD_VECTOR 0x41

void sched_init(void);
void sched_start(void);

struct task *task_create(const char *name, void (*entry)(void *), void *arg, int priority);
void task_exit(void);

void sched_tick(struct registers *regs);
void sched_yield(void);
void sched_sleep_ms(uint64_t ms);

void sched_block(void *channel);
void sched_wake(void *channel);
void sched_wake_all(void *channel);

struct task *current_task(void);
int current_core(void);
void sched_register_core(uint8_t lapic_id, int logical_id);
int sched_core_count(void);

#endif
