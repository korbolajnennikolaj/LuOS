// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

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

#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_REVISION_ID 0x08
#define PCI_CLASS_PROG 0x09
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0 0x10
#define PCI_PRIMARY_BUS 0x18
#define PCI_SECONDARY_BUS 0x19
#define PCI_SUBORDINATE_BUS 0x1A
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID 0x2E
#define PCI_ROM_ADDRESS 0x30
#define PCI_ROM_ADDRESS_BRIDGE 0x38
#define PCI_CAPABILITY_LIST 0x34
#define PCI_INTERRUPT_LINE 0x3C

#define PCI_COMMAND_IO (1u << 0)
#define PCI_COMMAND_MEMORY (1u << 1)
#define PCI_COMMAND_MASTER (1u << 2)
#define PCI_COMMAND_INTX_DISABLE (1u << 10)
#define PCI_STATUS_CAP_LIST (1u << 4)

#define PCI_CAP_ID_PM 0x01
#define PCI_CAP_ID_AGP 0x02
#define PCI_CAP_ID_VPD 0x03
#define PCI_CAP_ID_VNDR 0x09
#define PCI_CAP_ID_EXP 0x10

#define PCI_EXT_CAP_ID_ERR 0x01
#define PCI_EXT_CAP_ID_VC 0x02
#define PCI_EXT_CAP_ID_DSN 0x03
#define PCI_EXT_CAP_ID_VNDR 0x0B
#define PCI_EXT_CAP_ID_ACS 0x0D
#define PCI_EXT_CAP_ID_ARI 0x0E
#define PCI_EXT_CAP_ID_ATS 0x0F
#define PCI_EXT_CAP_ID_SRIOV 0x10
#define PCI_EXT_CAP_ID_REBAR 0x15
#define PCI_EXT_CAP_ID_LTR 0x18
#define PCI_EXT_CAP_ID_SECPCI 0x19
#define PCI_EXT_CAP_ID_PASID 0x1B
#define PCI_EXT_CAP_ID_L1SS 0x1E

#define PCI_CFG_SPACE_SIZE 256
#define PCI_CFG_SPACE_EXP_SIZE 4096

#define PCI_BAR_PRESENT (1u << 0)
#define PCI_BAR_IO (1u << 1)
#define PCI_BAR_MEM64 (1u << 2)
#define PCI_BAR_PREFETCH (1u << 3)
#define PCI_BAR_UPPER_HALF (1u << 4)

struct pci_bar {
    uint64_t base;
    uint64_t size;
    uint32_t flags;
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

    uint16_t segment;
    uint8_t header_type;
    uint8_t revision;
    uint16_t subsys_vendor;
    uint16_t subsys_id;

    uint8_t pm_cap;
    uint8_t msi_cap;
    uint8_t msix_cap;
    uint8_t pcie_cap;

    bool is_bridge;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    int16_t parent;

    struct pci_bar bars[6];
    uint64_t rom_base;
    uint32_t rom_size;

    uint16_t cfg_size;
    uint16_t msix_entries;
    volatile uint32_t *msix_table;
    volatile uint32_t *msix_pba;
    bool msi_enabled;
    bool msix_enabled;
} pci_device;

void pci_init(void);
void pci_scan_devices(void);

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

uint32_t pci_cfg_read(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width);
void pci_cfg_write(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, int width, uint32_t value);
bool pci_cfg_reachable(uint16_t seg, uint8_t bus, uint16_t offset);

uint8_t pci_dev_read8(struct pci_device *dev, uint16_t offset);
uint16_t pci_dev_read16(struct pci_device *dev, uint16_t offset);
uint32_t pci_dev_read32(struct pci_device *dev, uint16_t offset);
void pci_dev_write8(struct pci_device *dev, uint16_t offset, uint8_t value);
void pci_dev_write16(struct pci_device *dev, uint16_t offset, uint16_t value);
void pci_dev_write32(struct pci_device *dev, uint16_t offset, uint32_t value);

bool pci_ecam_available(void);
int pci_ecam_segment_count(void);

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);
uint8_t pci_find_next_capability(struct pci_device *dev, uint8_t start, uint8_t cap_id);
uint16_t pci_find_ext_capability(struct pci_device *dev, uint16_t cap_id);
uint16_t pci_find_next_ext_capability(struct pci_device *dev, uint16_t start, uint16_t cap_id);

void pci_enable_bus_mastering(struct pci_device* dev);
void pci_enable_interrupts(struct pci_device* dev);
void pci_set_intx(struct pci_device *dev, bool enable);
void pci_set_command_bits(struct pci_device *dev, uint16_t set, uint16_t clear);

struct usb_controller* pci_get_usb_controllers(void);

int pci_get_device_count(void);
struct pci_device *pci_get_device(int index);
struct pci_device *pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *from);
struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *from);
struct pci_device *pci_find_bdf(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t func);
struct pci_device *pci_upstream_bridge(struct pci_device *dev);

uint64_t pci_bar_phys(struct pci_device* dev, int bar, bool* is_io);
uint64_t pci_bar_size(struct pci_device *dev, int bar);

const char *pci_ext_cap_name(uint16_t id);

#endif
