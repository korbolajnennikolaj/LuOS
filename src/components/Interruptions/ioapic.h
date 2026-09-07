#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

#ifndef IOAPIC_BASE_PHYS
#define IOAPIC_BASE_PHYS 0xFEC00000
#endif

void init_ioapic(void);

void ioapic_map_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id);

void ioapic_map_pci_irq(uint8_t irq_line, uint8_t vector, uint8_t lapic_id);

void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);

void ioapic_apply_isa_overrides(void);

#endif
