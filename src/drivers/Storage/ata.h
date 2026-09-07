#ifndef ATA_H
#define ATA_H

#include "block_device.h"
#include "components/drivers.h"

#include <stdbool.h>
#include <stdint.h>

#define ATA_PRIMARY_DATA 0x1F0
#define ATA_PRIMARY_ERROR 0x1F1
#define ATA_PRIMARY_FEATURES 0x1F1
#define ATA_PRIMARY_SECTOR_COUNT 0x1F2
#define ATA_PRIMARY_LBA_LOW 0x1F3
#define ATA_PRIMARY_LBA_MID 0x1F4
#define ATA_PRIMARY_LBA_HIGH 0x1F5
#define ATA_PRIMARY_DRIVE_SELECT 0x1F6
#define ATA_PRIMARY_COMMAND 0x1F7
#define ATA_PRIMARY_STATUS 0x1F7
#define ATA_PRIMARY_ALT_STATUS 0x3F6
#define ATA_PRIMARY_CONTROL 0x3F6
#define ATA_PRIMARY_DEV_CTRL 0x3F6

#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define ATA_SECONDARY_DATA 0x170
#define ATA_SECONDARY_ERROR 0x171
#define ATA_SECONDARY_FEATURES 0x171
#define ATA_SECONDARY_SECTOR_COUNT 0x172
#define ATA_SECONDARY_LBA_LOW 0x173
#define ATA_SECONDARY_LBA_MID 0x174
#define ATA_SECONDARY_LBA_HIGH 0x175
#define ATA_SECONDARY_DRIVE_SELECT 0x176
#define ATA_SECONDARY_COMMAND 0x177
#define ATA_SECONDARY_STATUS 0x177
#define ATA_SECONDARY_ALT_STATUS 0x376
#define ATA_SECONDARY_CONTROL 0x376
#define ATA_SECONDARY_DEV_CTRL 0x376

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_READ_SECTORS_EXT 0x24
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_SET_FEATURES 0xEF

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DSC 0x10
#define ATA_SR_DRQ 0x08
#define ATA_SR_CORR 0x04
#define ATA_SR_IDX 0x02
#define ATA_SR_ERR 0x01

#define ATA_TYPE_ATA 0x00
#define ATA_TYPE_ATAPI 0x01
#define ATA_TYPE_SATA 0x02
#define ATA_TYPE_UNKNOWN 0xFF

#define ATA_SECTOR_SIZE_512 512
#define ATA_SECTOR_SIZE_4096 4096

#define ATA_FEATURE_48BIT_LBA 0x08
#define ATA_FEATURE_DMA 0x10
#define ATA_FEATURE_ADVANCED_FORMAT 0x80

#define ATA_MAX_DRIVES 4
#define ATA_CHANNEL_PRIMARY 0
#define ATA_CHANNEL_SECONDARY 1
#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE 1

#define ATA_TIMEOUT_MS 1000

#define BLOCK_ERR_TIMEOUT -1
#define BLOCK_ERR_IO -2
#define BLOCK_OK 0
#define BLOCK_ERR_PARAM -3
#define BLOCK_ERR_UNSUPP -4

#define ATA_IDENTIFY_WORDS 256

typedef struct {
    uint8_t type;
    uint8_t channel;
    uint8_t drive;
    uint16_t base_port;
    uint16_t control_port;
    uint16_t features;
    uint64_t total_sectors;
    uint32_t logical_sector_size;
    uint32_t physical_sector_size;
    uint16_t max_sectors_per_transfer;
    uint8_t lba48_supported;
    uint8_t dma_supported;
    uint8_t ultra_dma_supported;
    uint8_t advanced_format;
    uint8_t present;
    char model[41];
    char serial[21];
    char firmware[9];
    uint16_t identify_words[ATA_IDENTIFY_WORDS];
    struct block_device blkdev;
} ata_device_t;

typedef struct ata_driver {
    int disk_count;
    ata_device_t drives[ATA_MAX_DRIVES];

    int (*read)(int disk,
                uint64_t lba, uint32_t count, void *buf);

    int (*write)(int disk,
                 uint64_t lba, uint32_t count, void *buf);

    int (*get_disk_count)(void);
} ata_driver;

struct ata_driver *return_ata_driver(void);
struct driver *return_meta_ata_driver(void);

#endif
