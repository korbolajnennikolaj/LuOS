#ifndef LUOS_ISO9660_H
#define LUOS_ISO9660_H

#include "drivers/Storage/block_device.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ISO9660_BLOCK_SIZE 2048u
#define ISO9660_PVD_LBA 16u
#define ISO9660_ID "CD001"

#define ISO9660_VD_TYPE_PRIMARY 1
#define ISO9660_VD_TYPE_TERMINATOR 255

#define ISO9660_FLAG_DIRECTORY 0x02

typedef struct __attribute__((packed)) {
    uint8_t record_len;
    uint8_t ext_attr_len;
    uint32_t extent_lba_le;
    uint32_t extent_lba_be;
    uint32_t data_len_le;
    uint32_t data_len_be;
    uint8_t recording_datetime[7];
    uint8_t file_flags;
    uint8_t file_unit_size;
    uint8_t interleave_gap;
    uint16_t vol_seq_le;
    uint16_t vol_seq_be;
    uint8_t name_len;

} iso9660_dirrecord_t;

typedef struct iso9660_fs {
    struct block_device *dev;
    uint32_t sectors_per_block;
    uint32_t root_extent_lba;
    uint32_t root_data_length;
    char label[33];
} iso9660_fs_t;

typedef struct iso9660_file {
    iso9660_fs_t *fs;
    uint32_t extent_lba;
    uint32_t size;
} iso9660_file_t;

typedef struct iso9660_diriter {
    iso9660_fs_t *fs;
    uint8_t *buf;
    uint32_t buf_len;
    uint32_t data_len;
    uint32_t offset;
} iso9660_diriter_t;

extern const fs_ops_t iso9660_ops;

#endif
