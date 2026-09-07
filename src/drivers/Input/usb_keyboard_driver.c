#include "usb_keyboard_driver.h"

#include "components/drivers.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "drivers/USB/xhci.h"
#include "keyboard_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void delay_ms(uint64_t ms);

#define MAX_USB_KEYBOARDS 4
#define USB_KBD_BUFFER_SIZE 128
#define USB_KBD_EXT_SIZE 32

#define HID_GET_REPORT 0x01
#define HID_SET_REPORT 0x09
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B

static const char hid_ascii[] = {
    0, 0, 0, 0, 'a','b','c','d','e','f','g','h','i','j','k','l',
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\n', 27, '\b','\t',' ', '-','=','[',']','\\', '#', ';','\'','`',
    ',','.','/',
    0,0,0,0,0,0,0,
    0,0,0,0,0,
    0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,
    0,0,0,0,0,0,0,0,0,
    0,
    0,
};

static const char hid_ascii_shift[] = {
    0, 0, 0, 0, 'A','B','C','D','E','F','G','H','I','J','K','L',
    'M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b','\t',' ', '_','+','{','}','|', '~', ':','"', '~',
    '<','>','?',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static const struct { uint8_t hid; uint8_t ext; } hid_to_ext[] = {
    { 0x39, 0x00 },
    { 0x3A, 0x3B },
    { 0x3B, 0x3C },
    { 0x3C, 0x3D },
    { 0x3D, 0x3E },
    { 0x3E, 0x3F },
    { 0x3F, 0x40 },
    { 0x40, 0x41 },
    { 0x41, 0x42 },
    { 0x42, 0x43 },
    { 0x43, 0x44 },
    { 0x44, 0x57 },
    { 0x45, 0x58 },
    { 0x46, PS2_EXT_PRINT_SCREEN },
    { 0x49, PS2_EXT_INSERT },
    { 0x4A, PS2_EXT_HOME },
    { 0x4B, PS2_EXT_PAGE_UP },
    { 0x4C, PS2_EXT_DELETE },
    { 0x4D, PS2_EXT_END },
    { 0x4E, PS2_EXT_PAGE_DOWN },
    { 0x4F, PS2_EXT_RIGHT },
    { 0x50, PS2_EXT_LEFT },
    { 0x51, PS2_EXT_DOWN },
    { 0x52, PS2_EXT_UP },
    { 0x53, 0x45 },
    { 0x54, PS2_EXT_KP_DIVIDE },
    { 0x58, PS2_EXT_KP_ENTER },
    { 0x65, PS2_EXT_MENU },
    { 0xE3, PS2_EXT_LMETA },
    { 0xE7, PS2_EXT_RMETA },
    { 0, 0 },
};

typedef struct usb_keyboard_instance {
    struct usb_device *dev;
    uint8_t last_report[8];
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    bool active;
    bool pending;
    bool armed;

    uint8_t dma_report[8] __attribute__((aligned(64)));
    bool pressed_keys[256];
} usb_keyboard_instance;

static struct usb_keyboard_instance keyboards[MAX_USB_KEYBOARDS];
static int keyboard_count = 0;

static uint8_t key_buffer[USB_KBD_BUFFER_SIZE];

static uint8_t mod_buffer[USB_KBD_BUFFER_SIZE];
static uint8_t last_dequeued_mods = 0;
static uint16_t buffer_head = 0, buffer_tail = 0;

static uint16_t ext_buffer[USB_KBD_EXT_SIZE];
static uint8_t ext_head = 0, ext_tail = 0;

static struct ps2_keyboard_state kbd_state = {0};

static uint8_t dma_led_buffer __attribute__((aligned(64)));

bool usb_kbd_has_key(void) {
    return buffer_head != buffer_tail;
}

uint8_t usb_kbd_get_key(void) {
    if (buffer_head == buffer_tail) return 0;
    uint8_t sc = key_buffer[buffer_tail];
    last_dequeued_mods = mod_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % USB_KBD_BUFFER_SIZE;
    return sc;
}

bool usb_kbd_is_key_pressed(uint8_t sc) {
    for (int k = 0; k < keyboard_count; k++)
        if (keyboards[k].active && keyboards[k].pressed_keys[sc])
            return true;
    return false;
}

bool usb_kbd_has_extended_key(void) {
    return ext_tail != ext_head;
}

uint16_t usb_kbd_get_extended_key(void) {
    if (ext_tail == ext_head) return 0;
    uint16_t key = ext_buffer[ext_tail];
    ext_tail = (ext_tail + 1) % USB_KBD_EXT_SIZE;
    return key;
}

void usb_kbd_set_leds(bool caps, bool num, bool scroll) {
    kbd_state.is_caps_lock = caps;
    kbd_state.is_num_lock = num;
    kbd_state.is_scroll_lock = scroll;
    dma_led_buffer = ((uint8_t)num << 0)
    | ((uint8_t)caps << 1)
    | ((uint8_t)scroll << 2);
    struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    if (!core || !core->control_transfer) return;
    for (int i = 0; i < keyboard_count; i++)
        if (keyboards[i].active)
            core->control_transfer(keyboards[i].dev,
                                   0x21, HID_SET_REPORT, 0x0200,
                                   0, 1, &dma_led_buffer);
}

char usb_kbd_scancode_to_char(uint8_t sc) {
    if (sc >= sizeof(hid_ascii)) return 0;
    bool shift = kbd_state.is_shift_pressed;

    if (kbd_state.is_caps_lock && sc >= 0x04 && sc <= 0x1D) shift = !shift;
    return shift ? hid_ascii_shift[sc] : hid_ascii[sc];
}

static bool push_extended_if_special(uint8_t hid) {
    for (int i = 0; hid_to_ext[i].hid != 0; i++) {
        if (hid_to_ext[i].hid != hid) continue;
        if (hid_to_ext[i].ext == 0) return true;
        uint8_t next = (ext_head + 1) % USB_KBD_EXT_SIZE;
        if (next != ext_tail) {
            ext_buffer[ext_head] = PS2_EXTKEY(hid_to_ext[i].ext);
            ext_head = next;
        }
        return true;
    }
    return false;
}

static void process_report(struct usb_keyboard_instance *kbd, const uint8_t *rep)
{

    bool all_ff = true;
    for (int i = 2; i < 8; i++)
        if (rep[i] != 0x01) { all_ff = false; break; }
        if (all_ff && rep[2] == 0x01) {
            for (int i = 0; i < 8; i++) kbd->last_report[i] = rep[i];
            for (int i = 0; i < 256; i++) kbd->pressed_keys[i] = false;
            return;
        }

        bool changed = false;
    for (int i = 0; i < 8; i++)
        if (rep[i] != kbd->last_report[i]) { changed = true; break; }
        if (!changed) return;

        uint8_t m = rep[0];
    kbd_state.is_shift_pressed = ((m & 0x02) || (m & 0x20)) != 0;
    kbd_state.is_ctrl_pressed = ((m & 0x01) || (m & 0x10)) != 0;
    kbd_state.is_alt_pressed = ((m & 0x04) || (m & 0x40)) != 0;
    kbd_state.is_ralt_pressed = ((m & 0x40)) != 0;
    kbd_state.is_rctrl_pressed = ((m & 0x10)) != 0;

    for (int i = 2; i < 8; i++) {
        uint8_t key = rep[i];
        if (!key || key == 0x01) continue;

        bool was = false;
        for (int j = 2; j < 8; j++)
            if (kbd->last_report[j] == key) { was = true; break; }
            if (was || kbd->pressed_keys[key]) continue;

            kbd->pressed_keys[key] = true;

        if (key == 0x39) {
            usb_kbd_set_leds(!kbd_state.is_caps_lock,
                             kbd_state.is_num_lock,
                             kbd_state.is_scroll_lock);
            continue;
        }
        if (key == 0x53) {
            usb_kbd_set_leds(kbd_state.is_caps_lock,
                             !kbd_state.is_num_lock,
                             kbd_state.is_scroll_lock);
            continue;
        }
        if (key == 0x47) {
            usb_kbd_set_leds(kbd_state.is_caps_lock,
                             kbd_state.is_num_lock,
                             !kbd_state.is_scroll_lock);
            continue;
        }

        if (push_extended_if_special(key)) continue;

        uint16_t next = (buffer_head + 1) % USB_KBD_BUFFER_SIZE;
        if (next != buffer_tail) {
            key_buffer[buffer_head] = key;
            mod_buffer[buffer_head] = m;
            buffer_head = next;
        }
    }

    for (int i = 2; i < 8; i++) {
        uint8_t old = kbd->last_report[i];
        if (!old || old == 0x01) continue;
        bool still = false;
        for (int j = 2; j < 8; j++)
            if (rep[j] == old) { still = true; break; }
            if (!still) kbd->pressed_keys[old] = false;
    }

    for (int i = 0; i < 8; i++) kbd->last_report[i] = rep[i];
}

static void usb_kbd_on_event(const usb_event_t *evt, void *ctx) {
    struct usb_keyboard_instance *kbd = (struct usb_keyboard_instance *)ctx;
    if (!kbd || !kbd->active || !kbd->dev) return;

    if (!usb_event_verify(evt, kbd->dev, kbd->endpoint_address,
        USB_XFER_INTERRUPT)) {
        return;
        }

        if (evt->src != USB_SRC_XHCI) {
            if (evt->type == USB_EVENT_TRANSFER_DONE &&
                evt->data && evt->data_len >= 8)
                process_report(kbd, (const uint8_t *)evt->data);
            kbd->pending = false;
            return;
        }

        if (evt->type != USB_EVENT_TRANSFER_DONE) {

            kbd->pending = false;
            return;
        }

        if (evt->data && evt->data_len >= 8) {
            process_report(kbd, (const uint8_t *)evt->data);
            kbd->pending = false;
            return;
        }

        if (kbd->dev->ctrl &&
            kbd->dev->ctrl->type == USB_TYPE_XHCI &&
            (uint8_t)kbd->dev->address == evt->slot_id)
        {
            asm volatile("clflush (%0)" :: "r"(kbd->dma_report) : "memory");
            asm volatile("mfence" ::: "memory");
            process_report(kbd, kbd->dma_report);
            kbd->pending = false;
        }
}

static void xhci_kbd_poll(struct usb_core_driver *core) {
    struct xhci_driver *xdrv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);

    void *seen_ctrl[MAX_USB_KEYBOARDS] = {0};
    int seen_cnt = 0;

    if (xdrv && xdrv->poll_event_ring) {
        for (int k = 0; k < keyboard_count; k++) {
            struct usb_keyboard_instance *kbd = &keyboards[k];
            if (!kbd->active || !kbd->dev || !kbd->dev->ctrl) continue;
            if (kbd->dev->ctrl->type != USB_TYPE_XHCI) continue;
            struct xhci_controller *xc = (struct xhci_controller *)kbd->dev->ctrl;
            bool already = false;
            for (int s = 0; s < seen_cnt; s++)
                if (seen_ctrl[s] == xc) { already = true; break; }
                if (!already && seen_cnt < MAX_USB_KEYBOARDS) {
                    seen_ctrl[seen_cnt++] = xc;

                    xdrv->poll_event_ring(xc);
                }
        }
    }

    for (int k = 0; k < keyboard_count; k++) {
        struct usb_keyboard_instance *kbd = &keyboards[k];
        if (!kbd->active || !kbd->dev || !kbd->dev->ctrl) continue;

        bool is_xhci = (kbd->dev->ctrl->type == USB_TYPE_XHCI);

        if (kbd->pending) {
            if (is_xhci) continue;
            int probe = core->interrupt_transfer(kbd->dev,
                                                 kbd->endpoint_address,
                                                 kbd->dma_report, 8, 1);
            if (probe != -2) {

                kbd->pending = false;
                if (probe == -1) {
                    core->control_transfer(kbd->dev, 0x02, 0x01, 0, kbd->endpoint_address, 0, NULL);
                    if (core->reset_endpoint_toggle)
                        core->reset_endpoint_toggle(kbd->dev, kbd->endpoint_address);
                    kbd->armed = false;

                }
            }
            asm volatile("pause");
            continue;
        }

        if (!kbd->armed) {

            volatile uint8_t *d = (volatile uint8_t *)kbd->dma_report;
            for (int i = 0; i < 8; i++) d[i] = 0;
            asm volatile("mfence" ::: "memory");
            asm volatile("clflush (%0)" :: "r"(kbd->dma_report) : "memory");
            asm volatile("mfence" ::: "memory");
        }

        int ret = core->interrupt_transfer(kbd->dev,
                                           kbd->endpoint_address,
                                           kbd->dma_report, 8, 1);
        if (ret == -2) {
            kbd->pending = true;
            kbd->armed = true;
        } else if (ret == 0 && is_xhci) {
            asm volatile("clflush (%0)" :: "r"(kbd->dma_report) : "memory");
            asm volatile("mfence" ::: "memory");
            process_report(kbd, kbd->dma_report);
        }
        asm volatile("pause");
    }

    if (xdrv && xdrv->poll_event_ring) {
        for (int s = 0; s < seen_cnt; s++)
            xdrv->poll_event_ring((struct xhci_controller *)seen_ctrl[s]);
    }
}

void usb_kbd_handler_poll(void) {
    struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    if (!core) return;

    xhci_kbd_poll(core);
    if (core->poll_transfers) core->poll_transfers();
    usb_event_dispatch_all();
    asm volatile("pause");
}

static int usb_kbd_class_request(struct usb_core_driver *core, struct usb_device *dev,
                                  uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
                                  uint16_t len, void *data) {
    int ret = -1;
    for (int attempt = 0; attempt < 5 && ret != 0; attempt++) {
        if (attempt > 0) delay_ms(20);
        ret = core->control_transfer(dev, type, req, val, idx, len, data);
    }
    return ret;
}

static int find_free_kbd_slot(void) {
    for (int i = 0; i < keyboard_count; i++)
        if (!keyboards[i].active) return i;
    if (keyboard_count < MAX_USB_KEYBOARDS) return keyboard_count;
    return -1;
}

static bool usb_kbd_try_attach(struct usb_core_driver *core, struct usb_device *dev) {
    if (!core || !core->control_transfer || !dev) return false;

    if (dev->device_class != 0x03 ||
        dev->device_subclass != 0x01 ||
        dev->device_protocol != 0x01) return false;

    for (int k = 0; k < keyboard_count; k++)
        if (keyboards[k].active && keyboards[k].dev == dev) return false;

    int slot = find_free_kbd_slot();
    if (slot < 0) return false;

    struct usb_keyboard_instance *kbd = &keyboards[slot];
    kbd->dev = dev;
    kbd->active = true;
    kbd->pending = false;
    kbd->armed = false;
    kbd->endpoint_address = dev->endpoint_address ? dev->endpoint_address : 0x81;
    kbd->max_packet_size = dev->max_packet_size ? dev->max_packet_size : 8;
    for (int j = 0; j < 8; j++) kbd->last_report[j] = 0;
    for (int j = 0; j < 8; j++) kbd->dma_report[j] = 0;
    for (int j = 0; j < 256; j++) kbd->pressed_keys[j] = false;

    int ret;

    delay_ms(5);
    ret = usb_kbd_class_request(core, dev, 0x21, HID_SET_PROTOCOL, 0x0000,
                                 dev->hid_interface, 0, NULL); (void)ret;
    ret = usb_kbd_class_request(core, dev, 0x21, HID_SET_IDLE, 0x0000,
                                 dev->hid_interface, 0, NULL); (void)ret;
    uint8_t led = 0;
    ret = usb_kbd_class_request(core, dev, 0x21, HID_SET_REPORT, 0x0200,
                                dev->hid_interface, 1, &led); (void)ret;
    uint8_t init_rep[8] = {0};
    ret = usb_kbd_class_request(core, dev, 0xA1, HID_GET_REPORT, 0x0100,
                                dev->hid_interface, 8, init_rep);
    if (ret == 0)
        for (int j = 0; j < 8; j++) kbd->last_report[j] = init_rep[j];

    usb_event_register_handler_for_device(
        usb_kbd_on_event,
        kbd,
        dev,
        kbd->endpoint_address,
        USB_XFER_INTERRUPT
    );

    if (slot == keyboard_count) keyboard_count++;
    return true;
}

static void usb_kbd_on_usb_event(const usb_event_t *evt, void *ctx) {
    (void)ctx;
    if (!evt) return;

    if (evt->type == USB_EVENT_DEVICE_CONN) {
        struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
        struct usb_device *dev = (struct usb_device *)evt->device;
        if (usb_kbd_try_attach(core, dev))
            usb_kbd_set_leds(false, false, false);
        return;
    }

    if (evt->type == USB_EVENT_DEVICE_DISC) {
        for (int k = 0; k < keyboard_count; k++) {
            if (keyboards[k].active && keyboards[k].dev == evt->device) {
                keyboards[k].active = false;
                keyboards[k].pending = false;
                keyboards[k].dev = NULL;
            }
        }
    }
}

void usb_kbd_input(const char *prompt, char *buffer, uint32_t max_len, uint32_t color, void (*print)(const char *, uint32_t))
{
    struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    if (prompt) print(prompt, color);
    uint32_t idx = 0;

    while (idx < max_len - 1) {
        xhci_kbd_poll(core);
        if (core && core->poll_transfers) core->poll_transfers();
        usb_event_dispatch_all();

        if (usb_kbd_has_key()) {
            uint8_t sc = usb_kbd_get_key();
            char c = usb_kbd_scancode_to_char(sc);
            if (sc == 0x28) { print("\n", color); break; }
            else if (sc == 0x2A) { if (idx>0) { idx--; print("\b \b", color); } }
            else if (c >= 32 && c <= 126) { buffer[idx++] = c; char s[2]={c,0}; print(s,color); }
        } else {
            for (int p = 0; p < 4; p++) {
                xhci_kbd_poll(core);
                if (core && core->poll_transfers) core->poll_transfers();
                usb_event_dispatch_all();
                if (usb_kbd_has_key()) break;
                asm volatile("pause");
            }
        }
    }
    buffer[idx] = '\0';
}

static int usb_kbd_get_count(void) { return keyboard_count; }

static struct ps2_keyboard_state *usb_kbd_get_state(void) { return &kbd_state; }

static uint8_t usb_kbd_last_key_modifiers(void) { return last_dequeued_mods; }

static struct usb_keyboard_driver drv = {
    .keyboard_type = 2,
    .get_state = usb_kbd_get_state,
    .set_leds = usb_kbd_set_leds,
    .is_key_pressed = usb_kbd_is_key_pressed,
    .get_key = usb_kbd_get_key,
    .has_key = usb_kbd_has_key,
    .scancode_to_char = usb_kbd_scancode_to_char,
    .keyboard_handler = usb_kbd_handler_poll,
    .input = usb_kbd_input,
    .has_extended_key = usb_kbd_has_extended_key,
    .get_extended_key = usb_kbd_get_extended_key,
    .keyboard_count = usb_kbd_get_count,
    .last_key_modifiers = usb_kbd_last_key_modifiers,
};

struct usb_keyboard_driver *return_usb_keyboard_driver(void) {
    keyboard_count = 0;
    buffer_head = 0;
    buffer_tail = 0;
    last_dequeued_mods = 0;
    ext_head = 0;
    ext_tail = 0;

    struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    if (!core || !core->control_transfer) return &drv;

    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        struct usb_device *dev = (struct usb_device *)device_table[USB_DEVICE][i];
        if (!dev) continue;
        usb_kbd_try_attach(core, dev);
    }

    usb_event_register_handler(usb_kbd_on_usb_event, NULL);

    if (keyboard_count > 0) usb_kbd_set_leds(false, false, false);
    return &drv;
}

struct driver *return_meta_usb_keyboard_driver(void) {
    static struct dependency usb_dep[] = {
        MAKE_DEPENDENCY(USB_DRIVER, USB_CORE_SLOT)
    };
    static struct driver meta = {
        .name = "USB Keyboard Driver",
        .type = KEYBOARD_DRIVER,
        .sub_type = USB_KEYBOARD,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &usb_dep[0] },
        .dependency_count = 1,
        .self = &drv,
        .init = (void (*)(void))return_usb_keyboard_driver,
    };
    return &meta;
}
