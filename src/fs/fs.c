#include "fs/fs.h"

#include "fs/exfat.h"
#include "fs/ext4.h"
#include "fs/fat32.h"
#include "fs/iso9660.h"

#include <stdio.h>

int fs_mount_type(struct block_device *dev, enum fs_type type, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;
    switch (type) {
        case FS_TYPE_FAT32: return fat32_mount(dev, out);
        case FS_TYPE_EXFAT: return exfat_mount(dev, out);
        case FS_TYPE_EXT4: return ext4_mount(dev, out);
        case FS_TYPE_ISO9660: return iso9660_mount(dev, out);
        default: return FS_ERR_PARAM;
    }
}

int fs_mount_auto(struct block_device *dev, fs_t *out) {
    if (!dev || !out) return FS_ERR_PARAM;

    if (exfat_probe(dev) == FS_OK) return exfat_mount(dev, out);
    if (fat32_probe(dev) == FS_OK) return fat32_mount(dev, out);
    if (ext4_probe(dev) == FS_OK) return ext4_mount(dev, out);
    if (iso9660_probe(dev) == FS_OK) return iso9660_mount(dev, out);

    printf_color(0xFFFF5555, "[fs] failed to determine the filesystem on disk '%s'\n",
                 dev->name[0] ? dev->name : "?");
    return FS_ERR_CORRUPT;
}
