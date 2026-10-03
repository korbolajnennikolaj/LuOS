#ifndef LUOS_NTFS_H
#define LUOS_NTFS_H

#include "drivers/Storage/block_device.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTFS_OEM_ID "NTFS    "
#define NTFS_FIXUP_STRIDE 512u

#define NTFS_REC_ROOT 5u
#define NTFS_REC_VOLUME 3u
#define NTFS_REC_UPCASE 10u
#define NTFS_REC_FIRST_USER 24u

#define NTFS_AT_STANDARD_INFORMATION 0x10u
#define NTFS_AT_ATTRIBUTE_LIST 0x20u
#define NTFS_AT_FILE_NAME 0x30u
#define NTFS_AT_VOLUME_NAME 0x60u
#define NTFS_AT_DATA 0x80u
#define NTFS_AT_INDEX_ROOT 0x90u
#define NTFS_AT_INDEX_ALLOCATION 0xA0u
#define NTFS_AT_BITMAP 0xB0u
#define NTFS_AT_END 0xFFFFFFFFu

#define NTFS_ATTR_FLAG_COMPRESSED 0x0001u
#define NTFS_ATTR_FLAG_ENCRYPTED 0x4000u
#define NTFS_ATTR_FLAG_SPARSE 0x8000u

#define NTFS_MFT_FLAG_IN_USE 0x0001u
#define NTFS_MFT_FLAG_DIRECTORY 0x0002u

#define NTFS_INDEX_ENTRY_SUBNODE 0x0001u
#define NTFS_INDEX_ENTRY_LAST 0x0002u

#define NTFS_NAMESPACE_DOS 2u

#define NTFS_REF_MASK 0x0000FFFFFFFFFFFFull

typedef struct __attribute__((packed)) {
    uint8_t jmp[3];
    char oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t unused0[3];
    uint16_t unused1;
    uint8_t media;
    uint16_t unused2;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t unused3;
    uint32_t unused4;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mftmirr_lcn;
    int8_t clusters_per_mft_record;
    uint8_t unused5[3];
    int8_t clusters_per_index_record;
    uint8_t unused6[3];
    uint64_t volume_serial;
} ntfs_bootsector_t;

typedef struct ntfs_run {
    uint64_t vcn;
    int64_t lcn;
    uint64_t length;
} ntfs_run_t;

typedef struct ntfs_attr {
    bool resident;
    uint16_t flags;
    uint8_t *value;
    uint64_t value_length;
    ntfs_run_t *runs;
    uint32_t run_count;
    uint64_t data_size;
    uint64_t init_size;
} ntfs_attr_t;

typedef struct ntfs_fs {
    struct block_device *dev;
    uint32_t bytes_per_sector;
    uint32_t cluster_size;
    uint64_t mft_lcn;
    uint32_t record_size;
    uint32_t index_record_size;
    ntfs_attr_t mft;
    uint16_t *upcase;
    char label[32];
} ntfs_fs_t;

typedef struct ntfs_file {
    ntfs_fs_t *fs;
    ntfs_attr_t data;
} ntfs_file_t;

typedef struct ntfs_dirent_cache {
    char name[FS_MAX_NAME + 1];
    bool is_dir;
    uint64_t size;
} ntfs_dirent_cache_t;

typedef struct ntfs_diriter {
    ntfs_dirent_cache_t *entries;
    uint32_t count;
    uint32_t capacity;
    uint32_t index;
} ntfs_diriter_t;

extern const fs_ops_t ntfs_ops;

#endif
