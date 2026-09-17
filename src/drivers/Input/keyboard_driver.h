#ifndef KEYBOARD_DRIVER_H
#define KEYBOARD_DRIVER_H

#include "ps2_keyboard_driver.h"
#include "usb_keyboard_driver.h"

#include <stdbool.h>
#include <stdint.h>

enum key_code {

    KEY_A = 0, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
    KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
    KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
    KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,

    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
    KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,

    KEY_MINUS,
    KEY_EQUALS,
    KEY_LBRACKET,
    KEY_RBRACKET,
    KEY_BACKSLASH,
    KEY_SEMICOLON,
    KEY_APOSTROPHE,
    KEY_GRAVE,
    KEY_COMMA,
    KEY_DOT,
    KEY_SLASH,

    KEY_ESC,
    KEY_TAB,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_SPACE,

    KEY_LSHIFT, KEY_RSHIFT,
    KEY_LCTRL, KEY_RCTRL,
    KEY_LALT, KEY_RALT,

    KEY_CAPS_LOCK,
    KEY_NUM_LOCK,
    KEY_SCROLL_LOCK,

    KEY_F1, KEY_F2, KEY_F3, KEY_F4,
    KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,

    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_INSERT, KEY_DELETE,

    KEY_KP_0, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4,
    KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
    KEY_KP_DOT,
    KEY_KP_PLUS, KEY_KP_MINUS, KEY_KP_MULTIPLY, KEY_KP_DIVIDE,
    KEY_KP_ENTER,

    KEY_LMETA,
    KEY_RMETA,
    KEY_MENU,
    KEY_PRINT_SCREEN,

    KEY_CODE_COUNT
};

enum KEYBOARD_TYPE {
    PS2_KEYBOARD = 0,
    USB_KEYBOARD = 1,
    VIRTUAL_KEYBOARD = 2,
};

#define KEYBOARD_MAX_BACKENDS 2

typedef struct keyboard_backend {
    enum KEYBOARD_TYPE type;
    bool active;
    union {
        struct ps2_keyboard_driver *ps2;
        struct usb_keyboard_driver *usb;
    } drv;
} keyboard_backend;

struct key_event {
    enum KEYBOARD_TYPE source;
    uint8_t scancode;
    enum key_code code;
    char ch;

    bool ctrl;
    bool shift;
    bool alt;

    bool is_enter;
    bool is_backspace;
};

typedef struct keyboard_driver {

    struct keyboard_backend backends[KEYBOARD_MAX_BACKENDS];
    uint8_t backend_count;

    struct ps2_keyboard_state *(*get_state)(void);

    void (*set_leds)(bool caps_lock, bool num_lock, bool scroll_lock);

    bool (*is_key_pressed)(uint8_t scancode);
    uint8_t (*get_key)(void);
    bool (*has_key)(void);
    char (*scancode_to_char)(uint8_t scancode);

    void (*keyboard_handler)(void);

    void (*claim_pump)(void);
    void (*release_pump)(void);

    bool (*has_extended_key)(void);
    uint16_t (*get_extended_key)(void);

    void (*input)(const char *prompt,
                  char *buffer,
                  uint32_t max_len,
                  uint32_t color,
                  void (*print)(const char *, uint32_t));

    int (*register_backend)(enum KEYBOARD_TYPE type, void *drv);
    int (*get_active_type)(void);

    bool (*is_special_key)(uint8_t scancode);

    bool (*is_key_pressed_enum)(enum key_code key);

    enum key_code (*scancode_to_key_code)(uint8_t sc);

    bool (*get_key_event)(struct key_event *out);

    bool (*is_key_held_from)(enum KEYBOARD_TYPE source, uint8_t scancode);
} keyboard_driver;

struct keyboard_driver *return_keyboard_driver(void);
struct driver *return_meta_keyboard_driver(void);

#endif
