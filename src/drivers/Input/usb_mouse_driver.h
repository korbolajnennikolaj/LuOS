#ifndef USB_MOUSE_DRIVER_H
#define USB_MOUSE_DRIVER_H

#include "components/drivers.h"

#include <stdbool.h>
#include <stdint.h>

#define USB_MOUSE_CLASS 0x03
#define USB_MOUSE_SUBCLASS 0x01

#define USB_MOUSE_PROTOCOL 0x02
#define USB_MOUSE_PROTOCOL_GENERIC 0x00

#define HID_GET_REPORT 0x01
#define HID_SET_REPORT 0x09
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B

#define MAX_USB_MICE 4

typedef struct usb_mouse_state {
    int32_t x;
    int32_t y;
    int32_t wheel;
    bool btn_left;
    bool btn_right;
    bool btn_middle;
} usb_mouse_state;

typedef struct usb_mouse_driver {
    struct usb_mouse_state *(*get_state)(void);
    void (*reset_deltas)(void);
    int (*mouse_count)(void);
    void (*mouse_handler)(void);
} usb_mouse_driver;

void usb_mouse_handler_poll(void);
struct usb_mouse_driver *return_usb_mouse_driver(void);
struct driver *return_meta_usb_mouse_driver(void);

#endif
