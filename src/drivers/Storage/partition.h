#ifndef LUOS_PARTITION_H
#define LUOS_PARTITION_H

#include "drivers/Storage/block_device.h"

#include <stdbool.h>
#include <stdint.h>

#define PART_MAX_PER_DISK 32

#define PART_TYPE_GPT_PROTECTIVE 0xEE
#define PART_TYPE_EXTENDED_CHS 0x05
#define PART_TYPE_EXTENDED_LBA 0x0F

typedef struct partition_info {
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t mbr_type;
    uint8_t gpt_type_guid[16];
    bool bootable;
    bool is_gpt;
    char name[16];
} partition_info_t;

int partition_scan(struct block_device *disk, partition_info_t *out, int max, int *out_count);

int partition_register_all(struct block_device *disk);

int partition_unregister_all(struct block_device *disk);

#endif
