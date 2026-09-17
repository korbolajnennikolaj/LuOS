#include "ps2_keyboard_driver.h"

#include "components/drivers.h"
#include "components/Interruptions/isr.h"
#include "kernel/scheduler/spinlock.h"
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

static spinlock_t ps2_bus_lock = SPINLOCK_INIT;
static volatile bool leds_dirty = false;

static void ps2_process_byte(uint8_t raw);

static void ps2_wait_ack_locked(void) {
    for (int i = 0; i < 16; i++) {
        wait_able_read();

        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
        if (status & PS2_STATUS_AUX_DATA) return;

        uint8_t b = inb(PS2_DATA_PORT);
        if (b == PS2_ACK || b == PS2_RESEND) return;

        ps2_process_byte(b);
    }
}

static void ps2_program_leds_locked(void) {
    uint8_t leds = ((uint8_t)kbd_state.is_scroll_lock << 0)
                 | ((uint8_t)kbd_state.is_num_lock << 1)
                 | ((uint8_t)kbd_state.is_caps_lock << 2);

    send_keyboard_command(0xED);
    ps2_wait_ack_locked();
    send_keyboard_command(leds);
    ps2_wait_ack_locked();
}

static void keyboard_set_leds(bool caps, bool num, bool scroll) {
    uint64_t flags = spin_lock_irqsave(&ps2_bus_lock);

    kbd_state.is_caps_lock = caps;
    kbd_state.is_num_lock = num;
    kbd_state.is_scroll_lock = scroll;
    leds_dirty = false;

    ps2_program_leds_locked();

    spin_unlock_irqrestore(&ps2_bus_lock, flags);
}

static bool key_buffer_push(uint8_t key) {
    uint16_t head = buffer_head;
    uint16_t tail = __atomic_load_n(&buffer_tail, __ATOMIC_ACQUIRE);
    uint16_t next = (head + 1) % PS2_KEYBOARD_BUFFER_SIZE;
    if (next == tail) return false;
    key_buffer[head] = key;
    __atomic_store_n(&buffer_head, next, __ATOMIC_RELEASE);
    return true;
}

static bool ext_buffer_push(uint16_t key) {
    uint8_t head = ext_head;
    uint8_t tail = __atomic_load_n(&ext_tail, __ATOMIC_ACQUIRE);
    uint8_t next = (head + 1) % EXT_BUFFER_SIZE;
    if (next == tail) return false;
    ext_buffer[head] = key;
    __atomic_store_n(&ext_head, next, __ATOMIC_RELEASE);
    return true;
}
static void ps2_keyboard_irq_wrapper(struct registers *r) {
    (void)r;
    ps2_keyboard_handler();
}

static void ps2_process_byte(uint8_t raw) {

    if (raw == PS2_KEY_EXTENDED) {
        kbd_state.extended_pending = true;
        return;
    }

    bool pressed = !(raw & 0x80);
    uint8_t key = raw & 0x7F;

    if (kbd_state.extended_pending) {
        kbd_state.extended_pending = false;

        if (key == PS2_EXT_RCTRL) {
            kbd_state.is_rctrl_pressed = pressed;
            return;
        }
        if (key == PS2_EXT_RALT) {
            kbd_state.is_ralt_pressed = pressed;
            return;
        }

        if (key == PS2_EXT_KP_ENTER) {
            bool was_held = ext_held[PS2_KEY_ENTER & 0x7F];
            ext_held[PS2_KEY_ENTER & 0x7F] = pressed;
            if (pressed && !was_held)
                key_buffer_push(PS2_KEY_ENTER);
            return;
        }

        bool was_ext_held = ext_held[key & 0x7F];
        ext_held[key & 0x7F] = pressed;
        if (pressed && !was_ext_held)
            ext_buffer_push(PS2_EXTKEY(key));
        return;
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
                kbd_state.is_caps_lock = !kbd_state.is_caps_lock;
                leds_dirty = true;
            }
            return;
        case PS2_KEY_NUM_LOCK:
            if (pressed) {
                kbd_state.is_num_lock = !kbd_state.is_num_lock;
                leds_dirty = true;
            }
            return;
        case PS2_KEY_SCROLL_LOCK:
            if (pressed) {
                kbd_state.is_scroll_lock = !kbd_state.is_scroll_lock;
                leds_dirty = true;
            }
            return;
    }

    if (key < 128) {
        bool was_held = key_held[key];
        key_held[key] = pressed;
        if (pressed && !was_held)
            key_buffer_push(key);
    }
}

static void ps2_drain_locked(void) {
    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
        if (status & PS2_STATUS_AUX_DATA) return;

        uint8_t raw = inb(PS2_DATA_PORT);
        if (raw == PS2_ACK || raw == PS2_RESEND) continue;

        ps2_process_byte(raw);
    }
}

void ps2_keyboard_handler(void) {
    uint64_t flags = spin_lock_irqsave(&ps2_bus_lock);

    ps2_drain_locked();

    if (leds_dirty) {
        leds_dirty = false;
        ps2_program_leds_locked();
    }

    spin_unlock_irqrestore(&ps2_bus_lock, flags);
}

static uint8_t keyboard_get_key(void) {
    uint16_t tail = buffer_tail;
    uint16_t head = __atomic_load_n(&buffer_head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0;
    uint8_t key = key_buffer[tail];
    __atomic_store_n(&buffer_tail, (tail + 1) % PS2_KEYBOARD_BUFFER_SIZE, __ATOMIC_RELEASE);
    return key;
}

static bool keyboard_has_key(void) {
    return buffer_tail != __atomic_load_n(&buffer_head, __ATOMIC_ACQUIRE);
}

static bool keyboard_has_extended_key(void) {
    return ext_tail != __atomic_load_n(&ext_head, __ATOMIC_ACQUIRE);
}

static uint16_t keyboard_get_extended_key(void) {
    uint8_t tail = ext_tail;
    uint8_t head = __atomic_load_n(&ext_head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0;
    uint16_t key = ext_buffer[tail];
    __atomic_store_n(&ext_tail, (tail + 1) % EXT_BUFFER_SIZE, __ATOMIC_RELEASE);
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
