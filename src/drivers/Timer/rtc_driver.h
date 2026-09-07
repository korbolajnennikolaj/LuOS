#ifndef RTC_DRIVER_H
#define RTC_DRIVER_H

#include "components/drivers.h"

#include <stdint.h>

typedef struct system_time {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t weekday;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t century;
} system_time;

#define TIME_SOURCE_RTC 0x01
#define TIME_SOURCE_UEFI 0x02
#define TIME_SOURCE_LIMINE 0x04
#define TIME_SOURCE_ACPI 0x08
#define TIME_SOURCE_FALLBACK 0x10

typedef struct rtc_driver {
    struct system_time* (*get_rtc_time)(void);
} rtc_driver;

struct rtc_driver* return_rtc_driver(void);
struct driver* return_meta_rtc_driver(void);

#endif
