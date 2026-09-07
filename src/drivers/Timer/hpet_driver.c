#include "hpet_driver.h"

#include "components/ACPI/hpet_acpi.h"
#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "kernel/limine.h"
#include "pit_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    volatile uint64_t config_and_capability;
    volatile uint64_t comparator_value;
    volatile uint64_t fsb_interrupt_route;
    uint64_t _reserved;
} __attribute__((packed)) hpet_timer_reg_t;

typedef struct {
    volatile uint64_t general_capabilities;
    uint64_t _res0;
    volatile uint64_t general_configuration;
    uint64_t _res1;
    volatile uint64_t general_interrupt_status;
    uint64_t _res2[25];
    volatile uint64_t main_counter_value;
    uint64_t _res3;
    hpet_timer_reg_t timers[];
} __attribute__((packed)) hpet_regs_t;

#define HPET_CAP_VENDOR_SHIFT 16
#define HPET_CAP_VENDOR_MASK 0xFFFFULL
#define HPET_CAP_PERIOD_SHIFT 32
#define HPET_CAP_LEG_RT (1ULL << 15)
#define HPET_CAP_64BIT (1ULL << 13)
#define HPET_CAP_TIMERS_SHIFT 8
#define HPET_CAP_TIMERS_MASK 0x1FULL

#define HPET_CONF_ENABLE (1ULL << 0)
#define HPET_CONF_LEG_RT_CNF (1ULL << 1)

#define HPET_TCONF_INT_EN (1ULL << 2)
#define HPET_TCONF_PERIODIC (1ULL << 3)
#define HPET_TCONF_PER_CAP (1ULL << 4)
#define HPET_TCONF_64BIT_CAP (1ULL << 5)
#define HPET_TCONF_VAL_SET (1ULL << 6)
#define HPET_TCONF_32BIT_MODE (1ULL << 8)
#define HPET_TCONF_FSB_EN (1ULL << 14)
#define HPET_TCONF_FSB_CAP (1ULL << 15)

#define HPET_PERIOD_FS_MIN 0x00000005UL
#define HPET_PERIOD_FS_MAX 0x05F5E100UL

#define HPET_PHYS_ADDR_0 0xFED00000ULL
#define HPET_PHYS_ADDR_1 0xFED80000ULL
#define HPET_PHYS_ADDR_2 0xFED01000ULL

extern volatile struct limine_hhdm_request hhdm_req;

static hpet_regs_t *hpet_regs = NULL;
static uint64_t hpet_frequency = 0;
static uint64_t hpet_ticks_per_ms = 0;
static uint64_t hpet_ticks_per_us = 0;
static uint64_t hpet_ticks_per_ns = 0;
static uint32_t timer_count = 0;
static bool is_64bit_counter = false;
static bool initialized = false;

static inline uintptr_t phys_to_virt(uint64_t phys) {

    if (!hhdm_req.response) return (uintptr_t)phys;
    return (uintptr_t)(phys + hhdm_req.response->offset);
}

static inline void hpet_io_delay(void) {
    asm volatile("" ::: "memory");

    (void)hpet_regs->general_capabilities;
    asm volatile("" ::: "memory");
}

uint64_t get_hpet_counter(void) {
    if (!hpet_regs) return 0;
    asm volatile("" ::: "memory");
    uint64_t v = hpet_regs->main_counter_value;
    asm volatile("" ::: "memory");
    return v;
}

uint64_t get_hpet_frequency(void) { return hpet_frequency; }
uint32_t get_hpet_timer_count(void) { return timer_count; }
bool is_hpet_64bit(void) { return is_64bit_counter; }
bool is_hpet_available(void) { return initialized; }

uint64_t get_hpet_ms(void) {
    if (hpet_ticks_per_ms == 0) return 0;
    return get_hpet_counter() / hpet_ticks_per_ms;
}

void sleep_hpet_cycles(uint64_t cycles) {
    if (!hpet_regs || cycles == 0) return;
    uint64_t start = get_hpet_counter();
    while ((get_hpet_counter() - start) < cycles)
        asm volatile("pause");
}

void sleep_hpet_ms(uint64_t ms) {
    if (hpet_ticks_per_ms > 0) sleep_hpet_cycles(ms * hpet_ticks_per_ms);
}

void sleep_hpet_us(uint64_t us) {
    if (hpet_ticks_per_us > 0) sleep_hpet_cycles(us * hpet_ticks_per_us);
}

void sleep_hpet_ns(uint64_t ns) {
    if (hpet_ticks_per_ns > 0) sleep_hpet_cycles(ns * hpet_ticks_per_ns);
}

bool setup_hpet_timer(uint8_t idx, uint64_t ticks, bool periodic, bool interrupts, uint8_t vector)
{
    if (!hpet_regs || idx >= timer_count) return false;

    uint64_t config = hpet_regs->timers[idx].config_and_capability;

    config &= ~(HPET_TCONF_INT_EN |
                HPET_TCONF_PERIODIC |
                HPET_TCONF_VAL_SET |
                HPET_TCONF_FSB_EN |
                HPET_TCONF_32BIT_MODE);

    if (periodic) {

        if (!(hpet_regs->timers[idx].config_and_capability & HPET_TCONF_PER_CAP))
            return false;

        hpet_regs->timers[idx].config_and_capability =
            config | HPET_TCONF_PERIODIC | HPET_TCONF_VAL_SET;
        hpet_io_delay();

        hpet_regs->timers[idx].comparator_value = get_hpet_counter() + ticks;
        hpet_io_delay();

        hpet_regs->timers[idx].comparator_value = ticks;
        hpet_io_delay();

        config |= HPET_TCONF_PERIODIC;

    } else {

        hpet_regs->timers[idx].comparator_value = get_hpet_counter() + ticks;
        hpet_io_delay();
    }

    if (interrupts) {
        if (hpet_regs->timers[idx].config_and_capability & HPET_TCONF_FSB_CAP) {

            uint64_t msi_addr = 0xFEE00000ULL;
            uint64_t msi_data = (uint64_t)vector;
            hpet_regs->timers[idx].fsb_interrupt_route =
                (msi_addr << 32) | msi_data;
            config |= HPET_TCONF_FSB_EN;
        }
        config |= HPET_TCONF_INT_EN;
    }

    hpet_regs->timers[idx].config_and_capability = config;
    hpet_io_delay();
    return true;
}

uint64_t get_hpet_uptime_ms(void) {
    if (!initialized || hpet_ticks_per_ms == 0) return 0;

    return get_hpet_counter() / hpet_ticks_per_ms;
}

void calibrate_hpet_with_pit(void *pit_ptr) {
    (void)pit_ptr;
    if (!hpet_regs) return;

    uint64_t caps = hpet_regs->general_capabilities;
    uint32_t period_fs = (uint32_t)(caps >> HPET_CAP_PERIOD_SHIFT);

    if (period_fs < HPET_PERIOD_FS_MIN || period_fs > HPET_PERIOD_FS_MAX)
        return;

    hpet_frequency = 1000000000000000ULL / period_fs;
    hpet_ticks_per_ms = hpet_frequency / 1000ULL;
    hpet_ticks_per_us = hpet_frequency / 1000000ULL;
    hpet_ticks_per_ns = hpet_frequency / 1000000000ULL;
}

struct hpet_driver hpet_driver_loaded = {
    .get_hpet_ms = get_hpet_ms,
    .get_hpet_uptime_ms = get_hpet_uptime_ms,
    .sleep_hpet_ms = sleep_hpet_ms,
    .calibrate_hpet_with_pit = calibrate_hpet_with_pit,
    .get_hpet_counter = get_hpet_counter,
    .get_hpet_frequency = get_hpet_frequency,
    .get_hpet_timer_count = get_hpet_timer_count,
    .is_hpet_64bit = is_hpet_64bit,
    .is_hpet_available = is_hpet_available,
    .setup_hpet_timer = setup_hpet_timer,
    .sleep_hpet_ns = sleep_hpet_ns,
    .sleep_hpet_us = sleep_hpet_us,
    .sleep_hpet_cycles = sleep_hpet_cycles,
};

struct hpet_driver *return_hpet_driver() {
    struct pit_driver* pit_local = get_self_driver(TIMER_DRIVER, PIT_TIMER);
    if (initialized) return &hpet_driver_loaded;

    uint64_t acpi_addr = hpet_acpi_get_physical_address(acpi_get_hpet());

    uint64_t addrs[5];
    int addr_count = 0;

    if (acpi_addr != 0)
        addrs[addr_count++] = acpi_addr;

    static const uint64_t hardcoded[] = {
        HPET_PHYS_ADDR_0, HPET_PHYS_ADDR_1, HPET_PHYS_ADDR_2
    };
    for (size_t i = 0; i < sizeof(hardcoded) / sizeof(hardcoded[0]); i++) {
        if (hardcoded[i] != acpi_addr)
            addrs[addr_count++] = hardcoded[i];
    }
    addrs[addr_count] = 0;

    for (int i = 0; addrs[i]; i++) {
        hpet_regs_t *candidate = (hpet_regs_t *)phys_to_virt(addrs[i]);

        uint64_t caps = candidate->general_capabilities;

        uint16_t vendor = (uint16_t)((caps >> HPET_CAP_VENDOR_SHIFT) & HPET_CAP_VENDOR_MASK);
        if (vendor == 0x0000 || vendor == 0xFFFF)
            continue;

        uint32_t period_fs = (uint32_t)(caps >> HPET_CAP_PERIOD_SHIFT);
        if (period_fs < HPET_PERIOD_FS_MIN || period_fs > HPET_PERIOD_FS_MAX)
            continue;

        hpet_regs = candidate;
        break;
    }

    if (!hpet_regs) return NULL;

    uint64_t caps = hpet_regs->general_capabilities;
    timer_count = (uint32_t)((caps >> HPET_CAP_TIMERS_SHIFT) & HPET_CAP_TIMERS_MASK) + 1;
    is_64bit_counter = (caps & HPET_CAP_64BIT) != 0;

    hpet_regs->general_configuration &= ~HPET_CONF_ENABLE;
    asm volatile("" ::: "memory");
    hpet_regs->main_counter_value = 0;
    asm volatile("" ::: "memory");
    hpet_regs->general_configuration |= HPET_CONF_ENABLE;
    asm volatile("" ::: "memory");

    calibrate_hpet_with_pit(pit_local);

    if (hpet_ticks_per_ms == 0) {
        hpet_regs = NULL;
        return NULL;
    }

    uint64_t t0 = get_hpet_counter();
    for (volatile int j = 0; j < 100000; j++) asm volatile("pause");
    uint64_t t1 = get_hpet_counter();

    if (t1 <= t0) {

        hpet_regs->general_configuration &= ~HPET_CONF_ENABLE;
        hpet_regs = NULL;
        return NULL;
    }

    initialized = true;
    return &hpet_driver_loaded;
}

struct driver* return_meta_hpet_driver() {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, PIT_TIMER)
    };

    static struct driver meta = {
        .name = "HPET Timer Driver",
        .type = TIMER_DRIVER,
        .sub_type = HPET_TIMER,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = {&dep[0]},
        .dependency_count = 1,
        .self = &hpet_driver_loaded,
        .init = (void*)return_hpet_driver
    };

    return &meta;
}
