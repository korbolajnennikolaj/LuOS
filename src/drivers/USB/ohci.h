#ifndef OHCI_H
#define OHCI_H

#include "kernel/scheduler/spinlock.h"
#include "usb_controller.h"

#include <stdint.h>

struct tsc_driver;
struct usb_device;

#define MAX_OHCI_CONTROLLERS 8
#define MAX_OHCI_HID_SLOTS 4
#define MAX_OHCI_BULK_SLOTS 4
#define OHCI_MAX_PORTS 15

#define OHCI_HcRevision 0x00
#define OHCI_HcControl 0x04
#define OHCI_HcCommandStatus 0x08
#define OHCI_HcInterruptStatus 0x0C
#define OHCI_HcInterruptEnable 0x10
#define OHCI_HcInterruptDisable 0x14
#define OHCI_HcHCCA 0x18
#define OHCI_HcPeriodCurrentED 0x1C
#define OHCI_HcControlHeadED 0x20
#define OHCI_HcControlCurrentED 0x24
#define OHCI_HcBulkHeadED 0x28
#define OHCI_HcBulkCurrentED 0x2C
#define OHCI_HcDoneHead 0x30
#define OHCI_HcFmInterval 0x34
#define OHCI_HcFmRemaining 0x38
#define OHCI_HcFmNumber 0x3C
#define OHCI_HcPeriodicStart 0x40
#define OHCI_HcLSThreshold 0x44
#define OHCI_HcRhDescriptorA 0x48
#define OHCI_HcRhDescriptorB 0x4C
#define OHCI_HcRhStatus 0x50
#define OHCI_HcRhPortStatus 0x54

typedef struct ohci_td {
    uint32_t flags;
    uint32_t cbp;
    uint32_t next_td;
    uint32_t be;
} __attribute__((packed, aligned(16))) ohci_td;

typedef struct ohci_iso_td {
    uint32_t flags;
    uint32_t bp0;
    uint32_t next_td;
    uint32_t be;
    uint16_t offset[8];
} __attribute__((packed, aligned(32))) ohci_iso_td;

typedef struct ohci_hcca {
    uint32_t hcca_interrupt_table[32];
    uint16_t hcca_frame_number;
    uint16_t hcca_pad1;
    uint32_t hcca_done_head;
    uint8_t reserved[120];
} __attribute__((packed, aligned(256))) ohci_hcca;

typedef struct ohci_controller {
    enum USB_CONTROLLER_TYPE type;
    uint64_t base_addr;
    uint8_t num_ports;
    uint8_t initialized;
    uint32_t next_td_index;

    uint8_t is_low_speed;
    uint8_t power_on_delay_ms;
    spinlock_t lock;
} ohci_controller;

typedef struct ohci_driver {
    int (*get_controller_count)(void);
    struct ohci_controller* (*get_controller)(int index);
    int (*control_transfer)(struct ohci_controller* ohci,
                            uint8_t dev_addr, uint8_t endpoint,
                            void* setup_packet, uint16_t setup_len,
                            void* data, uint16_t data_len, uint8_t direction);
    int (*interrupt_transfer)(struct ohci_controller* ohci,
                              uint8_t dev_addr, uint8_t endpoint,
                              void* data, uint16_t data_len, uint8_t direction);

    int (*bulk_transfer)(struct ohci_controller* ohci,
                         uint8_t dev_addr, uint8_t endpoint,
                         void* data, uint16_t data_len, uint8_t direction);

    int (*iso_transfer)(struct ohci_controller* ohci,
                        uint8_t dev_addr, uint8_t endpoint,
                        void* data, uint16_t total_len,
                        uint8_t n_frames, const uint16_t* frame_lens,
                        uint8_t direction);

    int (*reset_port)(struct ohci_controller* ohci, uint8_t port);
    int (*enumerate_device)(struct ohci_controller* ohci, uint8_t port);
    void (*start)(struct ohci_controller* ohci);
    void (*stop)(struct ohci_controller* ohci);
    void (*reset_endpoint_toggle)(struct ohci_controller* ohci, uint8_t dev_addr, uint8_t endpoint);

    void (*notify_disconnect)(struct usb_device* dev);
} ohci_driver;

extern uint8_t ohci_dev_is_ls[MAX_OHCI_CONTROLLERS][128];
extern uint8_t ohci_dev_mps0[MAX_OHCI_CONTROLLERS][128];

struct ohci_driver* return_ohci_driver(void);
struct driver* return_meta_ohci_driver(void);
void ohci_irq(void);
void ohci_poll_iso(struct ohci_controller* o);
int ohci_controller_index(struct ohci_controller* o);
uint8_t ohci_port_count(struct ohci_controller* o);
void ohci_delay_ms(uint64_t ms);
uint32_t ohci_mmio_read(struct ohci_controller* o, uint32_t reg);
void ohci_mmio_write(struct ohci_controller* o, uint32_t reg, uint32_t val);

#endif
