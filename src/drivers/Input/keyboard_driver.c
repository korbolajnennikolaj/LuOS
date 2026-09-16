#include "drivers/Input/keyboard_driver.h"

#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/usb_core.h"

#include <stddef.h>

#define REPEAT_DELAY_MS 500
#define REPEAT_RATE_MS 30

static struct keyboard_driver vkbd;

static enum KEYBOARD_TYPE last_key_source = PS2_KEYBOARD;

#define FOR_EACH_BACKEND(idx) \
    for (uint8_t idx = 0; idx < vkbd.backend_count; idx++)

#define USB_HID_ENTER 0x28
#define USB_HID_BACKSPACE 0x2A

static inline uint64_t kbd_uptime_ms(void) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return tsc ? tsc->get_tsc_uptime_ms() : 0;
}

static uint8_t backend_get_key(struct keyboard_backend *b) {
    if (!b->active) return 0;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->get_key)
        return b->drv.ps2->get_key();
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->get_key)
        return b->drv.usb->get_key();
    return 0;
}

static bool backend_has_key(struct keyboard_backend *b) {
    if (!b->active) return false;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->has_key)
        return b->drv.ps2->has_key();
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->has_key)
        return b->drv.usb->has_key();
    return false;
}

static bool backend_has_extended(struct keyboard_backend *b) {
    if (!b->active) return false;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->has_extended_key)
        return b->drv.ps2->has_extended_key();
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->has_extended_key)
        return b->drv.usb->has_extended_key();
    return false;
}

static uint16_t backend_get_extended(struct keyboard_backend *b) {
    if (!b->active) return 0;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->get_extended_key)
        return b->drv.ps2->get_extended_key();
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->get_extended_key)
        return b->drv.usb->get_extended_key();
    return 0;
}

static void backend_poll(struct keyboard_backend *b) {
    if (!b->active) return;
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->keyboard_handler) {
        b->drv.usb->keyboard_handler();
        return;
    }

    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->keyboard_handler) {
        uint64_t flags;
        asm volatile("pushfq; pop %0" : "=r"(flags));
        asm volatile("cli");
        b->drv.ps2->keyboard_handler();
        if (flags & (1u << 9)) asm volatile("sti");
    }
}

static bool backend_is_key_held(struct keyboard_backend *b, uint8_t sc) {
    if (!b->active) return false;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->is_key_pressed)
        return b->drv.ps2->is_key_pressed(sc);
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->is_key_pressed)
        return b->drv.usb->is_key_pressed(sc);
    return false;
}

static char backend_scancode_to_char(struct keyboard_backend *b, uint8_t sc) {
    if (!b->active) return 0;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->scancode_to_char)
        return b->drv.ps2->scancode_to_char(sc);
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->scancode_to_char)
        return b->drv.usb->scancode_to_char(sc);
    return 0;
}

static enum key_code sc_to_key_for(enum KEYBOARD_TYPE source, uint8_t sc);

static struct ps2_keyboard_state *backend_get_state(struct keyboard_backend *b) {
    if (!b->active) return NULL;
    if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->get_state)
        return b->drv.ps2->get_state();
    if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->get_state)
        return b->drv.usb->get_state();
    return NULL;
}

static bool is_enter(struct keyboard_backend *b, uint8_t sc) {
    if (b->type == PS2_KEYBOARD) return sc == PS2_KEY_ENTER;
    if (b->type == USB_KEYBOARD) return sc == USB_HID_ENTER;
    return false;
}

static bool is_backspace(struct keyboard_backend *b, uint8_t sc) {
    if (b->type == PS2_KEYBOARD) return sc == PS2_KEY_BACKSPACE;
    if (b->type == USB_KEYBOARD) return sc == USB_HID_BACKSPACE;
    return false;
}

static struct ps2_keyboard_state *vkbd_get_state(void) {

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (!b->active) continue;
        if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->get_state)
            return b->drv.ps2->get_state();
    }
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (!b->active) continue;
        if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->get_state)
            return b->drv.usb->get_state();
    }
    return NULL;
}

static void vkbd_set_leds(bool caps_lock, bool num_lock, bool scroll_lock)
{
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (!b->active) continue;
        if (b->type == PS2_KEYBOARD && b->drv.ps2 && b->drv.ps2->set_leds)
            b->drv.ps2->set_leds(caps_lock, num_lock, scroll_lock);
        else if (b->type == USB_KEYBOARD && b->drv.usb && b->drv.usb->set_leds)
            b->drv.usb->set_leds(caps_lock, num_lock, scroll_lock);
    }
}

#define VKBD_EXTERNAL_PUMP_STALE_MS 200
#define VKBD_MIN_POLL_INTERVAL_MS 5

static volatile uint64_t vkbd_last_external_poll_ms = 0;

static bool vkbd_locks_inited = false;
static bool vkbd_caps_lock = false;
static bool vkbd_num_lock = false;
static bool vkbd_scroll_lock = false;

static bool backend_caps_seen[KEYBOARD_MAX_BACKENDS];
static bool backend_num_seen[KEYBOARD_MAX_BACKENDS];
static bool backend_scroll_seen[KEYBOARD_MAX_BACKENDS];

static void vkbd_sync_locks(void) {
    bool changed = false;

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        struct ps2_keyboard_state *st = backend_get_state(b);
        if (!st) continue;

        if (!vkbd_locks_inited) {
            vkbd_caps_lock = st->is_caps_lock;
            vkbd_num_lock = st->is_num_lock;
            vkbd_scroll_lock = st->is_scroll_lock;
        } else {
            if (st->is_caps_lock != backend_caps_seen[i]) {
                vkbd_caps_lock = st->is_caps_lock;
                changed = true;
            }
            if (st->is_num_lock != backend_num_seen[i]) {
                vkbd_num_lock = st->is_num_lock;
                changed = true;
            }
            if (st->is_scroll_lock != backend_scroll_seen[i]) {
                vkbd_scroll_lock = st->is_scroll_lock;
                changed = true;
            }
        }

        backend_caps_seen[i] = st->is_caps_lock;
        backend_num_seen[i] = st->is_num_lock;
        backend_scroll_seen[i] = st->is_scroll_lock;
    }

    vkbd_locks_inited = true;
    if (!changed) return;

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        struct ps2_keyboard_state *st = backend_get_state(b);
        if (!st) continue;

        st->is_caps_lock = vkbd_caps_lock;
        st->is_num_lock = vkbd_num_lock;
        st->is_scroll_lock = vkbd_scroll_lock;

        backend_caps_seen[i] = vkbd_caps_lock;
        backend_num_seen[i] = vkbd_num_lock;
        backend_scroll_seen[i] = vkbd_scroll_lock;
    }

    vkbd_set_leds(vkbd_caps_lock, vkbd_num_lock, vkbd_scroll_lock);
}

static void vkbd_poll_backends(void) {
    FOR_EACH_BACKEND(i) backend_poll(&vkbd.backends[i]);
}

static bool vkbd_external_pump_alive(void) {
    uint64_t last = vkbd_last_external_poll_ms;
    if (last == 0) return false;

    uint64_t now = kbd_uptime_ms();
    return (now >= last) && ((now - last) < VKBD_EXTERNAL_PUMP_STALE_MS);
}

static bool vkbd_is_key_pressed(uint8_t scancode) {
    FOR_EACH_BACKEND(i)
        if (backend_is_key_held(&vkbd.backends[i], scancode)) return true;
    return false;
}

static uint8_t vkbd_get_key(void) {
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (backend_has_key(b)) {
            last_key_source = b->type;
            return backend_get_key(b);
        }
    }
    return 0;
}

static bool vkbd_has_key(void) {
    FOR_EACH_BACKEND(i)
        if (backend_has_key(&vkbd.backends[i])) return true;
    return false;
}

static bool vkbd_has_extended_key(void) {
    FOR_EACH_BACKEND(i)
        if (backend_has_extended(&vkbd.backends[i])) return true;
    return false;
}

static uint16_t vkbd_get_extended_key(void) {
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (backend_has_extended(b)) return backend_get_extended(b);
    }
    return 0;
}

static char vkbd_scancode_to_char(uint8_t scancode) {

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (b->active && b->type == last_key_source)
            return backend_scancode_to_char(b, scancode);
    }

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (b->active && backend_has_key(b))
            return backend_scancode_to_char(b, scancode);
    }

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (b->active) return backend_scancode_to_char(b, scancode);
    }
    return 0;
}

static bool vkbd_get_key_event(struct key_event *out) {
    if (!out) return false;

    struct keyboard_backend *src = NULL;
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (b->active && backend_has_key(b)) { src = b; break; }
    }
    if (!src) return false;

    uint8_t sc = backend_get_key(src);
    last_key_source = src->type;

    out->source = src->type;
    out->scancode = sc;
    out->ch = backend_scancode_to_char(src, sc);
    out->code = sc_to_key_for(src->type, sc);
    out->is_enter = is_enter(src, sc);
    out->is_backspace = is_backspace(src, sc);

    bool got_mods = false;
    if (src->type == USB_KEYBOARD && src->drv.usb && src->drv.usb->last_key_modifiers) {
        uint8_t m = src->drv.usb->last_key_modifiers();
        out->ctrl = (m & 0x11) != 0;
        out->shift = (m & 0x22) != 0;
        out->alt = (m & 0x44) != 0;
        got_mods = true;
    }

    if (!got_mods) {
        struct ps2_keyboard_state *st = backend_get_state(src);
        out->ctrl = st ? (st->is_ctrl_pressed || st->is_rctrl_pressed) : false;
        out->shift = st ? st->is_shift_pressed : false;
        out->alt = st ? st->is_alt_pressed : false;
    }

    return true;
}

static bool vkbd_is_key_held_from(enum KEYBOARD_TYPE source, uint8_t scancode) {
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (b->active && b->type == source) return backend_is_key_held(b, scancode);
    }
    return false;
}

static void vkbd_keyboard_handler(void) {
    uint64_t now = kbd_uptime_ms();

    if (now != 0 && vkbd_last_external_poll_ms != 0 &&
        (now - vkbd_last_external_poll_ms) < VKBD_MIN_POLL_INTERVAL_MS) return;

    vkbd_poll_backends();
    vkbd_sync_locks();

    vkbd_last_external_poll_ms = now;
}

static void vkbd_input(const char *prompt, char *buffer, uint32_t max_len, uint32_t color, void (*print)(const char *, uint32_t))
{
    if (prompt) print(prompt, color);

    uint32_t idx = 0;

    uint8_t hold_sc = 0;
    struct keyboard_backend *hold_backend = NULL;
    uint64_t hold_start_ms = 0;
    uint64_t last_repeat_ms = 0;

    while (idx < max_len - 1) {

        if (!vkbd_external_pump_alive()) {
            vkbd_poll_backends();
            vkbd_sync_locks();
        }

        if (hold_sc != 0 && hold_backend != NULL) {
            if (!backend_is_key_held(hold_backend, hold_sc)) {
                hold_sc = 0; hold_backend = NULL;
            } else {
                uint64_t now_ms = kbd_uptime_ms();

                if (now_ms - hold_start_ms >= REPEAT_DELAY_MS) {
                    if (!last_repeat_ms) last_repeat_ms = hold_start_ms + REPEAT_DELAY_MS;
                    if (now_ms - last_repeat_ms >= REPEAT_RATE_MS) {
                        last_repeat_ms = now_ms;
                        if (is_backspace(hold_backend, hold_sc)) {
                            if (idx > 0) { idx--; print("\b \b", color); }
                        } else {
                            char c = backend_scancode_to_char(hold_backend, hold_sc);
                            if (c >= 32 && c <= 126 && idx < max_len - 1) {
                                buffer[idx++] = c;
                                char s[2] = { c, '\0' };
                                print(s, color);
                            }
                        }
                    }
                }
            }
        }

        if (!vkbd_has_key()) {
            bool has_usb = false;
            FOR_EACH_BACKEND(i) {
                struct keyboard_backend *b = &vkbd.backends[i];
                if (b->active && b->type == USB_KEYBOARD && b->drv.usb &&
                    b->drv.usb->keyboard_count && b->drv.usb->keyboard_count() > 0) {
                    has_usb = true; break;
                }
            }
            if (!has_usb) asm volatile("hlt");
            else asm volatile("pause");
            continue;
        }

        struct keyboard_backend *src = NULL;
        FOR_EACH_BACKEND(i) {
            struct keyboard_backend *b = &vkbd.backends[i];
            if (b->active && backend_has_key(b)) { src = b; break; }
        }
        if (!src) continue;

        uint8_t sc = backend_get_key(src);

        if (is_enter(src, sc)) {
            print("\n", color);
            hold_sc = 0; hold_backend = NULL;
            break;
        }

        if (is_backspace(src, sc)) {
            if (idx > 0) { idx--; print("\b \b", color); }
            hold_sc = sc; hold_backend = src;
            hold_start_ms = kbd_uptime_ms(); last_repeat_ms = 0;
            continue;
        }

        char c = backend_scancode_to_char(src, sc);
        if (c >= 32 && c <= 126) {
            buffer[idx++] = c;
            char s[2] = { c, '\0' };
            print(s, color);
            hold_sc = sc; hold_backend = src;
            hold_start_ms = kbd_uptime_ms(); last_repeat_ms = 0;
        }
    }
    buffer[idx] = '\0';
}

static int vkbd_register_backend(enum KEYBOARD_TYPE type, void *drv)
{
    if (!drv || vkbd.backend_count >= KEYBOARD_MAX_BACKENDS) return -1;
    struct keyboard_backend *b = &vkbd.backends[vkbd.backend_count];
    b->type = type;
    b->active = true;
    if (type == PS2_KEYBOARD)
        b->drv.ps2 = (struct ps2_keyboard_driver *)drv;
    else
        b->drv.usb = (struct usb_keyboard_driver *)drv;
    return (int)vkbd.backend_count++;
}

static const uint8_t key_code_ps2_base[KEY_CODE_COUNT] = {

    [KEY_A]=0x1E,[KEY_B]=0x30,[KEY_C]=0x2E,[KEY_D]=0x20,[KEY_E]=0x12,
    [KEY_F]=0x21,[KEY_G]=0x22,[KEY_H]=0x23,[KEY_I]=0x17,[KEY_J]=0x24,
    [KEY_K]=0x25,[KEY_L]=0x26,[KEY_M]=0x32,[KEY_N]=0x31,[KEY_O]=0x18,
    [KEY_P]=0x19,[KEY_Q]=0x10,[KEY_R]=0x13,[KEY_S]=0x1F,[KEY_T]=0x14,
    [KEY_U]=0x16,[KEY_V]=0x2F,[KEY_W]=0x11,[KEY_X]=0x2D,[KEY_Y]=0x15,
    [KEY_Z]=0x2C,

    [KEY_1]=0x02,[KEY_2]=0x03,[KEY_3]=0x04,[KEY_4]=0x05,[KEY_5]=0x06,
    [KEY_6]=0x07,[KEY_7]=0x08,[KEY_8]=0x09,[KEY_9]=0x0A,[KEY_0]=0x0B,

    [KEY_MINUS]=0x0C, [KEY_EQUALS]=0x0D,
    [KEY_LBRACKET]=0x1A,[KEY_RBRACKET]=0x1B,
    [KEY_BACKSLASH]=0x2B,[KEY_SEMICOLON]=0x27,
    [KEY_APOSTROPHE]=0x28,[KEY_GRAVE]=0x29,
    [KEY_COMMA]=0x33, [KEY_DOT]=0x34, [KEY_SLASH]=0x35,

    [KEY_ESC]=0x01, [KEY_TAB]=0x0F, [KEY_ENTER]=0x1C,
    [KEY_BACKSPACE]=0x0E,[KEY_SPACE]=0x39,

    [KEY_LSHIFT]=0x2A, [KEY_RSHIFT]=0x36,
    [KEY_LCTRL]=0x1D, [KEY_LALT]=0x38,

    [KEY_CAPS_LOCK]=0x3A,[KEY_NUM_LOCK]=0x45,[KEY_SCROLL_LOCK]=0x46,

    [KEY_F1]=0x3B,[KEY_F2]=0x3C,[KEY_F3]=0x3D,[KEY_F4]=0x3E,
    [KEY_F5]=0x3F,[KEY_F6]=0x40,[KEY_F7]=0x41,[KEY_F8]=0x42,
    [KEY_F9]=0x43,[KEY_F10]=0x44,[KEY_F11]=0x57,[KEY_F12]=0x58,

    [KEY_KP_0]=0x52,[KEY_KP_1]=0x4F,[KEY_KP_2]=0x50,[KEY_KP_3]=0x51,
    [KEY_KP_4]=0x4B,[KEY_KP_5]=0x4C,[KEY_KP_6]=0x4D,[KEY_KP_7]=0x47,
    [KEY_KP_8]=0x48,[KEY_KP_9]=0x49,[KEY_KP_DOT]=0x53,
    [KEY_KP_PLUS]=0x4E,[KEY_KP_MINUS]=0x4A,[KEY_KP_MULTIPLY]=0x37,

    [KEY_RCTRL]=0,[KEY_RALT]=0,
    [KEY_UP]=0,[KEY_DOWN]=0,[KEY_LEFT]=0,[KEY_RIGHT]=0,
    [KEY_HOME]=0,[KEY_END]=0,[KEY_PAGE_UP]=0,[KEY_PAGE_DOWN]=0,
    [KEY_INSERT]=0,[KEY_DELETE]=0,
    [KEY_KP_DIVIDE]=0,[KEY_KP_ENTER]=0,
    [KEY_LMETA]=0,[KEY_RMETA]=0,[KEY_MENU]=0,[KEY_PRINT_SCREEN]=0,
};

static const uint8_t key_code_ps2_ext[KEY_CODE_COUNT] = {
    [KEY_RCTRL] = PS2_EXT_RCTRL,
    [KEY_RALT] = PS2_EXT_RALT,
    [KEY_UP] = PS2_EXT_UP,
    [KEY_DOWN] = PS2_EXT_DOWN,
    [KEY_LEFT] = PS2_EXT_LEFT,
    [KEY_RIGHT] = PS2_EXT_RIGHT,
    [KEY_HOME] = PS2_EXT_HOME,
    [KEY_END] = PS2_EXT_END,
    [KEY_PAGE_UP] = PS2_EXT_PAGE_UP,
    [KEY_PAGE_DOWN] = PS2_EXT_PAGE_DOWN,
    [KEY_INSERT] = PS2_EXT_INSERT,
    [KEY_DELETE] = PS2_EXT_DELETE,
    [KEY_KP_DIVIDE] = PS2_EXT_KP_DIVIDE,
    [KEY_KP_ENTER] = PS2_EXT_KP_ENTER,
    [KEY_LMETA] = PS2_EXT_LMETA,
    [KEY_RMETA] = PS2_EXT_RMETA,
    [KEY_MENU] = PS2_EXT_MENU,
    [KEY_PRINT_SCREEN]= PS2_EXT_PRINT_SCREEN,
};

static const uint8_t key_code_usb[KEY_CODE_COUNT] = {

    [KEY_A]=0x04,[KEY_B]=0x05,[KEY_C]=0x06,[KEY_D]=0x07,[KEY_E]=0x08,
    [KEY_F]=0x09,[KEY_G]=0x0A,[KEY_H]=0x0B,[KEY_I]=0x0C,[KEY_J]=0x0D,
    [KEY_K]=0x0E,[KEY_L]=0x0F,[KEY_M]=0x10,[KEY_N]=0x11,[KEY_O]=0x12,
    [KEY_P]=0x13,[KEY_Q]=0x14,[KEY_R]=0x15,[KEY_S]=0x16,[KEY_T]=0x17,
    [KEY_U]=0x18,[KEY_V]=0x19,[KEY_W]=0x1A,[KEY_X]=0x1B,[KEY_Y]=0x1C,
    [KEY_Z]=0x1D,

    [KEY_1]=0x1E,[KEY_2]=0x1F,[KEY_3]=0x20,[KEY_4]=0x21,[KEY_5]=0x22,
    [KEY_6]=0x23,[KEY_7]=0x24,[KEY_8]=0x25,[KEY_9]=0x26,[KEY_0]=0x27,

    [KEY_MINUS]=0x2D, [KEY_EQUALS]=0x2E,
    [KEY_LBRACKET]=0x2F,[KEY_RBRACKET]=0x30,
    [KEY_BACKSLASH]=0x31,[KEY_SEMICOLON]=0x33,
    [KEY_APOSTROPHE]=0x34,[KEY_GRAVE]=0x35,
    [KEY_COMMA]=0x36, [KEY_DOT]=0x37, [KEY_SLASH]=0x38,

    [KEY_ESC]=0x29, [KEY_TAB]=0x2B, [KEY_ENTER]=0x28,
    [KEY_BACKSPACE]=0x2A,[KEY_SPACE]=0x2C,

    [KEY_LSHIFT]=0xE1, [KEY_RSHIFT]=0xE5,
    [KEY_LCTRL]=0xE0, [KEY_RCTRL]=0xE4,
    [KEY_LALT]=0xE2, [KEY_RALT]=0xE6,

    [KEY_CAPS_LOCK]=0x39,[KEY_NUM_LOCK]=0x53,[KEY_SCROLL_LOCK]=0x47,

    [KEY_F1]=0x3A,[KEY_F2]=0x3B,[KEY_F3]=0x3C,[KEY_F4]=0x3D,
    [KEY_F5]=0x3E,[KEY_F6]=0x3F,[KEY_F7]=0x40,[KEY_F8]=0x41,
    [KEY_F9]=0x42,[KEY_F10]=0x43,[KEY_F11]=0x44,[KEY_F12]=0x45,

    [KEY_UP]=0x52, [KEY_DOWN]=0x51,
    [KEY_LEFT]=0x50, [KEY_RIGHT]=0x4F,
    [KEY_HOME]=0x4A, [KEY_END]=0x4D,
    [KEY_PAGE_UP]=0x4B, [KEY_PAGE_DOWN]=0x4E,
    [KEY_INSERT]=0x49, [KEY_DELETE]=0x4C,

    [KEY_KP_0]=0x62,[KEY_KP_1]=0x59,[KEY_KP_2]=0x5A,[KEY_KP_3]=0x5B,
    [KEY_KP_4]=0x5C,[KEY_KP_5]=0x5D,[KEY_KP_6]=0x5E,[KEY_KP_7]=0x5F,
    [KEY_KP_8]=0x60,[KEY_KP_9]=0x61,[KEY_KP_DOT]=0x63,
    [KEY_KP_PLUS]=0x57,[KEY_KP_MINUS]=0x56,
    [KEY_KP_MULTIPLY]=0x55,[KEY_KP_DIVIDE]=0x54,[KEY_KP_ENTER]=0x58,

    [KEY_LMETA]=0xE3, [KEY_RMETA]=0xE7,
    [KEY_MENU]=0x65, [KEY_PRINT_SCREEN]=0x46,
};

#define _NK KEY_CODE_COUNT

static const enum key_code ps2_sc_to_key[0x60] = {
    [0x00]=_NK,
    [0x01]=KEY_ESC,
    [0x02]=KEY_1, [0x03]=KEY_2, [0x04]=KEY_3,
    [0x05]=KEY_4, [0x06]=KEY_5, [0x07]=KEY_6,
    [0x08]=KEY_7, [0x09]=KEY_8, [0x0A]=KEY_9,
    [0x0B]=KEY_0, [0x0C]=KEY_MINUS, [0x0D]=KEY_EQUALS,
    [0x0E]=KEY_BACKSPACE,[0x0F]=KEY_TAB,
    [0x10]=KEY_Q, [0x11]=KEY_W, [0x12]=KEY_E,
    [0x13]=KEY_R, [0x14]=KEY_T, [0x15]=KEY_Y,
    [0x16]=KEY_U, [0x17]=KEY_I, [0x18]=KEY_O,
    [0x19]=KEY_P, [0x1A]=KEY_LBRACKET,[0x1B]=KEY_RBRACKET,
    [0x1C]=KEY_ENTER, [0x1D]=KEY_LCTRL,
    [0x1E]=KEY_A, [0x1F]=KEY_S, [0x20]=KEY_D,
    [0x21]=KEY_F, [0x22]=KEY_G, [0x23]=KEY_H,
    [0x24]=KEY_J, [0x25]=KEY_K, [0x26]=KEY_L,
    [0x27]=KEY_SEMICOLON,[0x28]=KEY_APOSTROPHE,[0x29]=KEY_GRAVE,
    [0x2A]=KEY_LSHIFT, [0x2B]=KEY_BACKSLASH,
    [0x2C]=KEY_Z, [0x2D]=KEY_X, [0x2E]=KEY_C,
    [0x2F]=KEY_V, [0x30]=KEY_B, [0x31]=KEY_N,
    [0x32]=KEY_M, [0x33]=KEY_COMMA, [0x34]=KEY_DOT,
    [0x35]=KEY_SLASH, [0x36]=KEY_RSHIFT,
    [0x37]=KEY_KP_MULTIPLY,
    [0x38]=KEY_LALT, [0x39]=KEY_SPACE, [0x3A]=KEY_CAPS_LOCK,
    [0x3B]=KEY_F1, [0x3C]=KEY_F2, [0x3D]=KEY_F3, [0x3E]=KEY_F4,
    [0x3F]=KEY_F5, [0x40]=KEY_F6, [0x41]=KEY_F7, [0x42]=KEY_F8,
    [0x43]=KEY_F9, [0x44]=KEY_F10,
    [0x45]=KEY_NUM_LOCK,[0x46]=KEY_SCROLL_LOCK,
    [0x47]=KEY_KP_7,[0x48]=KEY_KP_8,[0x49]=KEY_KP_9,
    [0x4A]=KEY_KP_MINUS,
    [0x4B]=KEY_KP_4,[0x4C]=KEY_KP_5,[0x4D]=KEY_KP_6,
    [0x4E]=KEY_KP_PLUS,
    [0x4F]=KEY_KP_1,[0x50]=KEY_KP_2,[0x51]=KEY_KP_3,
    [0x52]=KEY_KP_0,[0x53]=KEY_KP_DOT,
    [0x57]=KEY_F11, [0x58]=KEY_F12,
};

static const enum key_code ps2_ext_sc_to_key[0x60] = {
    [PS2_EXT_RCTRL] = KEY_RCTRL,
    [PS2_EXT_RALT] = KEY_RALT,
    [PS2_EXT_UP] = KEY_UP,
    [PS2_EXT_DOWN] = KEY_DOWN,
    [PS2_EXT_LEFT] = KEY_LEFT,
    [PS2_EXT_RIGHT] = KEY_RIGHT,
    [PS2_EXT_HOME] = KEY_HOME,
    [PS2_EXT_END] = KEY_END,
    [PS2_EXT_PAGE_UP] = KEY_PAGE_UP,
    [PS2_EXT_PAGE_DOWN] = KEY_PAGE_DOWN,
    [PS2_EXT_INSERT] = KEY_INSERT,
    [PS2_EXT_DELETE] = KEY_DELETE,
    [PS2_EXT_KP_DIVIDE] = KEY_KP_DIVIDE,
    [PS2_EXT_KP_ENTER] = KEY_KP_ENTER,
    [PS2_EXT_LMETA] = KEY_LMETA,
    [PS2_EXT_RMETA] = KEY_RMETA,
    [PS2_EXT_MENU] = KEY_MENU,
    [PS2_EXT_PRINT_SCREEN] = KEY_PRINT_SCREEN,
};

static const enum key_code usb_hid_to_key[0x100] = {

    [0x04]=KEY_A,[0x05]=KEY_B,[0x06]=KEY_C,[0x07]=KEY_D,
    [0x08]=KEY_E,[0x09]=KEY_F,[0x0A]=KEY_G,[0x0B]=KEY_H,
    [0x0C]=KEY_I,[0x0D]=KEY_J,[0x0E]=KEY_K,[0x0F]=KEY_L,
    [0x10]=KEY_M,[0x11]=KEY_N,[0x12]=KEY_O,[0x13]=KEY_P,
    [0x14]=KEY_Q,[0x15]=KEY_R,[0x16]=KEY_S,[0x17]=KEY_T,
    [0x18]=KEY_U,[0x19]=KEY_V,[0x1A]=KEY_W,[0x1B]=KEY_X,
    [0x1C]=KEY_Y,[0x1D]=KEY_Z,

    [0x1E]=KEY_1,[0x1F]=KEY_2,[0x20]=KEY_3,[0x21]=KEY_4,
    [0x22]=KEY_5,[0x23]=KEY_6,[0x24]=KEY_7,[0x25]=KEY_8,
    [0x26]=KEY_9,[0x27]=KEY_0,

    [0x28]=KEY_ENTER, [0x29]=KEY_ESC, [0x2A]=KEY_BACKSPACE,
    [0x2B]=KEY_TAB, [0x2C]=KEY_SPACE,

    [0x2D]=KEY_MINUS, [0x2E]=KEY_EQUALS,
    [0x2F]=KEY_LBRACKET,[0x30]=KEY_RBRACKET, [0x31]=KEY_BACKSLASH,
    [0x33]=KEY_SEMICOLON,[0x34]=KEY_APOSTROPHE,[0x35]=KEY_GRAVE,
    [0x36]=KEY_COMMA, [0x37]=KEY_DOT, [0x38]=KEY_SLASH,

    [0x39]=KEY_CAPS_LOCK,[0x47]=KEY_SCROLL_LOCK,

    [0x3A]=KEY_F1, [0x3B]=KEY_F2, [0x3C]=KEY_F3, [0x3D]=KEY_F4,
    [0x3E]=KEY_F5, [0x3F]=KEY_F6, [0x40]=KEY_F7, [0x41]=KEY_F8,
    [0x42]=KEY_F9, [0x43]=KEY_F10,[0x44]=KEY_F11,[0x45]=KEY_F12,

    [0x46]=KEY_PRINT_SCREEN,

    [0x49]=KEY_INSERT, [0x4A]=KEY_HOME, [0x4B]=KEY_PAGE_UP,
    [0x4C]=KEY_DELETE, [0x4D]=KEY_END, [0x4E]=KEY_PAGE_DOWN,
    [0x4F]=KEY_RIGHT, [0x50]=KEY_LEFT,
    [0x51]=KEY_DOWN, [0x52]=KEY_UP,

    [0x53]=KEY_NUM_LOCK,[0x54]=KEY_KP_DIVIDE,[0x55]=KEY_KP_MULTIPLY,
    [0x56]=KEY_KP_MINUS,[0x57]=KEY_KP_PLUS, [0x58]=KEY_KP_ENTER,
    [0x59]=KEY_KP_1,[0x5A]=KEY_KP_2,[0x5B]=KEY_KP_3,
    [0x5C]=KEY_KP_4,[0x5D]=KEY_KP_5,[0x5E]=KEY_KP_6,
    [0x5F]=KEY_KP_7,[0x60]=KEY_KP_8,[0x61]=KEY_KP_9,
    [0x62]=KEY_KP_0,[0x63]=KEY_KP_DOT,

    [0x65]=KEY_MENU,

    [0xE0]=KEY_LCTRL, [0xE1]=KEY_LSHIFT,[0xE2]=KEY_LALT,
    [0xE3]=KEY_LMETA, [0xE4]=KEY_RCTRL, [0xE5]=KEY_RSHIFT,
    [0xE6]=KEY_RALT, [0xE7]=KEY_RMETA,
};

#undef _NK

static enum key_code sc_to_key_for(enum KEYBOARD_TYPE source, uint8_t sc) {
    if (sc & 0x80) {
        uint8_t ext = sc & 0x7F;
        if (ext < (uint8_t)(sizeof(ps2_ext_sc_to_key) / sizeof(ps2_ext_sc_to_key[0])))
            return ps2_ext_sc_to_key[ext];
        return KEY_CODE_COUNT;
    }

    if (source == PS2_KEYBOARD) {
        if (sc < (uint8_t)(sizeof(ps2_sc_to_key) / sizeof(ps2_sc_to_key[0])))
            return ps2_sc_to_key[sc];
        return KEY_CODE_COUNT;
    }

    if (source == USB_KEYBOARD)
        return usb_hid_to_key[sc];

    return KEY_CODE_COUNT;
}

static enum key_code vkbd_scancode_to_key_code(uint8_t sc) {

    if (sc & 0x80) {
        uint8_t ext = sc & 0x7F;
        if (ext < (uint8_t)(sizeof(ps2_ext_sc_to_key) / sizeof(ps2_ext_sc_to_key[0])))
            return ps2_ext_sc_to_key[ext];
        return KEY_CODE_COUNT;
    }

    if (last_key_source == PS2_KEYBOARD) {
        if (sc < (uint8_t)(sizeof(ps2_sc_to_key) / sizeof(ps2_sc_to_key[0])))
            return ps2_sc_to_key[sc];
        return KEY_CODE_COUNT;
    }

    if (last_key_source == USB_KEYBOARD)
        return usb_hid_to_key[sc];

    return KEY_CODE_COUNT;
}

static bool vkbd_is_key_pressed_enum(enum key_code key) {
    if ((unsigned)key >= KEY_CODE_COUNT) return false;

    struct ps2_keyboard_state *st = vkbd_get_state();
    if (st) {
        switch (key) {
            case KEY_LSHIFT:
            case KEY_RSHIFT: return st->is_shift_pressed;
            case KEY_LCTRL: return st->is_ctrl_pressed;
            case KEY_RCTRL: return st->is_rctrl_pressed;
            case KEY_LALT: return st->is_alt_pressed;
            case KEY_RALT: return st->is_ralt_pressed;
            case KEY_CAPS_LOCK: return st->is_caps_lock;
            case KEY_NUM_LOCK: return st->is_num_lock;
            case KEY_SCROLL_LOCK: return st->is_scroll_lock;
            default: break;
        }
    }

    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (!b->active) continue;

        if (b->type == PS2_KEYBOARD && b->drv.ps2) {
            uint8_t base = key_code_ps2_base[key];
            uint8_t ext = key_code_ps2_ext[key];
            if (base && b->drv.ps2->is_key_pressed)
                return b->drv.ps2->is_key_pressed(base);
            if (ext && b->drv.ps2->is_ext_key_pressed)
                return b->drv.ps2->is_ext_key_pressed(PS2_EXTKEY(ext));
        }

        if (b->type == USB_KEYBOARD && b->drv.usb) {
            uint8_t hid = key_code_usb[key];
            if (hid && b->drv.usb->is_key_pressed)
                return b->drv.usb->is_key_pressed(hid);
        }
    }
    return false;
}

static bool vkbd_is_special_key(uint8_t sc) {

    switch (sc) {
        case PS2_KEY_ESC:
        case PS2_KEY_BACKSPACE:
        case PS2_KEY_TAB:
        case PS2_KEY_ENTER:
        case PS2_KEY_LCTRL:
        case PS2_KEY_LSHIFT:
        case PS2_KEY_RSHIFT:
        case PS2_KEY_LALT:
        case PS2_KEY_SPACE:
        case PS2_KEY_CAPS_LOCK:
        case PS2_KEY_F1: case PS2_KEY_F2: case PS2_KEY_F3: case PS2_KEY_F4:
        case PS2_KEY_F5: case PS2_KEY_F6: case PS2_KEY_F7: case PS2_KEY_F8:
        case PS2_KEY_F9: case PS2_KEY_F10: case PS2_KEY_F11: case PS2_KEY_F12:
        case PS2_KEY_NUM_LOCK:
        case PS2_KEY_SCROLL_LOCK:
            return true;
        default:
            return false;
    }
}

static int vkbd_get_active_type(void) {
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];

        if (b->active && b->type == USB_KEYBOARD && b->drv.usb &&
            b->drv.usb->keyboard_count && b->drv.usb->keyboard_count() > 0)
            return USB_KEYBOARD;
    }
    FOR_EACH_BACKEND(i) {
        struct keyboard_backend *b = &vkbd.backends[i];
        if (b->active && b->type == PS2_KEYBOARD && b->drv.ps2)
            return PS2_KEYBOARD;
    }
    return -1;
}

static struct keyboard_driver vkbd = {
    .backend_count = 0,
    .get_state = vkbd_get_state,
    .set_leds = vkbd_set_leds,
    .is_key_pressed = vkbd_is_key_pressed,
    .get_key = vkbd_get_key,
    .has_key = vkbd_has_key,
    .has_extended_key = vkbd_has_extended_key,
    .get_extended_key = vkbd_get_extended_key,
    .scancode_to_char = vkbd_scancode_to_char,
    .keyboard_handler = vkbd_keyboard_handler,
    .input = vkbd_input,
    .register_backend = vkbd_register_backend,
    .get_active_type = vkbd_get_active_type,
    .is_special_key = vkbd_is_special_key,
    .is_key_pressed_enum = vkbd_is_key_pressed_enum,
    .scancode_to_key_code = vkbd_scancode_to_key_code,
    .get_key_event = vkbd_get_key_event,
    .is_key_held_from = vkbd_is_key_held_from,
};

struct keyboard_driver *return_keyboard_driver(void) {
    vkbd.backend_count = 0;
    struct ps2_keyboard_driver *ps2 =
        (struct ps2_keyboard_driver *)get_self_driver(KEYBOARD_DRIVER, PS2_KEYBOARD);
    if (ps2) vkbd.register_backend(PS2_KEYBOARD, ps2);
    struct usb_keyboard_driver *usb =
        (struct usb_keyboard_driver *)get_self_driver(KEYBOARD_DRIVER, USB_KEYBOARD);
    if (usb) vkbd.register_backend(USB_KEYBOARD, usb);
    return &vkbd;
}

struct driver *return_meta_keyboard_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(KEYBOARD_DRIVER, USB_KEYBOARD),
        MAKE_DEPENDENCY(KEYBOARD_DRIVER, PS2_KEYBOARD),
    };
    static struct driver meta = {
        .name = "Virtual Keyboard Driver",
        .type = KEYBOARD_DRIVER,
        .sub_type = VIRTUAL_KEYBOARD,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0], &dep[1] },
        .dependency_count = 2,
        .self = &vkbd,
        .init = (void *)return_keyboard_driver,
    };
    return &meta;
}
