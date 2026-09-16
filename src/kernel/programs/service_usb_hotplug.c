#include "service_usb_hotplug.h"

#include "components/drivers.h"
#include "drivers/USB/usb_core.h"
#include "kernel/scheduler/scheduler.h"

#define USB_HOTPLUG_TICK_MS 20
#define USB_HOTPLUG_SURVEY_TICKS 25
#define USB_HOTPLUG_WATCHDOG_MS 5000
#define USB_HOTPLUG_MAX_TRACKED 64

static volatile uint64_t usb_hotplug_last_beat_ms = 0;

static volatile uint64_t usb_hotplug_polls = 0;
static volatile uint64_t usb_hotplug_connects = 0;
static volatile uint64_t usb_hotplug_disconnects = 0;
static volatile uint64_t usb_hotplug_rescans = 0;

static volatile bool usb_hotplug_forced = false;
static volatile bool usb_hotplug_scanning = false;

static volatile int usb_hotplug_devices = 0;

static void *tracked[USB_HOTPLUG_MAX_TRACKED] = {0};
static bool tracked_valid = false;

static void usb_hotplug_survey(void) {
    void *current[USB_HOTPLUG_MAX_TRACKED] = {0};
    int limit = MAX_USB_DEVICES;
    if (limit > USB_HOTPLUG_MAX_TRACKED) limit = USB_HOTPLUG_MAX_TRACKED;

    int count = 0;
    for (int i = 0; i < limit; i++) {
        struct usb_device *dev = (struct usb_device *)device_table[USB_DEVICE][i];
        if (dev && dev->valid) {
            current[i] = dev;
            count++;
        }
    }

    usb_hotplug_devices = count;

    if (!tracked_valid) {
        for (int i = 0; i < limit; i++) tracked[i] = current[i];
        tracked_valid = true;
        return;
    }

    for (int i = 0; i < limit; i++) {
        if (current[i] == tracked[i]) continue;

        if (tracked[i] != NULL) usb_hotplug_disconnects++;
        if (current[i] != NULL) usb_hotplug_connects++;

        tracked[i] = current[i];
    }
}

static void usb_hotplug_entry(void *arg) {
    (void)arg;

    service_t *svc = get_usb_hotplug_service();

    usb_hotplug_last_beat_ms = service_uptime_ms();

    struct usb_core_driver *usb = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    uint32_t tick = 0;

    while (!service_stop_requested(svc->id)) {
        if (!usb) usb = get_self_driver(USB_DRIVER, USB_CORE_SLOT);

        if (usb_hotplug_forced && usb && usb->scan_all) {
            usb_hotplug_forced = false;
            usb_hotplug_scanning = true;
            usb->scan_all();
            usb_hotplug_scanning = false;
            usb_hotplug_rescans++;
            tracked_valid = false;
        }

        if (usb && usb->poll_transfers) {
            usb->poll_transfers();
            usb_hotplug_polls++;
        }

        if ((tick % USB_HOTPLUG_SURVEY_TICKS) == 0) usb_hotplug_survey();

        tick++;
        usb_hotplug_last_beat_ms = service_uptime_ms();
        scheduler_sleep_ms(USB_HOTPLUG_TICK_MS);
    }
}

static bool usb_hotplug_update(void *arg) {
    (void)arg;
    if (usb_hotplug_last_beat_ms == 0) return true;
    if (usb_hotplug_scanning) return true;
    return (service_uptime_ms() - usb_hotplug_last_beat_ms) < USB_HOTPLUG_WATCHDOG_MS;
}

int usb_hotplug_device_count(void) {
    return usb_hotplug_devices;
}

uint64_t usb_hotplug_connect_count(void) {
    return usb_hotplug_connects;
}

uint64_t usb_hotplug_disconnect_count(void) {
    return usb_hotplug_disconnects;
}

uint64_t usb_hotplug_poll_count(void) {
    return usb_hotplug_polls;
}

uint64_t usb_hotplug_rescan_count(void) {
    return usb_hotplug_rescans;
}

bool usb_hotplug_busy(void) {
    return usb_hotplug_scanning;
}

void usb_hotplug_request_rescan(void) {
    usb_hotplug_forced = true;
}

service_t *get_usb_hotplug_service(void) {
    static service_t usb_hotplug_service = {
        .name = "usb_hotplug",
        .entry = usb_hotplug_entry,
        .arg = NULL,
        .update = usb_hotplug_update,
        .priority = SERVICE_HIGH_PRIORITY,
        .state = SERVICE_STATE_STOPPED,
        .dependency_count = 0,
        .restart_limit = SERVICE_RESTART_LIMIT_DEFAULT,
        .restart_count = 0,
        .task = NULL
    };

    return &usb_hotplug_service;
}
