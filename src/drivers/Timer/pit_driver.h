#ifndef PIT_DRIVER_H
#define PIT_DRIVER_H

#include "rtc_driver.h"

#include <stdint.h>

typedef struct pit_driver {
    uint64_t (*get_pit_ms)(void);
    void (*sleep_pit_ms)(uint64_t ms);
    void (*calibrate_with_rtc)();
} pit_driver;

struct pit_driver* return_pit_driver(void);
struct driver* return_meta_pit_driver(void);

#endif
