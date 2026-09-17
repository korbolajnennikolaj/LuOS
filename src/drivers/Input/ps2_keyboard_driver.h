#ifndef PS2_KEYBOARD_DRIVER_H
#define PS2_KEYBOARD_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#define PS2_KEYBOARD_BUFFER_SIZE 128

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUTPUT_FULL (1 << 0)
#define PS2_STATUS_INPUT_FULL (1 << 1)
#define PS2_STATUS_AUX_DATA (1 << 5)

#define PS2_ACK 0xFA
#define PS2_RESEND 0xFE

#define PS2_KEY_ESC 0x01
#define PS2_KEY_1 0x02
#define PS2_KEY_2 0x03
#define PS2_KEY_3 0x04
#define PS2_KEY_4 0x05
#define PS2_KEY_5 0x06
#define PS2_KEY_6 0x07
#define PS2_KEY_7 0x08
#define PS2_KEY_8 0x09
#define PS2_KEY_9 0x0A
#define PS2_KEY_0 0x0B
#define PS2_KEY_MINUS 0x0C
#define PS2_KEY_EQUALS 0x0D
#define PS2_KEY_BACKSPACE 0x0E
#define PS2_KEY_TAB 0x0F
#define PS2_KEY_Q 0x10
#define PS2_KEY_W 0x11
#define PS2_KEY_E 0x12
#define PS2_KEY_R 0x13
#define PS2_KEY_T 0x14
#define PS2_KEY_Y 0x15
#define PS2_KEY_U 0x16
#define PS2_KEY_I 0x17
#define PS2_KEY_O 0x18
#define PS2_KEY_P 0x19
#define PS2_KEY_LBRACKET 0x1A
#define PS2_KEY_RBRACKET 0x1B
#define PS2_KEY_ENTER 0x1C
#define PS2_KEY_LCTRL 0x1D
#define PS2_KEY_A 0x1E
#define PS2_KEY_S 0x1F
#define PS2_KEY_D 0x20
#define PS2_KEY_F 0x21
#define PS2_KEY_G 0x22
#define PS2_KEY_H 0x23
#define PS2_KEY_J 0x24
#define PS2_KEY_K 0x25
#define PS2_KEY_L 0x26
#define PS2_KEY_SEMICOLON 0x27
#define PS2_KEY_APOSTROPHE 0x28
#define PS2_KEY_GRAVE 0x29
#define PS2_KEY_LSHIFT 0x2A
#define PS2_KEY_BACKSLASH 0x2B
#define PS2_KEY_Z 0x2C
#define PS2_KEY_X 0x2D
#define PS2_KEY_C 0x2E
#define PS2_KEY_V 0x2F
#define PS2_KEY_B 0x30
#define PS2_KEY_N 0x31
#define PS2_KEY_M 0x32
#define PS2_KEY_COMMA 0x33
#define PS2_KEY_DOT 0x34
#define PS2_KEY_SLASH 0x35
#define PS2_KEY_RSHIFT 0x36
#define PS2_KEY_KP_MULTIPLY 0x37
#define PS2_KEY_LALT 0x38
#define PS2_KEY_SPACE 0x39
#define PS2_KEY_CAPS_LOCK 0x3A
#define PS2_KEY_F1 0x3B
#define PS2_KEY_F2 0x3C
#define PS2_KEY_F3 0x3D
#define PS2_KEY_F4 0x3E
#define PS2_KEY_F5 0x3F
#define PS2_KEY_F6 0x40
#define PS2_KEY_F7 0x41
#define PS2_KEY_F8 0x42
#define PS2_KEY_F9 0x43
#define PS2_KEY_F10 0x44
#define PS2_KEY_NUM_LOCK 0x45
#define PS2_KEY_SCROLL_LOCK 0x46
#define PS2_KEY_KP_7 0x47
#define PS2_KEY_KP_8 0x48
#define PS2_KEY_KP_9 0x49
#define PS2_KEY_KP_MINUS 0x4A
#define PS2_KEY_KP_4 0x4B
#define PS2_KEY_KP_5 0x4C
#define PS2_KEY_KP_6 0x4D
#define PS2_KEY_KP_PLUS 0x4E
#define PS2_KEY_KP_1 0x4F
#define PS2_KEY_KP_2 0x50
#define PS2_KEY_KP_3 0x51
#define PS2_KEY_KP_0 0x52
#define PS2_KEY_KP_DOT 0x53
#define PS2_KEY_F11 0x57
#define PS2_KEY_F12 0x58

#define PS2_KEY_EXTENDED 0xE0

#define PS2_EXT_RCTRL 0x1D
#define PS2_EXT_RALT 0x38
#define PS2_EXT_KP_DIVIDE 0x35
#define PS2_EXT_KP_ENTER 0x1C
#define PS2_EXT_HOME 0x47
#define PS2_EXT_UP 0x48
#define PS2_EXT_PAGE_UP 0x49
#define PS2_EXT_LEFT 0x4B
#define PS2_EXT_RIGHT 0x4D
#define PS2_EXT_END 0x4F
#define PS2_EXT_DOWN 0x50
#define PS2_EXT_PAGE_DOWN 0x51
#define PS2_EXT_INSERT 0x52
#define PS2_EXT_DELETE 0x53
#define PS2_EXT_LMETA 0x5B
#define PS2_EXT_RMETA 0x5C
#define PS2_EXT_MENU 0x5D
#define PS2_EXT_PRINT_SCREEN 0x37
#define PS2_EXT_PAUSE 0x45

#define PS2_EXTKEY(byte) ((uint16_t)(0xE000u | (uint8_t)(byte)))

typedef struct ps2_keyboard_state {
    bool is_shift_pressed;
    bool is_ctrl_pressed;
    bool is_alt_pressed;
    bool is_rctrl_pressed;
    bool is_ralt_pressed;
    bool is_caps_lock;
    bool is_num_lock;
    bool is_scroll_lock;
    bool extended_pending;
} ps2_keyboard_state;

typedef struct ps2_keyboard_driver {
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

    uint16_t (*get_extended_key)(void);
    bool (*has_extended_key)(void);

    bool (*is_ext_key_pressed)(uint16_t ext_code);
} ps2_keyboard_driver;

void ps2_keyboard_handler(void);

uint64_t ps2_bus_acquire(void);
void ps2_bus_release(uint64_t flags);
struct ps2_keyboard_driver *return_ps2_keyboard_driver(void);
struct driver *return_meta_ps2_keyboard_driver(void);

#endif
