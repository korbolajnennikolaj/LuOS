#include "pit_driver.h"

#include "components/drivers.h"
#include "drivers/Timer/timer.h"

#include <ports.h>
#include <stddef.h>

#define PIT_CH0_DATA 0x40
#define PIT_COMMAND 0x43

static volatile uint64_t pit_milliseconds = 0;
static uint32_t ticks_per_ms = 1193;
static uint8_t initialized = 0;

static uint8_t rtc_read_reg(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void rtc_wait_update() {
    while (rtc_read_reg(0x0A) & 0x80);
}

static uint8_t rtc_get_seconds() {
    rtc_wait_update();
    return rtc_read_reg(0x00);
}

static uint16_t pit_read_raw() {
    outb(PIT_COMMAND, 0x00);
    uint8_t lo = inb(PIT_CH0_DATA);
    uint8_t hi = inb(PIT_CH0_DATA);
    return (uint16_t)(lo | (hi << 8));
}

static void calibrate_with_rtc() {
    outb(PIT_COMMAND, 0x34);
    outb(0x40, 0xFF);
    outb(0x40, 0xFF);

    uint8_t s = rtc_get_seconds();
    while (rtc_get_seconds() == s);

    uint16_t start_tick = pit_read_raw();
    uint8_t target_s = rtc_get_seconds();

    uint32_t total_ticks = 0;
    uint16_t last_tick = start_tick;

    while (rtc_get_seconds() == target_s) {
        uint16_t current_tick = pit_read_raw();
        if (current_tick > last_tick) {
            total_ticks += (last_tick + (0xFFFF - current_tick));
        } else {
            total_ticks += (last_tick - current_tick);
        }
        last_tick = current_tick;
    }

    if (total_ticks > 1000) {
        ticks_per_ms = total_ticks / 1000;
    }
}

static void sleep_ms(uint64_t ms) {
    for (uint64_t i = 0; i < ms; i++) {
        uint32_t elapsed = 0;
        uint16_t last = pit_read_raw();
        while (elapsed < ticks_per_ms) {
            uint16_t current = pit_read_raw();
            if (current > last) {
                elapsed += (last + (0xFFFF - current));
            } else {
                elapsed += (last - current);
            }
            last = current;
            asm volatile("pause");
        }
    }
    pit_milliseconds += ms;
}

struct pit_driver driver_pit = {
    .get_pit_ms = (void*)0,
    .sleep_pit_ms = sleep_ms,
    .calibrate_with_rtc = calibrate_with_rtc
};

struct pit_driver* return_pit_driver(void) {
    if (!initialized) {
        calibrate_with_rtc();
        outb(PIT_COMMAND, 0x34);
        outb(0x40, 0xFF);
        outb(0x40, 0xFF);
        initialized = 1;
    }

    return &driver_pit;
}

struct driver* return_meta_pit_driver(void) {
    static struct driver meta = {
        .name = "PIT Timer Driver",
        .type = TIMER_DRIVER,
        .sub_type = PIT_TIMER,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { NULL },
        .dependency_count = 0,
        .self = &driver_pit,
        .init = (void*)return_pit_driver
    };
    return &meta;
}
