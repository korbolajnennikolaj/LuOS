#include "drivers.h"

#include "components/ACPI/acpi.h"
#include "components/Interruptions/idt.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/pci.h"
#include "components/ACPI/madt.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Input/mouse_driver.h"
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
    struct limine_video_driver* video_main_driver = get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    if (!drv){
        if (video_main_driver != NULL) {
            video_main_driver->printf("Failed to register driver: NULL pointer\n", LIMINE_COLOR_LIGHT_RED);
        }
        return;
    }

    for (int i = 0; i < drv->dependency_count; i++){
        struct dependency* dep = drv->dependencies[i];
        spin_lock(&driver_lock);
        int dep_met = driver_table[dep->type][dep->sub_type] != 0;
        spin_unlock(&driver_lock);
        if (!dep_met) {
            if (video_main_driver != NULL) {
                video_main_driver->printf("Failed to register driver ", LIMINE_COLOR_LIGHT_RED);
                video_main_driver->printf(drv->name, LIMINE_COLOR_LIGHT_CYAN);
                video_main_driver->printf(", dependency not met\n", LIMINE_COLOR_LIGHT_RED);
            }
            return;
        }
    }

    spin_lock(&driver_lock);
    if (driver_table[drv->type][drv->sub_type]) {
        spin_unlock(&driver_lock);
        if (video_main_driver != NULL) {
            video_main_driver->printf("Failed to register driver ", LIMINE_COLOR_LIGHT_RED);
            video_main_driver->printf(drv->name, LIMINE_COLOR_LIGHT_CYAN);
            video_main_driver->printf(", slot already occupied\n", LIMINE_COLOR_LIGHT_RED);
        }
        return;
    }
    driver_table[drv->type][drv->sub_type] = drv;
    spin_unlock(&driver_lock);

    if (drv->init) drv->init();
    drv->status = DRIVER_STATUS_READY;
    if (video_main_driver != NULL) {
        video_main_driver->printf("Driver ", LIMINE_COLOR_GOLD);
        video_main_driver->printf(drv->name, LIMINE_COLOR_LIGHT_CYAN);
        video_main_driver->printf(" registered successfully\n", LIMINE_COLOR_GOLD);
    }

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

    init_ioapic();
    init_idt();

    register_driver(return_meta_limine_video_driver());

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
        for (uint32_t i = 0; i < disk_count; i++) {
            struct block_device *dev = block_device_get(i);
            if (dev) partition_register_all(dev);
        }
    }

    asm volatile("sti");
}
