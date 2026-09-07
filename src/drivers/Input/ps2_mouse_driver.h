#ifndef PS2_MOUSE_DRIVER_H
#define PS2_MOUSE_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUTPUT_FULL (1 << 0)
#define PS2_STATUS_INPUT_FULL (1 << 1)
#define PS2_STATUS_AUX_DATA (1 << 5)

#define PS2_CMD_ENABLE_AUX 0xA8
#define PS2_CMD_DISABLE_AUX 0xA7
#define PS2_CMD_WRITE_AUX 0xD4
#define PS2_CMD_READ_CONFIG 0x20
#define PS2_CMD_WRITE_CONFIG 0x60

#define PS2_CONFIG_AUX_IRQ (1 << 1)
#define PS2_CONFIG_AUX_CLOCK (1 << 5)

#define MOUSE_CMD_RESET 0xFF
#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE_STREAM 0xF4
#define MOUSE_CMD_DISABLE_STREAM 0xF5
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define MOUSE_CMD_GET_DEVICE_ID 0xF2
#define MOUSE_CMD_STATUS_REQ 0xE9

#define MOUSE_PACKET_BTN_LEFT (1 << 0)
#define MOUSE_PACKET_BTN_RIGHT (1 << 1)
#define MOUSE_PACKET_BTN_MIDDLE (1 << 2)
#define MOUSE_PACKET_ALWAYS1 (1 << 3)
#define MOUSE_PACKET_X_SIGN (1 << 4)
#define MOUSE_PACKET_Y_SIGN (1 << 5)
#define MOUSE_PACKET_X_OVERFLOW (1 << 6)
#define MOUSE_PACKET_Y_OVERFLOW (1 << 7)

#define PS2_MOUSE_IRQ_VECTOR 44

typedef struct ps2_mouse_state {
    int32_t x;
    int32_t y;
    int32_t wheel;
    bool btn_left;
    bool btn_right;
    bool btn_middle;
    bool has_wheel;
} ps2_mouse_state;

typedef struct ps2_mouse_driver {
    uint8_t mouse_type;
    struct ps2_mouse_state *(*get_state)(void);
    void (*reset_deltas)(void);
    void (*mouse_handler)(void);
} ps2_mouse_driver;

void ps2_mouse_handler(void);
struct ps2_mouse_driver *return_ps2_mouse_driver(void);
struct driver *return_meta_ps2_mouse_driver(void);

#endif
