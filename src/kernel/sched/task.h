#ifndef TASK_H
#define TASK_H

#include "components/Interruptions/isr.h"

#include <stdint.h>

#define TASK_NAME_MAX 32
#define TASK_STACK_SIZE (16 * 1024)
#define PRIO_LEVELS 16
#define PRIO_DEFAULT 8
#define PRIO_IDLE (PRIO_LEVELS - 1)
#define TIME_SLICE_TICKS 20

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
    enum task_state state;
    int priority;
    int time_slice;
    struct registers regs;
    void *kstack;
    void (*entry)(void *);
    void *arg;
    uint64_t wake_tick;
    void *wait_channel;
    struct task *next;
};

#endif
