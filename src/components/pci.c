// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "components/pci.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/ACPI/mcfg.h"
#include "components/Memory/vmm.h"
#include "drivers/USB/usb_controller.h"
#include "kernel/scheduler/spinlock.h"

#include <ports.h>
#include <stdbool.h>
#include <stddef.h>

static struct pci_device pci_devices[MAX_PCI_DEVICES];
static int pci_device_count = 0;

static struct usb_controller usb_controllers[8];
static int usb_controller_count = 0;

static spinlock_t pci_legacy_lock = SPINLOCK_INIT;

#define PCI_MAX_ECAM 8

struct pci_ecam_window {
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint64_t phys;
    volatile uint8_t *virt;
};

static struct pci_ecam_window ecam[PCI_MAX_ECAM];
static int ecam_count = 0;

static volatile uint8_t *pci_ecam_addr(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    for (int i = 0; i < ecam_count; i++) {
        struct pci_ecam_window *w = &ecam[i];
        if (w->segment != seg || bus < w->start_bus || bus > w->end_bus) continue;
        uint64_t off = ((uint64_t)(bus - w->start_bus) << 20) | ((uint64_t)dev << 15) |
                       ((uint64_t)func << 12) | (offset & 0xFFF);
        return w->virt + off;
    }
    return NULL;
}

static inline uint32_t pci_legacy_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return ((uint32_t)bus << 16) | ((uint32_t)(slot & 0x1F) << 11) | ((uint32_t)(func & 7) << 8) |
           (offset & 0xFC) | 0x80000000u;
}

static uint32_t pci_legacy_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, int width) {
    uint64_t flags = spin_lock_irqsave(&pci_legacy_lock);
    outl(0xCF8, pci_legacy_address(bus, slot, func, offset));
    uint32_t v;
    if (width == 1) v = inb((uint16_t)(0xCFC + (offset & 3)));
    else if (width == 2) v = inw((uint16_t)(0xCFC + (offset & 2)));
    else v = inl(0xCFC);
    spin_unlock_irqrestore(&pci_legacy_lock, flags);
    return v;
}

static void pci_legacy_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, int width, uint32_t value) {
    uint64_t flags = spin_lock_irqsave(&pci_legacy_lock);
    outl(0xCF8, pci_legacy_address(bus, slot, func, offset));
    if (width == 1) outb((uint16_t)(0xCFC + (offset & 3)), (uint8_t)value);
    else if (width == 2) outw((uint16_t)(0xCFC + (offset & 2)), (uint16_t)value);
    else outl(0xCFC, value);
    spin_unlock_irqrestore(&pci_legacy_lock, flags);
}

uint32_t pci_cfg_read(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width) {
    volatile uint8_t *p = pci_ecam_addr(seg, bus, dev, func, offset);
    if (p) {
        if (width == 1) return *(volatile uint8_t *)p;
        if (width == 2) return *(volatile uint16_t *)((uintptr_t)p & ~1ULL);
        return *(volatile uint32_t *)((uintptr_t)p & ~3ULL);
    }
    if (seg != 0 || offset > 0xFF) {
        if (width == 1) return 0xFF;
        if (width == 2) return 0xFFFF;
        return 0xFFFFFFFFu;
    }
    return pci_legacy_read(bus, dev, func, (uint8_t)offset, width);
}

void pci_cfg_write(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width, uint32_t value) {
    volatile uint8_t *p = pci_ecam_addr(seg, bus, dev, func, offset);
    if (p) {
        if (width == 1) *(volatile uint8_t *)p = (uint8_t)value;
        else if (width == 2) *(volatile uint16_t *)((uintptr_t)p & ~1ULL) = (uint16_t)value;
        else *(volatile uint32_t *)((uintptr_t)p & ~3ULL) = value;
        return;
    }
    if (seg != 0 || offset > 0xFF) return;
    pci_legacy_write(bus, dev, func, (uint8_t)offset, width, value);
}

bool pci_cfg_reachable(uint16_t seg, uint8_t bus, uint16_t offset) {
    if (pci_ecam_addr(seg, bus, 0, 0, 0)) return true;
    return seg == 0 && offset <= 0xFF;
}

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return pci_cfg_read(0, bus, slot, func, offset & 0xFC, 4);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    pci_cfg_write(0, bus, slot, func, offset & 0xFC, 4, value);
}

uint8_t pci_dev_read8(struct pci_device *d, uint16_t o) { return (uint8_t)pci_cfg_read(d->segment, d->bus, d->slot, d->func, o, 1); }
uint16_t pci_dev_read16(struct pci_device *d, uint16_t o) { return (uint16_t)pci_cfg_read(d->segment, d->bus, d->slot, d->func, o, 2); }
uint32_t pci_dev_read32(struct pci_device *d, uint16_t o) { return pci_cfg_read(d->segment, d->bus, d->slot, d->func, o, 4); }
void pci_dev_write8(struct pci_device *d, uint16_t o, uint8_t v) { pci_cfg_write(d->segment, d->bus, d->slot, d->func, o, 1, v); }
void pci_dev_write16(struct pci_device *d, uint16_t o, uint16_t v) { pci_cfg_write(d->segment, d->bus, d->slot, d->func, o, 2, v); }
void pci_dev_write32(struct pci_device *d, uint16_t o, uint32_t v) { pci_cfg_write(d->segment, d->bus, d->slot, d->func, o, 4, v); }

bool pci_ecam_available(void) { return ecam_count > 0; }
int pci_ecam_segment_count(void) { return ecam_count; }

static void pci_ecam_init(void) {
    const ACPI_MCFG *mcfg = acpi_get_mcfg();
    size_t n = mcfg_get_entry_count(mcfg);
    if (!n) {
        LOG_INFO("no MCFG table, PCI config space limited to 256 bytes (CF8/CFC)");
        return;
    }

    for (size_t i = 0; i < n && ecam_count < PCI_MAX_ECAM; i++) {
        const ACPI_MCFG_ENTRY *e = mcfg_get_entry(mcfg, i);
        if (!e || !e->BaseAddress || e->EndBus < e->StartBus) continue;

        uint64_t size = ((uint64_t)(e->EndBus - e->StartBus) + 1) << 20;
        uint64_t virt = vmm_ioremap(e->BaseAddress, size, VMM_CACHE_UC);
        if (!virt) {
            LOG_WARNING("cannot map ECAM window 0x%llx (+%llu MiB)", (unsigned long long)e->BaseAddress,
                        (unsigned long long)(size >> 20));
            continue;
        }

        struct pci_ecam_window *w = &ecam[ecam_count];
        w->segment = e->SegmentGroup;
        w->start_bus = e->StartBus;
        w->end_bus = e->EndBus;
        w->phys = e->BaseAddress;
        w->virt = (volatile uint8_t *)virt;

        if (w->segment == 0 && w->start_bus == 0) {
            uint32_t via_ecam = *(volatile uint32_t *)w->virt;
            uint32_t via_io = pci_legacy_read(0, 0, 0, 0, 4);
            if (via_ecam != via_io) {
                LOG_WARNING("ECAM at 0x%llx disagrees with CF8/CFC (0x%08x vs 0x%08x), not using it",
                            (unsigned long long)w->phys, via_ecam, via_io);
                vmm_iounmap(virt);
                continue;
            }
        }

        ecam_count++;
        LOG_INFO("PCIe ECAM segment %u buses %02x-%02x at 0x%llx", (unsigned)w->segment, (unsigned)w->start_bus,
                 (unsigned)w->end_bus, (unsigned long long)w->phys);
    }
}

uint8_t pci_find_next_capability(struct pci_device *dev, uint8_t start, uint8_t cap_id) {
    uint8_t pos;
    if (start == 0) {
        if (!(pci_dev_read16(dev, PCI_STATUS) & PCI_STATUS_CAP_LIST)) return 0;
        pos = pci_dev_read8(dev, PCI_CAPABILITY_LIST);
    } else {
        pos = pci_dev_read8(dev, start + 1);
    }
    for (int ttl = 48; ttl > 0 && pos >= 0x40; ttl--) {
        pos &= 0xFC;
        uint8_t id = pci_dev_read8(dev, pos);
        if (id == 0xFF) break;
        if (id == cap_id) return pos;
        pos = pci_dev_read8(dev, pos + 1);
    }
    return 0;
}

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id) {
    return pci_find_next_capability(dev, 0, cap_id);
}

uint16_t pci_find_next_ext_capability(struct pci_device *dev, uint16_t start, uint16_t cap_id) {
    if (dev->cfg_size <= PCI_CFG_SPACE_SIZE) return 0;

    uint16_t pos = 0x100;
    if (start) {
        uint32_t h = pci_dev_read32(dev, start);
        if (h == 0xFFFFFFFFu) return 0;
        pos = (uint16_t)((h >> 20) & 0xFFC);
        if (pos < 0x100) return 0;
    }

    for (int ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8; ttl > 0; ttl--) {
        uint32_t header = pci_dev_read32(dev, pos);
        if (header == 0 || header == 0xFFFFFFFFu) break;
        if ((header & 0xFFFF) == cap_id) return pos;
        uint16_t next = (uint16_t)((header >> 20) & 0xFFC);
        if (next < 0x100) break;
        pos = next;
    }
    return 0;
}

uint16_t pci_find_ext_capability(struct pci_device *dev, uint16_t cap_id) {
    return pci_find_next_ext_capability(dev, 0, cap_id);
}

void pci_set_command_bits(struct pci_device *dev, uint16_t set, uint16_t clear) {
    uint16_t cmd = pci_dev_read16(dev, PCI_COMMAND);
    uint16_t n = (uint16_t)((cmd & ~clear) | set);
    if (n != cmd) pci_dev_write16(dev, PCI_COMMAND, n);
}

void pci_enable_bus_mastering(struct pci_device* dev) {
    pci_set_command_bits(dev, PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 0);
}

void pci_enable_interrupts(struct pci_device* dev) {
    pci_set_command_bits(dev, 0, PCI_COMMAND_INTX_DISABLE);
}

void pci_set_intx(struct pci_device *dev, bool enable) {
    if (enable) pci_set_command_bits(dev, 0, PCI_COMMAND_INTX_DISABLE);
    else pci_set_command_bits(dev, PCI_COMMAND_INTX_DISABLE, 0);
}

static void pci_size_bars(struct pci_device *dev) {
    int nbars = (dev->header_type & 0x7F) == 0 ? 6 : ((dev->header_type & 0x7F) == 1 ? 2 : 0);
    uint16_t rom_reg = (dev->header_type & 0x7F) == 1 ? PCI_ROM_ADDRESS_BRIDGE : PCI_ROM_ADDRESS;

    if ((dev->header_type & 0x7F) == 2) return;

    bool keep_decode = dev->class_code == 0x06 && dev->subclass == 0x00;

    uint64_t rflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    uint16_t cmd = pci_dev_read16(dev, PCI_COMMAND);
    if (!keep_decode)
        pci_dev_write16(dev, PCI_COMMAND, cmd & (uint16_t)~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));

    for (int i = 0; i < nbars; i++) {
        uint16_t reg = (uint16_t)(PCI_BAR0 + i * 4);
        uint32_t orig = pci_dev_read32(dev, reg);
        pci_dev_write32(dev, reg, 0xFFFFFFFFu);
        uint32_t mask = pci_dev_read32(dev, reg);
        pci_dev_write32(dev, reg, orig);

        struct pci_bar *b = &dev->bars[i];
        if (mask == 0 || mask == 0xFFFFFFFFu) continue;

        if (orig & 1) {
            uint32_t sz = ~(mask & ~0x3u) + 1;
            sz &= 0xFFFF;
            b->base = orig & ~0x3u;
            b->size = sz;
            b->flags = PCI_BAR_PRESENT | PCI_BAR_IO;
            continue;
        }

        uint32_t type = (orig >> 1) & 3;
        b->flags = PCI_BAR_PRESENT;
        if (orig & 0x8) b->flags |= PCI_BAR_PREFETCH;

        if (type == 2 && i + 1 < nbars) {
            uint16_t reg_hi = (uint16_t)(reg + 4);
            uint32_t orig_hi = pci_dev_read32(dev, reg_hi);
            pci_dev_write32(dev, reg_hi, 0xFFFFFFFFu);
            uint32_t mask_hi = pci_dev_read32(dev, reg_hi);
            pci_dev_write32(dev, reg_hi, orig_hi);

            uint64_t m = ((uint64_t)mask_hi << 32) | (mask & ~0xFu);
            b->size = ~m + 1;
            b->base = ((uint64_t)orig_hi << 32) | (orig & ~0xFu);
            b->flags |= PCI_BAR_MEM64;
            dev->bars[i + 1].flags = PCI_BAR_UPPER_HALF;
            i++;
        } else {
            b->size = (uint32_t)(~(mask & ~0xFu) + 1);
            b->base = orig & ~0xFu;
        }
    }

    uint32_t rom_orig = pci_dev_read32(dev, rom_reg);
    pci_dev_write32(dev, rom_reg, 0xFFFFF800u);
    uint32_t rom_mask = pci_dev_read32(dev, rom_reg);
    pci_dev_write32(dev, rom_reg, rom_orig);
    if (rom_mask & 0xFFFFF800u) {
        dev->rom_size = ~(rom_mask & 0xFFFFF800u) + 1;
        dev->rom_base = rom_orig & 0xFFFFF800u;
    }

    if (!keep_decode) pci_dev_write16(dev, PCI_COMMAND, cmd);
    if (rflags & (1 << 9)) asm volatile("sti");
}

static void pci_probe_caps(struct pci_device *dev) {
    dev->pm_cap = pci_find_capability(dev, PCI_CAP_ID_PM);
    dev->msi_cap = pci_find_capability(dev, 0x05);
    dev->msix_cap = pci_find_capability(dev, 0x11);
    dev->pcie_cap = pci_find_capability(dev, PCI_CAP_ID_EXP);

    dev->cfg_size = PCI_CFG_SPACE_SIZE;
    if (dev->pcie_cap && pci_cfg_reachable(dev->segment, dev->bus, 0x100)) {
        uint32_t h = pci_dev_read32(dev, 0x100);
        if (h != 0xFFFFFFFFu) dev->cfg_size = PCI_CFG_SPACE_EXP_SIZE;
    }
}

static void pci_add_usb_controller(struct pci_device *dev) {
    if (usb_controller_count >= 8) {
        LOG_WARNING("more than 8 USB controllers, %02x:%02x.%x ignored", dev->bus, dev->slot, dev->func);
        return;
    }
    struct usb_controller* usb = &usb_controllers[usb_controller_count++];
    usb->pci = dev;

    if (dev->prog_if == 0x00) usb->type = USB_TYPE_UHCI;
    else if (dev->prog_if == 0x10) usb->type = USB_TYPE_OHCI;
    else if (dev->prog_if == 0x20) usb->type = USB_TYPE_EHCI;
    else if (dev->prog_if == 0x30) usb->type = USB_TYPE_XHCI;

    if (usb->type == USB_TYPE_UHCI) {
        usb->is_io_space = true;
        usb->base_addr = dev->bar4 & ~0x3;
    } else {
        usb->is_io_space = false;
        uint32_t bar0 = dev->bar0;
        if (((bar0 >> 1) & 0x3) == 0x2) {
            usb->base_addr = ((uint64_t)dev->bar1 << 32) | (bar0 & 0xFFFFFFF0);
        } else {
            usb->base_addr = bar0 & 0xFFFFFFF0;
        }
    }
    pci_enable_bus_mastering(dev);
    LOG_DEBUG("USB controller prog-if 0x%02x at %02x:%02x.%x, base 0x%llx (%s)",
              (unsigned)dev->prog_if, dev->bus, dev->slot, dev->func,
              (unsigned long long)usb->base_addr, usb->is_io_space ? "io" : "mmio");
}

static void pci_scan_segment(uint16_t seg) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint8_t header0 = 0;
            for (int func = 0; func < 8; func++) {

                if (pci_device_count >= MAX_PCI_DEVICES) {
                    LOG_WARNING("PCI device table full (%d), stopping scan at %04x:%02x:%02x.%x",
                                MAX_PCI_DEVICES, seg, bus, slot, func);
                    return;
                }

                uint32_t vendor_device = pci_cfg_read(seg, (uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0, 4);

                if ((vendor_device & 0xFFFF) == 0xFFFF || (vendor_device & 0xFFFF) == 0) {
                    if (func == 0) break;
                    continue;
                }

                struct pci_device* dev = &pci_devices[pci_device_count];
                *dev = (struct pci_device){0};
                dev->segment = seg;
                dev->bus = (uint8_t)bus;
                dev->slot = (uint8_t)slot;
                dev->func = (uint8_t)func;
                dev->vendor_id = vendor_device & 0xFFFF;
                dev->device_id = vendor_device >> 16;
                dev->parent = -1;

                uint32_t class_reg = pci_dev_read32(dev, 0x08);
                dev->class_code = class_reg >> 24;
                dev->subclass = (class_reg >> 16) & 0xFF;
                dev->prog_if = (class_reg >> 8) & 0xFF;
                dev->revision = class_reg & 0xFF;

                dev->header_type = pci_dev_read8(dev, PCI_HEADER_TYPE);
                if (func == 0) header0 = dev->header_type;

                dev->bar0 = pci_dev_read32(dev, 0x10);
                dev->bar1 = pci_dev_read32(dev, 0x14);
                dev->bar2 = pci_dev_read32(dev, 0x18);
                dev->bar3 = pci_dev_read32(dev, 0x1C);
                dev->bar4 = pci_dev_read32(dev, 0x20);
                dev->bar5 = pci_dev_read32(dev, 0x24);

                uint32_t intr = pci_dev_read32(dev, 0x3C);
                dev->irq_line = (uint8_t)(intr & 0xFF);
                dev->irq_pin = (uint8_t)((intr >> 8) & 0xFF);

                if ((dev->header_type & 0x7F) == 0) {
                    dev->subsys_vendor = pci_dev_read16(dev, PCI_SUBSYSTEM_VENDOR_ID);
                    dev->subsys_id = pci_dev_read16(dev, PCI_SUBSYSTEM_ID);
                } else if ((dev->header_type & 0x7F) == 1) {
                    dev->is_bridge = true;
                    dev->secondary_bus = pci_dev_read8(dev, PCI_SECONDARY_BUS);
                    dev->subordinate_bus = pci_dev_read8(dev, PCI_SUBORDINATE_BUS);
                }

                pci_size_bars(dev);
                pci_probe_caps(dev);
                pci_enable_interrupts(dev);

                LOG_DEBUG("%04x:%02x:%02x.%x %04x:%04x rev %02x class %02x/%02x/%02x irq %u pin %u cfg %u%s%s",
                          (unsigned)seg, bus, slot, func, (unsigned)dev->vendor_id, (unsigned)dev->device_id,
                          (unsigned)dev->revision, (unsigned)dev->class_code, (unsigned)dev->subclass,
                          (unsigned)dev->prog_if, (unsigned)dev->irq_line, (unsigned)dev->irq_pin,
                          (unsigned)dev->cfg_size, dev->msix_cap ? " msix" : (dev->msi_cap ? " msi" : ""),
                          dev->is_bridge ? " bridge" : "");
                for (int b = 0; b < 6; b++) {
                    struct pci_bar *bar = &dev->bars[b];
                    if (!(bar->flags & PCI_BAR_PRESENT)) continue;
                    LOG_DEBUG("    BAR%d %s 0x%llx size 0x%llx%s%s", b, (bar->flags & PCI_BAR_IO) ? "io " : "mem",
                              (unsigned long long)bar->base, (unsigned long long)bar->size,
                              (bar->flags & PCI_BAR_MEM64) ? " 64-bit" : "",
                              (bar->flags & PCI_BAR_PREFETCH) ? " prefetch" : "");
                }

                if (dev->class_code == 0x0C && dev->subclass == 0x03)
                    pci_add_usb_controller(dev);

                device_table[PCI_DEVICE][pci_device_count] = dev;
                pci_device_count++;

                if (func == 0 && !(header0 & 0x80)) break;
            }
        }
    }
}

static void pci_link_bridges(void) {
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        for (int j = 0; j < pci_device_count; j++) {
            struct pci_device *b = &pci_devices[j];
            if (!b->is_bridge || b->segment != d->segment || b->secondary_bus == 0) continue;
            if (b->secondary_bus == d->bus) {
                d->parent = (int16_t)j;
                break;
            }
        }
    }
}

#define PCI_BRIDGE_MEMORY_BASE 0x20
#define PCI_BRIDGE_MEMORY_LIMIT 0x22

static void pci_assign_display_roms(void) {
    static struct { uint64_t start, end; } ext[MAX_PCI_DEVICES * 7 + PCI_MAX_ECAM];
    int n = 0;

    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        for (int b = 0; b < 6; b++) {
            struct pci_bar *bar = &d->bars[b];
            if ((bar->flags & PCI_BAR_PRESENT) && !(bar->flags & (PCI_BAR_IO | PCI_BAR_UPPER_HALF)) &&
                bar->size && bar->base && bar->base + bar->size <= (1ULL << 32))
                ext[n++] = (typeof(ext[0])){ bar->base, bar->base + bar->size - 1 };
        }
        if (d->rom_size && d->rom_base) ext[n++] = (typeof(ext[0])){ d->rom_base, d->rom_base + d->rom_size - 1 };
    }
    for (int i = 0; i < ecam_count; i++) {
        uint64_t size = (uint64_t)(ecam[i].end_bus - ecam[i].start_bus + 1) << 20;
        ext[n++] = (typeof(ext[0])){ ecam[i].phys, ecam[i].phys + size - 1 };
    }
#define ROM_EXT_SORT()                                                          \
    do {                                                                        \
        for (int i = 1; i < n; i++)                                             \
            for (int k = i; k > 0 && ext[k - 1].start > ext[k].start; k--) {    \
                typeof(ext[0]) t = ext[k - 1];                                  \
                ext[k - 1] = ext[k];                                            \
                ext[k] = t;                                                     \
            }                                                                   \
    } while (0)
    ROM_EXT_SORT();

    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if (d->class_code != PCI_CLASS_DISPLAY || !d->rom_size || d->rom_base) continue;

        uint64_t lo = 0, bound = 0xFC000000ULL;
        bool windowed = false;
        for (struct pci_device *b = pci_upstream_bridge(d); b; b = pci_upstream_bridge(b)) {
            uint16_t base16 = pci_dev_read16(b, PCI_BRIDGE_MEMORY_BASE);
            uint16_t limit16 = pci_dev_read16(b, PCI_BRIDGE_MEMORY_LIMIT);
            if ((base16 & 0xFFF0) == 0 || (base16 & 0xFFF0) > (limit16 & 0xFFF0)) {
                windowed = false;
                break;
            }
            uint64_t wlo = base16 & 0xFFF00000u, wbound = ((limit16 & 0xFFF00000u) | 0xFFFFFu) + 1;
            if (!windowed) {
                lo = wlo;
                bound = wbound;
                windowed = true;
            } else {
                if (wlo > lo) lo = wlo;
                if (wbound < bound) bound = wbound;
            }
        }
        if (!windowed) {
            lo = 0;
            bound = 0xFC000000ULL;
        }

        uint64_t align = d->rom_size < 4096 ? 4096 : d->rom_size;
        uint64_t best = 0, best_len = 0;
        uint64_t prev_end = lo - 1;
        if (!lo) prev_end = n ? ext[0].start - 1 : (0x80000000ULL - 1);
        for (int k = 0; k <= n; k++) {
            uint64_t gap_lo = prev_end + 1 > lo ? prev_end + 1 : lo;
            uint64_t gap_hi = (k == n || ext[k].start >= bound) ? bound : ext[k].start;
            if (gap_hi > gap_lo) {
                uint64_t alo = (gap_lo + align - 1) & ~(align - 1);
                if (alo + d->rom_size <= gap_hi && gap_hi - alo > best_len) {
                    best = alo;
                    best_len = gap_hi - alo;
                }
            }
            if (k < n) prev_end = ext[k].end;
        }

        if (!best) {
            LOG_WARNING("no free MMIO window for the %u KiB ROM BAR of %04x:%02x:%02x.%x, left unassigned",
                        (unsigned)(d->rom_size / 1024), (unsigned)d->segment, (unsigned)d->bus,
                        (unsigned)d->slot, (unsigned)d->func);
            continue;
        }

        uint16_t rom_reg = (d->header_type & 0x7F) == 1 ? PCI_ROM_ADDRESS_BRIDGE : PCI_ROM_ADDRESS;
        pci_dev_write32(d, rom_reg, (uint32_t)best);
        d->rom_base = best;
        ext[n++] = (typeof(ext[0])){ best, best + d->rom_size - 1 };
        ROM_EXT_SORT();
        LOG_INFO("ROM BAR of %04x:%02x:%02x.%x (%u KiB) assigned at 0x%llx — the firmware had left it unassigned",
                 (unsigned)d->segment, (unsigned)d->bus, (unsigned)d->slot, (unsigned)d->func,
                 (unsigned)(d->rom_size / 1024), (unsigned long long)best);
    }
}

void pci_scan_devices(void) {
    pci_device_count = 0;
    usb_controller_count = 0;

    pci_scan_segment(0);
    for (int i = 0; i < ecam_count; i++) {
        bool seen = false;
        for (int k = 0; k < i; k++) seen |= ecam[k].segment == ecam[i].segment;
        if (ecam[i].segment != 0 && !seen) pci_scan_segment(ecam[i].segment);
    }
    pci_link_bridges();
    pci_assign_display_roms();
}

struct usb_controller* pci_get_usb_controllers(void) {
    return usb_controllers;
}

int pci_get_device_count(void) {
    return pci_device_count;
}

struct pci_device *pci_get_device(int index) {
    if (index < 0 || index >= pci_device_count) return NULL;
    return &pci_devices[index];
}

static int pci_index_of(struct pci_device *dev) {
    if (!dev) return -1;
    return (int)(dev - pci_devices);
}

struct pci_device *pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *from) {
    for (int i = pci_index_of(from) + 1; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if ((vendor == 0xFFFF || d->vendor_id == vendor) && (device == 0xFFFF || d->device_id == device)) return d;
    }
    return NULL;
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *from) {
    for (int i = pci_index_of(from) + 1; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if (d->class_code == class_code && (subclass == 0xFF || d->subclass == subclass)) return d;
    }
    return NULL;
}

struct pci_device *pci_find_bdf(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func) {
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if (d->segment == seg && d->bus == bus && d->slot == slot && d->func == func) return d;
    }
    return NULL;
}

struct pci_device *pci_upstream_bridge(struct pci_device *dev) {
    if (!dev || dev->parent < 0) return NULL;
    return &pci_devices[dev->parent];
}

uint64_t pci_bar_phys(struct pci_device* dev, int bar, bool* is_io) {
    if (is_io) *is_io = false;
    if (!dev || bar < 0 || bar > 5) return 0;

    uint32_t low = pci_dev_read32(dev, (uint16_t)(0x10 + bar * 4));
    if (low == 0 || low == 0xFFFFFFFF) return 0;

    if (low & 0x1) {
        if (is_io) *is_io = true;
        return low & ~0x3ULL;
    }

    if (((low >> 1) & 0x3) == 0x2) {
        if (bar >= 5) return low & ~0xFULL;
        uint32_t high = pci_dev_read32(dev, (uint16_t)(0x10 + (bar + 1) * 4));
        return ((uint64_t)low & ~0xFULL) | ((uint64_t)high << 32);
    }

    return low & ~0xFULL;
}

uint64_t pci_bar_size(struct pci_device *dev, int bar) {
    if (!dev || bar < 0 || bar > 5) return 0;
    if (!(dev->bars[bar].flags & PCI_BAR_PRESENT)) return 0;
    return dev->bars[bar].size;
}

const char *pci_ext_cap_name(uint16_t id) {
    switch (id) {
        case PCI_EXT_CAP_ID_ERR: return "AER";
        case PCI_EXT_CAP_ID_VC: return "VC";
        case PCI_EXT_CAP_ID_DSN: return "DSN";
        case 0x04: return "PowerBudget";
        case PCI_EXT_CAP_ID_VNDR: return "Vendor";
        case PCI_EXT_CAP_ID_ACS: return "ACS";
        case PCI_EXT_CAP_ID_ARI: return "ARI";
        case PCI_EXT_CAP_ID_ATS: return "ATS";
        case PCI_EXT_CAP_ID_SRIOV: return "SR-IOV";
        case 0x13: return "PRI";
        case PCI_EXT_CAP_ID_REBAR: return "ResizableBAR";
        case 0x16: return "DPA";
        case 0x17: return "TPH";
        case PCI_EXT_CAP_ID_LTR: return "LTR";
        case PCI_EXT_CAP_ID_SECPCI: return "SecondaryPCIe";
        case PCI_EXT_CAP_ID_PASID: return "PASID";
        case PCI_EXT_CAP_ID_L1SS: return "L1SS";
        case 0x25: return "DLF";
        case 0x26: return "PL16GT";
        case 0x27: return "LaneMargin";
        default: return "?";
    }
}

void pci_init(void) {
    static bool initialized = false;

    if (!initialized) {
        pci_ecam_init();
        pci_scan_devices();
        initialized = true;
        LOG_INFO("PCI scan found %d device(s), %d USB controller(s), ECAM %s", pci_device_count,
                 usb_controller_count, ecam_count ? "on" : "off");
    }
}
