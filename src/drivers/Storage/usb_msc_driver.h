#ifndef USB_MSC_DRIVER_H
#define USB_MSC_DRIVER_H

#include "components/drivers.h"
#include "drivers/Storage/block_device.h"
#include "drivers/USB/usb_core.h"

#include <stdbool.h>
#include <stdint.h>

#define USB_MSC_MAX_DEVICES 32

#define USB_CLASS_MSC 0x08
#define USB_SUBCLASS_SCSI 0x06
#define USB_PROTOCOL_BOT 0x50

#define USB_MSC_REQ_RESET 0xFF
#define USB_MSC_REQ_GET_MAX_LUN 0xFE

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A

#define CBW_SIGNATURE 0x43425355UL
#define CSW_SIGNATURE 0x53425355UL
#define CBW_FLAGS_IN 0x80
#define CBW_FLAGS_OUT 0x00

#define CSW_STATUS_GOOD 0x00
#define CSW_STATUS_FAILED 0x01
#define CSW_STATUS_PHASE_ERROR 0x02

#define SENSE_NO_SENSE 0x00
#define SENSE_NOT_READY 0x02
#define SENSE_MEDIUM_ERROR 0x03
#define SENSE_UNIT_ATTENTION 0x06

#define MSC_OK 0
#define MSC_ERR_IO -2
#define MSC_ERR_TIMEOUT -1
#define MSC_ERR_PARAM -3

typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
} __attribute__((packed)) usb_cbw_t;

typedef struct {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;
} __attribute__((packed)) usb_csw_t;

typedef struct {
    uint8_t peripheral;
    uint8_t removable;
    uint8_t version;
    uint8_t response_format;
    uint8_t additional_length;
    uint8_t reserved[3];
    char vendor_id[8];
    char product_id[16];
    char product_rev[4];
} __attribute__((packed)) scsi_inquiry_data_t;

typedef struct {
    uint32_t lba_last;
    uint32_t block_size;
} __attribute__((packed)) scsi_read_capacity_t;

typedef struct {
    uint8_t error_code;
    uint8_t reserved1;
    uint8_t sense_key;
    uint8_t info[4];
    uint8_t additional_length;
    uint8_t reserved2[4];
    uint8_t asc;
    uint8_t ascq;
    uint8_t reserved3[4];
} __attribute__((packed)) scsi_sense_data_t;

typedef struct usb_msc_device {
    struct usb_device *usb_dev;
    bool present;
    uint8_t lun;
    uint8_t ep_bulk_in;
    uint8_t ep_bulk_out;
    uint16_t ep_in_mps;
    uint16_t ep_out_mps;
    uint64_t sector_count;
    uint32_t sector_size;
    uint32_t cbw_tag;
    bool bulk_configured;
    struct block_device blkdev;
} usb_msc_device_t;

typedef struct usb_msc_driver {
    int disk_count;

    int (*read)(int disk, uint64_t lba, uint32_t count, void *buf);
    int (*write)(int disk, uint64_t lba, uint32_t count, void *buf);
    int (*get_disk_count)(void);
} usb_msc_driver;

int usb_msc_set_verbose(int on);

struct usb_msc_driver *return_usb_msc_driver(void);
struct driver *return_meta_usb_msc_driver(void);

#endif
