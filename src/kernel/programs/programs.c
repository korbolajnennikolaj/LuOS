#include "programs.h"

#include "service_keyboard_updater.h"
#include "service_mouse_updater.h"
#include "service_root_fs.h"
#include "service_shell.h"
#include "service_usb_hotplug.h"

#include "components/drivers.h"
#include "drivers/Timer/timer.h"

#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"

#include <stddef.h>
#include <string.h>

program_t *programs_list[MAX_PROGRAMS] = {0};
int programs_count = 0;
spinlock_t programs_lock = SPINLOCK_INIT;

static uint32_t next_program_id = 1;

void register_program(program_t *prog) {
    if (!prog || !prog->entry) return;

    spin_lock(&programs_lock);

    if (programs_count >= MAX_PROGRAMS) {
        spin_unlock(&programs_lock);
        return;
    }

    for (int i = 0; i < programs_count; i++) {
        if (strncmp(programs_list[i]->name, prog->name, PROGRAM_NAME_MAX) == 0) {
            spin_unlock(&programs_lock);
            return;
        }
    }

    prog->id = next_program_id++;
    programs_list[programs_count++] = prog;

    spin_unlock(&programs_lock);
}

program_t *get_program(uint32_t id) {
    spin_lock(&programs_lock);
    for (int i = 0; i < programs_count; i++) {
        if (programs_list[i]->id == id) {
            program_t *p = programs_list[i];
            spin_unlock(&programs_lock);
            return p;
        }
    }
    spin_unlock(&programs_lock);
    return NULL;
}

program_t *get_program_by_name(const char *name) {
    if (!name) return NULL;

    spin_lock(&programs_lock);
    for (int i = 0; i < programs_count; i++) {
        if (strncmp(programs_list[i]->name, name, PROGRAM_NAME_MAX) == 0) {
            program_t *p = programs_list[i];
            spin_unlock(&programs_lock);
            return p;
        }
    }
    spin_unlock(&programs_lock);
    return NULL;
}

int run_program(uint32_t id) {
    program_t *prog = get_program(id);
    if (!prog) return -1;

    struct task *t = task_create_balanced(prog->name, prog->entry, prog->arg, PRIO_DEFAULT);
    return t ? 0 : -2;
}

int run_program_by_name(const char *name) {
    program_t *prog = get_program_by_name(name);
    if (!prog) return -1;

    struct task *t = task_create_balanced(prog->name, prog->entry, prog->arg, PRIO_DEFAULT);
    return t ? 0 : -2;
}

service_t *services_list[MAX_SERVICES] = {0};
int services_count = 0;
spinlock_t services_lock = SPINLOCK_INIT;

static uint32_t next_service_id = 1;
static bool stop_requested[MAX_SERVICES] = {0};

static const int service_priority_to_sched[SERVICE_PRIORITY_COUNT] = {
    [SERVICE_ROOT_PRIORITY] = 0,
    [SERVICE_HIGH_PRIORITY] = PRIO_DEFAULT / 2,
    [SERVICE_NORMAL_PRIORITY] = PRIO_DEFAULT,
    [SERVICE_LOW_PRIORITY] = PRIO_IDLE - 1,
};

int register_service(service_t *svc) {
    if (!svc || !svc->entry) return -1;

    spin_lock(&services_lock);

    if (services_count >= MAX_SERVICES) {
        spin_unlock(&services_lock);
        return -2;
    }

    for (int i = 0; i < services_count; i++) {
        if (strncmp(services_list[i]->name, svc->name, SERVICE_NAME_MAX) == 0) {
            spin_unlock(&services_lock);
            return -3;
        }
    }

    svc->id = next_service_id++;
    svc->state = SERVICE_STATE_STOPPED;
    svc->restart_count = 0;
    if (svc->restart_limit == 0) svc->restart_limit = SERVICE_RESTART_LIMIT_DEFAULT;
    svc->task = NULL;

    services_list[services_count] = svc;
    stop_requested[services_count] = false;
    services_count++;

    spin_unlock(&services_lock);
    return 0;
}

static int service_index(uint32_t id) {
    for (int i = 0; i < services_count; i++) {
        if (services_list[i]->id == id) return i;
    }
    return -1;
}

service_t *get_service(uint32_t id) {
    spin_lock(&services_lock);
    int idx = service_index(id);
    service_t *svc = (idx >= 0) ? services_list[idx] : NULL;
    spin_unlock(&services_lock);
    return svc;
}

service_t *get_service_by_name(const char *name) {
    if (!name) return NULL;

    spin_lock(&services_lock);
    service_t *found = NULL;
    for (int i = 0; i < services_count; i++) {
        if (strncmp(services_list[i]->name, name, SERVICE_NAME_MAX) == 0) {
            found = services_list[i];
            break;
        }
    }
    spin_unlock(&services_lock);
    return found;
}

int service_request_start(uint32_t id) {
    spin_lock(&services_lock);
    int idx = service_index(id);
    if (idx < 0) { spin_unlock(&services_lock); return -1; }

    service_t *svc = services_list[idx];
    if (svc->state == SERVICE_STATE_FAILED || svc->state == SERVICE_STATE_STOPPED) {
        svc->state = SERVICE_STATE_STOPPED;
        svc->restart_count = 0;
        stop_requested[idx] = false;
    }
    spin_unlock(&services_lock);
    return 0;
}

int service_request_stop(uint32_t id) {
    spin_lock(&services_lock);
    int idx = service_index(id);
    if (idx < 0) { spin_unlock(&services_lock); return -1; }
    stop_requested[idx] = true;
    spin_unlock(&services_lock);
    return 0;
}

int service_request_restart(uint32_t id) {
    spin_lock(&services_lock);
    int idx = service_index(id);
    if (idx < 0) { spin_unlock(&services_lock); return -1; }

    service_t *svc = services_list[idx];
    svc->state = SERVICE_STATE_NEEDS_RESTART;
    svc->restart_count = 0;
    stop_requested[idx] = false;
    spin_unlock(&services_lock);
    return 0;
}

bool service_stop_requested(uint32_t id) {
    spin_lock(&services_lock);
    int idx = service_index(id);
    bool req = (idx >= 0) ? stop_requested[idx] : false;
    spin_unlock(&services_lock);
    return req;
}

uint64_t service_uptime_ms(void) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return tsc ? tsc->get_tsc_uptime_ms() : 0;
}

static void service_trampoline(void *arg) {
    service_t *svc = (service_t *)arg;

    svc->entry(svc->arg);

    spin_lock(&services_lock);
    int idx = service_index(svc->id);
    bool was_stop_requested = (idx >= 0) ? stop_requested[idx] : false;

    svc->task = NULL;
    svc->state = was_stop_requested ? SERVICE_STATE_STOPPED
                                     : SERVICE_STATE_NEEDS_RESTART;
    spin_unlock(&services_lock);

    task_exit();
}

static bool dependencies_satisfied(const service_t *svc) {
    for (int i = 0; i < svc->dependency_count; i++) {
        int didx = service_index(svc->dependencies[i].service_id);
        if (didx < 0) return false;
        if (services_list[didx]->state != SERVICE_STATE_RUNNING) return false;
    }
    return true;
}

static void try_start_service(service_t *svc) {
    int sched_prio = (svc->priority < SERVICE_PRIORITY_COUNT)
                          ? service_priority_to_sched[svc->priority]
                          : PRIO_DEFAULT;

    struct task *t = task_create_balanced(svc->name, service_trampoline, svc, sched_prio);
    if (t) {
        svc->task = t;
        svc->state = SERVICE_STATE_STARTING;
    }
}

#define SERVICE_MANAGER_TICK_MS 200

static void service_manager_loop(void *arg) {
    (void)arg;

    while (1) {
        spin_lock(&services_lock);

        for (int i = 0; i < services_count; i++) {
            service_t *svc = services_list[i];

            switch (svc->state) {

                case SERVICE_STATE_STOPPED:
                case SERVICE_STATE_NEEDS_RESTART:
                    if (svc->task != NULL) {
                        if (svc->state == SERVICE_STATE_NEEDS_RESTART &&
                            svc->update && svc->update(svc->arg)) {
                            svc->state = SERVICE_STATE_RUNNING;
                        }
                        break;
                    }
                    if (stop_requested[i]) break;

                    if (svc->state == SERVICE_STATE_NEEDS_RESTART) {
                        svc->restart_count++;
                        if (svc->restart_limit != 0 &&
                            svc->restart_count > svc->restart_limit) {
                            svc->state = SERVICE_STATE_FAILED;
                            break;
                        }
                    }
                    if (dependencies_satisfied(svc)) {
                        try_start_service(svc);
                    }
                    break;

                case SERVICE_STATE_STARTING:
                    svc->state = SERVICE_STATE_RUNNING;
                    break;

                case SERVICE_STATE_RUNNING:
                    if (svc->update && !svc->update(svc->arg)) {
                        svc->state = SERVICE_STATE_NEEDS_RESTART;
                    } else {
                        svc->restart_count = 0;
                    }
                    break;

                case SERVICE_STATE_FAILED:
                default:
                    break;
            }
        }

        spin_unlock(&services_lock);

        scheduler_sleep_ms(SERVICE_MANAGER_TICK_MS);
    }
}

static void add_service_dependency(service_t *svc, service_t *on) {
    if (!svc || !on) return;
    if (svc->dependency_count >= MAX_SERVICE_DEPENDENCIES) return;

    svc->dependencies[svc->dependency_count++] = MAKE_SERVICE_DEPENDENCY(on->id);
}

void start_service_manager(void) {
    service_t *keyboard_updater = get_keyboard_updater_service();
    service_t *mouse_updater = get_mouse_updater_service();
    service_t *root_fs = get_root_fs_service();
    service_t *usb_hotplug = get_usb_hotplug_service();
    service_t *shell = get_shell_service();

    register_service(keyboard_updater);
    register_service(mouse_updater);
    register_service(root_fs);
    register_service(usb_hotplug);

    shell->dependency_count = 0;
    add_service_dependency(shell, keyboard_updater);
    add_service_dependency(shell, root_fs);

    register_service(shell);

    task_create("service_manager", service_manager_loop, NULL, service_priority_to_sched[SERVICE_ROOT_PRIORITY]);
}