// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

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
#define MSI_CTRL_MMC_SHIFT 1
#define MSI_CTRL_MME_SHIFT 4
#define MSI_CTRL_64BIT (1 << 7)
#define MSI_CTRL_PVM (1 << 8)

#define MSIX_CTRL_TABLE_SIZE 0x07FF
#define MSIX_CTRL_FUNC_MASK (1 << 14)
#define MSIX_CTRL_ENABLE (1 << 15)
#define MSIX_ENTRY_SIZE 16
#define MSIX_ENTRY_VECTOR_CTRL_MASK 0x1

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

struct pci_device;

uint32_t msi_compose_address(uint8_t lapic_id);
uint32_t msi_compose_data(uint8_t vector);

int pci_msi_vector_count(struct pci_device *dev);
int pci_msi_enable_dev(struct pci_device *dev, uint8_t vector, uint8_t lapic_id);
void pci_msi_disable_dev(struct pci_device *dev);
void pci_msi_mask(struct pci_device *dev, bool masked);

int pci_msix_table_size(struct pci_device *dev);
int pci_msix_map(struct pci_device *dev);
int pci_msix_set_entry(struct pci_device *dev, int entry, uint8_t vector, uint8_t lapic_id);
void pci_msix_mask_entry(struct pci_device *dev, int entry, bool masked);
bool pci_msix_entry_pending(struct pci_device *dev, int entry);
int pci_msix_enable_dev(struct pci_device *dev);
void pci_msix_disable_dev(struct pci_device *dev);

extern uint32_t pci_read_config (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
extern uint32_t pci_cfg_read(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width);
extern void pci_cfg_write(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width, uint32_t value);

static inline uint8_t pci_read8(uint8_t b, uint8_t d, uint8_t f, uint8_t reg) {
    return (uint8_t)pci_cfg_read(0, b, d, f, reg, 1);
}
static inline uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg) {
    return (uint16_t)pci_cfg_read(0, b, d, f, reg, 2);
}
static inline void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg, uint16_t val) {
    pci_cfg_write(0, b, d, f, reg, 2, val);
}

#endif
