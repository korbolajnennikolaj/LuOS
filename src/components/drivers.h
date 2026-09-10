#ifndef DRIVERS_H
#define DRIVERS_H

#include "components/pci.h"
#include "kernel/scheduler/spinlock.h"

#include <stdint.h>

#define MAX_DRIVERS_PER_TYPE 64
#define AMOUNT_DRIVERS_TYPE 7

#define MAX_DEVICES_PER_TYPE 64
#define AMOUNT_DEVICES_TYPE 3

#define MAX_DEPENDENCIES_PER_DRIVER 8

#define MAKE_DEPENDENCY(_t, _s) ((struct dependency){ .type = (_t), .sub_type = (_s) })

enum DRIVER_TYPE {
    LIMINE_VIDEO_DRIVER = 0,
    TIMER_DRIVER = 1,
    KEYBOARD_DRIVER = 2,
    MOUSE_DRIVER = 5,
    USB_DRIVER = 4,
    STORAGE_DRIVER = 6,
};

enum DEVICE_TYPE {
    PCI_DEVICE = 0,
    USB_DEVICE = 1,
    STORAGE_DEVICE = 2,
};

enum DRIVER_STATUS{
    DRIVER_STATUS_UNINITIALIZED = 0,
    DRIVER_STATUS_INITIALIZING = 1,
    DRIVER_STATUS_READY = 2,
    DRIVER_STATUS_FAILED = 3
};

typedef struct dependency {
    enum DRIVER_TYPE type;
    int sub_type;
} dependency;

typedef struct driver {
    const char* name;
    enum DRIVER_TYPE type;
    int sub_type;
    enum DRIVER_STATUS status;

    struct dependency* dependencies[MAX_DEPENDENCIES_PER_DRIVER];
    int dependency_count;

    void* self;
    void (*init)(void);
} driver;

extern struct driver* driver_table[AMOUNT_DRIVERS_TYPE][MAX_DRIVERS_PER_TYPE];
extern void* device_table[AMOUNT_DEVICES_TYPE][MAX(MAX_DEVICES_PER_TYPE, MAX_PCI_DEVICES)];
extern spinlock_t driver_lock;

void init_drivers(void);

void register_driver(struct driver* drv);
void* get_self_driver(enum DRIVER_TYPE type, int sub_type);
struct driver* get_meta_driver(enum DRIVER_TYPE type, int sub_type);

#endif
