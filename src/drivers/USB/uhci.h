#ifndef UHCI_H
#define UHCI_H

#include "kernel/scheduler/spinlock.h"
#include "usb_controller.h"

#include <stdint.h>

struct tsc_driver;
struct usb_device;

#define UHCI_CMD 0x00
#define UHCI_STS 0x02
#define UHCI_INTR 0x04
#define UHCI_FRNUM 0x06
#define UHCI_FRBASEADD 0x08
#define UHCI_PORTSC1 0x10
#define UHCI_LEGSUP 0xC0

#define UHCI_CMD_RUN 0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_CF 0x0040
#define UHCI_CMD_MAXP 0x0080

#define UHCI_STS_USBINT (1 << 0)
#define UHCI_STS_USBERR (1 << 1)
#define UHCI_STS_RD (1 << 2)
#define UHCI_STS_HSE (1 << 3)
#define UHCI_STS_HCPE (1 << 4)
#define UHCI_STS_HCH (1 << 5)

#define PORTSC_CSC (1 << 1)
#define PORTSC_PES (1 << 2)
#define PORTSC_PEC (1 << 3)
#define PORTSC_PR (1 << 9)

typedef struct uhci_td {
    uint32_t link_ptr;
    uint32_t control_status;
    uint32_t token;
    uint32_t buffer_ptr;
} __attribute__((packed)) uhci_td;

typedef struct uhci_controller {
    enum USB_CONTROLLER_TYPE type;
    uint16_t io_base;
    uint8_t num_ports;
    uint8_t initialized;

    uint32_t* frame_list;
    struct uhci_td* td_pool;
    uint32_t next_td_index;
    spinlock_t lock;
} uhci_controller;

typedef struct uhci_driver {
    int (*get_controller_count)(void);
    struct uhci_controller* (*get_controller)(int index);
    int (*control_transfer)(struct uhci_controller* uhci, uint8_t dev_addr, uint8_t endpoint, void* setup_packet, uint16_t setup_len, void* data, uint16_t data_len, uint8_t direction);
    int (*interrupt_transfer)(struct uhci_controller* uhci, uint8_t dev_addr, uint8_t endpoint, void* data, uint16_t data_len, uint8_t direction);

    int (*bulk_transfer)(struct uhci_controller* uhci, uint8_t dev_addr, uint8_t endpoint,
                         void* data, uint16_t data_len, uint8_t direction);

    int (*iso_transfer)(struct uhci_controller* uhci, uint8_t dev_addr, uint8_t endpoint,
                        void* data, uint16_t total_len,
                        uint8_t n_frames, const uint16_t* frame_lens,
                        uint8_t direction);

    int (*reset_port)(struct uhci_controller* uhci, uint8_t port);
    int (*enumerate_device)(struct uhci_controller* uhci, uint8_t port);
    void (*start)(struct uhci_controller* uhci);
    void (*stop)(struct uhci_controller* uhci);
    void (*reset_endpoint_toggle)(struct uhci_controller* uhci, uint8_t dev_addr, uint8_t endpoint);

    void (*notify_disconnect)(struct usb_device* dev);
} uhci_driver;

struct uhci_driver* return_uhci_driver(void);
struct driver* return_meta_uhci_driver(void);
void uhci_irq(void);

#endif
