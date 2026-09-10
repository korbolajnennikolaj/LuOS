#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "components/drivers.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_BLOCK_DEVICES 64

enum STORAGE_TYPE
{
    AHCI_STORAGE = 0,
    NVME_STORAGE = 1,
    USBMSC_STORAGE = 2,
    ATA_STORAGE = 3,
};

typedef struct block_device {
    char name[16];
    uint64_t sector_count;
    uint32_t sector_size;

    void *priv;

    int (*read_sectors)(struct block_device*, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct block_device*, uint64_t lba, uint32_t count, void *buf);
    int (*flush)(struct block_device*);
} block_device;

void block_device_register(block_device *dev);

void block_device_unregister(block_device *dev);

block_device* block_device_get(uint32_t index);
uint32_t get_disk_index_from_name(const char *name);
uint32_t get_device_count();

#endif
