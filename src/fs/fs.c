#include "fs/fs.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "fs/exfat.h"
#include "fs/ext4.h"
#include "fs/fat32.h"
#include "fs/iso9660.h"

#include <string.h>

const char *fs_type_name(enum fs_type type) {
    switch (type) {
        case FS_TYPE_FAT12: return "FAT12";
        case FS_TYPE_FAT16: return "FAT16";
        case FS_TYPE_FAT32: return "FAT32";
        case FS_TYPE_EXFAT: return "exFAT";
        case FS_TYPE_EXT4: return "ext4";
        case FS_TYPE_ISO9660: return "ISO9660";
        case FS_TYPE_RAMFS: return "ramfs";
        case FS_TYPE_NTFS: return "NTFS";
        case FS_TYPE_ARCHIVE: return "archive";
        default: return "unknown";
    }
}

static const char *fs_type_label(enum fs_type type) {
    return fs_type_name(type);
}

static int fs_log_mount(struct block_device *dev, enum fs_type type, fs_t *out, int r) {
    const char *name = dev->name[0] ? dev->name : "?";
    if (r == FS_OK)
        LOG_DEBUG("mounted %s on '%s'%s%s%s", fs_type_label(out->type), name,
                 out->label[0] ? " (label \"" : "", out->label, out->label[0] ? "\")" : "");
    else
        LOG_WARNING("mounting %s on '%s' failed (error %d)", fs_type_label(type), name, r);
    return r;
}

int fs_mount_type(struct block_device *dev, enum fs_type type, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;
    switch (type) {
        case FS_TYPE_FAT12:
        case FS_TYPE_FAT16:
        case FS_TYPE_FAT32: return fs_log_mount(dev, type, out, fat32_mount(dev, out));
        case FS_TYPE_NTFS: return fs_log_mount(dev, type, out, ntfs_mount(dev, out));
        case FS_TYPE_ARCHIVE: return fs_log_mount(dev, type, out, archive_mount(dev, out));
        case FS_TYPE_EXFAT: return fs_log_mount(dev, type, out, exfat_mount(dev, out));
        case FS_TYPE_EXT4: return fs_log_mount(dev, type, out, ext4_mount(dev, out));
        case FS_TYPE_ISO9660: return fs_log_mount(dev, type, out, iso9660_mount(dev, out));
        default:
            LOG_ERROR("unsupported filesystem type %d requested for '%s'", (int)type, dev->name);
            return FS_ERR_PARAM;
    }
}

int fs_mount_auto(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;

    if (exfat_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_EXFAT, out, exfat_mount(dev, out));
    if (ntfs_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_NTFS, out, ntfs_mount(dev, out));
    if (fat32_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_FAT32, out, fat32_mount(dev, out));
    if (ext4_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_EXT4, out, ext4_mount(dev, out));
    if (iso9660_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_ISO9660, out, iso9660_mount(dev, out));
    if (archive_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_ARCHIVE, out, archive_mount(dev, out));

    return FS_ERR_CORRUPT;
}

#define FS_ZERO_CHUNK_BYTES 65536u
#define FS_WIPE_BYTES (1024u * 1024u)

int fs_dev_zero(struct block_device *dev, uint64_t lba, uint64_t count) {
    if (!dev || !dev->write_sectors) return FS_ERR_PARAM;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;
    uint32_t chunk = FS_ZERO_CHUNK_BYTES / ss;
    if (chunk == 0) chunk = 1;
    uint8_t *zero = kmalloc((size_t)chunk * ss);
    if (!zero) return FS_ERR_NOMEM;
    memset(zero, 0, (size_t)chunk * ss);

    int r = FS_OK;
    while (count > 0) {
        uint32_t n = count > chunk ? chunk : (uint32_t)count;
        if (dev->write_sectors(dev, lba, n, zero) != 0) {
            LOG_ERROR("%s: failed to clear %u sector(s) at LBA %llu", dev->name, n, (unsigned long long)lba);
            r = FS_ERR_IO;
            break;
        }
        lba += n;
        count -= n;
    }
    kfree(zero);
    return r;
}

int fs_dev_wipe(struct block_device *dev) {
    if (!dev) return FS_ERR_PARAM;
    uint32_t ss = dev->sector_size ? dev->sector_size : 512;
    uint64_t span = FS_WIPE_BYTES / ss;
    if (span > dev->sector_count) span = dev->sector_count;
    int r = fs_dev_zero(dev, 0, span);
    if (r != FS_OK) return r;
    if (dev->sector_count > span) {
        uint64_t tail = dev->sector_count - span;
        if (tail < span) tail = span;
        r = fs_dev_zero(dev, tail, dev->sector_count - tail);
    }
    return r;
}

uint32_t fs_new_serial(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint32_t v = lo ^ (hi * 0x9E3779B1u);
    v ^= v >> 16;
    v *= 0x85EBCA6Bu;
    v ^= v >> 13;
    return v ? v : 0x4C754F53u;
}

static bool fs_name_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

int fs_format_type_from_name(const char *name, struct block_device *dev, enum fs_type *out) {
    if (!name || !out) return FS_ERR_PARAM;
    if (fs_name_ieq(name, "fat12")) *out = FS_TYPE_FAT12;
    else if (fs_name_ieq(name, "fat16")) *out = FS_TYPE_FAT16;
    else if (fs_name_ieq(name, "fat32") || fs_name_ieq(name, "vfat")) *out = FS_TYPE_FAT32;
    else if (fs_name_ieq(name, "exfat")) *out = FS_TYPE_EXFAT;
    else if (fs_name_ieq(name, "fat") || fs_name_ieq(name, "msdos")) {
        if (!dev) return FS_ERR_PARAM;
        uint64_t bytes = dev->sector_count * (dev->sector_size ? dev->sector_size : 512);
        if (bytes < 16ull * 1024 * 1024) *out = FS_TYPE_FAT12;
        else if (bytes < 512ull * 1024 * 1024) *out = FS_TYPE_FAT16;
        else *out = FS_TYPE_FAT32;
    } else {
        return FS_ERR_NOSUPP;
    }
    return FS_OK;
}

int fs_format(struct block_device *dev, enum fs_type type, const char *label, uint64_t hidden_sectors) {
    if (!dev || !dev->write_sectors) return FS_ERR_PARAM;
    int r;
    switch (type) {
        case FS_TYPE_FAT12:
        case FS_TYPE_FAT16:
        case FS_TYPE_FAT32: r = fat_format(dev, type, label, hidden_sectors); break;
        case FS_TYPE_EXFAT: r = exfat_format(dev, label, hidden_sectors); break;
        default: return FS_ERR_NOSUPP;
    }
    if (r == FS_OK && dev->flush) dev->flush(dev);
    if (r == FS_OK) LOG_INFO("'%s' formatted as %s", dev->name, fs_type_name(type));
    else LOG_WARNING("formatting '%s' as %s failed (error %d)", dev->name, fs_type_name(type), r);
    return r;
}
