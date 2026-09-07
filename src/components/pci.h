#ifndef PCI_H
#define PCI_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PCI_DEVICES 256

#include "drivers/USB/usb_controller.h"

enum PCI_DEVICE_TYPE {
    PCI_CLASS_UNCLASSIFIED = 0x00,
    PCI_CLASS_STORAGE = 0x01,
    PCI_CLASS_NETWORK = 0x02,
    PCI_CLASS_DISPLAY = 0x03,
    PCI_CLASS_MULTIMEDIA = 0x04,
    PCI_CLASS_MEMORY = 0x05,
    PCI_CLASS_BRIDGE = 0x06,
    PCI_CLASS_COMMUNICATION = 0x07,
    PCI_CLASS_SYSTEM_PERIPH = 0x08,
    PCI_CLASS_INPUT_DEVICE = 0x09,
    PCI_CLASS_DOCKING = 0x0A,
    PCI_CLASS_PROCESSOR = 0x0B,
    PCI_CLASS_SERIAL_BUS = 0x0C,
    PCI_CLASS_WIRELESS = 0x0D,
    PCI_CLASS_INTELLIGENT = 0x0E,
    PCI_CLASS_SATELLITE = 0x0F,
    PCI_CLASS_ENCRYPTION = 0x10,
    PCI_CLASS_UNDEFINED = 0xFF
};

enum PCI_SERIAL_DEVICE_TYPE {
    PCI_SUBCLASS_USB = 0x03,
    PCI_SUBCLASS_SATA = 0x06,
    PCI_SUBCLASS_SMBUS = 0x05
};

typedef struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint32_t bar0;
    uint32_t bar1;
    uint32_t bar2;
    uint32_t bar3;
    uint32_t bar4;
    uint32_t bar5;

    uint8_t irq_line;
    uint8_t irq_pin;
} pci_device;

void pci_init(void);
void pci_scan_devices(void);

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

void pci_enable_bus_mastering(struct pci_device* dev);
void pci_enable_interrupts(struct pci_device* dev);

struct usb_controller* pci_get_usb_controllers(void);

int pci_get_device_count(void);

uint64_t pci_bar_phys(struct pci_device* dev, int bar, bool* is_io);

#endif
