#include "service_mouse_updater.h"

#include "service_usb_hotplug.h"

#include "components/drivers.h"
#include "drivers/Input/mouse_driver.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/scheduler/scheduler.h"

#define MOUSE_UPDATER_TICK_MS 10
#define MOUSE_UPDATER_WATCHDOG_MS 2000
#define MOUSE_UPDATER_STALE_MS 200

static volatile uint64_t mouse_updater_polls = 0;
static volatile uint64_t mouse_updater_events = 0;
static volatile uint64_t mouse_updater_last_beat_ms = 0;

static volatile int mouse_updater_type = -1;

static int32_t cursor_x = 0;
static int32_t cursor_y = 0;
static int32_t cursor_max_x = 0;
static int32_t cursor_max_y = 0;

static mouse_sample_t pending = {0};

static void mouse_updater_load_limits(void) {
    struct limine_video_driver *video = get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    if (!video || !video->get_display_resolution) return;

    uint64_t width = 0;
    uint64_t height = 0;
    video->get_display_resolution(&width, &height);

    if (width > 0) cursor_max_x = (int32_t)width - 1;
    if (height > 0) cursor_max_y = (int32_t)height - 1;

    if (cursor_x == 0 && cursor_y == 0) {
        cursor_x = cursor_max_x / 2;
        cursor_y = cursor_max_y / 2;
    }
}

static int32_t clamp_axis(int32_t value, int32_t max) {
    if (value < 0) return 0;
    if (max > 0 && value > max) return max;
    return value;
}

static void mouse_updater_absorb(mouse_state_t *st) {
    if (!st) return;

    cursor_x = clamp_axis(cursor_x + st->x, cursor_max_x);
    cursor_y = clamp_axis(cursor_y - st->y, cursor_max_y);

    pending.dx += st->x;
    pending.dy += st->y;
    pending.wheel += st->wheel;

    pending.btn_left = st->btn_left;
    pending.btn_right = st->btn_right;
    pending.btn_middle = st->btn_middle;

    if (st->x || st->y || st->wheel || st->btn_left || st->btn_right || st->btn_middle)
        mouse_updater_events++;
}

static bool mouse_updater_running(void) {
    uint64_t last = mouse_updater_last_beat_ms;
    if (last == 0) return false;

    uint64_t now = service_uptime_ms();
    return (now >= last) && ((now - last) < MOUSE_UPDATER_STALE_MS);
}

static void mouse_updater_absorb_direct(void) {
    if (mouse_updater_running()) return;

    struct mouse_driver *mouse = get_self_driver(MOUSE_DRIVER, VIRTUAL_MOUSE);
    if (!mouse || !mouse->get_state) return;

    if (cursor_max_x == 0 && cursor_max_y == 0) mouse_updater_load_limits();

    mouse_updater_absorb(mouse->get_state());
    if (mouse->reset_deltas) mouse->reset_deltas();
}

static void mouse_updater_entry(void *arg) {
    (void)arg;

    service_t *svc = get_mouse_updater_service();

    mouse_updater_last_beat_ms = service_uptime_ms();
    mouse_updater_load_limits();

    struct mouse_driver *mouse = get_self_driver(MOUSE_DRIVER, VIRTUAL_MOUSE);

    while (!service_stop_requested(svc->id)) {
        if (!mouse) mouse = get_self_driver(MOUSE_DRIVER, VIRTUAL_MOUSE);

        if (mouse) {
            if (mouse->mouse_handler) mouse->mouse_handler();
            if (mouse->get_active_type) mouse_updater_type = mouse->get_active_type();

            if (mouse->get_state) {
                mouse_updater_absorb(mouse->get_state());
                if (mouse->reset_deltas) mouse->reset_deltas();
            }

            mouse_updater_polls++;
        }

        mouse_updater_last_beat_ms = service_uptime_ms();
        scheduler_sleep_ms(MOUSE_UPDATER_TICK_MS);
    }
}

static bool mouse_updater_update(void *arg) {
    (void)arg;
    if (mouse_updater_last_beat_ms == 0) return true;
    if (usb_hotplug_busy()) return true;
    return (service_uptime_ms() - mouse_updater_last_beat_ms) < MOUSE_UPDATER_WATCHDOG_MS;
}

int32_t mouse_updater_x(void) {
    mouse_updater_absorb_direct();
    return cursor_x;
}

int32_t mouse_updater_y(void) {
    mouse_updater_absorb_direct();
    return cursor_y;
}

bool mouse_updater_take(mouse_sample_t *out) {
    if (!out) return false;

    mouse_updater_absorb_direct();

    *out = pending;

    pending.dx = 0;
    pending.dy = 0;
    pending.wheel = 0;

    return out->dx || out->dy || out->wheel ||
           out->btn_left || out->btn_right || out->btn_middle;
}

int mouse_updater_active_type(void) {
    if (mouse_updater_type < 0) {
        struct mouse_driver *mouse = get_self_driver(MOUSE_DRIVER, VIRTUAL_MOUSE);
        if (mouse && mouse->get_active_type) return mouse->get_active_type();
    }
    return mouse_updater_type;
}

uint64_t mouse_updater_event_count(void) {
    return mouse_updater_events;
}

uint64_t mouse_updater_poll_count(void) {
    return mouse_updater_polls;
}

service_t *get_mouse_updater_service(void) {
    static service_t mouse_updater_service = {
        .name = "mouse_updater",
        .entry = mouse_updater_entry,
        .arg = NULL,
        .update = mouse_updater_update,
        .priority = SERVICE_HIGH_PRIORITY,
        .state = SERVICE_STATE_STOPPED,
        .dependency_count = 0,
        .restart_limit = SERVICE_RESTART_LIMIT_DEFAULT,
        .restart_count = 0,
        .task = NULL
    };

    return &mouse_updater_service;
}
