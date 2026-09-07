#ifndef APIC_DRIVER_H
#define APIC_DRIVER_H

#include "drivers/Timer/pit_driver.h"

#include <stdbool.h>
#include <stdint.h>

#define IA32_APIC_BASE_MSR 0x1B

#define APIC_ID 0x020
#define APIC_EOI 0x0B0
#define APIC_SPURIOUS 0x0F0
#define APIC_LVT_TIMER 0x320
#define APIC_TIMER_INIT 0x380
#define APIC_TIMER_CURR 0x390
#define APIC_TIMER_DIV 0x3E0

#define TIMER_ONESHOT 0
#define TIMER_PERIODIC (1 << 17)

#define APIC_TIMER_VECTOR 0x40

typedef struct apic_driver {
    void (*calibrate_with_pit)(struct pit_driver *pit);
    uint64_t (*get_apic_ms)(void);
    uint64_t (*get_frequency)(void);
    void (*sleep_apic_ms)(uint64_t ms);
    void (*set_periodic_mode)(uint32_t ms);
    void (*set_oneshot_mode)(uint32_t ms);
    bool (*is_apic_available)(void);
    void (*send_eoi)(void);
} apic_driver;

void apic_send_eoi(void);
uint8_t apic_get_lapic_id(void);
void apic_timer_handler(void);

struct apic_driver *return_apic_driver(void);
struct driver *return_meta_apic_driver(void);

#endif
