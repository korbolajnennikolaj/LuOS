#include "fs/fs.h"

#include "components/logger.h"
#include "fs/exfat.h"
#include "fs/ext4.h"
#include "fs/fat32.h"
#include "fs/iso9660.h"

static const char *fs_type_label(enum fs_type type) {
    switch (type) {
        case FS_TYPE_FAT32: return "FAT32";
        case FS_TYPE_EXFAT: return "exFAT";
        case FS_TYPE_EXT4: return "ext4";
        case FS_TYPE_ISO9660: return "ISO9660";
        default: return "unknown";
    }
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
        case FS_TYPE_FAT32: return fs_log_mount(dev, type, out, fat32_mount(dev, out));
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
    if (fat32_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_FAT32, out, fat32_mount(dev, out));
    if (ext4_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_EXT4, out, ext4_mount(dev, out));
    if (iso9660_probe(dev) == FS_OK) return fs_log_mount(dev, FS_TYPE_ISO9660, out, iso9660_mount(dev, out));

    return FS_ERR_CORRUPT;
}
