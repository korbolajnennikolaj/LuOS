#include "usb_mouse_driver.h"

#include "components/drivers.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "drivers/USB/xhci.h"
#include "kernel/scheduler/spinlock.h"
#include "mouse_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void delay_ms(uint64_t ms);

static int usb_mouse_class_request(struct usb_core_driver *core, struct usb_device *dev,
                                    uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
                                    uint16_t len, void *data) {
    int ret = -1;
    for (int attempt = 0; attempt < 5 && ret != 0; attempt++) {
        if (attempt > 0) delay_ms(20);
        ret = core->control_transfer(dev, type, req, val, idx, len, data);
    }
    return ret;
}

typedef struct usb_mouse_instance {
    struct usb_device *dev;
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    bool active;
    bool pending;
    uint8_t dma_report[8] __attribute__((aligned(64)));
} usb_mouse_instance;

static struct usb_mouse_instance mice[MAX_USB_MICE];
static int mouse_count = 0;
static struct usb_mouse_state shared_state = {0};
static spinlock_t usb_mouse_lock = SPINLOCK_INIT;

static void process_mouse_report(const uint8_t *buf, uint16_t len) {
    if (len < 3) return;
    spin_lock(&usb_mouse_lock);
    shared_state.btn_left = (buf[0] & (1 << 0)) != 0;
    shared_state.btn_right = (buf[0] & (1 << 1)) != 0;
    shared_state.btn_middle = (buf[0] & (1 << 2)) != 0;
    shared_state.x += (int8_t)buf[1];

    shared_state.y += (int8_t)buf[2];
    if (len >= 4) shared_state.wheel += (int8_t)buf[3];
    spin_unlock(&usb_mouse_lock);
}

static void usb_mouse_event_handler(const usb_event_t *evt, void *ctx) {
    struct usb_mouse_instance *m = (struct usb_mouse_instance *)ctx;
    if (!m || !m->active || !m->dev) return;

    if (!usb_event_verify(evt, m->dev, m->endpoint_address, USB_XFER_INTERRUPT))
        return;

    if (m->dev->ctrl && m->dev->ctrl->type != USB_TYPE_XHCI) {
        if (evt->type == USB_EVENT_TRANSFER_DONE) {
            if (evt->data && evt->data_len >= 3)
                process_mouse_report((const uint8_t *)evt->data, evt->data_len);
            else {
                asm volatile("clflush (%0)" :: "r"(m->dma_report) : "memory");
                asm volatile("mfence" ::: "memory");
                uint16_t dlen = evt->data_len ? evt->data_len : 4;
                process_mouse_report(m->dma_report, dlen);
            }
        }
        m->pending = false;
        return;
    }

    if (evt->type != USB_EVENT_TRANSFER_DONE) return;

    if (evt->data && evt->data_len >= 3) {
        process_mouse_report((const uint8_t *)evt->data, evt->data_len);
        m->pending = false;
        return;
    }

    asm volatile("clflush (%0)" :: "r"(m->dma_report) : "memory");
    asm volatile("mfence" ::: "memory");
    uint16_t dlen = evt->data_len ? evt->data_len : 4;
    process_mouse_report(m->dma_report, dlen);
    m->pending = false;
}

static void usb_mouse_poll(struct usb_core_driver *core) {
    for (int i = 0; i < mouse_count; i++) {
        struct usb_mouse_instance *m = &mice[i];
        if (!m->active || !m->dev) continue;

        bool is_xhci = (m->dev->ctrl && m->dev->ctrl->type == USB_TYPE_XHCI);

        if (m->pending) {
            if (is_xhci) continue;
            int probe = core->interrupt_transfer(m->dev, m->endpoint_address,
                                                 m->dma_report, 8, 1);

            if (probe != -2) {
                m->pending = false;
                if (probe == -1) {
                    core->control_transfer(m->dev, 0x02, 0x01, 0, m->endpoint_address, 0, NULL);
                    if (core->reset_endpoint_toggle)
                        core->reset_endpoint_toggle(m->dev, m->endpoint_address);
                }
            }
            continue;
        }

        for (int j = 0; j < 8; j++) m->dma_report[j] = 0;
        asm volatile("mfence" ::: "memory");
        asm volatile("clflush (%0)" :: "r"(m->dma_report) : "memory");
        asm volatile("mfence" ::: "memory");

        int ret = core->interrupt_transfer(m->dev, m->endpoint_address,
                                           m->dma_report, 8, 1);
        if (ret == -2) {
            m->pending = true;
        } else if (ret == -1) {
            core->control_transfer(m->dev, 0x02, 0x01, 0, m->endpoint_address, 0, NULL);
            if (core->reset_endpoint_toggle)
                core->reset_endpoint_toggle(m->dev, m->endpoint_address);
        }
    }
}

void usb_mouse_handler_poll(void) {
    struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    if (!core) return;

    struct xhci_driver *xdrv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
    if (xdrv && xdrv->poll_event_ring) {
        void *seen[MAX_USB_MICE] = {0};
        int seen_cnt = 0;
        for (int i = 0; i < mouse_count; i++) {
            struct usb_mouse_instance *m = &mice[i];
            if (!m->active || !m->dev || !m->dev->ctrl) continue;
            if (m->dev->ctrl->type != USB_TYPE_XHCI) continue;
            struct xhci_controller *xc = (struct xhci_controller *)m->dev->ctrl;
            bool already = false;
            for (int s = 0; s < seen_cnt; s++)
                if (seen[s] == xc) { already = true; break; }
                if (!already && seen_cnt < MAX_USB_MICE) {
                    seen[seen_cnt++] = xc;
                    xdrv->poll_event_ring(xc);
                }
        }
    }

    usb_mouse_poll(core);
    if (core->poll_transfers) core->poll_transfers();
    usb_event_dispatch_all();
    asm volatile("pause");
}

static struct usb_mouse_state *mouse_get_state(void) { return &shared_state; }
static void mouse_reset_deltas(void) {
    spin_lock(&usb_mouse_lock);
    shared_state.x = 0; shared_state.y = 0; shared_state.wheel = 0;
    spin_unlock(&usb_mouse_lock);
}
static int mouse_get_count(void) { return mouse_count; }

static int find_free_mouse_slot(void) {
    for (int i = 0; i < mouse_count; i++)
        if (!mice[i].active) return i;
    if (mouse_count < MAX_USB_MICE) return mouse_count;
    return -1;
}

static bool usb_mouse_try_attach(struct usb_core_driver *core, struct usb_device *dev) {
    if (!core || !dev) return false;

    if (dev->device_class != USB_MOUSE_CLASS) return false;
    if (dev->device_subclass != USB_MOUSE_SUBCLASS) return false;
    if (dev->device_protocol != USB_MOUSE_PROTOCOL &&
        dev->device_protocol != USB_MOUSE_PROTOCOL_GENERIC) return false;

    for (int i = 0; i < mouse_count; i++)
        if (mice[i].active && mice[i].dev == dev) return false;

    int slot = find_free_mouse_slot();
    if (slot < 0) return false;

    struct usb_mouse_instance *m = &mice[slot];
    m->dev = dev;
    m->active = true;
    m->pending = false;
    m->endpoint_address = dev->endpoint_address ? dev->endpoint_address : 0x81;
    m->max_packet_size = dev->max_packet_size ? dev->max_packet_size : 8;
    for (int j = 0; j < 8; j++) m->dma_report[j] = 0;

    delay_ms(5);
    usb_mouse_class_request(core, dev, 0x21, HID_SET_PROTOCOL,
                             0x0000, dev->hid_interface, 0, NULL);
    usb_mouse_class_request(core, dev, 0x21, HID_SET_IDLE,
                             0x0000, dev->hid_interface, 0, NULL);

    usb_event_register_handler_for_device(
        usb_mouse_event_handler,
        m,
        dev,
        m->endpoint_address,
        USB_XFER_INTERRUPT
    );

    int ret = core->interrupt_transfer(m->dev, m->endpoint_address,
                                       m->dma_report, 8, 1);
    if (ret == -2) m->pending = true;

    if (slot == mouse_count) mouse_count++;
    return true;
}

static void usb_mouse_on_usb_event(const usb_event_t *evt, void *ctx) {
    (void)ctx;
    if (!evt) return;

    if (evt->type == USB_EVENT_DEVICE_CONN) {
        struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
        usb_mouse_try_attach(core, (struct usb_device *)evt->device);
        return;
    }

    if (evt->type == USB_EVENT_DEVICE_DISC) {
        for (int i = 0; i < mouse_count; i++) {
            if (mice[i].active && mice[i].dev == evt->device) {
                mice[i].active = false;
                mice[i].pending = false;
                mice[i].dev = NULL;
            }
        }
    }
}

static void mouse_init(void) {
    mouse_count = 0;
    struct usb_core_driver *core = get_self_driver(USB_DRIVER, USB_CORE_SLOT);
    if (!core) return;

    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        struct usb_device *dev = (struct usb_device *)device_table[USB_DEVICE][i];
        if (!dev) continue;
        usb_mouse_try_attach(core, dev);
    }

    usb_event_register_handler(usb_mouse_on_usb_event, NULL);
}

static struct usb_mouse_driver drv_usb_mouse = {
    .get_state = mouse_get_state,
    .reset_deltas = mouse_reset_deltas,
    .mouse_count = mouse_get_count,
    .mouse_handler= usb_mouse_handler_poll,
};

struct usb_mouse_driver *return_usb_mouse_driver(void) {
    mouse_init();

    return &drv_usb_mouse;
}

struct driver *return_meta_usb_mouse_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(USB_DRIVER, USB_CORE_SLOT),
    };
    static struct driver meta = {
        .name = "USB Mouse Driver",
        .type = MOUSE_DRIVER,
        .sub_type = USB_MOUSE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { dep },
        .dependency_count = 1,
        .self = &drv_usb_mouse,
        .init = (void *)return_usb_mouse_driver,
    };
    return &meta;
}
