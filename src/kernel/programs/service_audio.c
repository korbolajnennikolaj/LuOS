#include "service_audio.h"

#include "service_usb_hotplug.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "drivers/Audio/audio_core.h"
#include "kernel/scheduler/scheduler.h"

#define AUDIO_SERVICE_TICK_MS 2
#define AUDIO_SERVICE_WATCHDOG_MS 3000

static volatile uint64_t audio_service_last_beat_ms = 0;
static volatile uint64_t audio_service_polls = 0;
static volatile uint64_t audio_service_gap_ms = 0;

static void audio_service_entry(void *arg) {
    (void)arg;

    service_t *svc = get_audio_service();
    struct audio_core_driver *core = get_self_driver(AUDIO_DRIVER, AUDIO_CORE_SLOT);
    if (!core) LOG_WARNING("audio core not available, audio pump idle");

    audio_service_last_beat_ms = service_uptime_ms();

    while (!service_stop_requested(svc->id)) {
        if (!core) core = get_self_driver(AUDIO_DRIVER, AUDIO_CORE_SLOT);
        if (core && core->poll) core->poll();

        uint64_t now = service_uptime_ms();
        uint64_t gap = now - audio_service_last_beat_ms;
        if (gap > audio_service_gap_ms) audio_service_gap_ms = gap;
        audio_service_last_beat_ms = now;
        audio_service_polls++;
        scheduler_sleep_ms(AUDIO_SERVICE_TICK_MS);
    }
}

static bool audio_service_update(void *arg) {
    (void)arg;
    if (audio_service_last_beat_ms == 0) return true;
    if (usb_hotplug_busy()) return true;
    uint64_t silent = service_uptime_ms() - audio_service_last_beat_ms;
    if (silent < AUDIO_SERVICE_WATCHDOG_MS) return true;
    LOG_WARNING("audio pump silent for %llu ms (watchdog %u ms)", (unsigned long long)silent, AUDIO_SERVICE_WATCHDOG_MS);
    return false;
}

uint64_t audio_service_poll_count(void) {
    return audio_service_polls;
}

uint64_t audio_service_max_gap_ms(void) {
    return audio_service_gap_ms;
}

void audio_service_reset_stats(void) {
    audio_service_gap_ms = 0;
}

service_t *get_audio_service(void) {
    static service_t audio_service = {
        .name = "audio",
        .entry = audio_service_entry,
        .arg = NULL,
        .update = audio_service_update,
        .priority = SERVICE_HIGH_PRIORITY,
        .state = SERVICE_STATE_STOPPED,
        .dependency_count = 0,
        .restart_limit = SERVICE_RESTART_LIMIT_DEFAULT,
        .restart_count = 0,
        .task = NULL
    };

    return &audio_service;
}
