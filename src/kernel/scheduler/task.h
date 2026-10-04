#ifndef TASK_H
#define TASK_H

#include "components/Interruptions/isr.h"

#include <stdint.h>

#define TASK_NAME_MAX 32

#define TASK_STACK_SIZE (64 * 1024)
#define TASK_STACK_MIN (16 * 1024)

#define PRIO_LEVELS 16
#define PRIO_DEFAULT 8
#define PRIO_IDLE (PRIO_LEVELS - 1)

#define TIME_SLICE_TICKS 20

#define TASK_ANY_CORE (-1)

#define TASK_FX_SIZE 512

enum task_state {
    TASK_READY = 0,
    TASK_RUNNING = 1,
    TASK_BLOCKED = 2,
    TASK_SLEEPING = 3,
    TASK_ZOMBIE = 4,
};

struct task {
    uint32_t tid;
    char name[TASK_NAME_MAX];
    volatile enum task_state state;

    int base_priority;
    int priority;
    int time_slice;

    struct registers regs;

    void *kstack;
    uint32_t stack_size;

    void (*entry)(void *);
    void *arg;

    uint64_t wake_tick;
    void *wait_channel;

    int core;
    int affinity;

    int err_no;

    volatile uint8_t on_rq;
    volatile uint8_t on_cpu;
    uint8_t is_idle;
    uint8_t boosted;

    uint64_t enqueue_tick;
    uint64_t run_ticks;
    uint64_t switches;
    uint64_t last_ran_tick;

    struct task *next;
    struct task *sleep_next;

    uint8_t *fx;
    uint8_t fx_raw[TASK_FX_SIZE + 16];
};

#endif
