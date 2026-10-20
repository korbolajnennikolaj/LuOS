// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "components/Interruptions/msi.h"

#include "components/Interruptions/idt.h"
#include "components/Interruptions/isr.h"
#include "components/Memory/vmm.h"
#include "components/logger.h"
#include "components/pci.h"

#include <ports.h>
#include <stddef.h>

static msi_vector_t s_vectors[MSI_MAX_VECTORS];
static int s_vector_count = 0;

static void* get_stub_for_vector(uint8_t vector) {
    return isr_stub_table[vector];
}

uint32_t msi_compose_address(uint8_t lapic_id) {
    return 0xFEE00000U | ((uint32_t)lapic_id << 12);
}

uint32_t msi_compose_data(uint8_t vector) {
    return (uint32_t)vector;
}

void msi_init(void) {
    for (int i = 0; i < MSI_MAX_VECTORS; i++) {
        s_vectors[i].vector = 0;
        s_vectors[i].enabled = false;
        s_vectors[i].handler = (void*)0;
        s_vectors[i].cap_offset = 0;
        s_vectors[i].is_msix = false;
    }
    s_vector_count = 0;
}

uint8_t msi_find_cap(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id) {
    uint16_t status = pci_read16(bus, dev, func, 0x06);
    if (!(status & (1 << 4))) return 0;

    uint8_t cap_ptr = pci_read8(bus, dev, func, 0x34) & 0xFC;

    int safety = 0;
    while (cap_ptr && safety < 64) {
        uint8_t id = pci_read8(bus, dev, func, cap_ptr);
        uint8_t next = pci_read8(bus, dev, func, cap_ptr + 1);

        if (id == cap_id) return cap_ptr;

        cap_ptr = next & 0xFC;
        safety++;
    }
    return 0;
}

bool msi_is_available(uint8_t bus, uint8_t dev, uint8_t func) {
    if (msi_find_cap(bus, dev, func, PCI_CAP_ID_MSI)) return true;
    if (msi_find_cap(bus, dev, func, PCI_CAP_ID_MSIX)) return true;
    return false;
}


int pci_msi_vector_count(struct pci_device *dev) {
    if (!dev || !dev->msi_cap) return 0;
    uint16_t ctrl = pci_dev_read16(dev, dev->msi_cap + 2);
    return 1 << ((ctrl >> MSI_CTRL_MMC_SHIFT) & 0x7);
}

static uint16_t pci_msi_mask_reg(struct pci_device *dev, uint16_t ctrl) {
    return (uint16_t)(dev->msi_cap + ((ctrl & MSI_CTRL_64BIT) ? 0x10 : 0x0C));
}

void pci_msi_mask(struct pci_device *dev, bool masked) {
    if (!dev || !dev->msi_cap) return;
    uint16_t ctrl = pci_dev_read16(dev, dev->msi_cap + 2);
    if (!(ctrl & MSI_CTRL_PVM)) return;
    uint16_t reg = pci_msi_mask_reg(dev, ctrl);
    uint32_t m = pci_dev_read32(dev, reg);
    m = masked ? (m | 1u) : (m & ~1u);
    pci_dev_write32(dev, reg, m);
}

int pci_msi_enable_dev(struct pci_device *dev, uint8_t vector, uint8_t lapic_id) {
    if (!dev || !dev->msi_cap) return -1;
    if (dev->msix_enabled) pci_msix_disable_dev(dev);

    uint8_t cap = dev->msi_cap;
    uint16_t ctrl = pci_dev_read16(dev, cap + 2);
    ctrl &= (uint16_t)~MSI_CTRL_ENABLE;
    pci_dev_write16(dev, cap + 2, ctrl);

    pci_dev_write32(dev, cap + 4, msi_compose_address(lapic_id));
    if (ctrl & MSI_CTRL_64BIT) {
        pci_dev_write32(dev, cap + 8, 0);
        pci_dev_write16(dev, cap + 12, (uint16_t)msi_compose_data(vector));
    } else {
        pci_dev_write16(dev, cap + 8, (uint16_t)msi_compose_data(vector));
    }

    if (ctrl & MSI_CTRL_PVM) {
        uint16_t reg = pci_msi_mask_reg(dev, ctrl);
        pci_dev_write32(dev, reg, pci_dev_read32(dev, reg) & ~1u);
    }

    ctrl &= (uint16_t)~(0x7 << MSI_CTRL_MME_SHIFT);
    ctrl |= MSI_CTRL_ENABLE;
    pci_dev_write16(dev, cap + 2, ctrl);

    pci_set_intx(dev, false);
    dev->msi_enabled = true;
    LOG_DEBUG("%04x:%02x:%02x.%x: MSI vector 0x%02x -> LAPIC %u", (unsigned)dev->segment, (unsigned)dev->bus,
              (unsigned)dev->slot, (unsigned)dev->func, (unsigned)vector, (unsigned)lapic_id);
    return 0;
}

void pci_msi_disable_dev(struct pci_device *dev) {
    if (!dev || !dev->msi_cap) return;
    uint16_t ctrl = pci_dev_read16(dev, dev->msi_cap + 2);
    pci_dev_write16(dev, dev->msi_cap + 2, ctrl & (uint16_t)~MSI_CTRL_ENABLE);
    dev->msi_enabled = false;
}


int pci_msix_table_size(struct pci_device *dev) {
    if (!dev || !dev->msix_cap) return 0;
    return (pci_dev_read16(dev, dev->msix_cap + 2) & MSIX_CTRL_TABLE_SIZE) + 1;
}

static uint64_t pci_msix_bar_base(struct pci_device *dev, uint8_t bir) {
    if (bir > 5) return 0;
    bool io = false;
    uint64_t base = pci_bar_phys(dev, bir, &io);
    if (io) return 0;
    return base;
}

int pci_msix_map(struct pci_device *dev) {
    if (!dev || !dev->msix_cap) return -1;
    if (dev->msix_table) return 0;

    uint8_t cap = dev->msix_cap;
    int entries = pci_msix_table_size(dev);
    uint32_t tbl = pci_dev_read32(dev, cap + 4);
    uint32_t pba = pci_dev_read32(dev, cap + 8);

    uint64_t tbl_base = pci_msix_bar_base(dev, (uint8_t)(tbl & 7));
    uint64_t pba_base = pci_msix_bar_base(dev, (uint8_t)(pba & 7));
    if (!tbl_base) {
        LOG_WARNING("%02x:%02x.%x: MSI-X table BAR%u is unusable", (unsigned)dev->bus, (unsigned)dev->slot,
                    (unsigned)dev->func, (unsigned)(tbl & 7));
        return -1;
    }

    pci_set_command_bits(dev, PCI_COMMAND_MEMORY, 0);

    uint64_t tv = vmm_ioremap(tbl_base + (tbl & ~7u), (uint64_t)entries * MSIX_ENTRY_SIZE, VMM_CACHE_UC);
    if (!tv) return -1;
    dev->msix_table = (volatile uint32_t *)tv;
    dev->msix_entries = (uint16_t)entries;

    if (pba_base) {
        uint64_t pv = vmm_ioremap(pba_base + (pba & ~7u), (uint64_t)((entries + 63) / 64) * 8, VMM_CACHE_UC);
        dev->msix_pba = (volatile uint32_t *)pv;
    }

    for (int i = 0; i < entries; i++)
        dev->msix_table[i * 4 + 3] |= MSIX_ENTRY_VECTOR_CTRL_MASK;

    return 0;
}

int pci_msix_set_entry(struct pci_device *dev, int entry, uint8_t vector, uint8_t lapic_id) {
    if (!dev || !dev->msix_table || entry < 0 || entry >= dev->msix_entries) return -1;
    volatile uint32_t *e = dev->msix_table + entry * 4;
    e[3] |= MSIX_ENTRY_VECTOR_CTRL_MASK;
    e[0] = msi_compose_address(lapic_id);
    e[1] = 0;
    e[2] = msi_compose_data(vector);
    (void)e[3];
    return 0;
}

void pci_msix_mask_entry(struct pci_device *dev, int entry, bool masked) {
    if (!dev || !dev->msix_table || entry < 0 || entry >= dev->msix_entries) return;
    volatile uint32_t *e = dev->msix_table + entry * 4;
    uint32_t c = e[3];
    e[3] = masked ? (c | MSIX_ENTRY_VECTOR_CTRL_MASK) : (c & ~MSIX_ENTRY_VECTOR_CTRL_MASK);
    (void)e[3];
}

bool pci_msix_entry_pending(struct pci_device *dev, int entry) {
    if (!dev || !dev->msix_pba || entry < 0 || entry >= dev->msix_entries) return false;
    return (dev->msix_pba[entry / 32] >> (entry % 32)) & 1;
}

int pci_msix_enable_dev(struct pci_device *dev) {
    if (!dev || !dev->msix_cap || !dev->msix_table) return -1;
    if (dev->msi_enabled) pci_msi_disable_dev(dev);

    uint8_t cap = dev->msix_cap;
    uint16_t ctrl = pci_dev_read16(dev, cap + 2);
    pci_dev_write16(dev, cap + 2, ctrl | MSIX_CTRL_ENABLE | MSIX_CTRL_FUNC_MASK);
    pci_set_intx(dev, false);
    ctrl = pci_dev_read16(dev, cap + 2);
    pci_dev_write16(dev, cap + 2, ctrl & (uint16_t)~MSIX_CTRL_FUNC_MASK);
    dev->msix_enabled = true;
    LOG_DEBUG("%04x:%02x:%02x.%x: MSI-X enabled, %u entries", (unsigned)dev->segment, (unsigned)dev->bus,
              (unsigned)dev->slot, (unsigned)dev->func, (unsigned)dev->msix_entries);
    return 0;
}

void pci_msix_disable_dev(struct pci_device *dev) {
    if (!dev || !dev->msix_cap) return;
    uint16_t ctrl = pci_dev_read16(dev, dev->msix_cap + 2);
    pci_dev_write16(dev, dev->msix_cap + 2, ctrl & (uint16_t)~MSIX_CTRL_ENABLE);
    dev->msix_enabled = false;
}


bool msi_enable(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector, uint8_t lapic_id, void (*handler)(void))
{
    if (s_vector_count >= MSI_MAX_VECTORS) {
        LOG_WARNING("%02x:%02x.%x: MSI vector table full (%d)", (unsigned)bus, (unsigned)dev, (unsigned)func, MSI_MAX_VECTORS);
        return false;
    }

    struct pci_device *pdev = pci_find_bdf(0, bus, dev, func);
    if (!pdev) {
        LOG_DEBUG("%02x:%02x.%x is not in the PCI table", (unsigned)bus, (unsigned)dev, (unsigned)func);
        return false;
    }

    bool msix = false;
    uint8_t cap = pdev->msi_cap;
    if (cap) {
        if (pci_msi_enable_dev(pdev, vector, lapic_id) != 0) return false;
    } else if (pdev->msix_cap) {
        cap = pdev->msix_cap;
        msix = true;
        if (pci_msix_map(pdev) != 0) return false;
        pci_msix_set_entry(pdev, 0, vector, lapic_id);
        pci_msix_mask_entry(pdev, 0, false);
        if (pci_msix_enable_dev(pdev) != 0) return false;
    } else {
        LOG_DEBUG("%02x:%02x.%x has no MSI/MSI-X capability", (unsigned)bus, (unsigned)dev, (unsigned)func);
        return false;
    }

    idt_set_gate(vector, get_stub_for_vector(vector), 0x8E);

    int idx = s_vector_count++;
    s_vectors[idx].vector = vector;
    s_vectors[idx].pci_bus = bus;
    s_vectors[idx].pci_dev = dev;
    s_vectors[idx].pci_func = func;
    s_vectors[idx].cap_offset = cap;
    s_vectors[idx].is_msix = msix;
    s_vectors[idx].enabled = true;
    s_vectors[idx].handler = handler;
    LOG_DEBUG("%02x:%02x.%x: %s enabled, vector 0x%02x -> LAPIC %u, cap at 0x%02x",
              (unsigned)bus, (unsigned)dev, (unsigned)func, msix ? "MSI-X" : "MSI",
              (unsigned)vector, (unsigned)lapic_id, (unsigned)cap);
    return true;
}

void msi_disable(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < s_vector_count; i++) {
        msi_vector_t *v = &s_vectors[i];
        if (v->pci_bus != bus || v->pci_dev != dev || v->pci_func != func)
            continue;
        if (!v->enabled) continue;

        LOG_DEBUG("%02x:%02x.%x: disabling %s vector 0x%02x", (unsigned)bus, (unsigned)dev, (unsigned)func,
                  v->is_msix ? "MSI-X" : "MSI", (unsigned)v->vector);

        struct pci_device *pdev = pci_find_bdf(0, bus, dev, func);
        if (pdev) {
            if (v->is_msix) pci_msix_disable_dev(pdev);
            else pci_msi_disable_dev(pdev);
        }
        v->enabled = false;
    }
}

void msi_dispatch(uint8_t vector) {
    for (int i = 0; i < s_vector_count; i++) {
        if (s_vectors[i].vector == vector && s_vectors[i].enabled) {
            static uint8_t dbg_once[256] = {0};
            if (!dbg_once[vector]) {
                dbg_once[vector] = 1;
                LOG_DEBUG("first interrupt on vector 0x%02x", (unsigned)vector);
            }
            if (s_vectors[i].handler)
                s_vectors[i].handler();
            return;
        }
    }
    static uint8_t no_handler_warned[256] = {0};
    if (!no_handler_warned[vector]) {
        no_handler_warned[vector] = 1;
        LOG_WARNING("interrupt on vector 0x%02x with no registered MSI handler", (unsigned)vector);
    }
}
