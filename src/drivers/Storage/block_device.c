#include "block_device.h"

#include "components/drivers.h"
#include "components/logger.h"

#include <string.h>

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
    LOG_DEBUG("block device '%s' registered as #%u (%llu sectors x %u bytes)", dev->name, count,
              (unsigned long long)dev->sector_count, (unsigned)dev->sector_size);
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
