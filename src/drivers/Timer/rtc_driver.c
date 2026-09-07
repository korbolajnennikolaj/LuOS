#include "rtc_driver.h"

#include "drivers/Timer/timer.h"
#include <kernel/limine.h>

#include <ports.h>
#include <stddef.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71
#define BCD_TO_BIN(bcd) ((bcd & 0x0F) + ((bcd >> 4) * 10))

static volatile struct limine_boot_time_request boot_time_req = {
    .id = LIMINE_BOOT_TIME_REQUEST,
    .revision = 0
};

static int rtc_initialized = 0;
static struct system_time cached_time;

static uint8_t read_cmos(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int validate_time(struct system_time *time) {
    if (time->hours >= 24) return 0;
    if (time->minutes >= 60) return 0;
    if (time->seconds >= 60) return 0;
    if (time->month < 1 || time->month > 12) return 0;
    if (time->day < 1 || time->day > 31) return 0;
    if (time->year < 2000 || time->year > 2100) return 0;

    return 1;
}

static int read_and_validate_rtc(struct system_time *time) {
    uint8_t reg_b;

    int attempts = 0;
    while (read_cmos(0x0A) & 0x80) {
        attempts++;
        if (attempts > 1000) return 0;
        for (int i = 0; i < 1000; i++) asm volatile("nop");
    }

    time->seconds = read_cmos(0x00);
    time->minutes = read_cmos(0x02);
    time->hours = read_cmos(0x04);
    time->weekday = read_cmos(0x06);
    time->day = read_cmos(0x07);
    time->month = read_cmos(0x08);
    time->year = read_cmos(0x09);

    reg_b = read_cmos(0x0B);

    if (!(reg_b & 0x04)) {
        time->seconds = BCD_TO_BIN(time->seconds);
        time->minutes = BCD_TO_BIN(time->minutes);
        time->hours = BCD_TO_BIN(time->hours);
        time->day = BCD_TO_BIN(time->day);
        time->month = BCD_TO_BIN(time->month);
        time->year = BCD_TO_BIN(time->year);
    }

    if (!(reg_b & 0x02)) {
        if (time->hours & 0x80) {
            time->hours = ((time->hours & 0x7F) + 12) % 24;
        }
    }

    time->year = 2000 + time->year;
    time->century = 20;

    return validate_time(time);
}

static void init_rtc_once(void) {
    if (rtc_initialized) return;

    for (int i = 0; i < 5; i++) {
        if (read_and_validate_rtc(&cached_time)) {
            rtc_initialized = 1;
            return;
        }
        for (int j = 0; j < 100000; j++) asm volatile("pause");
    }

    if (boot_time_req.response != NULL) {
        uint64_t boot_time = boot_time_req.response->boot_time;
        cached_time.seconds = boot_time % 60;
        cached_time.minutes = (boot_time / 60) % 60;
        cached_time.hours = (boot_time / 3600) % 24;
        cached_time.year = 2024;
        cached_time.month = 1;
        cached_time.day = 1;
        cached_time.century = 20;
    } else {
        cached_time.seconds = 0;
        cached_time.minutes = 0;
        cached_time.hours = 0;
        cached_time.year = 2024;
        cached_time.month = 1;
        cached_time.day = 1;
        cached_time.century = 20;
    }

    rtc_initialized = 1;
}

struct system_time* get_rtc_time(void) {
    static struct system_time current_time;

    init_rtc_once();

    if (read_and_validate_rtc(&current_time)) {
        cached_time = current_time;
        return &current_time;
    }

    return &cached_time;
}

struct rtc_driver rtc_driver_loaded = {
    .get_rtc_time = get_rtc_time,
};

struct rtc_driver* return_rtc_driver(void) {
    return &rtc_driver_loaded;
}

struct driver* return_meta_rtc_driver(void) {

    static struct driver meta = {
        .name = "RTC Driver",
        .type = TIMER_DRIVER,
        .sub_type = RTC_TIMER,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = {NULL},
        .dependency_count = 0,
        .self = &rtc_driver_loaded,
        .init = (void*)return_rtc_driver
    };

    return &meta;
}
