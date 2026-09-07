#ifndef LUOS_EXFAT_H
#define LUOS_EXFAT_H

#include "drivers/Storage/block_device.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXFAT_BOOT_SIG_OFFSET 510
#define EXFAT_BOOT_SIG_0 0x55
#define EXFAT_BOOT_SIG_1 0xAA

typedef struct __attribute__((packed)) {
    uint8_t jmp[3];
    char fs_name[8];
    uint8_t must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t first_cluster_of_root;
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t bytes_per_sector_shift;
    uint8_t sectors_per_cluster_shift;
    uint8_t number_of_fats;
    uint8_t drive_select;
    uint8_t percent_in_use;
    uint8_t reserved[7];
} exfat_bootsector_t;

#define EXFAT_ENTRY_EOD 0x00
#define EXFAT_ENTRY_INUSE_BIT 0x80
#define EXFAT_ENTRY_TYPE_BITMAP 0x81
#define EXFAT_ENTRY_TYPE_UPCASE 0x82
#define EXFAT_ENTRY_TYPE_LABEL 0x83
#define EXFAT_ENTRY_TYPE_FILE 0x85
#define EXFAT_ENTRY_TYPE_STREAM 0xC0
#define EXFAT_ENTRY_TYPE_FILENAME 0xC1

#define EXFAT_ATTR_READ_ONLY 0x0001
#define EXFAT_ATTR_HIDDEN 0x0002
#define EXFAT_ATTR_SYSTEM 0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE 0x0020

#define EXFAT_STREAM_FLAG_NOFATCHAIN 0x02

typedef struct __attribute__((packed)) {
    uint8_t entry_type;
    uint8_t secondary_count;
    uint16_t set_checksum;
    uint16_t file_attributes;
    uint16_t reserved1;
    uint32_t create_timestamp;
    uint32_t modified_timestamp;
    uint32_t accessed_timestamp;
    uint8_t create_10ms;
    uint8_t modified_10ms;
    uint8_t create_utc_offset;
    uint8_t modified_utc_offset;
    uint8_t accessed_utc_offset;
    uint8_t reserved2[7];
} exfat_file_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t entry_type;
    uint8_t general_flags;
    uint8_t reserved1;
    uint8_t name_length;
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_stream_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t entry_type;
    uint8_t general_flags;
    uint16_t name[15];
} exfat_filename_entry_t;

typedef struct exfat_fs {
    struct block_device *dev;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_offset_lba;
    uint32_t fat_length_sectors;
    uint32_t cluster_heap_offset_lba;
    uint32_t cluster_count;
    uint32_t root_cluster;
    char label[24];
} exfat_fs_t;

typedef struct exfat_file {
    exfat_fs_t *fs;
    uint32_t first_cluster;
    bool no_fat_chain;
    uint64_t size;

    uint32_t cache_index;
    uint32_t cache_cluster;
} exfat_file_t;

typedef struct exfat_diriter {
    exfat_fs_t *fs;
    uint32_t dir_first_cluster;
    bool dir_no_fat_chain;
    uint64_t pos;
    uint8_t *cluster_buf;
    uint32_t cache_index;
    uint32_t cache_cluster;
    uint32_t loaded_cluster_idx;
} exfat_diriter_t;

extern const fs_ops_t exfat_ops;

#endif
