#include "block_device.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "kernel/scheduler/spinlock.h"

#include <string.h>

#define BLOCK_CACHE_LINES 32
#define BLOCK_CACHE_MAX_SECTORS 4
#define BLOCK_CACHE_MAX_BYTES (BLOCK_CACHE_MAX_SECTORS * 512)

typedef struct block_cache_line {
    uint64_t lba;
    uint32_t count;
    uint8_t valid;
    uint8_t data[BLOCK_CACHE_MAX_BYTES];
} block_cache_line_t;

typedef struct block_cache {
    spinlock_t lock;
    block_cache_line_t lines[BLOCK_CACHE_LINES];
} block_cache_t;

static int block_cache_line_contains(const block_cache_line_t *line, uint64_t lba, uint32_t count) {
    return line->valid &&
           lba >= line->lba &&
           lba + count <= line->lba + line->count;
}

static void block_cache_insert(block_cache_t *c, uint64_t lba, uint32_t count, const uint8_t *data) {
    if (count == 0 || count > BLOCK_CACHE_MAX_SECTORS) return;
    block_cache_line_t *line = &c->lines[lba % BLOCK_CACHE_LINES];
    line->lba = lba;
    line->count = count;
    memcpy(line->data, data, (size_t)count * 512u);
    line->valid = 1;
}

int block_cache_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    block_cache_t *c = dev ? dev->cache : NULL;
    if (!c || !dev->raw_read_sectors) {
        return dev && dev->raw_read_sectors ? dev->raw_read_sectors(dev, lba, count, buf) : -1;
    }
    if (count == 0 || count > BLOCK_CACHE_MAX_SECTORS) {
        return dev->raw_read_sectors(dev, lba, count, buf);
    }

    uint8_t *out = (uint8_t *)buf;
    uint64_t flags = spin_lock_irqsave(&c->lock);

    for (uint32_t i = 0; i < BLOCK_CACHE_LINES; i++) {
        if (block_cache_line_contains(&c->lines[i], lba, count)) {
            memcpy(out, c->lines[i].data + (size_t)(lba - c->lines[i].lba) * 512u, (size_t)count * 512u);
            spin_unlock_irqrestore(&c->lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&c->lock, flags);

    int r = dev->raw_read_sectors(dev, lba, count, out);
    if (r == 0) {
        flags = spin_lock_irqsave(&c->lock);
        block_cache_insert(c, lba, count, out);
        spin_unlock_irqrestore(&c->lock, flags);
    }
    return r;
}

int block_cache_write(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    block_cache_t *c = dev ? dev->cache : NULL;
    if (!dev || !dev->raw_write_sectors) return -1;

    int r = dev->raw_write_sectors(dev, lba, count, buf);
    if (r != 0) return r;

    if (c) {
        uint64_t flags = spin_lock_irqsave(&c->lock);
        for (uint32_t i = 0; i < BLOCK_CACHE_LINES; i++) {
            block_cache_line_t *line = &c->lines[i];
            if (line->valid && lba < line->lba + line->count && line->lba < lba + count) {
                line->valid = 0;
            }
        }
        spin_unlock_irqrestore(&c->lock, flags);
    }
    return r;
}

int block_cache_flush(struct block_device *dev) {
    if (!dev) return -1;
    if (dev->raw_flush) return dev->raw_flush(dev);
    if (dev->flush) return dev->flush(dev);
    return 0;
}

void block_cache_invalidate(struct block_device *dev) {
    block_cache_t *c = dev ? dev->cache : NULL;
    if (!c) return;
    uint64_t flags = spin_lock_irqsave(&c->lock);
    for (uint32_t i = 0; i < BLOCK_CACHE_LINES; i++) {
        c->lines[i].valid = 0;
    }
    spin_unlock_irqrestore(&c->lock, flags);
}

uint32_t get_device_count() {
    uint32_t count = 0;
    spin_lock(&driver_lock);
    for (uint32_t i = 0; i < MAX_BLOCK_DEVICES && i < MAX_DEVICES_PER_TYPE; i++) {
        if (device_table[STORAGE_DEVICE][i]) count++;
        else break;
    }
    spin_unlock(&driver_lock);
    return count;
}

void block_device_register(block_device *dev) {
    if (!dev) return;
    spin_lock(&driver_lock);
    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_BLOCK_DEVICES && i < MAX_DEVICES_PER_TYPE; i++) {
        if (device_table[STORAGE_DEVICE][i]) count++;
        else break;
    }
    if (count >= MAX_BLOCK_DEVICES || count >= MAX_DEVICES_PER_TYPE) {
        spin_unlock(&driver_lock);
        LOG_ERROR("block device table full, '%s' not registered", dev->name);
        return;
    }
    device_table[STORAGE_DEVICE][count] = dev;
    spin_unlock(&driver_lock);

    dev->raw_read_sectors = dev->read_sectors;
    dev->raw_write_sectors = dev->write_sectors;
    dev->raw_flush = dev->flush;
    dev->cache = NULL;

    if (dev->read_sectors && dev->sector_size == 512) {
        dev->cache = kmalloc(sizeof(block_cache_t));
        if (dev->cache) {
            memset(dev->cache, 0, sizeof(block_cache_t));
            dev->read_sectors = block_cache_read;
            dev->write_sectors = block_cache_write;
            dev->flush = block_cache_flush;
        }
    }

    LOG_DEBUG("block device '%s' registered as #%u (%llu sectors x %u bytes)%s", dev->name, count,
              (unsigned long long)dev->sector_count, (unsigned)dev->sector_size,
              dev->cache ? ", sector cache enabled" : "");
}

void block_device_unregister(block_device *dev) {
    if (!dev) return;

    uint32_t limit = MAX_BLOCK_DEVICES < MAX_DEVICES_PER_TYPE
                     ? MAX_BLOCK_DEVICES : MAX_DEVICES_PER_TYPE;
    bool found = false;

    spin_lock(&driver_lock);
    for (uint32_t i = 0; i < limit; i++) {
        if (device_table[STORAGE_DEVICE][i] != dev) continue;
        found = true;

        uint32_t j = i;
        while (j + 1 < limit && device_table[STORAGE_DEVICE][j + 1]) {
            device_table[STORAGE_DEVICE][j] = device_table[STORAGE_DEVICE][j + 1];
            j++;
        }
        device_table[STORAGE_DEVICE][j] = NULL;
        break;
    }
    spin_unlock(&driver_lock);

    if (dev->cache) {
        block_cache_invalidate(dev);
        kfree(dev->cache);
        dev->cache = NULL;
    }
    dev->read_sectors = dev->raw_read_sectors;
    dev->write_sectors = dev->raw_write_sectors;
    dev->flush = dev->raw_flush;

    if (found) LOG_DEBUG("block device '%s' unregistered", dev->name);
    else LOG_WARNING("block device '%s' was not registered", dev->name);
}

block_device* block_device_get(uint32_t index) {
    spin_lock(&driver_lock);
    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_BLOCK_DEVICES && i < MAX_DEVICES_PER_TYPE; i++) {
        if (device_table[STORAGE_DEVICE][i]) count++;
        else break;
    }
    if (index >= count) {
        spin_unlock(&driver_lock);
        return NULL;
    }
    block_device *dev = (block_device*)device_table[STORAGE_DEVICE][index];
    spin_unlock(&driver_lock);
    return dev;
}

uint32_t get_disk_index_from_name(const char *name) {
    uint32_t count = get_device_count();
    for (uint32_t i = 0; i < count; i++) {
        block_device *dev = block_device_get(i);
        if (dev && strcmp(dev->name, name) == 0) return i;
    }
    return UINT32_MAX;
}
