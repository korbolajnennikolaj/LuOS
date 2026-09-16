#ifndef PROGRAMS_H
#define PROGRAMS_H

#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROGRAM_NAME_MAX 32
#define SERVICE_NAME_MAX 32

#define MAX_PROGRAMS 64
#define MAX_SERVICES 64

#define MAX_SERVICE_DEPENDENCIES 8

#define MAKE_SERVICE_DEPENDENCY(_id) ((struct service_dependency){ .service_id = (_id) })

typedef struct program {
    char name[PROGRAM_NAME_MAX];
    uint32_t id;

    void (*entry)(void *arg);
    void *arg;
} program_t;

void register_program(program_t *prog);
program_t *get_program(uint32_t id);
program_t *get_program_by_name(const char *name);

int run_program(uint32_t id);
int run_program_by_name(const char *name);

extern program_t *programs_list[MAX_PROGRAMS];
extern int programs_count;
extern spinlock_t programs_lock;

enum SERVICE_STATE {
    SERVICE_STATE_STOPPED = 0,
    SERVICE_STATE_STARTING = 1,
    SERVICE_STATE_RUNNING = 2,
    SERVICE_STATE_NEEDS_RESTART = 3,
    SERVICE_STATE_FAILED = 4,
};

enum SERVICE_PRIORITY {
    SERVICE_ROOT_PRIORITY = 0,
    SERVICE_HIGH_PRIORITY = 1,
    SERVICE_NORMAL_PRIORITY = 2,
    SERVICE_LOW_PRIORITY = 3,
};

#define SERVICE_PRIORITY_COUNT 4

#define SERVICE_RESTART_LIMIT_DEFAULT 5

typedef struct service_dependency {
    uint32_t service_id;
} service_dependency;

typedef struct service {
    char name[SERVICE_NAME_MAX];
    uint32_t id;

    void (*entry)(void *arg);
    void *arg;

    bool (*update)(void *arg);

    uint8_t priority;
    uint8_t state;

    struct service_dependency dependencies[MAX_SERVICE_DEPENDENCIES];
    int dependency_count;

    uint8_t restart_limit;
    uint8_t restart_count;

    struct task *task;
} service_t;

int register_service(service_t *svc);

service_t *get_service(uint32_t id);
service_t *get_service_by_name(const char *name);

int service_request_start(uint32_t id);
int service_request_stop(uint32_t id);
int service_request_restart(uint32_t id);
bool service_stop_requested(uint32_t id);

extern service_t *services_list[MAX_SERVICES];
extern int services_count;
extern spinlock_t services_lock;

uint64_t service_uptime_ms(void);

void start_service_manager(void);

#endif