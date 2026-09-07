#include "components/pci.h"

#include "components/drivers.h"
#include "drivers/USB/usb_controller.h"

#include <ports.h>
#include <stdbool.h>
#include <stddef.h>

static struct pci_device pci_devices[MAX_PCI_DEVICES];
static int pci_device_count = 0;

static struct usb_controller usb_controllers[8];
static int usb_controller_count = 0;

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) |
                       (offset & 0xFC) |
                       ((uint32_t)0x80000000);
    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) |
                       (offset & 0xFC) |
                       ((uint32_t)0x80000000);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

void pci_enable_bus_mastering(struct pci_device* dev) {
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 2) | (1 << 1) | (1 << 0);
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_enable_interrupts(struct pci_device* dev) {
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    cmd &= ~(1 << 10);
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_scan_devices(void) {
    pci_device_count = 0;
    usb_controller_count = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {

                if (pci_device_count >= MAX_PCI_DEVICES) return;

                uint32_t vendor_device = pci_read_config(bus, slot, func, 0);

                if ((vendor_device & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }

                struct pci_device* dev = &pci_devices[pci_device_count];
                dev->bus = bus;
                dev->slot = slot;
                dev->func = func;
                dev->vendor_id = vendor_device & 0xFFFF;
                dev->device_id = vendor_device >> 16;

                uint32_t class_reg = pci_read_config(bus, slot, func, 0x08);
                dev->class_code = class_reg >> 24;
                dev->subclass = (class_reg >> 16) & 0xFF;
                dev->prog_if = (class_reg >> 8) & 0xFF;

                dev->bar0 = pci_read_config(bus, slot, func, 0x10);
                dev->bar1 = pci_read_config(bus, slot, func, 0x14);
                dev->bar2 = pci_read_config(bus, slot, func, 0x18);
                dev->bar3 = pci_read_config(bus, slot, func, 0x1C);
                dev->bar4 = pci_read_config(bus, slot, func, 0x20);
                dev->bar5 = pci_read_config(bus, slot, func, 0x24);

                uint32_t intr = pci_read_config(bus, slot, func, 0x3C);
                dev->irq_line = (uint8_t)(intr & 0xFF);
                dev->irq_pin = (uint8_t)((intr >> 8) & 0xFF);

                pci_enable_interrupts(dev);

                if (dev->class_code == 0x0C && dev->subclass == 0x03) {
                    if (usb_controller_count < 8) {
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
                    }
                }

                device_table[PCI_DEVICE][pci_device_count] = dev;
                pci_device_count++;

                uint32_t header = pci_read_config(bus, slot, 0, 0x0C);
                if (func == 0 && !(header & 0x00800000)) break;
            }
        }
    }
}

struct usb_controller* pci_get_usb_controllers(void) {
    return usb_controllers;
}

int pci_get_device_count(void) {
    return pci_device_count;
}

uint64_t pci_bar_phys(struct pci_device* dev, int bar, bool* is_io) {
    if (is_io) *is_io = false;
    if (!dev || bar < 0 || bar > 5) return 0;

    uint32_t low = pci_read_config(dev->bus, dev->slot, dev->func, (uint8_t)(0x10 + bar * 4));
    if (low == 0 || low == 0xFFFFFFFF) return 0;

    if (low & 0x1) {
        if (is_io) *is_io = true;
        return low & ~0x3ULL;
    }

    if (((low >> 1) & 0x3) == 0x2) {
        if (bar >= 5) return low & ~0xFULL;
        uint32_t high = pci_read_config(dev->bus, dev->slot, dev->func, (uint8_t)(0x10 + (bar + 1) * 4));
        return ((uint64_t)low & ~0xFULL) | ((uint64_t)high << 32);
    }

    return low & ~0xFULL;
}

void pci_init(void) {
    static bool initialized = false;

    if (!initialized) {
        pci_scan_devices();
        initialized = true;
    }
}
