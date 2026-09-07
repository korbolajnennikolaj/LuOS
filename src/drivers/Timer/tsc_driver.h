#ifndef TSC_DRIVER_H
#define TSC_DRIVER_H

#include "pit_driver.h"

#include <stdint.h>

typedef struct tsc_driver {
    uint64_t (*get_tsc_ms)(void);
    uint64_t (*get_tsc_uptime_ms)(void);
    void (*sleep_tsc_ticks)(uint64_t ticks);
    void (*sleep_tsc_ms)(uint64_t ms);
    void (*sleep_tsc_us)(uint64_t us);
} tsc_driver;

struct tsc_driver* return_tsc_driver(void);
struct driver* return_meta_tsc_driver(void);

#endif
