#ifndef USB_KEYBOARD_DRIVER_H
#define USB_KEYBOARD_DRIVER_H

#include "ps2_keyboard_driver.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct usb_keyboard_driver {
    uint8_t keyboard_type;

    struct ps2_keyboard_state *(*get_state)(void);

    void (*set_leds)(bool caps_lock, bool num_lock, bool scroll_lock);

    bool (*is_key_pressed)(uint8_t scancode);
    uint8_t (*get_key)(void);
    bool (*has_key)(void);
    char (*scancode_to_char)(uint8_t scancode);

    void (*keyboard_handler)(void);

    void (*input)(const char *prompt, char *buffer, uint32_t max_len,
                  uint32_t color, void (*print)(const char *, uint32_t));

    bool (*has_extended_key)(void);
    uint16_t (*get_extended_key)(void);

    int (*keyboard_count)(void);

    uint8_t (*last_key_modifiers)(void);
} usb_keyboard_driver;

struct usb_keyboard_driver *return_usb_keyboard_driver(void);
struct driver *return_meta_usb_keyboard_driver(void);

#endif
