#ifndef LUOS_FAT32_H
#define LUOS_FAT32_H

#include "drivers/Storage/block_device.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t jmp[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved0[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} fat32_bpb_t;

#define FAT32_BOOT_SIG_OFFSET 510
#define FAT32_BOOT_SIG_0 0x55
#define FAT32_BOOT_SIG_1 0xAA

typedef struct __attribute__((packed)) {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat32_dirent_raw_t;

typedef struct __attribute__((packed)) {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_lo;
    uint16_t name3[2];
} fat32_lfn_raw_t;

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LFN (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | \
                               FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

#define FAT32_DIRENT_FREE 0x00
#define FAT32_DIRENT_DELETED 0xE5
#define FAT32_DIRENT_KANJI_E5 0x05

#define FAT32_CLUSTER_FREE 0x00000000u
#define FAT32_CLUSTER_BAD 0x0FFFFFF7u
#define FAT32_CLUSTER_EOC_MIN 0x0FFFFFF8u
#define FAT32_CLUSTER_EOC 0x0FFFFFFFu
#define FAT32_CLUSTER_MASK 0x0FFFFFFFu

typedef struct fat32_fs {
    struct block_device *dev;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sector_count;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t total_sectors;
    uint32_t total_clusters;
    uint32_t next_free_hint;
    char label[12];
} fat32_fs_t;

typedef struct fat32_file {
    fat32_fs_t *fs;

    uint32_t first_cluster;
    uint32_t alloc_clusters;
    uint32_t last_cluster;

    uint32_t cache_index;
    uint32_t cache_cluster;

    uint8_t *iobuf;

    uint32_t dirent_cluster;
    uint32_t dirent_offset;

    bool dirty;
} fat32_file_t;

typedef struct fat32_diriter {
    fat32_fs_t *fs;
    uint32_t dir_start_cluster;
    uint32_t cur_cluster;
    uint32_t entry_index;
    uint8_t *cluster_buf;
} fat32_diriter_t;

extern const fs_ops_t fat32_ops;

#endif
