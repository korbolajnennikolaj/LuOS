#include "service_keyboard_updater.h"

#include "service_usb_hotplug.h"

#include "components/drivers.h"
#include "drivers/Input/keyboard_driver.h"
#include "kernel/scheduler/scheduler.h"

#define KEYBOARD_UPDATER_TICK_MS 20
#define KEYBOARD_UPDATER_WATCHDOG_MS 4000

static volatile uint64_t keyboard_updater_heartbeat = 0;
static volatile uint64_t keyboard_updater_last_beat_ms = 0;

static volatile int keyboard_updater_type = -1;

static void keyboard_updater_entry(void *arg) {
    (void)arg;

    service_t *svc = get_keyboard_updater_service();

    keyboard_updater_last_beat_ms = service_uptime_ms();

    struct keyboard_driver *kbd = get_self_driver(KEYBOARD_DRIVER, VIRTUAL_KEYBOARD);
    if (kbd && kbd->claim_pump) kbd->claim_pump();

    bool claimed = (kbd != NULL);

    while (!service_stop_requested(svc->id)) {
        if (!kbd) {
            kbd = get_self_driver(KEYBOARD_DRIVER, VIRTUAL_KEYBOARD);
            claimed = false;
        }

        if (kbd && !claimed) {
            if (kbd->claim_pump) kbd->claim_pump();
            claimed = true;
        }

        if (kbd) {
            if (kbd->keyboard_handler) kbd->keyboard_handler();
            if (kbd->get_active_type) keyboard_updater_type = kbd->get_active_type();
        }

        keyboard_updater_heartbeat++;
        keyboard_updater_last_beat_ms = service_uptime_ms();
        scheduler_sleep_ms(KEYBOARD_UPDATER_TICK_MS);
    }

    if (kbd && kbd->release_pump) kbd->release_pump();
}

static bool keyboard_updater_update(void *arg) {
    (void)arg;
    if (keyboard_updater_last_beat_ms == 0) return true;
    if (usb_hotplug_busy()) return true;
    return (service_uptime_ms() - keyboard_updater_last_beat_ms) < KEYBOARD_UPDATER_WATCHDOG_MS;
}

int keyboard_updater_active_type(void) {
    return keyboard_updater_type;
}

uint64_t keyboard_updater_poll_count(void) {
    return keyboard_updater_heartbeat;
}

service_t *get_keyboard_updater_service(void) {
    static service_t keyboard_updater_service = {
        .name = "keyboard_updater",
        .entry = keyboard_updater_entry,
        .arg = NULL,
        .update = keyboard_updater_update,
        .priority = SERVICE_HIGH_PRIORITY,
        .state = SERVICE_STATE_STOPPED,
        .dependency_count = 0,
        .restart_limit = SERVICE_RESTART_LIMIT_DEFAULT,
        .restart_count = 0,
        .task = NULL
    };

    return &keyboard_updater_service;
}
