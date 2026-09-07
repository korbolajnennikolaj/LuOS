#ifndef HPET_DRIVER_H
#define HPET_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct hpet_driver {
    uint64_t (*get_hpet_ms)(void);
    uint64_t (*get_hpet_uptime_ms)(void);
    void (*sleep_hpet_ms)(uint64_t ms);
    void (*calibrate_hpet_with_pit)(void* pit);
    uint64_t (*get_hpet_counter)(void);
    uint64_t (*get_hpet_frequency)(void);
    uint32_t (*get_hpet_timer_count)(void);
    bool (*is_hpet_64bit)(void);
    bool (*is_hpet_available)(void);
    bool (*setup_hpet_timer)(uint8_t timer_idx, uint64_t ticks, bool periodic, bool interrupts, uint8_t vector);
    void (*sleep_hpet_ns)(uint64_t ns);
    void (*sleep_hpet_us)(uint64_t us);
    void (*sleep_hpet_cycles)(uint64_t cycles);
} hpet_driver;

struct hpet_driver* return_hpet_driver(void);
struct driver* return_meta_hpet_driver(void);

#endif
