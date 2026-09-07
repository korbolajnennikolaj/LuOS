#ifndef MOUSE_DRIVER_H
#define MOUSE_DRIVER_H

#include "ps2_mouse_driver.h"
#include "usb_mouse_driver.h"

#include <stdbool.h>
#include <stdint.h>

enum MOUSE_TYPE {
    PS2_MOUSE = 0,
    USB_MOUSE = 1,
    VIRTUAL_MOUSE = 2,
};

#define MOUSE_MAX_BACKENDS 2

typedef struct mouse_state {
    int32_t x;
    int32_t y;
    int32_t wheel;
    bool btn_left;
    bool btn_right;
    bool btn_middle;
} mouse_state_t;

typedef struct mouse_backend {
    enum MOUSE_TYPE type;
    bool active;
    union {
        struct ps2_mouse_driver *ps2;
        struct usb_mouse_driver *usb;
    } drv;
} mouse_backend;

typedef struct mouse_driver {
    struct mouse_backend backends[MOUSE_MAX_BACKENDS];
    uint8_t backend_count;

    mouse_state_t *(*get_state)(void);
    void (*reset_deltas)(void);
    void (*mouse_handler)(void);

    int (*register_backend)(enum MOUSE_TYPE type, void *drv);
    int (*get_active_type)(void);
} mouse_driver;

struct mouse_driver *return_mouse_driver(void);
struct driver *return_meta_mouse_driver(void);

#endif
