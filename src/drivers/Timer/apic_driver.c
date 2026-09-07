#include "apic_driver.h"

#include "components/drivers.h"
#include "components/Interruptions/isr.h"
#include "drivers/Timer/timer.h"

#include <ports.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IA32_APIC_BASE_MSR 0x1B

#define APIC_REG_ID 0x020
#define APIC_REG_EOI 0x0B0
#define APIC_REG_SVR 0x0F0
#define APIC_REG_LVT_TIMER 0x320
#define APIC_REG_TIMER_DIV 0x3E0
#define APIC_REG_TIMER_INIT 0x380
#define APIC_REG_TIMER_CUR 0x390

#define APIC_TIMER_VECTOR 0x40
#define APIC_TIMER_PERIODIC (1 << 17)

static volatile uint64_t apic_milliseconds = 0;
static uint32_t ticks_per_ms = 0;
static volatile uint32_t *apic_base = NULL;

static bool apic_supported(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    return (edx & (1 << 9)) != 0;
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t val) {
    asm volatile("wrmsr"
        :
        : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32))
    );
}

static inline void apic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uintptr_t)apic_base + reg) = val;
}

static inline uint32_t apic_read(uint32_t reg) {
    return *(volatile uint32_t *)((uintptr_t)apic_base + reg);
}

void apic_send_eoi(void) {
    if (apic_base)
        apic_write(APIC_REG_EOI, 0);
}

uint8_t apic_get_lapic_id(void) {
    if (!apic_base) return 0;
    return (uint8_t)((apic_read(APIC_REG_ID) >> 24) & 0xFF);
}

static void apic_enable(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= (1ULL << 11);
    wrmsr(IA32_APIC_BASE_MSR, base);

    apic_base = (volatile uint32_t *)(
        (base & 0xFFFFF000ULL) + 0xFFFF800000000000ULL
    );

    apic_write(APIC_REG_SVR, apic_read(APIC_REG_SVR) | 0x100 | 0xFF);
}

static void apic_calibrate(struct pit_driver *pit) {
    apic_write(APIC_REG_TIMER_DIV, 0x3);
    apic_write(APIC_REG_TIMER_INIT, 0xFFFFFFFF);

    pit->sleep_pit_ms(10);

    uint32_t elapsed = 0xFFFFFFFF - apic_read(APIC_REG_TIMER_CUR);
    ticks_per_ms = elapsed / 10;
    if (!ticks_per_ms) ticks_per_ms = 10000;
}

static void apic_timer_isr(struct registers *regs) {
    (void)regs;
    apic_milliseconds++;
}

void apic_timer_handler(void) {
    apic_milliseconds++;
}

void sleep_apic_ms(uint64_t ms) {
    uint64_t end = apic_milliseconds + ms;
    while (apic_milliseconds < end)
        asm volatile("pause");
}

struct apic_driver apic_driver_loaded = {
    .calibrate_with_pit = apic_calibrate,
    .sleep_apic_ms = sleep_apic_ms,
    .send_eoi = apic_send_eoi,
    .is_apic_available = apic_supported
};

struct apic_driver *return_apic_driver(void) {
    struct pit_driver *pit_local = get_self_driver(TIMER_DRIVER, PIT_TIMER);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    if (!apic_supported())
        return NULL;

    apic_enable();
    apic_write(APIC_REG_EOI, 0);
    apic_calibrate(pit_local);

    irq_register_handler(APIC_TIMER_VECTOR, apic_timer_isr);

    apic_write(APIC_REG_LVT_TIMER, APIC_TIMER_VECTOR | APIC_TIMER_PERIODIC);
    apic_write(APIC_REG_TIMER_DIV, 0x3);
    apic_write(APIC_REG_TIMER_INIT, ticks_per_ms);

    return &apic_driver_loaded;
}

struct driver *return_meta_apic_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, PIT_TIMER)
    };

    static struct driver meta = {
        .name = "APIC Timer Driver",
        .type = TIMER_DRIVER,
        .sub_type = APIC_TIMER,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &apic_driver_loaded,
        .init = (void *)return_apic_driver
    };

    return &meta;
}
