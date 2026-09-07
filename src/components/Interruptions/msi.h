#ifndef MSI_H
#define MSI_H

#include <stdbool.h>
#include <stdint.h>

#define MSI_VECTOR_XHCI_BASE 0x50
#define MSI_VECTOR_EHCI_BASE 0x58
#define MSI_VECTOR_OHCI_BASE 0x5C
#define MSI_VECTOR_UHCI_BASE 0x60
#define MSI_VECTOR_MAX 0x64

#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_MSIX 0x11

#define MSI_CTRL_ENABLE (1 << 0)
#define MSI_CTRL_64BIT (1 << 7)

typedef struct {
    uint8_t vector;
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint8_t cap_offset;
    bool is_msix;
    bool enabled;
    void (*handler)(void);
} msi_vector_t;

#define MSI_MAX_VECTORS 16

void msi_init(void);
uint8_t msi_find_cap(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id);
bool msi_is_available(uint8_t bus, uint8_t dev, uint8_t func);
bool msi_enable(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector, uint8_t lapic_id, void (*handler)(void));
void msi_disable(uint8_t bus, uint8_t dev, uint8_t func);
void msi_dispatch(uint8_t vector);

extern uint32_t pci_read_config (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

static inline uint8_t pci_read8(uint8_t b, uint8_t d, uint8_t f, uint8_t reg) {
    uint32_t v = pci_read_config(b, d, f, reg & 0xFC);
    return (uint8_t)(v >> ((reg & 3) * 8));
}
static inline uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg) {
    uint32_t v = pci_read_config(b, d, f, reg & 0xFC);
    return (uint16_t)(v >> ((reg & 2) * 8));
}
static inline void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg, uint16_t val) {
    uint32_t v = pci_read_config(b, d, f, reg & 0xFC);
    uint32_t shift = (reg & 2) * 8;
    v &= ~(0xFFFFU << shift);
    v |= ((uint32_t)val << shift);
    pci_write_config(b, d, f, reg & 0xFC, v);
}

#endif
