#include "ps2_keyboard_driver.h"

#include "components/drivers.h"
#include "components/Interruptions/isr.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Video/limine_video_driver.h"
#include "keyboard_driver.h"

#include <ports.h>
#include <stddef.h>

#define EXT_BUFFER_SIZE 32

static uint8_t key_buffer[PS2_KEYBOARD_BUFFER_SIZE];
static uint16_t buffer_head = 0, buffer_tail = 0;

static uint16_t ext_buffer[EXT_BUFFER_SIZE];
static uint8_t ext_head = 0, ext_tail = 0;

static bool key_held[128];
static bool ext_held[128];

static struct ps2_keyboard_state kbd_state = {0};

static const char scancode_to_ascii[] = {
 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
 'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0, 'a', 's',
 'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x', 'c', 'v',
 'b','n','m',',','.','/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, '7','8','9','-','4', '5', '6', '+', '1',
 '2','3','0','.', 0, 0, 0, 0, 0, 0
};

static const char scancode_to_ascii_shift[] = {
 0, 27, '!','@','#','$','%','^','&','*','(',')', '_','+', '\b','\t',
 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0, 'A', 'S',
 'D','F','G','H','J','K','L',':','"', '~', 0, '|', 'Z','X', 'C', 'V',
 'B','N','M','<','>','?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, '7','8','9','-','4', '5', '6', '+', '1',
 '2','3','0','.', 0, 0, 0, 0, 0, 0
};

static void wait_able_write(void) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL));
}

static void wait_able_read(void) {
    uint32_t timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL));
}

static void send_keyboard_command(uint8_t cmd) {
    wait_able_write();
    outb(PS2_DATA_PORT, cmd);
}

static void keyboard_set_leds(bool caps, bool num, bool scroll) {
    send_keyboard_command(0xED);
    wait_able_read(); inb(PS2_DATA_PORT);
    uint8_t leds = ((uint8_t)scroll << 0)
                 | ((uint8_t)num << 1)
                 | ((uint8_t)caps << 2);
    send_keyboard_command(leds);
    wait_able_read(); inb(PS2_DATA_PORT);
    kbd_state.is_caps_lock = caps;
    kbd_state.is_num_lock = num;
    kbd_state.is_scroll_lock = scroll;
}

static void ps2_keyboard_irq_wrapper(struct registers *r) {
    (void)r;
    ps2_keyboard_handler();
}

void ps2_keyboard_handler(void) {

    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (!(status & PS2_STATUS_OUTPUT_FULL)) {
            return;
        }

        if (status & PS2_STATUS_AUX_DATA) {

            return;
        }

        uint8_t raw = inb(PS2_DATA_PORT);

        if (raw == PS2_KEY_EXTENDED) {
            kbd_state.extended_pending = true;
            continue;
        }

        bool pressed = !(raw & 0x80);
        uint8_t key = raw & 0x7F;

        if (kbd_state.extended_pending) {
            kbd_state.extended_pending = false;

            if (key == PS2_EXT_RCTRL) {
                kbd_state.is_rctrl_pressed = pressed;
                continue;
            }
            if (key == PS2_EXT_RALT) {
                kbd_state.is_ralt_pressed = pressed;
                continue;
            }

            if (key == PS2_EXT_KP_ENTER) {
                bool was_held = ext_held[PS2_KEY_ENTER & 0x7F];
                ext_held[PS2_KEY_ENTER & 0x7F] = pressed;
                if (pressed && !was_held) {
                    uint16_t next = (buffer_head + 1) % PS2_KEYBOARD_BUFFER_SIZE;
                    if (next != buffer_tail) {
                        key_buffer[buffer_head] = PS2_KEY_ENTER;
                        buffer_head = next;
                    }
                }
                continue;
            }

            bool was_ext_held = ext_held[key & 0x7F];
            ext_held[key & 0x7F] = pressed;
            if (pressed && !was_ext_held) {
                uint8_t next = (ext_head + 1) % EXT_BUFFER_SIZE;
                if (next != ext_tail) {
                    ext_buffer[ext_head] = PS2_EXTKEY(key);
                    ext_head = next;
                }
            }
            continue;
        }

        switch (key) {
            case PS2_KEY_LSHIFT:
            case PS2_KEY_RSHIFT:
                kbd_state.is_shift_pressed = pressed;
                break;
            case PS2_KEY_LCTRL:
                kbd_state.is_ctrl_pressed = pressed;
                break;
            case PS2_KEY_LALT:
                kbd_state.is_alt_pressed = pressed;
                break;
            case PS2_KEY_CAPS_LOCK:
                if (pressed) {
                    keyboard_set_leds(!kbd_state.is_caps_lock,
                                       kbd_state.is_num_lock,
                                       kbd_state.is_scroll_lock);
                }
                continue;
            case PS2_KEY_NUM_LOCK:
                if (pressed) {
                    keyboard_set_leds(kbd_state.is_caps_lock,
                                      !kbd_state.is_num_lock,
                                       kbd_state.is_scroll_lock);
                }
                continue;
            case PS2_KEY_SCROLL_LOCK:
                if (pressed) {
                    keyboard_set_leds(kbd_state.is_caps_lock,
                                       kbd_state.is_num_lock,
                                      !kbd_state.is_scroll_lock);
                }
                continue;
        }

        if (key < 128) {
            bool was_held = key_held[key];
            key_held[key] = pressed;
            if (pressed && !was_held) {
                uint16_t next = (buffer_head + 1) % PS2_KEYBOARD_BUFFER_SIZE;
                if (next != buffer_tail) {
                    key_buffer[buffer_head] = key;
                    buffer_head = next;
                }
            }
        }
    }
}

static uint8_t keyboard_get_key(void) {
    if (buffer_tail == buffer_head) return 0;
    uint8_t key = key_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % PS2_KEYBOARD_BUFFER_SIZE;
    return key;
}

static bool keyboard_has_key(void) {
    return buffer_tail != buffer_head;
}

static bool keyboard_has_extended_key(void) {
    return ext_tail != ext_head;
}

static uint16_t keyboard_get_extended_key(void) {
    if (ext_tail == ext_head) return 0;
    uint16_t key = ext_buffer[ext_tail];
    ext_tail = (ext_tail + 1) % EXT_BUFFER_SIZE;
    return key;
}

static struct ps2_keyboard_state *keyboard_get_state(void) {
    return &kbd_state;
}

static bool keyboard_is_key_pressed(uint8_t scancode) {
    if (scancode >= 128) return false;
    return key_held[scancode];
}

static bool keyboard_is_ext_key_pressed(uint16_t ext_code) {
    uint8_t low = (uint8_t)(ext_code & 0x7F);
    return ext_held[low];
}

static char keyboard_scancode_to_char(uint8_t sc) {
    if (sc >= sizeof(scancode_to_ascii)) return 0;

    bool shift = kbd_state.is_shift_pressed;

    if (kbd_state.is_caps_lock &&
        ((sc >= 0x10 && sc <= 0x19) ||
         (sc >= 0x1E && sc <= 0x26) ||
         (sc >= 0x2C && sc <= 0x32))) {
        shift = !shift;
    }

    return shift ? scancode_to_ascii_shift[sc] : scancode_to_ascii[sc];
}

static void input(const char *prompt, char *buffer, uint32_t max_len, uint32_t color, void (*print)(const char *, uint32_t))
{
    if (prompt) print(prompt, color);
    uint32_t idx = 0;

    while (idx < max_len - 1) {
        if (keyboard_has_key()) {
            uint8_t sc = keyboard_get_key();
            if (sc == PS2_KEY_ENTER) {
                print("\n", color);
                break;
            }
            if (sc == PS2_KEY_BACKSPACE) {
                if (idx > 0) { idx--; print("\b \b", color); }
                continue;
            }
            char c = keyboard_scancode_to_char(sc);
            if (c >= 32 && c <= 126) {
                buffer[idx++] = c;
                char s[2] = { c, '\0' };
                print(s, color);
            }
        }
        asm volatile("hlt");
    }
    buffer[idx] = '\0';
}

static void keyboard_init(void) {

    for (int i = 0; i < 128; i++) { key_held[i] = false; ext_held[i] = false; }

    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
        inb(PS2_DATA_PORT);

    outb(PS2_COMMAND_PORT, 0xAD);
    outb(PS2_COMMAND_PORT, 0x20);
    wait_able_read();
    uint8_t config = inb(PS2_DATA_PORT);
    config |= (uint8_t)(1 << 0);
    config |= (uint8_t)(1 << 6);
    config &= ~(uint8_t)(1 << 4);
    outb(PS2_COMMAND_PORT, 0x60);
    wait_able_write();
    outb(PS2_DATA_PORT, config);
    outb(PS2_COMMAND_PORT, 0xAE);
    wait_able_write();
    outb(PS2_DATA_PORT, 0xF4);

    wait_able_read();
    inb(PS2_DATA_PORT);

    irq_register_handler(33, ps2_keyboard_irq_wrapper);
}

static struct ps2_keyboard_driver drv = {
    .keyboard_type = 1,
    .get_state = keyboard_get_state,
    .set_leds = keyboard_set_leds,
    .is_key_pressed = keyboard_is_key_pressed,
    .get_key = keyboard_get_key,
    .has_key = keyboard_has_key,
    .scancode_to_char = keyboard_scancode_to_char,
    .keyboard_handler = ps2_keyboard_handler,
    .input = input,
    .get_extended_key = keyboard_get_extended_key,
    .has_extended_key = keyboard_has_extended_key,
    .is_ext_key_pressed = keyboard_is_ext_key_pressed,
};

struct ps2_keyboard_driver *return_ps2_keyboard_driver(void) {
    keyboard_init();
    return &drv;
}

struct driver *return_meta_ps2_keyboard_driver(void) {
    static struct driver meta = {
        .name = "PS/2 Keyboard Driver",
        .type = KEYBOARD_DRIVER,
        .sub_type = PS2_KEYBOARD,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { NULL },
        .dependency_count = 0,
        .self = &drv,
        .init = keyboard_init,
    };
    return &meta;
}
