#include "drivers/USB/usb_core.h"

#include "components/drivers.h"
#include "components/Interruptions/msi.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/ehci.h"
#include "drivers/USB/ohci.h"
#include "drivers/USB/uhci.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_event.h"
#include "drivers/USB/usb_hub.h"
#include "drivers/USB/usb_log.h"
#include "drivers/USB/xhci.h"
#include "drivers/USB/xhci_hub.h"
#include "drivers/Video/limine_video_driver.h"

#include <cpuid.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern unsigned int xhci_read_current_usbsts(void);

extern uint8_t uhci_dev_is_ls[4][128];

static struct usb_device usb_device_pool[MAX_USB_DEVICES];
static int usb_device_count = 0;

static uint8_t usb_next_address = 1;

#define POLL_BUF_SIZE 8
static uint8_t s_poll_buf[MAX_USB_DEVICES][POLL_BUF_SIZE]
__attribute__((aligned(64)));
static bool s_poll_pending[MAX_USB_DEVICES];

#define POLL_BULK_BUF_SIZE 512
static uint8_t s_bulk_poll_buf[MAX_USB_DEVICES][POLL_BULK_BUF_SIZE]
__attribute__((aligned(64)));
static bool s_bulk_poll_pending[MAX_USB_DEVICES];
static bool s_iso_poll_pending[MAX_USB_DEVICES];

void usb_scan_all(void);

int usb_get_device_count(void) { return usb_device_count; }
struct usb_device *usb_get_device(int idx) {
    if (idx < 0 || idx >= usb_device_count) return NULL;
    if (!usb_device_pool[idx].valid) return NULL;
    return &usb_device_pool[idx];
}

int usb_control_transfer(struct usb_device *dev, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data);

int usb_interrupt_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t direction);

static void usb_core_enqueue_event(const usb_event_t *evt);
static void usb_core_poll_transfers(void);
void usb_init_device(void *ctrl_ptr, uint8_t port, bool is_xhci);
void usb_init_device_topo(void *ctrl_ptr, uint8_t port, bool is_xhci,
                           uint8_t root_port, uint8_t hub_depth,
                           uint32_t route_string, uint8_t parent_hub_slot,
                           uint8_t speed_id);
static int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t direction);
static int usb_iso_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction);

static spinlock_t usb_core_lock = SPINLOCK_INIT;

static int usb_reserve_slot(void) {
    uint64_t flags = spin_lock_irqsave(&usb_core_lock);
    int slot = -1;
    for (int i = 0; i < usb_device_count; i++) {
        if (!usb_device_pool[i].valid) { slot = i; break; }
    }
    if (slot < 0 && usb_device_count < MAX_USB_DEVICES) slot = usb_device_count;
    if (slot >= 0) usb_device_pool[slot].valid = 1;
    spin_unlock_irqrestore(&usb_core_lock, flags);
    return slot;
}

void usb_core_remove_device(struct usb_device *dev) {
    if (!dev) return;
    long slot = dev - usb_device_pool;
    if (slot < 0 || slot >= usb_device_count) return;
    if (!dev->valid) return;

    usb_event_t disc_evt = {
        .type = USB_EVENT_DEVICE_DISC,
        .src = (dev->ctrl && dev->ctrl->type == USB_TYPE_XHCI) ? USB_SRC_XHCI : USB_SRC_UHCI,
        .port = dev->port,
        .device = dev,
        .cookie = dev,
    };
    usb_core_enqueue_event(&disc_evt);

    usb_event_unregister_for_device(dev);

    if (dev->ctrl) {
        switch (dev->ctrl->type) {
            case USB_TYPE_XHCI:
                if (dev->address > 0) {
                    struct xhci_driver *x_drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
                    if (x_drv && x_drv->disable_slot)
                        x_drv->disable_slot((struct xhci_controller *)dev->ctrl, (uint8_t)dev->address);
                }
                break;
            case USB_TYPE_EHCI: {
                struct ehci_driver *e_drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
                if (e_drv && e_drv->notify_disconnect) e_drv->notify_disconnect(dev);
                break;
            }
            case USB_TYPE_OHCI: {
                struct ohci_driver *o_drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
                if (o_drv && o_drv->notify_disconnect) o_drv->notify_disconnect(dev);
                break;
            }
            case USB_TYPE_UHCI: {
                struct uhci_driver *u_drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
                if (u_drv && u_drv->notify_disconnect) u_drv->notify_disconnect(dev);
                break;
            }
        }
    }

    { uint64_t flags = spin_lock_irqsave(&usb_core_lock);
    dev->valid = 0;
    device_table[USB_DEVICE][slot] = NULL;
    spin_unlock_irqrestore(&usb_core_lock, flags); }
    s_poll_pending[slot] = false;
    s_bulk_poll_pending[slot] = false;
    s_iso_poll_pending[slot] = false;
}

void delay_ms(uint64_t ms) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (tsc && tsc->sleep_tsc_ms) {
        tsc->sleep_tsc_ms(ms);
        return;
    }

    for (volatile uint64_t i = 0; i < ms * 2000000ull; i++) asm volatile("pause");
}

static void usb_debug(const char *msg, uint32_t val, int show_val) {
    if (show_val) usb_log_hex(USB_LOG_CORE, USB_LOG_INFO, msg, val);
    else usb_log(USB_LOG_CORE, USB_LOG_INFO, msg);
}

static void usb_core_enqueue_event(const usb_event_t *evt) {
    usb_push_event(evt);
}

int usb_control_transfer(struct usb_device *dev, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data)
{
    if (!dev || !dev->ctrl) return -1;

    static struct usb_setup_packet setup __attribute__((aligned(64)));
    setup.bmRequestType = type;
    setup.bRequest = req;
    setup.wValue = val;
    setup.wIndex = idx;
    setup.wLength = len;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->control_transfer)
                return drv->control_transfer(
                    (struct xhci_controller *)dev->ctrl,
                                             dev->address, 0, &setup, 8, data, len,
                                             (type & 0x80) ? 1 : 0);
                break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->control_transfer)
                return drv->control_transfer(
                    (struct ehci_controller *)dev->ctrl,
                                             dev->address, 0, &setup, 8, data, len,
                                             (type & 0x80) != 0);
                break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->control_transfer)
                return drv->control_transfer(
                    (struct ohci_controller *)dev->ctrl,
                                             dev->address, 0, &setup, 8, data, len,
                                             (type & 0x80) != 0);
                break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->control_transfer)
                return drv->control_transfer(
                    (struct uhci_controller *)dev->ctrl,
                                             dev->address, 0, &setup, 8, data, len,
                                             (type & 0x80) != 0);
                break;
        }
    }
    return -1;
}

int usb_interrupt_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t direction)
{
    if (!dev || !dev->ctrl) return -1;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->interrupt_transfer)
                return drv->interrupt_transfer(
                    (struct xhci_controller *)dev->ctrl,
                                               dev->address, endpoint, data, len, direction);
                break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->interrupt_transfer)
                return drv->interrupt_transfer(
                    (struct ehci_controller *)dev->ctrl,
                                               dev->address, endpoint, data, len, direction);
                break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->interrupt_transfer)
                return drv->interrupt_transfer(
                    (struct ohci_controller *)dev->ctrl,
                                               dev->address, endpoint, data, len, direction);
                break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->interrupt_transfer)
                return drv->interrupt_transfer(
                    (struct uhci_controller *)dev->ctrl,
                                               dev->address, endpoint, data, len, direction);
                break;
        }
    }
    return -1;
}

static int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t direction)
{
    if (!dev || !dev->ctrl) return -1;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->bulk_transfer)
                return drv->bulk_transfer(
                    (struct xhci_controller *)dev->ctrl,
                                          dev->address, endpoint, data, len, direction);
                break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->bulk_transfer)
                return drv->bulk_transfer(
                    (struct ehci_controller *)dev->ctrl,
                                          dev->address, endpoint, data, len, direction);
                break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->bulk_transfer)
                return drv->bulk_transfer(
                    (struct ohci_controller *)dev->ctrl,
                                          dev->address, endpoint, data, len, direction);
                break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->bulk_transfer)
                return drv->bulk_transfer(
                    (struct uhci_controller *)dev->ctrl,
                                          dev->address, endpoint, data, len, direction);
                break;
        }
    }
    return -1;
}

static void usb_reset_endpoint_toggle(struct usb_device *dev, uint8_t endpoint)
{
    if (!dev || !dev->ctrl) return;
    switch (dev->ctrl->type) {
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->reset_endpoint_toggle)
                drv->reset_endpoint_toggle(
                    (struct uhci_controller *)dev->ctrl,
                                           dev->address, endpoint);
                break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->reset_endpoint_toggle)
                drv->reset_endpoint_toggle(
                    (struct ohci_controller *)dev->ctrl,
                                           dev->address, endpoint);
                break;
        }
        case USB_TYPE_EHCI: {

            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->reset_endpoint_toggle)
                drv->reset_endpoint_toggle(dev, dev->address, endpoint);
            break;
        }
        case USB_TYPE_XHCI: {

            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->reset_bulk_toggle)
                drv->reset_bulk_toggle(
                    (struct xhci_controller *)dev->ctrl,
                                       dev->address, endpoint);
                break;
        }
        default:
            break;
    }
}

static int usb_iso_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction)
{
    if (!dev || !dev->ctrl) return -1;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->iso_transfer)
                return drv->iso_transfer(
                    (struct xhci_controller *)dev->ctrl,
                                         dev->address, endpoint, data, len,
                                         n_frames, frame_lens, direction);
                break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->iso_transfer)
                return drv->iso_transfer(
                    (struct ehci_controller *)dev->ctrl,
                                         dev->address, endpoint, data, len,
                                         n_frames, frame_lens, direction);
                break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->iso_transfer)
                return drv->iso_transfer(
                    (struct ohci_controller *)dev->ctrl,
                                         dev->address, endpoint, data, len,
                                         n_frames, frame_lens, direction);
                break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->iso_transfer)
                return drv->iso_transfer(
                    (struct uhci_controller *)dev->ctrl,
                                         dev->address, endpoint, data, len,
                                         n_frames, frame_lens, direction);
                break;
        }
    }
    return -1;
}

#define USB_ROOT_MAX_CTRL 8
#define USB_ROOT_MAX_PORTS 32

#define USB_ROOT_PORT_BUSY (-2)

typedef struct usb_root_port_state {
    uint8_t inited;
    struct root_hub hub;

    int16_t slot[USB_ROOT_MAX_PORTS + 1];

    uint8_t mismatch_tries[USB_ROOT_MAX_PORTS + 1];
} usb_root_port_state_t;

#define USB_ROOT_MISMATCH_MAX_TRIES 3

static usb_root_port_state_t s_root_xhci[USB_ROOT_MAX_CTRL];
static usb_root_port_state_t s_root_ehci[USB_ROOT_MAX_CTRL];
static usb_root_port_state_t s_root_ohci[USB_ROOT_MAX_CTRL];
static usb_root_port_state_t s_root_uhci[USB_ROOT_MAX_CTRL];

static void usb_root_ports_reset_state(void) {
    usb_root_port_state_t *tables[4] = { s_root_xhci, s_root_ehci, s_root_ohci, s_root_uhci };
    for (int t = 0; t < 4; t++) {
        for (int c = 0; c < USB_ROOT_MAX_CTRL; c++) {
            tables[t][c].inited = 0;
            for (int p = 0; p <= USB_ROOT_MAX_PORTS; p++) {
                tables[t][c].slot[p] = -1;
                tables[t][c].mismatch_tries[p] = 0;
            }
        }
    }
}

static int usb_root_find_device_slot(struct usb_controller *ctrl_generic, uint8_t port) {
    for (int i = 0; i < usb_device_count; i++) {
        struct usb_device *dev = &usb_device_pool[i];
        if (dev->valid && dev->ctrl == ctrl_generic && dev->port == port) return i;
    }
    return -1;
}

static void usb_root_port_mark(usb_root_port_state_t *table, int ci, uint8_t port, int16_t val) {
    if (ci < 0 || ci >= USB_ROOT_MAX_CTRL) return;
    if (port < 1 || port > USB_ROOT_MAX_PORTS) return;
    table[ci].slot[port] = val;
}

static void usb_root_ports_poll_one(usb_log_tag log_tag,
                                     usb_root_port_state_t *table,
                                     int ctrl_count_, void *(*get_ctrl)(int),
                                     enum HUB_CONTROLLER_TYPE hub_type,
                                     bool is_xhci, bool is_ehci,
                                     uint32_t change_mask) {
    if (ctrl_count_ > USB_ROOT_MAX_CTRL) ctrl_count_ = USB_ROOT_MAX_CTRL;

    for (int ci = 0; ci < ctrl_count_; ci++) {
        void *ctrl = get_ctrl(ci);
        if (!ctrl) continue;

        usb_root_port_state_t *st = &table[ci];
        if (!st->inited) {
            if (!root_hub_init(&st->hub, hub_type, ctrl)) continue;
            st->inited = 1;
        }

        struct root_hub *hub = &st->hub;
        uint8_t pc = hub->port_count;
        if (pc > USB_ROOT_MAX_PORTS) pc = USB_ROOT_MAX_PORTS;

        for (uint8_t port = 1; port <= pc; port++) {
            uint32_t pst = root_hub_port_status(hub, port);

            uint32_t change = pst & change_mask;
            bool connected_now = (pst & 0x1u) != 0;
            int recorded = st->slot[port];

            bool state_mismatch =
                (recorded != USB_ROOT_PORT_BUSY) &&
                (connected_now != (recorded >= 0));

            if (!change && !state_mismatch) continue;

            if (change) {

                st->mismatch_tries[port] = 0;
            } else {
                if (connected_now &&
                    st->mismatch_tries[port] >= USB_ROOT_MISMATCH_MAX_TRIES)
                    continue;
                if (connected_now) st->mismatch_tries[port]++;

                usb_logrow_begin(log_tag, USB_LOG_WARN);
                usb_logrow_str("root poll: c");
                usb_logrow_dec(ci);
                usb_logrow_str(" p");
                usb_logrow_dec(port);
                usb_logrow_str(connected_now ? " connect" : " disconnect");
                usb_logrow_str(" seen via CCS only (change bit lost)");
                usb_logrow_end();
            }

            bool connected = connected_now;
            int cur_slot = recorded;

            if (cur_slot == USB_ROOT_PORT_BUSY) {

                usb_logrow_begin(log_tag, USB_LOG_WARN);
                usb_logrow_str("root poll: skipped re-entrant hit on c");
                usb_logrow_dec(ci);
                usb_logrow_str(" p");
                usb_logrow_dec(port);
                usb_logrow_str(" (enumeration already in progress)");
                usb_logrow_end();
                continue;
            }

            if (is_xhci && connected && cur_slot >= 0 && !(pst & 0x2u)) {
                usb_log_port_event(log_tag, ci, port, "port-disabled, re-enumerating", pst, 0xFF);
                usb_hub_detach(cur_slot);
                st->slot[port] = -1;
                cur_slot = -1;
            }

            if (connected && cur_slot < 0) {

                bool bounced = false;
                for (int waited = 0; waited < 100; waited += 25) {
                    delay_ms(25);
                    if (!(root_hub_port_status(hub, port) & 0x1u)) { bounced = true; break; }
                }
                if (bounced) {

                    root_hub_clear_port_change(hub, port, change);
                    continue;
                }
                pst = root_hub_port_status(hub, port);
                connected = (pst & 0x1u) != 0;

                if (is_ehci && (pst & (1u << 13))) {
                    root_hub_exec(hub, HUB_CMD_PORT_OWNER_CLEAR, port, 0);
                    delay_ms(50);
                    pst = root_hub_port_status(hub, port);
                    connected = (pst & 0x1u) != 0;
                    if (!connected) {
                        root_hub_clear_port_change(hub, port, change);
                        continue;
                    }
                }

                int existing = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                if (existing >= 0) {
                    st->slot[port] = (int16_t)existing;
                    usb_log_port_event(log_tag, ci, port, "adopt-existing", pst, 0xFF);
                } else if (root_hub_port_reset(hub, port)) {

                    st->slot[port] = USB_ROOT_PORT_BUSY;
                    usb_init_device(ctrl, port, is_xhci);

                    int found = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                    uint32_t post = root_hub_port_status(hub, port);
                    if (found >= 0) {
                        st->slot[port] = (int16_t)found;
                        st->mismatch_tries[port] = 0;
                        usb_log_port_event(log_tag, ci, port, "connect", post, 0xFF);
                    } else {
                        st->slot[port] = -1;
                        usb_log_port_event(log_tag, ci, port, "enum-failed", post, 0xFF);
                    }
                } else if (is_ehci) {

                    root_hub_exec(hub, HUB_CMD_PORT_OWNER_SET, port, 0);
                    uint32_t post = root_hub_port_status(hub, port);
                    usb_log_port_event(log_tag, ci, port, "owner->companion", post, 0xFF);
                } else {
                    uint32_t post = root_hub_port_status(hub, port);
                    usb_log_port_event(log_tag, ci, port, "reset-failed", post, 0xFF);
                }
            } else if (!connected && cur_slot >= 0) {

                usb_hub_detach(cur_slot);
                st->slot[port] = -1;
                st->mismatch_tries[port] = 0;
                usb_log_port_event(log_tag, ci, port, "disconnect", pst, 0xFF);
            }

            root_hub_clear_port_change(hub, port, change);
        }
    }
}

static void *adapt_get_xhci(int i) {
    struct xhci_driver *d = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
    return (d && d->get_controller) ? (void *)d->get_controller(i) : NULL;
}
static void *adapt_get_ehci(int i) {
    struct ehci_driver *d = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
    return (d && d->get_controller) ? (void *)d->get_controller(i) : NULL;
}
static void *adapt_get_ohci(int i) {
    struct ohci_driver *d = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
    return (d && d->get_controller) ? (void *)d->get_controller(i) : NULL;
}
static void *adapt_get_uhci(int i) {
    struct uhci_driver *d = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
    return (d && d->get_controller) ? (void *)d->get_controller(i) : NULL;
}

static void usb_root_ports_poll(void) {
    struct xhci_driver *xhci = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
    if (xhci && xhci->get_controller_count) {
        int cnt = xhci->get_controller_count();

        usb_root_ports_poll_one(USB_LOG_XHCI, s_root_xhci, cnt, adapt_get_xhci,
                                 HUB_TYPE_XHCI, true, false,
                                 (1u << 17) | (1u << 18) | (1u << 19));
    }

    struct ehci_driver *ehci = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
    if (ehci && ehci->get_controller_count) {
        int cnt = ehci->get_controller_count();

        usb_root_ports_poll_one(USB_LOG_EHCI, s_root_ehci, cnt, adapt_get_ehci,
                                 HUB_TYPE_EHCI, false, true,
                                 (1u << 1));
    }

    struct ohci_driver *ohci = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
    if (ohci && ohci->get_controller_count) {
        int cnt = ohci->get_controller_count();

        usb_root_ports_poll_one(USB_LOG_OHCI, s_root_ohci, cnt, adapt_get_ohci,
                                 HUB_TYPE_OHCI, false, false,
                                 (1u << 16));
    }

    struct uhci_driver *uhci = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
    if (uhci && uhci->get_controller_count) {
        int cnt = uhci->get_controller_count();

        usb_root_ports_poll_one(USB_LOG_UHCI, s_root_uhci, cnt, adapt_get_uhci,
                                 HUB_TYPE_UHCI, false, false,
                                 (1u << 1));
    }
}

static void usb_core_poll_transfers(void) {
    for (int i = 0; i < usb_device_count; i++) {
        struct usb_device *dev = &usb_device_pool[i];
        if (!dev->valid) continue;
        bool is_xhci = (dev->ctrl && dev->ctrl->type == USB_TYPE_XHCI);

        if (dev->device_class == 0x03 &&
            dev->device_subclass == 0x01 &&
            (dev->device_protocol == 0x00 || dev->device_protocol == 0x01 ||
            dev->device_protocol == 0x02) &&
            dev->endpoint_address != 0)
        {
            if (!is_xhci && !dev->driver_managed) {
                uint8_t *buf = s_poll_buf[i];
                int ret = usb_interrupt_transfer(dev, dev->endpoint_address,
                                                 buf, POLL_BUF_SIZE, 1);
                if (ret == 0) {
                    s_poll_pending[i] = false;

                    usb_event_t evt = {
                        .type = USB_EVENT_TRANSFER_DONE,
                        .src = USB_SRC_UHCI,
                        .transfer_type = USB_XFER_INTERRUPT,
                        .endpoint = dev->endpoint_address,
                        .device = dev,
                        .cookie = dev,
                        .data = buf,
                        .data_len = POLL_BUF_SIZE,
                    };
                    usb_push_event(&evt);
                } else if (ret == -2) {
                    s_poll_pending[i] = true;
                } else {
                    s_poll_pending[i] = false;
                }
            }
        }

        if (dev->bulk_ep_count > 0 && !is_xhci &&
            dev->device_class != 0x08 && !dev->driver_managed) {
            struct usb_endpoint_info *bep = NULL;
        for (int bi = 0; bi < dev->bulk_ep_count; bi++) {
            if (dev->bulk_ep[bi].address & 0x80u) {
                bep = &dev->bulk_ep[bi]; break;
            }
        }
        if (bep) {
            int ret = usb_bulk_transfer(dev, bep->address,
                                        s_bulk_poll_buf[i],
                                        POLL_BULK_BUF_SIZE, 1);
            if (ret == 0) {
                s_bulk_poll_pending[i] = false;

                usb_event_t bevt = {
                    .type = USB_EVENT_BULK_DONE,
                    .src = USB_SRC_UHCI,
                    .transfer_type = USB_XFER_BULK,
                    .endpoint = bep->address,
                    .device = dev,
                    .cookie = dev,
                    .data = s_bulk_poll_buf[i],
                    .data_len = POLL_BULK_BUF_SIZE,
                };
                usb_push_bulk_event(&bevt);
            } else if (ret == -2) {
                s_bulk_poll_pending[i] = true;
            } else {
                s_bulk_poll_pending[i] = false;
            }
        }
            }

            if (dev->iso_ep_count > 0 && !is_xhci) {
                switch (dev->ctrl ? dev->ctrl->type : -1) {
                    case USB_TYPE_EHCI: {
                        struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
                        if (drv) { extern void ehci_poll_iso(void *e); ehci_poll_iso((struct ehci_controller *)dev->ctrl); }
                        break;
                    }
                    case USB_TYPE_OHCI: {
                        struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
                        if (drv) { extern void ohci_poll_iso(void *o); ohci_poll_iso((struct ohci_controller *)dev->ctrl); }
                        break;
                    }
                    case USB_TYPE_UHCI: {
                        struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
                        if (drv) { extern void uhci_poll_iso(void *u); uhci_poll_iso((struct uhci_controller *)dev->ctrl); }
                        break;
                    }
                    default: break;
                }
            }

            asm volatile("pause");
    }

    usb_root_ports_poll();

    usb_hub_poll();

    usb_event_dispatch_all();
    usb_bulk_dispatch_all();
    usb_iso_dispatch_all();
}

static int usb_validate_device_descriptor(struct usb_device_descriptor *desc) {
    if (desc->bLength != 18) return -1;
    if (desc->bDescriptorType != 0x01) return -1;
    uint8_t mps = desc->bMaxPacketSize0;

    if (mps != 8 && mps != 9 && mps != 16 && mps != 32 && mps != 64) return -1;
    if (desc->bcdUSB == 0) return -1;
    return 0;
}

static void usb_parse_config(struct usb_device *dev, const uint8_t *cfg, uint16_t total_len)
{
    const uint8_t *ptr = cfg;
    const uint8_t *end = cfg + total_len;

    uint8_t cur_interface = 0;
    bool in_hid = false;

    while (ptr < end) {
        uint8_t dlen = ptr[0];
        uint8_t dtype = ptr[1];
        if (dlen == 0 || ptr + dlen > end) break;

        if (dtype == 0x04) {
            uint8_t i_class = ptr[5];
            uint8_t i_subclass = ptr[6];
            uint8_t i_protocol = ptr[7];
            cur_interface = ptr[2];

            in_hid = (i_class == 0x03 && i_subclass == 0x01 &&
            (i_protocol == 0x00 || i_protocol == 0x01 || i_protocol == 0x02));

            if (i_class == 0x08) {
                dev->device_class = 0x08;
                dev->device_subclass = i_subclass;
                dev->device_protocol = i_protocol;
            }

            usb_debug("USB: iface cls=", i_class, 1);
            usb_debug("USB: iface sub=", i_subclass, 1);
            usb_debug("USB: iface prt=", i_protocol, 1);
            usb_debug("USB: in_hid=", in_hid, 1);
            if (in_hid) {

                if (dev->device_class == 0x03 && dev->device_protocol == 0x01) {

                    in_hid = false;
                } else {
                    dev->device_class = 0x03;
                    dev->device_subclass = 0x01;
                    dev->device_protocol = i_protocol;
                    dev->hid_interface = cur_interface;
                }
            }
        } else if (dtype == 0x05) {
            uint8_t ep_addr = ptr[2];
            uint8_t ep_attr = ptr[3];
            uint16_t mps = (uint16_t)(ptr[4] | ((uint16_t)ptr[5] << 8));
            uint8_t ep_xfer = ep_attr & 0x03u;

            if (in_hid) {

                if ((ep_addr & 0x80u) && ep_xfer == USB_EP_XFER_INTERRUPT) {
                    dev->endpoint_address = ep_addr;
                    dev->max_packet_size = mps ? mps : 8;
                    dev->hid_interval = (ptr + 6 < end) ? ptr[6] : 10;
                    if (dev->hid_interval == 0) dev->hid_interval = 10;
                }
            }

            if (dev->device_class == 0x09 &&
                (ep_addr & 0x80u) && ep_xfer == USB_EP_XFER_INTERRUPT) {
                dev->hub_status_ep = ep_addr;
                dev->hub_status_ep_mps = mps ? mps : 1;
                dev->hub_status_interval = (ptr + 6 < end) ? ptr[6] : 12;
                if (dev->hub_status_interval == 0) dev->hub_status_interval = 12;
            }

            if (ep_xfer == USB_EP_XFER_BULK &&
                dev->bulk_ep_count < USB_MAX_BULK_EP) {
                struct usb_endpoint_info *bep =
                &dev->bulk_ep[dev->bulk_ep_count++];
            bep->address = ep_addr;
            bep->attributes = ep_attr;
            bep->max_packet_size = mps ? mps : 64u;
            bep->interval = (ptr + 6 < end) ? ptr[6] : 0;
                }

                if (ep_xfer == USB_EP_XFER_ISO &&
                    dev->iso_ep_count < USB_MAX_ISO_EP) {
                    struct usb_endpoint_info *iep =
                    &dev->iso_ep[dev->iso_ep_count++];
                iep->address = ep_addr;
                iep->attributes = ep_attr;
                iep->max_packet_size = mps ? mps : 1023u;
                iep->interval = (ptr + 6 < end) ? ptr[6] : 1;
                    }
        }
        ptr += dlen;
    }

    if (in_hid && !dev->endpoint_address) {
        dev->endpoint_address = 0x81;
        dev->max_packet_size = 8;
    }
}

void usb_init_device(void *ctrl_ptr, uint8_t port, bool is_xhci) {
    usb_init_device_topo(ctrl_ptr, port, is_xhci, port, 0, 0, 0, 0);
}

void usb_init_device_topo(void *ctrl_ptr, uint8_t port, bool is_xhci,
                           uint8_t root_port, uint8_t hub_depth,
                           uint32_t route_string, uint8_t parent_hub_slot,
                           uint8_t speed_id) {
    int slot = usb_reserve_slot();
    if (slot < 0) return;
    struct usb_device *dev = &usb_device_pool[slot];

    uint8_t *p = (uint8_t *)dev;
    for (size_t i = 0; i < sizeof(struct usb_device); i++) p[i] = 0;
    dev->valid = 1;

    dev->port = port;
    dev->ctrl = (struct usb_controller *)ctrl_ptr;
    dev->root_port = root_port;
    dev->hub_depth = hub_depth;
    usb_debug("USB: init port ", port, 1);

    if (is_xhci) {
        struct xhci_driver *x_drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
        int slot_id = x_drv->enable_slot((struct xhci_controller *)ctrl_ptr);
        if (slot_id <= 0) goto fail;

        uint32_t my_route = route_string;
        if (hub_depth > 0 && hub_depth <= 5)
            my_route |= ((uint32_t)(port & 0xFu) << (4 * (hub_depth - 1)));
        dev->route_string = my_route;

        struct xhci_topology topo = {
            .root_port = root_port,
            .route_string = my_route,
            .parent_hub_slot = parent_hub_slot,
            .parent_port = (parent_hub_slot != 0) ? port : 0,
            .speed_id = speed_id,
        };

        if (x_drv->address_device((struct xhci_controller *)ctrl_ptr,
            slot_id, &topo) != 0) {

            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }
        dev->address = slot_id;
        delay_ms(100);

        static uint8_t desc_tmp[64] __attribute__((aligned(64)));
        struct usb_setup_packet setup = {0x80, 0x06, 0x0100, 0, 18};
        if (x_drv->control_transfer((struct xhci_controller *)dev->ctrl,
            dev->address, 0, &setup, 8, desc_tmp, 18, 1) != 0) {
            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }
        for (int i = 0; i < 18; i++) ((uint8_t *)&dev->desc)[i] = desc_tmp[i];

        if (usb_validate_device_descriptor(&dev->desc) != 0) {
            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }

        usb_debug("USB: VID:PID=", ((uint32_t)dev->desc.idVendor << 16) | dev->desc.idProduct, 1);
    } else {
        dev->address = 0;

        static uint8_t b8[8] __attribute__((aligned(64)));
        for (int i = 0; i < 8; i++) b8[i] = 0;

        int got_desc = 0;
        int last_xfer_ret = 0;
        for (int retry = 0; retry < 5; retry++) {
            for (int i = 0; i < 8; i++) b8[i] = 0;
            last_xfer_ret = usb_control_transfer(dev, 0x80, 0x06, 0x0100, 0, 8, b8);
            usb_debug("USB: GET_DESC8 ret=", (uint32_t)(last_xfer_ret & 0xFFFFFFFF), 1);
            if (last_xfer_ret == 0) {
                usb_debug("USB: b8[0]=", b8[0], 1);
                usb_debug("USB: b8[1]=", b8[1], 1);
                usb_debug("USB: b8[7]=", b8[7], 1);
                uint32_t sum = 0;
                for (int i = 0; i < 8; i++) sum |= b8[i];
                if (sum != 0 && b8[1] == 0x01 && b8[0] == 18) {

                    dev->max_packet_size = b8[7] ? (uint16_t)b8[7] : 8u;
                    got_desc = 1;
                    break;
                }
            }
            delay_ms(50);
        }

        if (!got_desc) {
            usb_debug("USB: FAIL got_desc=0 last_ret=", (uint32_t)(last_xfer_ret & 0xFFFFFFFF), 1);
            usb_debug("USB: b8[0]=", b8[0], 1);
            usb_debug("USB: b8[1]=", b8[1], 1);
            goto fail;
        }

        delay_ms(20);
        if (usb_next_address > 127) {
            usb_debug("USB: FAIL out of USB addresses", 0, 1);
            goto fail;
        }
        uint8_t new_addr = usb_next_address++;
        usb_debug("USB: SET_ADDR ", new_addr, 1);
        int sa_ret = usb_control_transfer(dev, 0x00, 0x05, new_addr, 0, 0, NULL);
        if (sa_ret != 0) {
            usb_debug("USB: FAIL SET_ADDR ret=", (uint32_t)(sa_ret & 0xFFFFFFFF), 1);
            goto fail;
        }
        dev->address = new_addr;
        delay_ms(50);

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_UHCI) {
            struct uhci_driver *uhci_drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (uhci_drv) {
                int uhci_cnt = uhci_drv->get_controller_count();
                for (int ki = 0; ki < uhci_cnt; ki++) {
                    if (uhci_drv->get_controller(ki) == (struct uhci_controller *)dev->ctrl) {

                        dev->is_low_speed = uhci_dev_is_ls[ki][0];
                        uhci_dev_is_ls[ki][new_addr & 0x7F] = uhci_dev_is_ls[ki][0];
                        usb_debug("LS save ki=", (uint32_t)ki, 1);
                        usb_debug("LS save addr=", (uint32_t)new_addr, 1);
                        usb_debug("LS save ls[0]=", (uint32_t)uhci_dev_is_ls[ki][0], 1);
                        break;
                    }
                }
            }
        }

        static uint8_t desc_full[18] __attribute__((aligned(64)));
        for (int i = 0; i < 18; i++) desc_full[i] = 0;
        int gd_ret = usb_control_transfer(dev, 0x80, 0x06, 0x0100, 0, 18, desc_full);
        if (gd_ret != 0) {
            usb_debug("USB: FAIL GET_DESC18 ret=", (uint32_t)(gd_ret & 0xFFFFFFFF), 1);
            goto fail;
        }
        for (int i = 0; i < 18; i++) ((uint8_t *)&dev->desc)[i] = desc_full[i];
        usb_debug("USB: desc bcdUSB=", dev->desc.bcdUSB, 1);
        usb_debug("USB: desc mps0=", dev->desc.bMaxPacketSize0, 1);
        usb_debug("USB: desc VID=", dev->desc.idVendor, 1);
        usb_debug("USB: desc PID=", dev->desc.idProduct, 1);
        if (usb_validate_device_descriptor(&dev->desc) != 0) {
            usb_debug("USB: FAIL validate bLen=", dev->desc.bLength, 1);
            usb_debug("USB: FAIL validate mps=", dev->desc.bMaxPacketSize0, 1);
            goto fail;
        }

        usb_debug("USB: VID:PID=", ((uint32_t)dev->desc.idVendor << 16) | dev->desc.idProduct, 1);
    }

    dev->device_class = dev->desc.bDeviceClass;
    dev->device_subclass = dev->desc.bDeviceSubClass;
    dev->device_protocol = dev->desc.bDeviceProtocol;

    if (dev->desc.iManufacturer > 0) {
        static uint8_t s_tmp[256] __attribute__((aligned(64)));
        if (usb_control_transfer(dev, 0x80, 0x06,
            (0x03 << 8) | dev->desc.iManufacturer,
                                 0x0409, 255, s_tmp) == 0) {
            int len = (s_tmp[0] - 2) / 2;
        int j;
        for (j = 0; j < len && j < 31; j++)
            dev->vendor_str[j] = (char)s_tmp[2 + j * 2];
            dev->vendor_str[j] = '\0';
                                 }
    }

    if (dev->desc.iProduct > 0) {
        static uint8_t s_tmp2[256] __attribute__((aligned(64)));
        if (usb_control_transfer(dev, 0x80, 0x06,
            (0x03 << 8) | dev->desc.iProduct,
                                 0x0409, 255, s_tmp2) == 0) {
            int len = (s_tmp2[0] - 2) / 2;
        int j;
        for (j = 0; j < len && j < 31; j++)
            dev->product_str[j] = (char)s_tmp2[2 + j * 2];
            dev->product_str[j] = '\0';
                                 }
    }

    static uint8_t cfg_desc[256] __attribute__((aligned(64)));
    for (int i = 0; i < 256; i++) cfg_desc[i] = 0;

    int cfg9_ret = -1;
    for (int _r = 0; _r < 3 && cfg9_ret != 0; _r++) {
        if (_r > 0) delay_ms(20);
        for (int i = 0; i < 9; i++) cfg_desc[i] = 0;
        cfg9_ret = usb_control_transfer(dev, 0x80, 0x06, (0x02 << 8), 0, 9, cfg_desc);
    }
    usb_debug("USB: GET_CFG9 ret=", (uint32_t)(cfg9_ret & 0xFFFFFFFF), 1);
    if (cfg9_ret == 0) {
        uint16_t total_len = cfg_desc[2] | ((uint16_t)cfg_desc[3] << 8);
        usb_debug("USB: cfg total_len=", total_len, 1);
        if (total_len > 256) total_len = 256;

        delay_ms(5);
        int cfgN_ret = usb_control_transfer(dev, 0x80, 0x06,
                                            (0x02 << 8), 0, total_len, cfg_desc);
        usb_debug("USB: GET_CFGN ret=", (uint32_t)(cfgN_ret & 0xFFFFFFFF), 1);
        if (cfgN_ret == 0) {
            usb_parse_config(dev, cfg_desc, total_len);
            usb_debug("USB: after parse cls=", (uint32_t)dev->device_class, 1);
            usb_debug("USB: after parse sub=", (uint32_t)dev->device_subclass, 1);
            usb_debug("USB: after parse prt=", (uint32_t)dev->device_protocol, 1);
            usb_debug("USB: after parse ep=", (uint32_t)dev->endpoint_address, 1);
        }
    }

    {

        uint8_t config_value = (cfg9_ret == 0 && cfg_desc[5] != 0) ? cfg_desc[5] : 1;

        delay_ms(5);

        int sc_ret = -1;
        for (int _sc = 0; _sc < 5 && sc_ret != 0; _sc++) {
            if (_sc > 0) delay_ms(20);
            sc_ret = usb_control_transfer(dev, 0x00, 0x09, config_value, 0, 0, NULL);
        }
        if (sc_ret != 0) {
            usb_debug("USB: WARN SET_CONFIGURATION failed after retries, ret=",
                      (uint32_t)(sc_ret & 0xFFFFFFFF), 1);
        }

        if (sc_ret == 0) delay_ms(15);
    }

    s_poll_pending[slot] = false;
    for (int i = 0; i < POLL_BUF_SIZE; i++) s_poll_buf[slot][i] = 0;
    s_bulk_poll_pending[slot] = false;
    s_iso_poll_pending[slot] = false;
    for (int i = 0; i < POLL_BULK_BUF_SIZE; i++) s_bulk_poll_buf[slot][i] = 0;

    { uint64_t flags = spin_lock_irqsave(&usb_core_lock);
    dev->valid = 1;
    device_table[USB_DEVICE][slot] = dev;
    if (slot == usb_device_count) usb_device_count++;
    spin_unlock_irqrestore(&usb_core_lock, flags); }

    usb_event_t conn_evt = {
        .type = USB_EVENT_DEVICE_CONN,
        .src = is_xhci ? USB_SRC_XHCI : USB_SRC_UHCI,
        .port = port,
        .device = dev,
        .cookie = dev,
    };
    usb_core_enqueue_event(&conn_evt);

    if (dev->device_class == 0x09) {
        usb_debug("USB: HUB detected, enumerating downstream ports", 0, 0);

        usb_hub_attach(dev, slot);

        usb_debug("USB: HUB downstream enumeration done", 0, 0);
    }
    return;

fail:
    { uint64_t flags = spin_lock_irqsave(&usb_core_lock);
    dev->valid = 0;
    spin_unlock_irqrestore(&usb_core_lock, flags); }
}

void usb_scan_all(void) {
    usb_device_count = 0;
    usb_next_address = 1;

    for (int _ci = 0; _ci < MAX_USB_DEVICES; _ci++) {
        device_table[USB_DEVICE][_ci] = NULL;
        usb_device_pool[_ci].valid = 0;
    }
    usb_all_rings_init();
    usb_hub_reset_state();
    usb_root_ports_reset_state();

    delay_ms(500);
    usb_debug("USB: scan EHCI/UHCI/XHCI", 0, 0);
    usb_log_port_table_header();

    struct xhci_driver *xhci = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
    if (xhci && xhci->get_controller_count) {
        int x_cnt = xhci->get_controller_count();
        for (int i = 0; i < x_cnt; i++) {
            struct xhci_controller *ctrl = xhci->get_controller(i);
            if (!ctrl) continue;
            struct root_hub hub;
            root_hub_init(&hub, USB_TYPE_XHCI, ctrl);
            for (uint8_t port = 1; port <= hub.port_count; port++) {
                uint8_t family, model, stepping;
                char vendor[13];
                cpuid_get_family_model(&family, &model, &stepping);
                cpuid_get_vendor(vendor);

                if (strcmp(vendor, "GenuineIntel") == 0 && family == 6 && model == 92 && stepping == 10)
                {

                };

                    uint32_t pst = root_hub_port_status(&hub, port);
                    if (pst & 0x01) {

                        usb_logrow_begin(USB_LOG_XHCI, USB_LOG_TRACE);
                        usb_logrow_str("pre-reset  USBSTS=");
                        usb_logrow_hex32(xhci_read_current_usbsts());
                        usb_logrow_end();

                        if (root_hub_port_reset(&hub, port)) {

                            usb_root_port_mark(s_root_xhci, i, port, USB_ROOT_PORT_BUSY);
                            usb_init_device(ctrl, port, true);

                            int found = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                            uint32_t post = root_hub_port_status(&hub, port);
                            usb_root_port_mark(s_root_xhci, i, port, (int16_t)found);
                            usb_log_port_event(USB_LOG_XHCI, i, port,
                                (found >= 0) ? "connect" : "enum-failed",
                                post, (uint8_t)((post >> 10) & 0xFu));
                        } else {
                            uint32_t post = root_hub_port_status(&hub, port);
                            usb_log_port_event(USB_LOG_XHCI, i, port, "reset-failed",
                                post, (uint8_t)((post >> 10) & 0xFu));
                        }

                        usb_logrow_begin(USB_LOG_XHCI, USB_LOG_TRACE);
                        usb_logrow_str("post-reset USBSTS=");
                        usb_logrow_hex32(xhci_read_current_usbsts());
                        usb_logrow_end();
                    }
            }
        }
    }

    struct ehci_driver *ehci = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
    if (ehci && ehci->get_controller_count) {
        int cnt = ehci->get_controller_count();
        for (int i = 0; i < cnt; i++) {
            struct ehci_controller *ctrl = ehci->get_controller(i);
            if (!ctrl) continue;
            struct root_hub hub;
            root_hub_init(&hub, USB_TYPE_EHCI, ctrl);
            for (uint8_t port = 1; port <= hub.port_count; port++) {
                uint32_t pst = root_hub_port_status(&hub, port);
                if (!(pst & 0x01)) continue;

                if (pst & (1u << 13)) {
                    root_hub_exec(&hub, HUB_CMD_PORT_OWNER_CLEAR, port, 0);
                    delay_ms(50);
                    pst = root_hub_port_status(&hub, port);
                    if (!(pst & 0x01)) continue;
                }

                if (!(pst & 0x01)) {

                    uint32_t pp = (pst >> 12) & 0x1u;
                    uint32_t ls = (pst >> 10) & 0x3u;
                    if (!pp || ls == 0x0u) continue;
                    root_hub_port_reset(&hub, port);
                    delay_ms(100);
                    pst = root_hub_port_status(&hub, port);
                    if (!(pst & 0x01)) continue;
                }
                root_hub_port_reset(&hub, port);

                delay_ms(100);
                pst = root_hub_port_status(&hub, port);
                if (!(pst & 0x01)) {
                    usb_log_port_event(USB_LOG_EHCI, i, port, "lost-after-reset", pst, 0xFF);
                    continue;
                }
                if (!(pst & (1u << 2))) {

                    root_hub_exec(&hub, HUB_CMD_PORT_OWNER_SET, port, 0);
                    usb_log_port_event(USB_LOG_EHCI, i, port, "owner->companion", pst, 0xFF);
                    continue;
                }
                usb_root_port_mark(s_root_ehci, i, port, USB_ROOT_PORT_BUSY);
                usb_init_device(ctrl, port, false);
                int after_slot = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                usb_root_port_mark(s_root_ehci, i, port, (int16_t)after_slot);
                pst = root_hub_port_status(&hub, port);
                usb_log_port_event(USB_LOG_EHCI, i, port, "connect", pst, 0xFF);
            }
        }
    }

    struct ohci_driver *ohci = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
    if (ohci && ohci->get_controller_count) {
        int cnt = ohci->get_controller_count();
        for (int i = 0; i < cnt; i++) {
            struct ohci_controller *ctrl = ohci->get_controller(i);
            if (!ctrl) continue;
            struct root_hub hub;
            root_hub_init(&hub, USB_TYPE_OHCI, ctrl);
            for (uint8_t port = 1; port <= hub.port_count; port++) {

                uint32_t pst = root_hub_port_status(&hub, port);
                if (!(pst & 0x01)) continue;
                root_hub_port_reset(&hub, port);
                delay_ms(50);
                pst = root_hub_port_status(&hub, port);
                if (!(pst & 0x01)) {
                    usb_log_port_event(USB_LOG_OHCI, i, port, "lost-after-reset", pst, 0xFF);
                    continue;
                }
                usb_root_port_mark(s_root_ohci, i, port, USB_ROOT_PORT_BUSY);
                usb_init_device(ctrl, port, false);
                int after_slot = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                usb_root_port_mark(s_root_ohci, i, port, (int16_t)after_slot);
                pst = root_hub_port_status(&hub, port);
                usb_log_port_event(USB_LOG_OHCI, i, port, "connect", pst, 0xFF);
            }
        }
    }

    struct uhci_driver *uhci = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
    if (uhci && uhci->get_controller_count) {
        int cnt = uhci->get_controller_count();
        for (int i = 0; i < cnt; i++) {
            struct uhci_controller *ctrl = uhci->get_controller(i);
            if (!ctrl) continue;
            struct root_hub hub;
            root_hub_init(&hub, USB_TYPE_UHCI, ctrl);
            for (uint8_t port = 1; port <= hub.port_count; port++) {
                uint32_t pst = root_hub_port_status(&hub, port);
                if (!(pst & 0x01)) continue;
                root_hub_port_reset(&hub, port);

                for (int w = 0; w < 100; w++) {
                    pst = root_hub_port_status(&hub, port);
                    if (pst & 0x04) break;
                    delay_ms(1);
                }

                pst = root_hub_port_status(&hub, port);
                if (!(pst & 0x01)) {
                    usb_log_port_event(USB_LOG_UHCI, i, port, "lost-after-reset", pst, 0xFF);
                    continue;
                }

                usb_root_port_mark(s_root_uhci, i, port, USB_ROOT_PORT_BUSY);
                usb_init_device(ctrl, port, false);
                int after_slot = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                usb_root_port_mark(s_root_uhci, i, port, (int16_t)after_slot);
                pst = root_hub_port_status(&hub, port);
                usb_log_port_event(USB_LOG_UHCI, i, port, "connect", pst, 0xFF);
            }
        }
    }

}

static struct usb_core_driver core = {
    .scan_all = usb_scan_all,
    .control_transfer = usb_control_transfer,
    .interrupt_transfer = usb_interrupt_transfer,
    .enqueue_event = usb_core_enqueue_event,
    .poll_transfers = usb_core_poll_transfers,
    .bulk_transfer = usb_bulk_transfer,
    .iso_transfer = usb_iso_transfer,
    .reset_endpoint_toggle = usb_reset_endpoint_toggle,
};

struct usb_core_driver *return_usb_core_driver(void) {
    usb_scan_all();
    return &core;
}

struct driver *return_meta_usb_core_driver(void) {
    static struct driver meta = {
        .name = "USB Core Driver",
        .type = USB_DRIVER,
        .sub_type = USB_CORE_SLOT,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { NULL },
        .dependency_count = 0,
        .self = &core,
        .init = usb_scan_all,
    };
    return &meta;
}
