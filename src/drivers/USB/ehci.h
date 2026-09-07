#ifndef EHCI_H
#define EHCI_H

#include "drivers/Timer/tsc_driver.h"
#include "usb_controller.h"

#include <stdint.h>

#define MAX_EHCI_CONTROLLERS 8

#define EHCI_CAPLENGTH 0x00
#define EHCI_HCSPARAMS 0x04
#define EHCI_USBCMD 0x00
#define EHCI_USBSTS 0x04
#define EHCI_USBINTR 0x08
#define EHCI_ASYNCLISTADDR 0x18
#define EHCI_PORTSC 0x44

#define EHCI_PERIODICLISTBASE 0x14

#define EHCI_PID_OUT 0
#define EHCI_PID_IN 1
#define EHCI_PID_SETUP 2

struct usb_device;

typedef struct ehci_qh {
    uint32_t horiz_link;
    uint32_t characteristics;
    uint32_t caps_overlay;
    uint32_t cur_link;
    uint32_t next_link;
    uint32_t alt_link;
    uint32_t token;
    uint32_t buffer[5];
    uint32_t ext_buffer[5];
} __attribute__((packed, aligned(32))) ehci_qh;

typedef struct ehci_qtd {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buffer[5];
    uint32_t ext_buffer[5];
} __attribute__((packed, aligned(32))) ehci_qtd;

typedef struct ehci_itd {
    uint32_t next_link;
    uint32_t transaction[8];
    uint32_t buffer_page[7];
    uint32_t ext_buffer[7];
} __attribute__((packed, aligned(32))) ehci_itd;

typedef struct ehci_controller {
    enum USB_CONTROLLER_TYPE type;
    uint32_t cap_base;
    uint32_t op_base;
    uint8_t num_ports;
    uint8_t initialized;
    struct ehci_qh async_qh;
    uint32_t next_qtd_index;
} ehci_controller;

typedef struct ehci_driver {
    int (*get_controller_count)(void);
    struct ehci_controller* (*get_controller)(int index);
    int (*control_transfer)(struct ehci_controller* ehci, uint8_t dev_addr, uint8_t endpoint,
                            void* setup_packet, uint16_t setup_len,
                            void* data, uint16_t data_len, uint8_t direction);

    int (*interrupt_transfer)(struct ehci_controller* ehci, uint8_t dev_addr, uint8_t endpoint,
                             void* data, uint16_t data_len, uint8_t direction);

    int (*bulk_transfer)(struct ehci_controller* ehci, uint8_t dev_addr, uint8_t endpoint,
                         void* data, uint16_t data_len, uint8_t direction);

    int (*iso_transfer)(struct ehci_controller* ehci, uint8_t dev_addr, uint8_t endpoint,
                        void* data, uint16_t total_len,
                        uint8_t n_frames, const uint16_t* frame_lens,
                        uint8_t direction);

    void (*reset_endpoint_toggle)(struct usb_device *dev, uint8_t dev_addr, uint8_t endpoint);

    void (*notify_disconnect)(struct usb_device *dev);

    int (*reset_port)(struct ehci_controller* ehci, uint8_t port);
    int (*enumerate_device)(struct ehci_controller* ehci, uint8_t port);
    void (*start)(struct ehci_controller* ehci);
    void (*stop)(struct ehci_controller* ehci);
} ehci_driver;

struct ehci_driver* return_ehci_driver(void);
struct driver* return_meta_ehci_driver(void);
void ehci_irq(void);

#endif
