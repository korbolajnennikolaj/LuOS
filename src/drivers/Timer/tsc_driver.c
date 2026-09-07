#include "tsc_driver.h"

#include "drivers/Timer/timer.h"

#include <stddef.h>
#include <stdint.h>

static uint64_t tsc_ticks_per_ms = 0;

static inline uint64_t readTSC(){
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void sleep_tsc_ticks(uint64_t ticks){
    uint64_t start_time = readTSC();
    while (readTSC() - start_time < ticks){
        asm volatile("nop");
    }
}

void sleep_tsc_ms(uint64_t ms){
    sleep_tsc_ticks(tsc_ticks_per_ms * ms);
}

void sleep_tsc_us(uint64_t us){
    sleep_tsc_ticks((us * tsc_ticks_per_ms) / 1000);
}

uint64_t get_tsc_uptime_ms(void) {
    if (tsc_ticks_per_ms == 0) return 0;
    uint64_t current_tsc = readTSC();
    return current_tsc / tsc_ticks_per_ms;
}

struct tsc_driver tsc_driver_loaded = {
    .get_tsc_ms = readTSC,
    .get_tsc_uptime_ms = get_tsc_uptime_ms,
    .sleep_tsc_ms = sleep_tsc_ms,
    .sleep_tsc_ticks = sleep_tsc_ticks,
    .sleep_tsc_us = sleep_tsc_us,
};

struct tsc_driver* return_tsc_driver(){
    struct pit_driver* pit = get_self_driver(TIMER_DRIVER, PIT_TIMER);
    uint64_t start_tsc = readTSC();
    pit->sleep_pit_ms(10);
    tsc_ticks_per_ms = (readTSC() - start_tsc) / 10;

    return &tsc_driver_loaded;
};

struct driver* return_meta_tsc_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, PIT_TIMER)
    };

    static struct driver meta = {
        .name = "TSC Driver",
        .type = TIMER_DRIVER,
        .sub_type = TSC_TIMER,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &tsc_driver_loaded,
        .init = (void*)return_tsc_driver
    };
    return &meta;
}
