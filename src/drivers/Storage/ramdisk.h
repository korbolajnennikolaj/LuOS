#ifndef RAMDISK_H
#define RAMDISK_H

#include "block_device.h"
#include "components/drivers.h"

#include <stdint.h>

#define RAMDISK_MAX_DISKS 8
#define RAMDISK_SECTOR_SIZE 512u

typedef struct ramdisk {
    struct block_device blkdev;
    uint8_t *data;
    uint64_t size;
    char path[64];
} ramdisk_t;

typedef struct ramdisk_driver {
    int disk_count;
    struct ramdisk disks[RAMDISK_MAX_DISKS];
} ramdisk_driver;

struct ramdisk_driver *return_ramdisk_driver(void);
struct driver *return_meta_ramdisk_driver(void);

#endif
