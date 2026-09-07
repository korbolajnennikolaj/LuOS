#include "ioapic.h"

#include "components/ACPI/madt.h"

#include <stdint.h>

#define HHDM_OFFSET 0xFFFF800000000000ULL
#define IOAPIC_BASE_VIRT (IOAPIC_BASE_PHYS + HHDM_OFFSET)

#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_ARB 0x02
#define IOAPIC_REDTBL_BASE 0x10

#define IOAPIC_MASKED (1 << 16)

#define IOAPIC_MAX_IRQ 24

static volatile uint32_t *s_ioapic = (volatile uint32_t *)IOAPIC_BASE_VIRT;

static void ioapic_write(uint8_t reg, uint32_t val) {
    s_ioapic[0] = reg;
    asm volatile("" ::: "memory");
    s_ioapic[4] = val;
}

static uint32_t ioapic_read(uint8_t reg) {
    s_ioapic[0] = reg;
    asm volatile("" ::: "memory");
    return s_ioapic[4];
}

static uint8_t ioapic_max_irq(void) {
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint8_t max = (uint8_t)((ver >> 16) & 0xFF);

    if (max == 0 || max > 23) max = 23;
    return max + 1;
}

void ioapic_map_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id) {

    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + irq * 2);
    uint8_t reg_hi = reg_lo + 1;

    uint32_t hi = ((uint32_t)lapic_id) << 24;
    uint32_t lo = (uint32_t)vector;

    ioapic_write(reg_lo, lo | IOAPIC_MASKED);
    ioapic_write(reg_hi, hi);

    ioapic_write(reg_lo, lo);
}

void ioapic_mask_irq(uint8_t irq) {
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + irq * 2);
    uint32_t lo = ioapic_read(reg_lo);
    ioapic_write(reg_lo, lo | IOAPIC_MASKED);
}

void ioapic_unmask_irq(uint8_t irq) {
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + irq * 2);
    uint32_t lo = ioapic_read(reg_lo);
    ioapic_write(reg_lo, lo & ~(uint32_t)IOAPIC_MASKED);
}

void ioapic_map_pci_irq(uint8_t irq_line, uint8_t vector, uint8_t lapic_id) {
    if (irq_line == 0xFF || irq_line == 0) return;

    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + irq_line * 2);
    uint8_t reg_hi = reg_lo + 1;

    uint32_t hi = ((uint32_t)lapic_id) << 24;
    uint32_t lo = (uint32_t)vector
                | (1U << 15)
                | (1U << 13);

    ioapic_write(reg_lo, lo | IOAPIC_MASKED);
    ioapic_write(reg_hi, hi);
    ioapic_write(reg_lo, lo);
}

void ioapic_apply_isa_overrides(void) {

    static const struct { uint8_t isa_irq; uint8_t vector; } legacy[] = {
        { 0, 32 }, { 1, 33 }, { 12, 44 },
    };

    for (unsigned i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        uint8_t isa_irq = legacy[i].isa_irq;
        uint8_t vector = legacy[i].vector;

        uint32_t gsi = madt_remap_isa_irq(isa_irq);
        if (gsi > 23) continue;

        uint16_t flags = 0;
        int iso_count = madt_get_iso_count();
        for (int j = 0; j < iso_count; j++) {
            const ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE *iso = madt_get_iso(j);
            if (iso && iso->Bus == 0 && iso->Source == isa_irq) {
                flags = iso->Flags;
                break;
            }
        }

        uint32_t active_low = ((flags & 0x3) == 3) ? 1u : 0u;
        uint32_t level_triggered = (((flags >> 2) & 0x3) == 3) ? 1u : 0u;

        uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + gsi * 2);
        uint8_t reg_hi = reg_lo + 1;

        uint32_t old_lo = ioapic_read(reg_lo);
        uint32_t was_masked = old_lo & IOAPIC_MASKED;

        uint32_t hi = 0;
        uint32_t lo = (uint32_t)vector
                    | (active_low << 13)
                    | (level_triggered << 15);

        ioapic_write(reg_lo, lo | IOAPIC_MASKED);
        ioapic_write(reg_hi, hi);
        ioapic_write(reg_lo, lo | was_masked);
    }
}

void init_ioapic(void) {
    uint8_t max = ioapic_max_irq();

    for (uint8_t i = 0; i < max; i++) {
        uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL_BASE + i * 2);
        uint8_t reg_hi = reg_lo + 1;

        ioapic_write(reg_hi, 0);
        ioapic_write(reg_lo, IOAPIC_MASKED | 0x20);
    }

    ioapic_map_irq(0, 32, 0);
    ioapic_map_irq(1, 33, 0);

    ioapic_map_irq(12, 44, 0);
    ioapic_mask_irq(12);
}
