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
    RAMDISK_STORAGE = 4,
};

struct block_cache;

typedef struct block_device {
    char name[16];
    uint64_t sector_count;
    uint32_t sector_size;

    void *priv;

    int (*read_sectors)(struct block_device*, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct block_device*, uint64_t lba, uint32_t count, void *buf);
    int (*flush)(struct block_device*);

    int (*raw_read_sectors)(struct block_device*, uint64_t lba, uint32_t count, void *buf);
    int (*raw_write_sectors)(struct block_device*, uint64_t lba, uint32_t count, void *buf);
    int (*raw_flush)(struct block_device*);
    struct block_cache *cache;
} block_device;

void block_device_register(block_device *dev);

void block_device_unregister(block_device *dev);

block_device* block_device_get(uint32_t index);
uint32_t get_disk_index_from_name(const char *name);
uint32_t get_device_count();

int block_cache_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int block_cache_write(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int block_cache_flush(struct block_device *dev);
void block_cache_invalidate(struct block_device *dev);

#endif
