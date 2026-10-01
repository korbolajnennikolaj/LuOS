#include "ata.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "kernel/scheduler/spinlock.h"

#include <ports.h>
#include <stddef.h>
#include <string.h>

static spinlock_t ata_lock = SPINLOCK_INIT;

static void (*delay_ms)(uint64_t);

#define ATA_DBG 0
#if ATA_DBG
#define ATA_TRACE(...) LOG_DEBUG(__VA_ARGS__)
#else
#define ATA_TRACE(...) do { } while (0)
#endif

static void ata_delay_400ns(uint16_t base_port) {
    for (int i = 0; i < 4; i++) {
        inb(base_port + 0x0E);
    }
}

static uint8_t ata_get_status(ata_device_t *dev) {
    return inb(dev->base_port + 7);
}

static uint8_t ata_get_alt_status(ata_device_t *dev) {
    return inb(dev->control_port);
}

static int ata_wait_ready(ata_device_t *dev, bool check_bsy) {
    uint8_t status;
    int t = ATA_TIMEOUT_MS;

    ATA_TRACE("enter check_bsy=%d chan=%u drive=%u base=0x%x ctrl=0x%x alt=0x%02x status=0x%02x sel=0x%02x",
              (int)check_bsy, (unsigned)dev->channel, (unsigned)dev->drive,
              (unsigned)dev->base_port, (unsigned)dev->control_port,
              (unsigned)ata_get_alt_status(dev), (unsigned)ata_get_status(dev),
              (unsigned)inb(dev->base_port + 6));

    if (check_bsy) {
        while (t-- > 0) {
            status = ata_get_alt_status(dev);
            if (!(status & ATA_SR_BSY)) break;
            delay_ms(1);
        }
        ATA_TRACE("bsy-loop exit t=%d status=0x%02x", t, (unsigned)status);
        if (t <= 0) {
            LOG_ERROR("hd%c: BSY timeout, status=0x%02x", 'a' + dev->channel * 2 + dev->drive, (unsigned)status);
            return BLOCK_ERR_TIMEOUT;
        }
    }

    t = ATA_TIMEOUT_MS;
    int iter = 0;
    while (t-- > 0) {
        status = ata_get_alt_status(dev);
        if (iter < 5 || (iter % 200) == 0) {
            ATA_TRACE("drdy-loop iter=%d status=0x%02x", iter, (unsigned)status);
        }
        iter++;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY))
            return BLOCK_OK;
        if (status & ATA_SR_ERR) {
            LOG_ERROR("hd%c: error status=0x%02x error=0x%02x", 'a' + dev->channel * 2 + dev->drive,
                      (unsigned)status, (unsigned)inb(dev->base_port + 1));
            return BLOCK_ERR_IO;
        }
        delay_ms(1);
    }

    LOG_ERROR("hd%c: DRDY timeout, status=0x%02x", 'a' + dev->channel * 2 + dev->drive, (unsigned)status);
    return BLOCK_ERR_TIMEOUT;
}

static int ata_wait_data(ata_device_t *dev) {
    uint8_t status;
    int t = ATA_TIMEOUT_MS;

    while (t-- > 0) {
        status = ata_get_status(dev);
        if (status & ATA_SR_DRQ) return BLOCK_OK;
        if (status & ATA_SR_ERR) {
            uint8_t err_reg = inb(dev->base_port + 1);
            LOG_ERROR("hd%c: error status=0x%02x error_reg=0x%02x", 'a' + dev->channel * 2 + dev->drive,
                      (unsigned)status, (unsigned)err_reg);
            return BLOCK_ERR_IO;
        }
        if (status & ATA_SR_DF) {
            LOG_ERROR("hd%c: device fault, status=0x%02x", 'a' + dev->channel * 2 + dev->drive, (unsigned)status);
            return BLOCK_ERR_IO;
        }
        delay_ms(1);
    }

    LOG_ERROR("hd%c: DRQ timeout, status=0x%02x", 'a' + dev->channel * 2 + dev->drive, (unsigned)status);
    return BLOCK_ERR_TIMEOUT;
}

static void ata_select_drive(ata_device_t *dev) {
    uint8_t drive_sel = 0xA0;

    if (dev->drive == ATA_DRIVE_SLAVE)
        drive_sel |= 0x10;

    outb(dev->base_port + 6, drive_sel);
    ata_delay_400ns(dev->base_port);
}

static bool ata_identify_device(ata_device_t *dev) {
    LOG_DEBUG("IDENTIFY channel %u drive %u", (unsigned)dev->channel, (unsigned)dev->drive);

    uint16_t id[256];
    memset(id, 0, sizeof(id));

    ata_select_drive(dev);

    outb(dev->base_port + 1, 0);
    outb(dev->base_port + 2, 0);
    outb(dev->base_port + 3, 0);
    outb(dev->base_port + 4, 0);
    outb(dev->base_port + 5, 0);
    outb(dev->base_port + 7, ATA_CMD_IDENTIFY);

    uint8_t status = ata_get_status(dev);
    if (status == 0) {
        LOG_DEBUG("channel %u drive %u: no device (status 0)", (unsigned)dev->channel, (unsigned)dev->drive);
        return false;
    }

    int timeout = ATA_TIMEOUT_MS;
    while (timeout-- > 0) {
        status = ata_get_status(dev);

        if (status & ATA_SR_ERR) {
            LOG_DEBUG("channel %u drive %u: IDENTIFY aborted, status=0x%02x signature %02x/%02x",
                      (unsigned)dev->channel, (unsigned)dev->drive, (unsigned)status,
                      (unsigned)inb(dev->base_port + 4), (unsigned)inb(dev->base_port + 5));
            return false;
        }

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            break;

        delay_ms(1);
    }

    if (timeout <= 0) {
        uint8_t lba_mid = inb(dev->base_port + 4);
        uint8_t lba_high = inb(dev->base_port + 5);

        if ((lba_mid == 0x14 && lba_high == 0xEB) ||
            (lba_mid == 0x69 && lba_high == 0x96)) {
            dev->type = ATA_TYPE_ATAPI;
            LOG_INFO("channel %u drive %u: ATAPI device found, not supported", (unsigned)dev->channel, (unsigned)dev->drive);
            return false;
        }

        LOG_WARNING("channel %u drive %u: IDENTIFY timed out, status=0x%02x",
                    (unsigned)dev->channel, (unsigned)dev->drive, (unsigned)status);
        return false;
    }

    for (int i = 0; i < 256; i++) {
        id[i] = inw(dev->base_port);
    }

    memcpy(dev->identify_words, id, sizeof(id));

    dev->type = ATA_TYPE_ATA;

    #define ATA_ID_WORD(n) ((uint32_t)id[(n)])
    #define ATA_ID_DWORD(n) (ATA_ID_WORD(n) | (ATA_ID_WORD((n) + 1) << 16))
    #define ATA_ID_QWORD(n) ((uint64_t)ATA_ID_DWORD(n) | ((uint64_t)ATA_ID_DWORD((n) + 2) << 32))

    for (int i = 0; i < 40; i += 2) {
        dev->model[i] = (id[27 + i/2] >> 8) & 0xFF;
        dev->model[i + 1] = id[27 + i/2] & 0xFF;
    }
    dev->model[40] = '\0';

    for (int i = 39; i >= 0; i--) {
        if (dev->model[i] == ' ') {
            dev->model[i] = '\0';
        } else {
            break;
        }
    }

    for (int i = 0; i < 20; i += 2) {
        dev->serial[i] = (id[10 + i/2] >> 8) & 0xFF;
        dev->serial[i + 1] = id[10 + i/2] & 0xFF;
    }
    dev->serial[20] = '\0';

    for (int i = 19; i >= 0; i--) {
        if (dev->serial[i] == ' ') {
            dev->serial[i] = '\0';
        } else {
            break;
        }
    }

    for (int i = 0; i < 8; i += 2) {
        dev->firmware[i] = (id[23 + i/2] >> 8) & 0xFF;
        dev->firmware[i + 1] = id[23 + i/2] & 0xFF;
    }
    dev->firmware[8] = '\0';

    for (int i = 7; i >= 0; i--) {
        if (dev->firmware[i] == ' ') {
            dev->firmware[i] = '\0';
        } else {
            break;
        }
    }

    if (ATA_ID_WORD(83) & (1 << 10)) {
        dev->lba48_supported = 1;
        dev->features |= ATA_FEATURE_48BIT_LBA;
    } else {
        dev->lba48_supported = 0;
    }

    uint64_t total_sectors_48 = ATA_ID_QWORD(100);
    uint32_t total_sectors_28 = ATA_ID_DWORD(60);

    if (dev->lba48_supported && total_sectors_48 > 0) {
        dev->total_sectors = total_sectors_48;
    } else {
        dev->total_sectors = total_sectors_28;
    }

    if (ATA_ID_WORD(49) & (1 << 8)) {
        dev->dma_supported = 1;
        dev->features |= ATA_FEATURE_DMA;
    } else {
        dev->dma_supported = 0;
    }

    if (ATA_ID_WORD(88) & 0xFF) {
        dev->ultra_dma_supported = 1;
    } else {
        dev->ultra_dma_supported = 0;
    }

    dev->logical_sector_size = ATA_SECTOR_SIZE_512;
    dev->physical_sector_size = ATA_SECTOR_SIZE_512;
    dev->advanced_format = 0;

    uint16_t word106 = (uint16_t)ATA_ID_WORD(106);

    if (word106 != 0 && word106 != 0xFFFF && (word106 & (1 << 14)) && !(word106 & (1 << 15))) {
        if (word106 & (1 << 13)) {

            uint32_t logical_size = ATA_ID_DWORD(117);
            if (logical_size >= 256 && logical_size <= 0x7FFFFFFF) {
                dev->logical_sector_size = logical_size * 2;
            }
        }

        if (word106 & (1 << 12)) {
            dev->advanced_format = 1;
            uint8_t exponent = word106 & 0x0F;
            dev->physical_sector_size = dev->logical_sector_size << exponent;
        }
    }

    if (dev->advanced_format) {
        dev->features |= ATA_FEATURE_ADVANCED_FORMAT;
    }

    uint16_t max_sectors = (uint16_t)(ATA_ID_WORD(47) & 0xFF);

    if (max_sectors == 0) {
        dev->max_sectors_per_transfer = 256;
    } else {
        dev->max_sectors_per_transfer = max_sectors;
    }

    #undef ATA_ID_WORD
    #undef ATA_ID_DWORD
    #undef ATA_ID_QWORD

    LOG_DEBUG("IDENTIFY OK: model=\"%s\" serial=\"%s\" fw=\"%s\" sectors=%llu logical=%u physical=%u lba48=%u dma=%u udma=%u max_xfer=%u",
              dev->model, dev->serial, dev->firmware, (unsigned long long)dev->total_sectors,
              (unsigned)dev->logical_sector_size, (unsigned)dev->physical_sector_size,
              (unsigned)dev->lba48_supported, (unsigned)dev->dma_supported,
              (unsigned)dev->ultra_dma_supported, (unsigned)dev->max_sectors_per_transfer);

    return true;
}

static int ata_issue_cmd(ata_device_t *dev, bool write,
                         uint64_t lba, uint32_t sectors, void *buf) {
    if (!dev->present) {
        LOG_ERROR("channel %u drive %u: command to absent device", (unsigned)dev->channel, (unsigned)dev->drive);
        return BLOCK_ERR_PARAM;
    }

    if (dev->lba48_supported && sectors > 65536) {
        LOG_ERROR("%s: %u sectors exceeds LBA48 limit of 65536", dev->blkdev.name, sectors);
        return BLOCK_ERR_PARAM;
    }

    ATA_TRACE("%s: %c lba=0x%llx n=%u", dev->blkdev.name, write ? 'W' : 'R', (unsigned long long)lba, sectors);

    ata_select_drive(dev);

    if (ata_wait_ready(dev, true) < 0) {
        LOG_ERROR("%s: %s lba=%llu n=%u, drive not ready", dev->blkdev.name, write ? "write" : "read",
                  (unsigned long long)lba, sectors);
        return BLOCK_ERR_TIMEOUT;
    }

    uint8_t drive_bit = (dev->drive == ATA_DRIVE_SLAVE) ? 0x10 : 0x00;
    if (dev->lba48_supported) {
        outb(dev->base_port + 6, 0x40 | drive_bit);
    } else {
        outb(dev->base_port + 6, 0xE0 | drive_bit | ((lba >> 24) & 0x0F));
    }

    if (dev->lba48_supported) {

        outb(dev->base_port + 1, 0);
        outb(dev->base_port + 2, (sectors >> 8) & 0xFF);
        outb(dev->base_port + 3, (lba >> 24) & 0xFF);
        outb(dev->base_port + 4, (lba >> 32) & 0xFF);
        outb(dev->base_port + 5, (lba >> 40) & 0xFF);

        outb(dev->base_port + 1, 0);
        outb(dev->base_port + 2, sectors & 0xFF);
        outb(dev->base_port + 3, lba & 0xFF);
        outb(dev->base_port + 4, (lba >> 8) & 0xFF);
        outb(dev->base_port + 5, (lba >> 16) & 0xFF);

    } else {

        outb(dev->base_port + 1, 0);
        outb(dev->base_port + 2, sectors & 0xFF);
        outb(dev->base_port + 3, lba & 0xFF);
        outb(dev->base_port + 4, (lba >> 8) & 0xFF);
        outb(dev->base_port + 5, (lba >> 16) & 0xFF);
    }

    if (dev->lba48_supported) {
        if (write) {
            outb(dev->base_port + 7, ATA_CMD_WRITE_SECTORS_EXT);
        } else {
            outb(dev->base_port + 7, ATA_CMD_READ_SECTORS_EXT);
        }
    } else {
        if (write) {
            outb(dev->base_port + 7, ATA_CMD_WRITE_SECTORS);
        } else {
            outb(dev->base_port + 7, ATA_CMD_READ_SECTORS);
        }
    }

    uint8_t *buf_bytes = (uint8_t *)buf;

    for (uint32_t i = 0; i < sectors; i++) {
        if (write) {
            if (ata_wait_data(dev) < 0) {
                LOG_ERROR("%s: write lba=%llu sector %u/%u, no DRQ", dev->blkdev.name,
                          (unsigned long long)lba, i + 1, sectors);
                return BLOCK_ERR_IO;
            }

            for (int j = 0; j < (int)(dev->logical_sector_size / 2); j++) {
                uint16_t data = buf_bytes[i * dev->logical_sector_size + j * 2]
                | (buf_bytes[i * dev->logical_sector_size + j * 2 + 1] << 8);
                outw(dev->base_port, data);
            }

            ata_delay_400ns(dev->base_port);
        } else {
            if (ata_wait_data(dev) < 0) {
                LOG_ERROR("%s: read lba=%llu sector %u/%u, no DRQ", dev->blkdev.name,
                          (unsigned long long)lba, i + 1, sectors);
                return BLOCK_ERR_IO;
            }

            for (int j = 0; j < (int)(dev->logical_sector_size / 2); j++) {
                uint16_t data = inw(dev->base_port);
                buf_bytes[i * dev->logical_sector_size + j * 2] = data & 0xFF;
                buf_bytes[i * dev->logical_sector_size + j * 2 + 1] = (data >> 8) & 0xFF;
            }

            ata_delay_400ns(dev->base_port);
        }
    }

    return BLOCK_OK;
}

static int ata_blk_read(struct block_device *self,
                        uint64_t lba, uint32_t count, void *buf) {
    ata_device_t *dev = (ata_device_t *)self->priv;
    return ata_issue_cmd(dev, false, lba, count, buf);
}

static int ata_blk_write(struct block_device *self,
    uint64_t lba, uint32_t count, void *buf) {
    ata_device_t *dev = (ata_device_t *)self->priv;
    return ata_issue_cmd(dev, true, lba, count, buf);
}

static int ata_blk_flush(struct block_device *self) {
    ata_device_t *dev = (ata_device_t *)self->priv;

    if (!dev->present) return BLOCK_ERR_PARAM;

    ata_select_drive(dev);

    if (ata_wait_ready(dev, true) < 0) {
        LOG_ERROR("%s: flush, drive not ready", dev->blkdev.name);
        return BLOCK_ERR_TIMEOUT;
    }

    if (dev->lba48_supported) {
        outb(dev->base_port + 7, ATA_CMD_FLUSH_CACHE_EXT);
    } else {
        outb(dev->base_port + 7, ATA_CMD_FLUSH_CACHE);
    }

    if (ata_wait_ready(dev, true) < 0) {
        LOG_ERROR("%s: flush cache did not complete", dev->blkdev.name);
        return BLOCK_ERR_TIMEOUT;
    }

    return BLOCK_OK;
}

static bool ata_channel_probe(ata_device_t *dev) {
    uint8_t drive_sel = 0xA0;
    if (dev->drive == ATA_DRIVE_SLAVE) drive_sel |= 0x10;
    outb(dev->base_port + 6, drive_sel);
    ata_delay_400ns(dev->base_port);

    uint8_t alt = inb(dev->control_port);
    if (alt == 0xFF) return false;

    outb(dev->base_port + 3, 0x55);
    outb(dev->base_port + 4, 0xAA);
    if (inb(dev->base_port + 3) != 0x55) return false;
    if (inb(dev->base_port + 4) != 0xAA) return false;

    outb(dev->base_port + 3, 0xAA);
    outb(dev->base_port + 4, 0x55);
    if (inb(dev->base_port + 3) != 0xAA) return false;
    if (inb(dev->base_port + 4) != 0x55) return false;

    return true;
}

static void ata_port_reset(ata_device_t *dev) {
    outb(dev->control_port, 0x04);
    ata_delay_400ns(dev->base_port);

    outb(dev->control_port, 0x00);
    ata_delay_400ns(dev->base_port);

    delay_ms(5);

    uint8_t status = ata_get_status(dev);

    if (status & ATA_SR_BSY) {
        LOG_WARNING("channel %u drive %u: still BSY after soft reset, status=0x%02x",
                    (unsigned)dev->channel, (unsigned)dev->drive, (unsigned)status);
    }
}

static bool ata_init_drive(ata_device_t *dev, uint8_t channel, uint8_t drive) {
    LOG_DEBUG("probing channel %u drive %u", (unsigned)channel, (unsigned)drive);

    memset(dev, 0, sizeof(ata_device_t));

    dev->channel = channel;
    dev->drive = drive;
    dev->type = ATA_TYPE_UNKNOWN;
    dev->logical_sector_size = ATA_SECTOR_SIZE_512;
    dev->physical_sector_size = ATA_SECTOR_SIZE_512;
    dev->max_sectors_per_transfer = 1;
    dev->present = false;

    if (channel == ATA_CHANNEL_PRIMARY) {
        dev->base_port = ATA_PRIMARY_DATA;
        dev->control_port = ATA_PRIMARY_CONTROL;
    } else {
        dev->base_port = ATA_SECONDARY_DATA;
        dev->control_port = ATA_SECONDARY_CONTROL;
    }

    if (!ata_channel_probe(dev)) {
        LOG_DEBUG("channel %u drive %u: no legacy IDE controller at 0x%x (normal on AHCI/NVMe systems)",
                  (unsigned)channel, (unsigned)drive, (unsigned)dev->base_port);
        return false;
    }

    ata_port_reset(dev);

    if (!ata_identify_device(dev)) {
        LOG_DEBUG("channel %u drive %u: no ATA device", (unsigned)channel, (unsigned)drive);
        return false;
    }

    dev->present = true;

    struct block_device *bd = &dev->blkdev;
    bd->name[0] = 'h';
    bd->name[1] = 'd';
    bd->name[2] = (char)('a' + channel * 2 + drive);
    bd->name[3] = '\0';
    bd->sector_count = dev->total_sectors;
    bd->sector_size = dev->logical_sector_size;
    bd->priv = dev;
    bd->read_sectors = ata_blk_read;
    bd->write_sectors = ata_blk_write;
    bd->flush = ata_blk_flush;

    LOG_INFO("%s: \"%s\" %llu sectors x %u bytes (%llu MB)%s", bd->name, dev->model,
             (unsigned long long)dev->total_sectors, (unsigned)dev->logical_sector_size,
             (unsigned long long)((dev->total_sectors * dev->logical_sector_size) >> 20),
             dev->lba48_supported ? ", LBA48" : "");

    block_device_register(bd);
    return true;
}

static struct ata_driver drv_ata;

static int ata_read(int disk, uint64_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= drv_ata.disk_count) return BLOCK_ERR_PARAM;
    spin_lock(&ata_lock);
    int ret = ata_issue_cmd(&drv_ata.drives[disk], false, lba, count, buf);
    spin_unlock(&ata_lock);
    return ret;
}

static int ata_write(int disk, uint64_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= drv_ata.disk_count) return BLOCK_ERR_PARAM;
    spin_lock(&ata_lock);
    int ret = ata_issue_cmd(&drv_ata.drives[disk], true, lba, count, buf);
    spin_unlock(&ata_lock);
    return ret;
}

static int ata_get_disk_count(void) {
    return drv_ata.disk_count;
}

static struct ata_driver drv_ata = {
    .disk_count = 0,
    .read = ata_read,
    .write = ata_write,
    .get_disk_count = ata_get_disk_count,
};

struct ata_driver *return_ata_driver(void) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc) { LOG_ERROR("no TSC driver, ATA disabled"); return NULL; }

    delay_ms = tsc->sleep_tsc_ms;

    int disk_idx = 0;

    for (int ch = 0; ch < 2; ch++) {
        for (int drv = 0; drv < 2; drv++) {
            if (disk_idx >= ATA_MAX_DRIVES) break;
            if (ata_init_drive(&drv_ata.drives[disk_idx], ch, drv)) {
                disk_idx++;
            }
        }
    }

    LOG_INFO("legacy ATA init done, %d disk(s)", disk_idx);

    drv_ata.disk_count = disk_idx;
    return disk_idx > 0 ? &drv_ata : NULL;
}

struct driver *return_meta_ata_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "ATA PIO Storage Driver",
        .type = STORAGE_DRIVER,
        .sub_type = ATA_STORAGE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &drv_ata,
        .init = (void *)return_ata_driver,
    };
    return &meta;
}
