#ifndef USB_CORE_H
#define USB_CORE_H

#include "drivers/USB/usb_event.h"
#include "root_hub.h"
#include "usb_controller.h"

#include <stdbool.h>
#include <stdint.h>

#define USB_CORE_SLOT 4
#define USB_DEVICE_FIRST_SLOT 5
#define MAX_USB_DEVICES (MAX_DRIVERS_PER_TYPE - USB_DEVICE_FIRST_SLOT)

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIGURATION 0x02
#define USB_DESC_STRING 0x03
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

#define HID_SET_PROTOCOL 0x0B
#define HID_SET_IDLE 0x0A

#define USB_EP_XFER_CONTROL 0x00
#define USB_EP_XFER_ISO 0x01
#define USB_EP_XFER_BULK 0x02
#define USB_EP_XFER_INTERRUPT 0x03

#define USB_MAX_BULK_EP 4
#define USB_MAX_ISO_EP 4

typedef struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor;

typedef struct usb_setup_packet {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet;

typedef struct usb_endpoint_info {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usb_endpoint_info;

typedef struct usb_device {
    uint8_t port;
    uint8_t address;
    struct usb_controller *ctrl;
    struct root_hub hub;
    struct usb_device_descriptor desc;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    char vendor_str[32];
    char product_str[32];

    uint8_t is_low_speed;

    uint8_t root_port;

    uint8_t hub_depth;

    uint32_t route_string;

    uint8_t endpoint_address;
    uint16_t max_packet_size;
    uint8_t hid_interface;
    uint8_t hid_interval;

    uint8_t hub_status_ep;
    uint16_t hub_status_ep_mps;
    uint8_t hub_status_interval;

    struct usb_endpoint_info bulk_ep[USB_MAX_BULK_EP];
    uint8_t bulk_ep_count;

    struct usb_endpoint_info iso_ep[USB_MAX_ISO_EP];
    uint8_t iso_ep_count;

    uint8_t driver_managed;

    uint8_t valid;
} usb_device;

typedef struct usb_core_driver {
    void (*scan_all)(void);

    int (*control_transfer)(struct usb_device *dev,
                             uint8_t type, uint8_t req,
                             uint16_t val, uint16_t idx,
                             uint16_t len, void *data);

    int (*interrupt_transfer)(struct usb_device *dev,
                               uint8_t endpoint,
                               void *data, uint16_t len,
                               uint8_t direction);

    void (*enqueue_event)(const usb_event_t *evt);

    void (*poll_transfers)(void);

    int (*bulk_transfer)(struct usb_device *dev,
                          uint8_t endpoint,
                          void *data, uint16_t len,
                          uint8_t direction);

    int (*iso_transfer)(struct usb_device *dev,
                         uint8_t endpoint,
                         void *data, uint16_t len,
                         uint8_t n_frames, const uint16_t *frame_lens,
                         uint8_t direction);

    void (*reset_endpoint_toggle)(struct usb_device *dev, uint8_t endpoint);
} usb_core_driver;

#define USB_ISO_MAX_FRAMES 8

struct usb_core_driver *return_usb_core_driver(void);
struct driver *return_meta_usb_core_driver(void);

int usb_get_device_count(void);
struct usb_device *usb_get_device(int idx);

void usb_core_remove_device(struct usb_device *dev);

#endif
