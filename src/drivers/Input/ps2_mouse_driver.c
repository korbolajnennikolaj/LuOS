#include "ps2_mouse_driver.h"

#include "components/drivers.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "kernel/sched/spinlock.h"
#include "mouse_driver.h"

#include <ports.h>
#include <stddef.h>

static struct ps2_mouse_state mouse_state = {0};
static spinlock_t mouse_lock = SPINLOCK_INIT;

#define PACKET_SIZE_STD 3
#define PACKET_SIZE_WHEEL 4

static uint8_t packet_buf[4];
static uint8_t packet_pos = 0;
static uint8_t packet_size = PACKET_SIZE_STD;

static void mouse_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL));
}

static void mouse_wait_read(void) {
    uint32_t timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL));
}

static uint8_t mouse_write(uint8_t cmd) {
    mouse_wait_write(); outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_AUX);
    mouse_wait_write(); outb(PS2_DATA_PORT, cmd);
    mouse_wait_read();
    return inb(PS2_DATA_PORT);
}

static bool mouse_detect_wheel(void) {
    mouse_write(MOUSE_CMD_SET_SAMPLE_RATE); mouse_write(200);
    mouse_write(MOUSE_CMD_SET_SAMPLE_RATE); mouse_write(100);
    mouse_write(MOUSE_CMD_SET_SAMPLE_RATE); mouse_write(80);
    mouse_write(MOUSE_CMD_GET_DEVICE_ID);
    return (inb(PS2_DATA_PORT) == 3);
}

void ps2_mouse_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
    if (!(status & PS2_STATUS_AUX_DATA)) return;

    uint8_t data = inb(PS2_DATA_PORT);

    if (packet_pos == 0 && !(data & MOUSE_PACKET_ALWAYS1)) return;

    packet_buf[packet_pos++] = data;
    if (packet_pos < packet_size) return;
    packet_pos = 0;

    uint8_t flags = packet_buf[0];

    spin_lock(&mouse_lock);

    mouse_state.btn_left = (flags & MOUSE_PACKET_BTN_LEFT) != 0;
    mouse_state.btn_right = (flags & MOUSE_PACKET_BTN_RIGHT) != 0;
    mouse_state.btn_middle = (flags & MOUSE_PACKET_BTN_MIDDLE) != 0;

    if ((flags & MOUSE_PACKET_X_OVERFLOW) || (flags & MOUSE_PACKET_Y_OVERFLOW)) {
        spin_unlock(&mouse_lock);
        return;
    }

    int32_t dx = (int8_t)packet_buf[1];
    int32_t dy = (int8_t)packet_buf[2];

    mouse_state.x += dx;
    mouse_state.y -= dy;

    if (mouse_state.has_wheel && packet_size == PACKET_SIZE_WHEEL) {
        int8_t dw = (int8_t)(packet_buf[3] & 0x0F);
        if (dw & 0x08) dw |= 0xF0;
        mouse_state.wheel += dw;
    }

    spin_unlock(&mouse_lock);
}

static void ps2_mouse_irq_wrapper(struct registers *r) {
    (void)r;
    ps2_mouse_handler();
}

static void mouse_init(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
        inb(PS2_DATA_PORT);

    mouse_wait_write(); outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_AUX);
    mouse_wait_write(); outb(PS2_COMMAND_PORT, PS2_CMD_READ_CONFIG);
    mouse_wait_read(); uint8_t config = inb(PS2_DATA_PORT);

    config |= PS2_CONFIG_AUX_IRQ;
    config &= ~PS2_CONFIG_AUX_CLOCK;

    mouse_wait_write(); outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_CONFIG);
    mouse_wait_write(); outb(PS2_DATA_PORT, config);

    mouse_write(MOUSE_CMD_RESET);
    mouse_wait_read(); inb(PS2_DATA_PORT);
    mouse_wait_read(); inb(PS2_DATA_PORT);

    mouse_write(MOUSE_CMD_SET_DEFAULTS);

    if (mouse_detect_wheel()) {
        packet_size = PACKET_SIZE_WHEEL;
        mouse_state.has_wheel = true;
    } else {
        packet_size = PACKET_SIZE_STD;
        mouse_state.has_wheel = false;
    }

    mouse_write(MOUSE_CMD_ENABLE_STREAM);
    irq_register_handler(PS2_MOUSE_IRQ_VECTOR, ps2_mouse_irq_wrapper);
    ioapic_unmask_irq(12);
}

static struct ps2_mouse_state *mouse_get_state(void) {
    return &mouse_state;
}

static void mouse_reset_deltas(void) {
    spin_lock(&mouse_lock);
    mouse_state.x = 0;
    mouse_state.y = 0;
    mouse_state.wheel = 0;
    spin_unlock(&mouse_lock);
}

static struct ps2_mouse_driver drv_ps2_mouse = {
    .mouse_type = PS2_MOUSE,
    .get_state = mouse_get_state,
    .reset_deltas = mouse_reset_deltas,
    .mouse_handler= ps2_mouse_handler,
};

struct ps2_mouse_driver *return_ps2_mouse_driver(void) {
    mouse_init();
    drv_ps2_mouse.mouse_type = mouse_state.has_wheel ? 3 : 0;
    return &drv_ps2_mouse;
}

struct driver *return_meta_ps2_mouse_driver(void) {
    static struct driver meta = {
        .name = "PS/2 Mouse Driver",
        .type = MOUSE_DRIVER,
        .sub_type = PS2_MOUSE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { NULL },
        .dependency_count = 0,
        .self = &drv_ps2_mouse,
        .init = (void *)return_ps2_mouse_driver,
    };
    return &meta;
}
