#include "drivers.h"

#include "components/ACPI/acpi.h"
#include "components/Interruptions/idt.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/logger.h"
#include "components/pci.h"
#include "components/ACPI/madt.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Input/mouse_driver.h"
#include "drivers/Serial/uart_driver.h"
#include "drivers/Storage/ahci.h"
#include "drivers/Storage/ata.h"
#include "drivers/Storage/block_device.h"
#include "drivers/Storage/nvme.h"
#include "drivers/Storage/partition.h"
#include "drivers/Storage/usb_msc_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/Video/limine_video_driver.h"

#include <stddef.h>

void* device_table[AMOUNT_DEVICES_TYPE][MAX(MAX_DEVICES_PER_TYPE, MAX_PCI_DEVICES)] = {0};
struct driver* driver_table[AMOUNT_DRIVERS_TYPE][MAX_DRIVERS_PER_TYPE] = {0};
spinlock_t driver_lock = SPINLOCK_INIT;

void register_driver(struct driver* drv) {
    if (!drv){
        LOG_ERROR("Failed to register driver: NULL pointer");
        return;
    }

    LOG_DEBUG("Registering driver %s (type %d, sub type %d)", drv->name, (int)drv->type, drv->sub_type);

    for (int i = 0; i < drv->dependency_count; i++){
        struct dependency* dep = drv->dependencies[i];
        spin_lock(&driver_lock);
        int dep_met = driver_table[dep->type][dep->sub_type] != 0;
        spin_unlock(&driver_lock);
        if (!dep_met) {
            drv->status = DRIVER_STATUS_FAILED;
            LOG_ERROR("Failed to register driver %s, dependency not met (type %d, sub type %d)",
                      drv->name, (int)dep->type, dep->sub_type);
            return;
        }
    }

    spin_lock(&driver_lock);
    if (driver_table[drv->type][drv->sub_type]) {
        spin_unlock(&driver_lock);
        LOG_ERROR("Failed to register driver %s, slot already occupied", drv->name);
        return;
    }
    driver_table[drv->type][drv->sub_type] = drv;
    spin_unlock(&driver_lock);

    drv->status = DRIVER_STATUS_INITIALIZING;
    if (drv->init) drv->init();
    drv->status = DRIVER_STATUS_READY;
    LOG_INFO("Driver %s registered successfully", drv->name);

    return;
}

void *get_self_driver(enum DRIVER_TYPE type, int sub_type) {
    spin_lock(&driver_lock);
    struct driver* meta = driver_table[type][sub_type];
    spin_unlock(&driver_lock);
    if (!meta) return NULL;
    return meta->self;
}

struct driver* get_meta_driver(enum DRIVER_TYPE type, int sub_type) {
    return driver_table[type][sub_type];
}

void init_drivers() {
    asm volatile("cli");

    register_driver(return_meta_uart_driver());

    init_ioapic();
    init_idt();

    register_driver(return_meta_limine_video_driver());

    acpi_init();

    register_driver(return_meta_rtc_driver());
    register_driver(return_meta_pit_driver());
    register_driver(return_meta_tsc_driver());
    register_driver(return_meta_apic_driver());
    register_driver(return_meta_hpet_driver());

    register_driver(return_meta_ps2_keyboard_driver());
    register_driver(return_meta_ps2_mouse_driver());

    pci_init();
    acpi_init();

    ioapic_apply_isa_overrides();

    uint32_t kbd_gsi = madt_remap_isa_irq(1);
    if (kbd_gsi <= 23) ioapic_unmask_irq((uint8_t)kbd_gsi);
    else LOG_WARNING("PS/2 keyboard IRQ1 remapped to GSI %u, outside IOAPIC range", kbd_gsi);

    uint32_t mouse_gsi = madt_remap_isa_irq(12);
    if (mouse_gsi <= 23) ioapic_unmask_irq((uint8_t)mouse_gsi);
    else LOG_WARNING("PS/2 mouse IRQ12 remapped to GSI %u, outside IOAPIC range", mouse_gsi);

    LOG_DEBUG("PS/2 IRQs routed: keyboard GSI %u, mouse GSI %u", kbd_gsi, mouse_gsi);

    struct uart_driver *uart = (struct uart_driver *)get_self_driver(SERIAL_DRIVER, UART_COM1);
    bool uart_ready = uart && uart->is_present();
    uint32_t uart_gsi = madt_remap_isa_irq(UART_COM1_IRQ);
    bool uart_irq = uart_ready && uart_gsi <= 23;
    if (uart_irq) {
        ioapic_unmask_irq((uint8_t)uart_gsi);
        LOG_DEBUG("COM1 IRQ%u routed to GSI %u", UART_COM1_IRQ, uart_gsi);
    } else if (uart_ready) {
        LOG_WARNING("COM1 IRQ%u remapped to GSI %u, outside IOAPIC range", UART_COM1_IRQ, uart_gsi);
    }

    msi_init();

    register_driver(return_meta_uhci_driver());
    register_driver(return_meta_ohci_driver());
    register_driver(return_meta_ehci_driver());
    register_driver(return_meta_xhci_driver());

    register_driver(return_meta_usb_core_driver());
    register_driver(return_meta_usb_keyboard_driver());
    register_driver(return_meta_keyboard_driver());
    register_driver(return_meta_usb_mouse_driver());
    register_driver(return_meta_mouse_driver());

    register_driver(return_meta_ahci_driver());
    register_driver(return_meta_nvme_driver());
    register_driver(return_meta_usb_msc_driver());
    register_driver(return_meta_ata_driver());

    {
        uint32_t disk_count = get_device_count();
        LOG_INFO("Scanning %u block device(s) for partitions", disk_count);
        for (uint32_t i = 0; i < disk_count; i++) {
            struct block_device *dev = block_device_get(i);
            if (dev) partition_register_all(dev);
        }
    }

    LOG_INFO("Driver initialization complete");

    asm volatile("sti");

    if (uart_irq) {
        if (!uart->enable_irq()) ioapic_mask_irq((uint8_t)uart_gsi);
    } else if (uart_ready) {
        uart->set_sync_mode();
    }
}
