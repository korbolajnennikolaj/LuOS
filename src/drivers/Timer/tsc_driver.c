#include "tsc_driver.h"

#include "components/ACPI/fadt.h"
#include "components/ACPI/hpet_acpi.h"
#include "components/logger.h"
#include "components/Memory/mm.h"
#include "drivers/Timer/timer.h"

#include <ports.h>
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

uint64_t get_tsc_uptime_us(void) {
    if (tsc_ticks_per_ms == 0) return 0;
    uint64_t t = readTSC();
    uint64_t ms = t / tsc_ticks_per_ms;
    uint64_t rem = t % tsc_ticks_per_ms;
    return ms * 1000u + (rem * 1000u) / tsc_ticks_per_ms;
}

uint64_t get_tsc_ticks_per_ms(void) {
    return tsc_ticks_per_ms;
}

struct tsc_driver tsc_driver_loaded = {
    .get_tsc_ms = readTSC,
    .get_tsc_uptime_ms = get_tsc_uptime_ms,
    .sleep_tsc_ms = sleep_tsc_ms,
    .sleep_tsc_ticks = sleep_tsc_ticks,
    .sleep_tsc_us = sleep_tsc_us,
    .get_tsc_uptime_us = get_tsc_uptime_us,
    .get_tsc_ticks_per_ms = get_tsc_ticks_per_ms,
};

#define TSC_CAL_MS 10u
#define TSC_AGREE_PERMILLE 20u
#define TSC_PM_TIMER_HZ 3579545ull
#define TSC_PIT_HZ 1193182ull
#define TSC_MAX_SOURCES 4

enum {
    TSC_SRC_CPUID = 0,
    TSC_SRC_PMTMR = 1,
    TSC_SRC_HPET = 2,
    TSC_SRC_PIT = 3,
};

static const char *const tsc_src_names[TSC_MAX_SOURCES] = { "CPUID 0x15", "ACPI PM timer", "HPET", "PIT" };

static void tsc_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static uint64_t tsc_from_cpuid(void) {
    uint32_t a, b, c, d;
    tsc_cpuid(0, &a, &b, &c, &d);
    if (b != 0x756E6547 || a < 0x15) return 0;

    uint32_t den, num, crystal_hz;
    tsc_cpuid(0x15, &den, &num, &crystal_hz, &d);
    if (!den || !num) return 0;

    if (!crystal_hz && a >= 0x16) {
        uint32_t base_mhz;
        tsc_cpuid(0x16, &base_mhz, &b, &c, &d);
        base_mhz &= 0xFFFF;
        if (base_mhz) crystal_hz = (uint32_t)(((uint64_t)base_mhz * 1000000ull * den) / num);
    }
    if (!crystal_hz) return 0;

    return ((uint64_t)crystal_hz * num / den) / 1000;
}

static uint64_t tsc_measure_counter(uint32_t (*read)(void *), void *ctx, uint32_t mask, uint64_t hz) {
    uint64_t want = (hz * TSC_CAL_MS) / 1000;
    uint64_t best = 0;

    for (int round = 0; round < 3; round++) {
        uint32_t c0 = read(ctx) & mask;
        uint64_t t0 = readTSC();
        uint32_t c1 = c0;
        uint32_t prev = c0;
        uint64_t spins = 0;
        uint64_t elapsed = 0;

        while (elapsed < want) {
            c1 = read(ctx) & mask;
            if (c1 != prev) {
                uint32_t step = (c1 - prev) & mask;
                if (step > (mask >> 1)) return 0;
                prev = c1;
            }
            elapsed = (c1 - c0) & mask;
            if (++spins > 2000000ull) return 0;
        }
        uint64_t t1 = readTSC();

        uint64_t per_ms = ((t1 - t0) * (hz / 1000)) / elapsed;
        if (!best || per_ms < best) best = per_ms;
    }
    return best;
}

static uint32_t tsc_read_hpet(void *ctx) {
    volatile uint64_t *regs = (volatile uint64_t *)ctx;
    return (uint32_t)regs[30];
}

static uint32_t tsc_read_pmtmr(void *ctx) {
    uint32_t v = 0;
    fadt_read_pm_timer((const ACPI_FADT *)ctx, &v);
    return v;
}

static volatile uint64_t *tsc_find_hpet(uint64_t *period_fs) {
    uint64_t addrs[] = { 0, 0xFED00000ULL, 0xFED80000ULL, 0xFED01000ULL };
    addrs[0] = hpet_acpi_get_physical_address(acpi_get_hpet());
    for (unsigned i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        if (!addrs[i] || addrs[i] >= 0x100000000ULL) continue;
        volatile uint64_t *regs = (volatile uint64_t *)mm_phys_to_virt(addrs[i]);
        uint64_t caps = regs[0];
        uint16_t vendor = (uint16_t)(caps >> 16);
        uint32_t period = (uint32_t)(caps >> 32);
        if (vendor == 0 || vendor == 0xFFFF) continue;
        if (period < 10000u || period > 100000000u) continue;
        *period_fs = period;
        return regs;
    }
    return NULL;
}

static uint64_t tsc_from_hpet(volatile uint64_t *regs, uint64_t period_fs) {
    if (!regs || !period_fs) return 0;

    uint64_t saved_cfg = regs[2];
    if (!(saved_cfg & 1)) regs[2] = saved_cfg | 1;

    uint64_t result = 0;
    uint32_t a = (uint32_t)regs[30];
    for (volatile int i = 0; i < 1000; i++) asm volatile("pause");
    uint32_t b = (uint32_t)regs[30];

    if (a != b) {
        uint64_t hpet_hz = 1000000000000000ull / period_fs;
        result = tsc_measure_counter(tsc_read_hpet, (void *)regs, 0xFFFFFFFFu, hpet_hz);
    }

    if (!(saved_cfg & 1)) regs[2] = saved_cfg;
    return result;
}

static uint64_t tsc_from_pmtmr(void) {
    const ACPI_FADT *fadt = acpi_get_fadt();
    uint32_t v;
    if (!fadt || !fadt_read_pm_timer(fadt, &v)) return 0;

    uint32_t mask = (fadt->Flags & ACPI_FADT_TMR_VAL_EXT) ? 0xFFFFFFFFu : 0x00FFFFFFu;
    return tsc_measure_counter(tsc_read_pmtmr, (void *)fadt, mask, TSC_PM_TIMER_HZ);
}

static uint64_t tsc_from_pit(void) {
    uint64_t best = 0;
    uint16_t latch = (uint16_t)((TSC_PIT_HZ * TSC_CAL_MS) / 1000);
    uint8_t saved = inb(0x61);

    for (int round = 0; round < 3; round++) {
        outb(0x61, (uint8_t)((saved & ~0x02) | 0x01));
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(latch & 0xFF));
        outb(0x42, (uint8_t)(latch >> 8));

        uint64_t t0 = readTSC();
        uint64_t spins = 0;
        while (!(inb(0x61) & 0x20)) {
            if (++spins > 1000000ull) {
                outb(0x61, saved);
                return 0;
            }
        }
        uint64_t t1 = readTSC();

        if (spins < 8) continue;
        uint64_t per_ms = (t1 - t0) / TSC_CAL_MS;
        if (!best || per_ms < best) best = per_ms;
    }

    outb(0x61, saved);
    return best;
}

static int tsc_agree(uint64_t a, uint64_t b) {
    if (!a || !b) return 0;
    uint64_t diff = a > b ? a - b : b - a;
    uint64_t base = a > b ? a : b;
    return diff * 1000u <= base * TSC_AGREE_PERMILLE;
}

static int tsc_vote(const uint64_t *val, const int *order, int count) {
    for (int i = 0; i < count; i++) {
        int s = order[i];
        if (!val[s]) continue;
        for (int j = 0; j < TSC_MAX_SOURCES; j++) {
            if (j != s && tsc_agree(val[s], val[j])) return s;
        }
    }
    return -1;
}

static void tsc_log_sources(const uint64_t *val) {
    LOG_DEBUG("TSC candidates (ticks/ms): CPUID %llu, PM timer %llu, HPET %llu, PIT %llu",
              (unsigned long long)val[TSC_SRC_CPUID], (unsigned long long)val[TSC_SRC_PMTMR],
              (unsigned long long)val[TSC_SRC_HPET], (unsigned long long)val[TSC_SRC_PIT]);
}

static int tsc_calibrate_with_hpet(volatile uint64_t *hpet, uint64_t period_fs, uint64_t *val) {
    val[TSC_SRC_CPUID] = tsc_from_cpuid();
    val[TSC_SRC_PMTMR] = tsc_from_pmtmr();
    val[TSC_SRC_HPET] = tsc_from_hpet(hpet, period_fs);
    val[TSC_SRC_PIT] = tsc_from_pit();
    tsc_log_sources(val);

    static const int order[] = { TSC_SRC_CPUID, TSC_SRC_PMTMR, TSC_SRC_HPET, TSC_SRC_PIT };
    int pick = tsc_vote(val, order, 4);

    if (val[TSC_SRC_HPET] && pick >= 0 && !tsc_agree(val[TSC_SRC_HPET], val[pick]))
        LOG_WARNING("HPET disagrees with %s (%llu vs %llu ticks/ms), HPET ignored for calibration",
                    tsc_src_names[pick], (unsigned long long)val[TSC_SRC_HPET], (unsigned long long)val[pick]);
    if (pick >= 0) return pick;

    if (val[TSC_SRC_CPUID]) return TSC_SRC_CPUID;
    if (val[TSC_SRC_PMTMR]) return TSC_SRC_PMTMR;
    if (val[TSC_SRC_PIT]) return TSC_SRC_PIT;
    if (val[TSC_SRC_HPET]) return TSC_SRC_HPET;
    return -1;
}

static int tsc_calibrate_without_hpet(uint64_t *val) {
    val[TSC_SRC_CPUID] = tsc_from_cpuid();
    val[TSC_SRC_PMTMR] = tsc_from_pmtmr();
    val[TSC_SRC_HPET] = 0;
    val[TSC_SRC_PIT] = tsc_from_pit();
    tsc_log_sources(val);

    static const int order[] = { TSC_SRC_CPUID, TSC_SRC_PMTMR, TSC_SRC_PIT };
    int pick = tsc_vote(val, order, 3);
    if (pick >= 0) return pick;

    if (val[TSC_SRC_CPUID]) return TSC_SRC_CPUID;
    if (val[TSC_SRC_PMTMR]) return TSC_SRC_PMTMR;
    if (val[TSC_SRC_PIT]) return TSC_SRC_PIT;
    return -1;
}

static uint64_t tsc_from_pit_driver(void) {
    struct pit_driver* pit = get_self_driver(TIMER_DRIVER, PIT_TIMER);
    if (!pit || !pit->sleep_pit_ms) return 0;
    uint64_t start_tsc = readTSC();
    pit->sleep_pit_ms(10);
    return (readTSC() - start_tsc) / 10;
}

struct tsc_driver* return_tsc_driver(){
    uint64_t val[TSC_MAX_SOURCES] = { 0 };
    uint64_t period_fs = 0;
    volatile uint64_t *hpet = tsc_find_hpet(&period_fs);

    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    int pick = hpet ? tsc_calibrate_with_hpet(hpet, period_fs, val) : tsc_calibrate_without_hpet(val);
    if (flags & (1u << 9)) asm volatile("sti" ::: "memory");

    const char *source;
    int confirmed = pick >= 0 && tsc_vote(val, &pick, 1) == pick;

    if (pick >= 0) {
        tsc_ticks_per_ms = val[pick];
        source = tsc_src_names[pick];
    } else {
        tsc_ticks_per_ms = tsc_from_pit_driver();
        source = "PIT driver";
    }

    if (tsc_ticks_per_ms == 0) {
        LOG_ERROR("TSC calibration failed, counter did not advance");
        return &tsc_driver_loaded;
    }

    LOG_INFO("TSC calibrated against %s%s: %llu ticks/ms (~%llu MHz), HPET %s",
             source, confirmed ? " (confirmed by another clock)" : " (unconfirmed)",
             (unsigned long long)tsc_ticks_per_ms, (unsigned long long)(tsc_ticks_per_ms / 1000),
             hpet ? "present" : "absent");

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
