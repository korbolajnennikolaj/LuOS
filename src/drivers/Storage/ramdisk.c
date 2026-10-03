#include "ramdisk.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "kernel/limine.h"

#include <string.h>

extern struct limine_module_request* get_module_request();

static struct ramdisk_driver drv_ramdisk;

static int ramdisk_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    ramdisk_t *rd = (ramdisk_t *)dev->priv;
    if (lba + count > dev->sector_count) return -1;
    memcpy(buf, rd->data + lba * RAMDISK_SECTOR_SIZE, (size_t)count * RAMDISK_SECTOR_SIZE);
    return 0;
}

static int ramdisk_write(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    ramdisk_t *rd = (ramdisk_t *)dev->priv;
    if (lba + count > dev->sector_count) return -1;
    memcpy(rd->data + lba * RAMDISK_SECTOR_SIZE, buf, (size_t)count * RAMDISK_SECTOR_SIZE);
    return 0;
}

static int ramdisk_flush(struct block_device *dev) {
    (void)dev;
    return 0;
}

struct ramdisk_driver *return_ramdisk_driver(void) {
    struct limine_module_request *req = get_module_request();
    drv_ramdisk.disk_count = 0;

    if (!req || !req->response || req->response->module_count == 0) {
        LOG_DEBUG("no boot modules, RAM disk driver idle");
        return NULL;
    }

    for (uint64_t i = 0; i < req->response->module_count && drv_ramdisk.disk_count < RAMDISK_MAX_DISKS; i++) {
        struct limine_file *mod = req->response->modules[i];
        if (!mod || !mod->address || mod->size == 0) continue;

        uint64_t sectors = (mod->size + RAMDISK_SECTOR_SIZE - 1) / RAMDISK_SECTOR_SIZE;
        uint8_t *data = kmalloc((size_t)(sectors * RAMDISK_SECTOR_SIZE));
        if (!data) {
            LOG_ERROR("module %llu: out of memory for %llu bytes", (unsigned long long)i, (unsigned long long)mod->size);
            continue;
        }
        memcpy(data, mod->address, (size_t)mod->size);
        memset(data + mod->size, 0, (size_t)(sectors * RAMDISK_SECTOR_SIZE - mod->size));

        int idx = drv_ramdisk.disk_count++;
        ramdisk_t *rd = &drv_ramdisk.disks[idx];
        rd->data = data;
        rd->size = mod->size;
        strncpy(rd->path, mod->path ? mod->path : "", sizeof(rd->path) - 1);
        rd->path[sizeof(rd->path) - 1] = '\0';

        struct block_device *bd = &rd->blkdev;
        bd->name[0] = 'r'; bd->name[1] = 'd';
        bd->name[2] = (char)('0' + idx);
        bd->name[3] = '\0';
        bd->sector_count = sectors;
        bd->sector_size = RAMDISK_SECTOR_SIZE;
        bd->priv = rd;
        bd->read_sectors = ramdisk_read;
        bd->write_sectors = ramdisk_write;
        bd->flush = ramdisk_flush;

        LOG_INFO("module '%s' -> %s: %llu bytes", rd->path, bd->name, (unsigned long long)rd->size);
        block_device_register(bd);
    }

    return drv_ramdisk.disk_count > 0 ? &drv_ramdisk : NULL;
}

struct driver *return_meta_ramdisk_driver(void) {
    static struct driver meta = {
        .name = "RAM Disk Driver",
        .type = STORAGE_DRIVER,
        .sub_type = RAMDISK_STORAGE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { 0 },
        .dependency_count = 0,
        .self = &drv_ramdisk,
        .init = (void *)return_ramdisk_driver,
    };
    return &meta;
}
