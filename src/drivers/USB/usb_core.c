#include "drivers/USB/usb_core.h"

#include "components/drivers.h"
#include "components/Interruptions/msi.h"
#include "components/logger.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/ehci.h"
#include "drivers/USB/ohci.h"
#include "drivers/USB/uhci.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core_internal.h"
#include "drivers/USB/usb_event.h"
#include "drivers/USB/usb_hub.h"
#include "drivers/USB/xhci.h"
#include "drivers/USB/xhci_hub.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/scheduler/scheduler.h"

#include <cpuid.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern unsigned int xhci_read_current_usbsts(void);

static struct usb_device usb_device_pool[MAX_USB_DEVICES];
static int usb_device_count = 0;

static uint8_t usb_next_address = 1;
static uint32_t usb_generation_counter = 0;

#define POLL_BUF_SIZE 8
static uint8_t s_poll_buf[MAX_USB_DEVICES][POLL_BUF_SIZE]
__attribute__((aligned(64)));
static bool s_poll_pending[MAX_USB_DEVICES];

#define POLL_BULK_BUF_SIZE 512
static uint8_t s_bulk_poll_buf[MAX_USB_DEVICES][POLL_BULK_BUF_SIZE]
__attribute__((aligned(64)));
static bool s_bulk_poll_pending[MAX_USB_DEVICES];
static bool s_iso_poll_pending[MAX_USB_DEVICES];
static uint8_t s_config_store[MAX_USB_DEVICES][USB_CONFIG_MAX] __attribute__((aligned(64)));

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
int usb_init_device_topo(void *ctrl_ptr, uint8_t port, bool is_xhci,
                           uint8_t root_port, uint8_t hub_depth,
                           uint32_t route_string, uint8_t parent_hub_slot,
                           uint8_t speed_id);
static int usb_bulk_transfer(struct usb_device *dev, uint8_t endpoint, void *data, uint16_t len, uint8_t direction);
static int usb_iso_open(struct usb_device *dev, const struct usb_endpoint_info *ep);
static int usb_iso_submit(struct usb_device *dev, struct usb_iso_request *req);
static void usb_iso_poll(struct usb_device *dev);
static void usb_iso_close(struct usb_device *dev, uint8_t endpoint);

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

static const char *const usb_port_log_names[USB_PORT_LOG_SOURCE_COUNT] = {
    [USB_PORT_LOG_HUB] = "hub_port",
    [USB_PORT_LOG_XHCI] = "xhci_port",
    [USB_PORT_LOG_EHCI] = "ehci_port",
    [USB_PORT_LOG_OHCI] = "ohci_port",
    [USB_PORT_LOG_UHCI] = "uhci_port",
};

static const char *usb_xhci_pls_name(uint32_t portsc) {
    switch ((portsc >> 5) & 0xFu) {
        case 0: return "U0";
        case 1: return "U1";
        case 2: return "U2";
        case 3: return "U3";
        case 4: return "Dis";
        case 5: return "RxD";
        case 7: return "Pol";
        case 9: return "Comp";
        case 15: return "Rsm";
        default: return "---";
    }
}

static const char *usb_xhci_speed_name(uint8_t speed_id) {
    switch (speed_id) {
        case 1: return "FS";
        case 2: return "LS";
        case 3: return "HS";
        case 4: return "SS";
        case 5: return "SS+";
        default: return "--";
    }
}

static void usb_port_change_append(char *buf, size_t cap, size_t *pos, const char *name) {
    if (*pos && *pos + 1 < cap) buf[(*pos)++] = ' ';
    while (*name && *pos + 1 < cap) buf[(*pos)++] = *name++;
    buf[*pos] = '\0';
}

void usb_log_port_event(enum usb_port_log_source source, enum logger_level_t level,
                        int ctrl_idx, uint8_t port, const char *event,
                        uint32_t portsc, uint8_t speed_id) {
    if ((unsigned)source >= USB_PORT_LOG_SOURCE_COUNT) source = USB_PORT_LOG_HUB;

    uint8_t ccs = 0, ped = 0;
    const char *pls = "---";
    const char *spd = "--";
    char changes[64];
    size_t n = 0;
    changes[0] = '\0';

    switch (source) {
        case USB_PORT_LOG_XHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 1)) ? 1 : 0;
            pls = usb_xhci_pls_name(portsc);
            spd = usb_xhci_speed_name(speed_id != 0xFF ? speed_id : (uint8_t)((portsc >> 10) & 0xFu));
            if (portsc & (1u << 17)) usb_port_change_append(changes, sizeof(changes), &n, "CSC");
            if (portsc & (1u << 18)) usb_port_change_append(changes, sizeof(changes), &n, "PEC");
            if (portsc & (1u << 19)) usb_port_change_append(changes, sizeof(changes), &n, "WRC");
            if (portsc & (1u << 20)) usb_port_change_append(changes, sizeof(changes), &n, "OCC");
            if (portsc & (1u << 21)) usb_port_change_append(changes, sizeof(changes), &n, "PRC");
            if (portsc & (1u << 22)) usb_port_change_append(changes, sizeof(changes), &n, "PLC");
            if (portsc & (1u << 23)) usb_port_change_append(changes, sizeof(changes), &n, "CEC");
            break;

        case USB_PORT_LOG_EHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 2)) ? 1 : 0;
            spd = ped ? "HS" : "FS/LS";
            if (portsc & (1u << 1)) usb_port_change_append(changes, sizeof(changes), &n, "CSC");
            if (portsc & (1u << 3)) usb_port_change_append(changes, sizeof(changes), &n, "PEC");
            if (portsc & (1u << 5)) usb_port_change_append(changes, sizeof(changes), &n, "OCC");
            if (portsc & (1u << 13)) usb_port_change_append(changes, sizeof(changes), &n, "OWNER=companion");
            break;

        case USB_PORT_LOG_OHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 1)) ? 1 : 0;
            spd = (portsc & (1u << 9)) ? "LS" : "FS";
            if (portsc & (1u << 16)) usb_port_change_append(changes, sizeof(changes), &n, "CSC");
            if (portsc & (1u << 17)) usb_port_change_append(changes, sizeof(changes), &n, "PESC");
            if (portsc & (1u << 18)) usb_port_change_append(changes, sizeof(changes), &n, "PSSC");
            if (portsc & (1u << 19)) usb_port_change_append(changes, sizeof(changes), &n, "OCIC");
            if (portsc & (1u << 20)) usb_port_change_append(changes, sizeof(changes), &n, "PRSC");
            break;

        case USB_PORT_LOG_UHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 2)) ? 1 : 0;
            spd = (portsc & (1u << 8)) ? "LS" : "FS";
            if (portsc & (1u << 1)) usb_port_change_append(changes, sizeof(changes), &n, "CSC");
            if (portsc & (1u << 3)) usb_port_change_append(changes, sizeof(changes), &n, "PEC");
            break;

        case USB_PORT_LOG_HUB: {
            uint16_t wstatus = (uint16_t)(portsc & 0xFFFFu);
            uint16_t wchange = (uint16_t)(portsc >> 16);
            ccs = (wstatus & 0x0001u) ? 1 : 0;
            ped = (wstatus & 0x0002u) ? 1 : 0;
            spd = (wstatus & 0x0400u) ? "HS" : (wstatus & 0x0200u) ? "LS" : "FS";
            if (wchange & 0x0001u) usb_port_change_append(changes, sizeof(changes), &n, "CSC");
            if (wchange & 0x0002u) usb_port_change_append(changes, sizeof(changes), &n, "PEC");
            if (wchange & 0x0004u) usb_port_change_append(changes, sizeof(changes), &n, "SUSP");
            if (wchange & 0x0008u) usb_port_change_append(changes, sizeof(changes), &n, "OCC");
            if (wchange & 0x0010u) usb_port_change_append(changes, sizeof(changes), &n, "PRC");
            break;
        }

        default:
            break;
    }

    logger_printf(level, usb_port_log_names[source],
                  "c%d p%02u %s CCS=%u PED=%u PLS=%s SPD=%s CHANGE=%s PORTSC=0x%08x",
                  ctrl_idx, (unsigned)port, event ? event : "?",
                  (unsigned)ccs, (unsigned)ped, pls, spd,
                  n ? changes : "-", portsc);
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

static int usb_iso_open(struct usb_device *dev, const struct usb_endpoint_info *ep)
{
    if (!dev || !dev->ctrl || !ep) return USB_ISO_ERR;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->iso_open) return drv->iso_open(dev, ep);
            break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->iso_open) return drv->iso_open(dev, ep);
            break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->iso_open) return drv->iso_open(dev, ep);
            break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->iso_open) return drv->iso_open(dev, ep);
            break;
        }
    }
    return USB_ISO_ERR;
}

static int usb_iso_submit(struct usb_device *dev, struct usb_iso_request *req)
{
    if (!dev || !dev->ctrl || !req || !req->n_packets || req->n_packets > USB_ISO_MAX_PACKETS) return USB_ISO_ERR;
    if (!dev->valid) return USB_ISO_NODEV;

    req->done = 0;
    req->completed = 0;
    req->errors = 0;
    req->status = USB_ISO_OK;
    for (uint16_t i = 0; i < req->n_packets; i++) req->actual[i] = 0;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->iso_submit) return drv->iso_submit(dev, req);
            break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->iso_submit) return drv->iso_submit(dev, req);
            break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->iso_submit) return drv->iso_submit(dev, req);
            break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->iso_submit) return drv->iso_submit(dev, req);
            break;
        }
    }
    return USB_ISO_ERR;
}

static void usb_iso_poll(struct usb_device *dev)
{
    if (!dev || !dev->ctrl) return;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->iso_poll) drv->iso_poll(dev);
            break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->iso_poll) drv->iso_poll(dev);
            break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->iso_poll) drv->iso_poll(dev);
            break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->iso_poll) drv->iso_poll(dev);
            break;
        }
    }
}

static void usb_iso_close(struct usb_device *dev, uint8_t endpoint)
{
    if (!dev || !dev->ctrl) return;

    switch (dev->ctrl->type) {
        case USB_TYPE_XHCI: {
            struct xhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
            if (drv && drv->iso_close) drv->iso_close(dev, endpoint);
            break;
        }
        case USB_TYPE_EHCI: {
            struct ehci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
            if (drv && drv->iso_close) drv->iso_close(dev, endpoint);
            break;
        }
        case USB_TYPE_OHCI: {
            struct ohci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
            if (drv && drv->iso_close) drv->iso_close(dev, endpoint);
            break;
        }
        case USB_TYPE_UHCI: {
            struct uhci_driver *drv = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
            if (drv && drv->iso_close) drv->iso_close(dev, endpoint);
            break;
        }
    }
}

void usb_iso_request_prepare(struct usb_iso_request *req, void *data, uint8_t endpoint, uint16_t n_packets, const uint16_t *lens)
{
    if (!req) return;
    if (n_packets > USB_ISO_MAX_PACKETS) n_packets = USB_ISO_MAX_PACKETS;
    req->data = data;
    req->endpoint = endpoint;
    req->n_packets = n_packets;
    uint32_t off = 0;
    for (uint16_t i = 0; i < n_packets; i++) {
        req->lens[i] = lens ? lens[i] : 0;
        req->offsets[i] = (uint16_t)off;
        req->actual[i] = 0;
        off += req->lens[i];
    }
    req->length = off;
    req->done = 0;
    req->queued = 0;
    req->completed = 0;
    req->errors = 0;
    req->status = USB_ISO_OK;
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
        if (dev->valid && dev->ctrl == ctrl_generic && dev->hub_depth == 0 && dev->port == port) return i;
    }
    return -1;
}

static void usb_root_port_mark(usb_root_port_state_t *table, int ci, uint8_t port, int16_t val) {
    if (ci < 0 || ci >= USB_ROOT_MAX_CTRL) return;
    if (port < 1 || port > USB_ROOT_MAX_PORTS) return;
    table[ci].slot[port] = val;
}

static void usb_root_ports_poll_one(enum usb_port_log_source log_source,
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

                logger_printf(LOGGER_LEVEL_WARNING, __func__,
                              "%s c%d p%u %s seen via CCS only (change bit lost)",
                              usb_port_log_names[log_source], ci, (unsigned)port,
                              connected_now ? "connect" : "disconnect");
            }

            bool connected = connected_now;
            int cur_slot = recorded;

            if (cur_slot == USB_ROOT_PORT_BUSY) {

                logger_printf(LOGGER_LEVEL_WARNING, __func__,
                              "%s c%d p%u skipped re-entrant hit (enumeration already in progress)",
                              usb_port_log_names[log_source], ci, (unsigned)port);
                continue;
            }

            if (is_xhci && connected && cur_slot >= 0 && !(pst & 0x2u)) {
                usb_log_port_event(log_source, LOGGER_LEVEL_INFO, ci, port, "port-disabled, re-enumerating", pst, 0xFF);
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
                    usb_log_port_event(log_source, LOGGER_LEVEL_INFO, ci, port, "adopt-existing", pst, 0xFF);
                } else if (root_hub_port_reset(hub, port)) {

                    st->slot[port] = USB_ROOT_PORT_BUSY;
                    usb_init_device(ctrl, port, is_xhci);

                    int found = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                    uint32_t post = root_hub_port_status(hub, port);
                    if (found >= 0) {
                        st->slot[port] = (int16_t)found;
                        st->mismatch_tries[port] = 0;
                        usb_log_port_event(log_source, LOGGER_LEVEL_INFO, ci, port, "connect", post, 0xFF);
                    } else {
                        st->slot[port] = -1;
                        usb_log_port_event(log_source, LOGGER_LEVEL_WARNING, ci, port, "enum-failed", post, 0xFF);
                    }
                } else if (is_ehci) {

                    root_hub_exec(hub, HUB_CMD_PORT_OWNER_SET, port, 0);
                    uint32_t post = root_hub_port_status(hub, port);
                    usb_log_port_event(log_source, LOGGER_LEVEL_INFO, ci, port, "owner->companion", post, 0xFF);
                } else {
                    uint32_t post = root_hub_port_status(hub, port);
                    usb_log_port_event(log_source, LOGGER_LEVEL_WARNING, ci, port, "reset-failed", post, 0xFF);
                }
            } else if (!connected && cur_slot >= 0) {

                usb_hub_detach(cur_slot);
                st->slot[port] = -1;
                st->mismatch_tries[port] = 0;
                usb_log_port_event(log_source, LOGGER_LEVEL_INFO, ci, port, "disconnect", pst, 0xFF);
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

        usb_root_ports_poll_one(USB_PORT_LOG_XHCI, s_root_xhci, cnt, adapt_get_xhci,
                                 HUB_TYPE_XHCI, true, false,
                                 (1u << 17) | (1u << 18) | (1u << 19));
    }

    struct ehci_driver *ehci = get_self_driver(USB_DRIVER, USB_TYPE_EHCI);
    if (ehci && ehci->get_controller_count) {
        int cnt = ehci->get_controller_count();

        usb_root_ports_poll_one(USB_PORT_LOG_EHCI, s_root_ehci, cnt, adapt_get_ehci,
                                 HUB_TYPE_EHCI, false, true,
                                 (1u << 1));
    }

    struct ohci_driver *ohci = get_self_driver(USB_DRIVER, USB_TYPE_OHCI);
    if (ohci && ohci->get_controller_count) {
        int cnt = ohci->get_controller_count();

        usb_root_ports_poll_one(USB_PORT_LOG_OHCI, s_root_ohci, cnt, adapt_get_ohci,
                                 HUB_TYPE_OHCI, false, false,
                                 (1u << 16));
    }

    struct uhci_driver *uhci = get_self_driver(USB_DRIVER, USB_TYPE_UHCI);
    if (uhci && uhci->get_controller_count) {
        int cnt = uhci->get_controller_count();

        usb_root_ports_poll_one(USB_PORT_LOG_UHCI, s_root_uhci, cnt, adapt_get_uhci,
                                 HUB_TYPE_UHCI, false, false,
                                 (1u << 1));
    }
}

static spinlock_t usb_pump_lock = SPINLOCK_INIT;
static volatile uint64_t usb_pump_last_ms = 0;
static void *usb_pump_owner = NULL;
static int usb_pump_depth = 0;

static bool usb_pump_enter(void) {
    void *me = (void *)current_task();

    uint64_t flags = spin_lock_irqsave(&usb_pump_lock);

    if (usb_pump_owner == NULL) {
        usb_pump_owner = me ? me : (void *)&usb_pump_depth;
        usb_pump_depth = 1;
        spin_unlock_irqrestore(&usb_pump_lock, flags);
        return true;
    }

    if (me && usb_pump_owner == me) {
        usb_pump_depth++;
        spin_unlock_irqrestore(&usb_pump_lock, flags);
        return true;
    }

    spin_unlock_irqrestore(&usb_pump_lock, flags);
    return false;
}

static void usb_pump_leave(void) {
    uint64_t flags = spin_lock_irqsave(&usb_pump_lock);
    if (usb_pump_depth > 0) usb_pump_depth--;
    if (usb_pump_depth == 0) usb_pump_owner = NULL;
    spin_unlock_irqrestore(&usb_pump_lock, flags);
}

static void usb_core_poll_transfers_locked(void) {
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

            asm volatile("pause");
    }

    usb_event_dispatch_all();
    usb_bulk_dispatch_all();
    usb_iso_dispatch_all();
}

static void usb_core_poll_topology_locked(void) {
    usb_root_ports_poll();
    usb_hub_poll();

    usb_event_dispatch_all();
}

static uint64_t usb_pump_now_ms(void) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return tsc ? tsc->get_tsc_uptime_ms() : 0;
}

bool usb_core_claim(uint32_t timeout_ms) {
    for (uint32_t waited = 0;; waited++) {
        if (usb_pump_enter()) return true;
        if (waited >= timeout_ms) return false;
        delay_ms(1);
    }
}

void usb_core_release(void) {
    usb_pump_leave();
}

uint64_t usb_core_pump_age_ms(void) {
    uint64_t last = usb_pump_last_ms;
    if (last == 0) return UINT64_MAX;

    uint64_t now = usb_pump_now_ms();
    return (now > last) ? (now - last) : 0;
}

static void usb_core_poll_transfers(void) {
    if (!usb_pump_enter()) return;
    usb_core_poll_transfers_locked();
    usb_pump_last_ms = usb_pump_now_ms();
    usb_pump_leave();
}

void usb_core_poll_topology(void) {
    if (!usb_pump_enter()) return;
    usb_core_poll_topology_locked();
    usb_pump_last_ms = usb_pump_now_ms();
    usb_pump_leave();
}

static int usb_validate_device_descriptor(struct usb_device_descriptor *desc) {
    if (desc->bLength != 18) return -1;
    if (desc->bDescriptorType != 0x01) return -1;
    uint8_t mps = desc->bMaxPacketSize0;

    if (mps != 8 && mps != 9 && mps != 16 && mps != 32 && mps != 64) return -1;
    if (desc->bcdUSB == 0) return -1;
    return 0;
}

static const char *usb_audio_sub_name(uint8_t sub) {
    switch (sub) {
        case 0x01: return "Audio Control";
        case 0x02: return "Audio Streaming";
        case 0x03: return "MIDI Streaming";
        default: return "Audio";
    }
}

const char *usb_class_name(uint8_t cls, uint8_t subclass, uint8_t protocol) {
    switch (cls) {
        case 0x00: return "Per-interface";
        case 0x01: return usb_audio_sub_name(subclass);
        case 0x02:
            if (subclass == 0x02) return "CDC ACM";
            if (subclass == 0x06) return "CDC Ethernet";
            if (subclass == 0x0D) return "CDC NCM";
            if (subclass == 0x0E) return "CDC MBIM";
            return "CDC Control";
        case 0x03:
            if (subclass == 0x01 && protocol == 0x01) return "HID Keyboard";
            if (subclass == 0x01 && protocol == 0x02) return "HID Mouse";
            return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Still Image";
        case 0x07: return "Printer";
        case 0x08:
            if (protocol == 0x62) return "Mass Storage (UAS)";
            return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC Data";
        case 0x0B: return "Smart Card";
        case 0x0D: return "Content Security";
        case 0x0E:
            if (subclass == 0x01) return "Video Control";
            if (subclass == 0x02) return "Video Streaming";
            return "Video";
        case 0x0F: return "Personal Healthcare";
        case 0x10: return "Audio/Video";
        case 0x11: return "Billboard";
        case 0x12: return "Type-C Bridge";
        case 0xDC: return "Diagnostic";
        case 0xE0:
            if (subclass == 0x01 && protocol == 0x01) return "Bluetooth";
            if (subclass == 0x01 && protocol == 0x03) return "RNDIS";
            return "Wireless";
        case 0xEF:
            if (subclass == 0x02 && protocol == 0x01) return "Composite (IAD)";
            return "Miscellaneous";
        case 0xFE:
            if (subclass == 0x01) return "DFU";
            if (subclass == 0x02) return "IrDA Bridge";
            if (subclass == 0x03) return "Test & Measurement";
            return "Application Specific";
        case 0xFF: return "Vendor Specific";
        default: return "Unknown";
    }
}

static const char *usb_function_class_name(const struct usb_function_info *f) {
    if (f->func_class == 0x01) return f->func_subclass == 0x03 ? "MIDI" : "Audio";
    if (f->func_class == 0x0E) return "Video";
    if (f->func_class == 0x02 && f->interface_count > 1) return usb_class_name(0x02, f->func_subclass, f->func_protocol);
    return usb_class_name(f->func_class, f->func_subclass, f->func_protocol);
}

struct usb_interface_info *usb_find_interface(struct usb_device *dev, uint8_t number) {
    if (!dev) return NULL;
    for (uint8_t i = 0; i < dev->interface_count; i++)
        if (dev->interfaces[i].number == number) return &dev->interfaces[i];
    return NULL;
}

struct usb_function_info *usb_find_function(struct usb_device *dev, uint8_t cls, int *iter) {
    if (!dev) return NULL;
    int start = iter ? *iter : 0;
    for (int i = start; i < dev->function_count; i++) {
        struct usb_function_info *f = &dev->functions[i];
        bool match = (f->func_class == cls);
        if (!match && !f->from_iad) {
            for (uint8_t k = 0; k < dev->interface_count && !match; k++)
                if (dev->interfaces[k].function == i && dev->interfaces[k].iface_class == cls) match = true;
        }
        if (match) {
            if (iter) *iter = i + 1;
            return f;
        }
    }
    if (iter) *iter = dev->function_count;
    return NULL;
}

static spinlock_t usb_claim_lock = SPINLOCK_INIT;

bool usb_function_claim(struct usb_device *dev, struct usb_function_info *fn, const char *driver) {
    if (!dev || !fn) return false;
    uint64_t flags = spin_lock_irqsave(&usb_claim_lock);
    bool ok = (fn->driver == NULL || fn->driver == driver);
    if (ok) fn->driver = driver;
    spin_unlock_irqrestore(&usb_claim_lock, flags);
    return ok;
}

void usb_function_release(struct usb_device *dev, struct usb_function_info *fn, const char *driver) {
    if (!dev || !fn) return;
    uint64_t flags = spin_lock_irqsave(&usb_claim_lock);
    if (fn->driver == driver) fn->driver = NULL;
    spin_unlock_irqrestore(&usb_claim_lock, flags);
}

bool usb_device_alive(struct usb_device *dev) {
    if (!dev || !dev->valid) return false;
    for (int i = 0; i < MAX_USB_DEVICES; i++)
        if (device_table[USB_DEVICE][i] == (void *)dev) return true;
    return false;
}

static struct usb_interface_info *usb_iface_slot(struct usb_device *dev, uint8_t number) {
    struct usb_interface_info *it = usb_find_interface(dev, number);
    if (it) return it;
    if (dev->interface_count >= USB_MAX_INTERFACES) return NULL;
    it = &dev->interfaces[dev->interface_count++];
    memset(it, 0, sizeof(*it));
    it->number = number;
    it->function = 0xFF;
    return it;
}

static int usb_new_function(struct usb_device *dev, uint8_t cls, uint8_t sub, uint8_t proto, uint8_t iad, uint8_t str) {
    if (dev->function_count >= USB_MAX_FUNCTIONS) return -1;
    struct usb_function_info *f = &dev->functions[dev->function_count];
    memset(f, 0, sizeof(*f));
    f->first_interface = 0xFF;
    f->func_class = cls;
    f->func_subclass = sub;
    f->func_protocol = proto;
    f->from_iad = iad;
    f->string_idx = str;
    return dev->function_count++;
}

static void usb_function_add_iface(struct usb_device *dev, int fi, struct usb_interface_info *it) {
    if (fi < 0 || !it || it->function != 0xFF) return;
    struct usb_function_info *f = &dev->functions[fi];
    it->function = (uint8_t)fi;
    if (it->number < 32) f->iface_mask |= (1u << it->number);
    if (f->first_interface == 0xFF || it->number < f->first_interface) f->first_interface = it->number;
    f->interface_count++;
}

static void usb_parse_functions(struct usb_device *dev, const uint8_t *cfg, uint16_t total_len)
{
    const uint8_t *p = cfg;
    const uint8_t *end = cfg + total_len;
    struct usb_interface_info *cur = NULL;
    uint8_t cur_alt = 0;
    uint8_t order[USB_MAX_INTERFACES];
    uint8_t order_count = 0;
    uint8_t groups[USB_MAX_INTERFACES][2];
    uint32_t group_mask[USB_MAX_INTERFACES];
    uint8_t group_count = 0;

    dev->interface_count = 0;
    dev->function_count = 0;

    while (p + 2 <= end) {
        uint8_t dl = p[0];
        uint8_t dt = p[1];
        if (dl < 2 || p + dl > end) break;

        if (dt == USB_DESC_IAD && dl >= 8) {
            int fi = usb_new_function(dev, p[4], p[5], p[6], 1, p[7]);
            if (fi >= 0) {
                struct usb_function_info *f = &dev->functions[fi];
                f->first_interface = p[2];
                for (uint8_t k = 0; k < p[3] && p[2] + k < 32; k++) f->iface_mask |= (1u << (p[2] + k));
            }
        } else if (dt == USB_DESC_INTERFACE && dl >= 9) {
            cur = usb_iface_slot(dev, p[2]);
            cur_alt = p[3];
            if (cur) {
                if (cur->alt_count == 0 && order_count < USB_MAX_INTERFACES) order[order_count++] = p[2];
                cur->alt_count++;
                if (cur_alt == 0 || cur->alt_count == 1) {
                    cur->iface_class = p[5];
                    cur->iface_subclass = p[6];
                    cur->iface_protocol = p[7];
                    cur->ep_count = p[4];
                    cur->string_idx = p[8];
                }
                if (p[4] > cur->max_ep_count) cur->max_ep_count = p[4];
            }
        } else if (dt == USB_DESC_ENDPOINT && dl >= 7 && cur) {
            if ((p[3] & 0x03u) == USB_EP_XFER_ISO) cur->has_iso = 1;
        } else if (dt == USB_DESC_CS_INTERFACE && dl >= 3 && cur && group_count < USB_MAX_INTERFACES) {
            uint32_t mask = 0;
            if (cur->iface_class == 0x01 && cur->iface_subclass == 0x01 && p[2] == 0x01 &&
                cur->iface_protocol != 0x20 && dl >= 8) {
                uint8_t n = p[7];
                for (uint8_t k = 0; k < n && 8 + k < dl; k++)
                    if (p[8 + k] < 32) mask |= (1u << p[8 + k]);
            } else if (cur->iface_class == 0x02 && p[2] == 0x06 && dl >= 5) {
                for (uint8_t k = 4; k < dl; k++)
                    if (p[k] < 32) mask |= (1u << p[k]);
            }
            if (mask) {
                groups[group_count][0] = cur->number;
                groups[group_count][1] = 0;
                group_mask[group_count] = mask;
                group_count++;
            }
        }
        p += dl;
    }

    for (int fi = 0; fi < dev->function_count; fi++) {
        struct usb_function_info *f = &dev->functions[fi];
        uint32_t want = f->iface_mask;
        f->iface_mask = 0;
        f->first_interface = 0xFF;
        f->interface_count = 0;
        for (uint8_t k = 0; k < order_count; k++) {
            uint8_t n = order[k];
            if (n < 32 && (want & (1u << n))) usb_function_add_iface(dev, fi, usb_find_interface(dev, n));
        }
    }

    int last_audio = -1;
    for (uint8_t k = 0; k < order_count; k++) {
        struct usb_interface_info *it = usb_find_interface(dev, order[k]);
        if (!it) continue;
        if (it->function != 0xFF) {
            last_audio = (dev->functions[it->function].func_class == 0x01) ? it->function : -1;
            continue;
        }

        int g = -1;
        for (uint8_t gi = 0; gi < group_count; gi++)
            if (groups[gi][0] == it->number) g = gi;

        if (g >= 0) {
            int fi = usb_new_function(dev, it->iface_class, it->iface_subclass, it->iface_protocol, 0, it->string_idx);
            usb_function_add_iface(dev, fi, it);
            for (uint8_t j = 0; j < order_count; j++) {
                uint8_t n = order[j];
                if (n < 32 && (group_mask[g] & (1u << n))) usb_function_add_iface(dev, fi, usb_find_interface(dev, n));
            }
            last_audio = (it->iface_class == 0x01) ? fi : -1;
            continue;
        }

        if (it->iface_class == 0x01 && it->iface_subclass != 0x01 && last_audio >= 0) {
            usb_function_add_iface(dev, last_audio, it);
            continue;
        }

        int fi = usb_new_function(dev, it->iface_class, it->iface_subclass, it->iface_protocol, 0, it->string_idx);
        usb_function_add_iface(dev, fi, it);
        last_audio = (it->iface_class == 0x01 && it->iface_subclass == 0x01) ? fi : -1;
    }

    for (int fi = 0; fi < dev->function_count; fi++) {
        struct usb_function_info *f = &dev->functions[fi];
        if (!f->from_iad && f->interface_count > 0) {
            struct usb_interface_info *first = usb_find_interface(dev, f->first_interface);
            if (first && first->iface_class == 0x01) f->func_subclass = 0x00;
        }
        if (f->first_interface == 0xFF) f->first_interface = 0;
    }
}

static void usb_log_functions(struct usb_device *dev) {
    for (int fi = 0; fi < dev->function_count; fi++) {
        struct usb_function_info *f = &dev->functions[fi];
        char ifs[64];
        size_t pos = 0;
        ifs[0] = 0;
        for (uint8_t k = 0; k < dev->interface_count && pos + 4 < sizeof(ifs); k++) {
            if (dev->interfaces[k].function != fi) continue;
            uint8_t n = dev->interfaces[k].number;
            if (pos) ifs[pos++] = ',';
            if (n >= 10) ifs[pos++] = (char)('0' + n / 10);
            ifs[pos++] = (char)('0' + n % 10);
            ifs[pos] = 0;
        }
        LOG_INFO("addr %u function %d: %s (class 0x%02x/0x%02x/0x%02x%s) interfaces %s",
                 (unsigned)dev->address, fi, usb_function_class_name(f), (unsigned)f->func_class,
                 (unsigned)f->func_subclass, (unsigned)f->func_protocol, f->from_iad ? ", IAD" : "", ifs);
    }
}

const char *usb_function_name(const struct usb_function_info *f) {
    return f ? usb_function_class_name(f) : "";
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

            LOG_DEBUG("interface %u: class=0x%02x sub=0x%02x proto=0x%02x hid_boot=%d",
                      (unsigned)cur_interface, (unsigned)i_class, (unsigned)i_subclass,
                      (unsigned)i_protocol, (int)in_hid);
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

                if (ep_xfer == USB_EP_XFER_ISO) {
                    struct usb_endpoint_info *iep = NULL;
                    for (uint8_t k = 0; k < dev->iso_ep_count; k++)
                        if (dev->iso_ep[k].address == ep_addr) iep = &dev->iso_ep[k];
                    if (!iep && dev->iso_ep_count < USB_MAX_ISO_EP) {
                        iep = &dev->iso_ep[dev->iso_ep_count++];
                        iep->address = ep_addr;
                        iep->max_packet_size = 0;
                    }
                    if (iep && (mps & 0x7FFu) >= (iep->max_packet_size & 0x7FFu)) {
                        iep->attributes = ep_attr;
                        iep->max_packet_size = mps ? mps : 1023u;
                        iep->interval = (ptr + 6 < end) ? ptr[6] : 1;
                    }
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

static int usb_find_parent_hub_slot(void *ctrl_ptr, uint8_t hub_address) {
    if (hub_address == 0) return -1;
    for (int i = 0; i < usb_device_count; i++) {
        struct usb_device *d = &usb_device_pool[i];
        if (d->valid && d->ctrl == (struct usb_controller *)ctrl_ptr &&
            d->device_class == 0x09 && d->address == hub_address)
            return i;
    }
    return -1;
}

static uint16_t usb_xhci_default_ep0_mps(uint8_t speed) {
    switch (speed) {
        case 2: return 8;
        case 1:
        case 3: return 64;
        default: return 512;
    }
}

int usb_init_device_topo(void *ctrl_ptr, uint8_t port, bool is_xhci,
                           uint8_t root_port, uint8_t hub_depth,
                           uint32_t route_string, uint8_t parent_hub_slot,
                           uint8_t speed_id) {
    int slot = usb_reserve_slot();
    if (slot < 0) return -1;
    struct usb_device *dev = &usb_device_pool[slot];

    uint8_t *p = (uint8_t *)dev;
    for (size_t i = 0; i < sizeof(struct usb_device); i++) p[i] = 0;
    dev->valid = 1;
    dev->parent_slot = -1;
    dev->generation = ++usb_generation_counter;

    dev->port = port;
    dev->ctrl = (struct usb_controller *)ctrl_ptr;
    dev->root_port = root_port;
    dev->hub_depth = hub_depth;
    dev->speed_id = speed_id;

    int parent_idx = (hub_depth > 0) ? usb_find_parent_hub_slot(ctrl_ptr, parent_hub_slot) : -1;
    if (parent_idx >= 0) {
        dev->parent_slot = (int16_t)parent_idx;
        dev->parent_port = port;
    }

    if (hub_depth > 0 && (speed_id == 1 || speed_id == 2) && parent_idx >= 0) {
        struct usb_device *parent = &usb_device_pool[parent_idx];
        if (parent->speed_id == 3) {
            dev->tt_hub_slot = parent->address;
            dev->tt_port = port;
        } else {
            dev->tt_hub_slot = parent->tt_hub_slot;
            dev->tt_port = parent->tt_port;
        }
    }
    LOG_DEBUG("enumerating %s port %u (root %u, depth %u, route 0x%05x, parent slot %u)",
              is_xhci ? "xHCI" : "legacy", (unsigned)port, (unsigned)root_port,
              (unsigned)hub_depth, route_string, (unsigned)parent_hub_slot);

    if (is_xhci) {
        struct xhci_driver *x_drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
        int slot_id = x_drv->enable_slot((struct xhci_controller *)ctrl_ptr);
        if (slot_id <= 0) {
            LOG_ERROR("port %u: xHCI Enable Slot failed (%d)", (unsigned)port, slot_id);
            goto fail;
        }

        uint32_t my_route = route_string;
        if (hub_depth > 0 && hub_depth <= 5)
            my_route |= ((uint32_t)(port & 0xFu) << (4 * (hub_depth - 1)));
        dev->route_string = my_route;

        uint8_t dev_speed = speed_id;
        if (hub_depth == 0) {
            struct xhci_hub rh = { .ctrl = (struct xhci_controller *)ctrl_ptr };
            uint32_t ps = xhci_hub_exec(&rh, HUB_CMD_PORT_STATUS, root_port, 0);
            dev_speed = (uint8_t)((ps >> 10) & 0xFu);
        }
        if (dev_speed == 0) dev_speed = 1;
        dev->speed_id = dev_speed;

        struct xhci_topology topo = {
            .root_port = root_port,
            .route_string = my_route,
            .parent_hub_slot = parent_hub_slot,
            .parent_port = (parent_hub_slot != 0) ? port : 0,
            .speed_id = speed_id,
            .tt_hub_slot = dev->tt_hub_slot,
            .tt_port = dev->tt_port,
        };

        if (hub_depth > 0)
            LOG_DEBUG("slot %d: route 0x%05x, speed %u, TT hub slot %u port %u",
                      slot_id, my_route, (unsigned)dev_speed,
                      (unsigned)dev->tt_hub_slot, (unsigned)dev->tt_port);

        if (x_drv->address_device((struct xhci_controller *)ctrl_ptr,
            slot_id, &topo) != 0) {
            LOG_ERROR("port %u: xHCI Address Device failed for slot %d", (unsigned)port, slot_id);

            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }
        dev->address = slot_id;
        delay_ms(100);

        static uint8_t desc_tmp[64] __attribute__((aligned(64)));
        for (int i = 0; i < 64; i++) desc_tmp[i] = 0;
        struct usb_setup_packet setup8 = {0x80, 0x06, 0x0100, 0, 8};
        if (x_drv->control_transfer((struct xhci_controller *)dev->ctrl,
            dev->address, 0, &setup8, 8, desc_tmp, 8, 1) != 0) {
            LOG_ERROR("slot %d: GET_DESCRIPTOR(device, 8) failed", slot_id);
            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }

        uint16_t cur_mps0 = usb_xhci_default_ep0_mps(dev_speed);
        uint16_t want_mps0 = cur_mps0;
        if (dev_speed >= 4) {
            if (desc_tmp[7] >= 3 && desc_tmp[7] <= 15) want_mps0 = (uint16_t)(1u << desc_tmp[7]);
        } else if (dev_speed == 1) {
            if (desc_tmp[7] == 8 || desc_tmp[7] == 16 || desc_tmp[7] == 32 || desc_tmp[7] == 64)
                want_mps0 = desc_tmp[7];
        }
        if (want_mps0 != cur_mps0) {
            if (!x_drv->update_ep0_mps ||
                x_drv->update_ep0_mps((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id, want_mps0) != 0) {
                LOG_ERROR("slot %d: could not set EP0 max packet size %u", slot_id, (unsigned)want_mps0);
                if (x_drv->disable_slot)
                    x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
                goto fail;
            }
        }

        for (int i = 0; i < 64; i++) desc_tmp[i] = 0;
        struct usb_setup_packet setup = {0x80, 0x06, 0x0100, 0, 18};
        if (x_drv->control_transfer((struct xhci_controller *)dev->ctrl,
            dev->address, 0, &setup, 8, desc_tmp, 18, 1) != 0) {
            LOG_ERROR("slot %d: GET_DESCRIPTOR(device) failed", slot_id);
            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }
        for (int i = 0; i < 18; i++) ((uint8_t *)&dev->desc)[i] = desc_tmp[i];

        if (usb_validate_device_descriptor(&dev->desc) != 0) {
            LOG_ERROR("slot %d: invalid device descriptor (bLength=%u mps0=%u)",
                      slot_id, (unsigned)dev->desc.bLength, (unsigned)dev->desc.bMaxPacketSize0);
            if (x_drv->disable_slot)
                x_drv->disable_slot((struct xhci_controller *)ctrl_ptr, (uint8_t)slot_id);
            goto fail;
        }

        LOG_DEBUG("slot %d: VID:PID=%04x:%04x bcdUSB=%04x mps0=%u",
                  slot_id, (unsigned)dev->desc.idVendor, (unsigned)dev->desc.idProduct,
                  (unsigned)dev->desc.bcdUSB, (unsigned)dev->desc.bMaxPacketSize0);
    } else {
        dev->address = 0;

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_UHCI) {
            int ki = uhci_controller_index((struct uhci_controller *)dev->ctrl);
            if (ki >= 0) {
                if (hub_depth > 0) uhci_dev_is_ls[ki][0] = (speed_id == 2) ? 1 : 0;
                else dev->speed_id = uhci_dev_is_ls[ki][0] ? 2 : 1;
            }
        }

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_OHCI) {
            int ki = ohci_controller_index((struct ohci_controller *)dev->ctrl);
            if (ki >= 0) {
                if (hub_depth > 0) ohci_dev_is_ls[ki][0] = (speed_id == 2) ? 1 : 0;
                else dev->speed_id = ohci_dev_is_ls[ki][0] ? 2 : 1;
            }
        }

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_EHCI) {
            int ki = ehci_controller_index((struct ehci_controller *)dev->ctrl);
            if (hub_depth == 0 || dev->speed_id == 0) dev->speed_id = 3;
            if (ki >= 0) {
                ehci_dev_speed[ki][0] = dev->speed_id;
                ehci_dev_tt_hub[ki][0] = dev->tt_hub_slot;
                ehci_dev_tt_port[ki][0] = dev->tt_port;
                ehci_dev_mps0[ki][0] = 0;
            }
            if (hub_depth > 0)
                LOG_DEBUG("ehci child port %u: speed %u, TT hub addr %u port %u",
                          (unsigned)port, (unsigned)dev->speed_id,
                          (unsigned)dev->tt_hub_slot, (unsigned)dev->tt_port);
        }

        static uint8_t b8[8] __attribute__((aligned(64)));
        for (int i = 0; i < 8; i++) b8[i] = 0;

        int got_desc = 0;
        int last_xfer_ret = 0;
        for (int retry = 0; retry < 5; retry++) {
            for (int i = 0; i < 8; i++) b8[i] = 0;
            last_xfer_ret = usb_control_transfer(dev, 0x80, 0x06, 0x0100, 0, 8, b8);
            LOG_DEBUG("port %u: GET_DESCRIPTOR(8) try %d ret=%d len=%u type=%u mps0=%u",
                      (unsigned)port, retry, last_xfer_ret, (unsigned)b8[0], (unsigned)b8[1], (unsigned)b8[7]);
            if (last_xfer_ret == 0) {
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
            LOG_ERROR("port %u: no valid device descriptor after 5 tries (ret=%d len=%u type=%u)",
                      (unsigned)port, last_xfer_ret, (unsigned)b8[0], (unsigned)b8[1]);
            goto fail;
        }

        delay_ms(20);
        if (usb_next_address > 127) {
            LOG_ERROR("port %u: out of USB addresses", (unsigned)port);
            goto fail;
        }
        uint8_t new_addr = usb_next_address++;
        LOG_DEBUG("port %u: SET_ADDRESS %u", (unsigned)port, (unsigned)new_addr);
        int sa_ret = usb_control_transfer(dev, 0x00, 0x05, new_addr, 0, 0, NULL);
        if (sa_ret != 0) {
            LOG_ERROR("port %u: SET_ADDRESS %u failed (%d)", (unsigned)port, (unsigned)new_addr, sa_ret);
            goto fail;
        }
        dev->address = new_addr;
        delay_ms(50);

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_UHCI) {
            int ki = uhci_controller_index((struct uhci_controller *)dev->ctrl);
            if (ki >= 0) {
                dev->is_low_speed = uhci_dev_is_ls[ki][0];
                uhci_dev_is_ls[ki][new_addr & 0x7F] = uhci_dev_is_ls[ki][0];
                uhci_dev_mps0[ki][new_addr & 0x7F] = (uint8_t)dev->max_packet_size;
                LOG_DEBUG("uhci%d addr %u: low_speed=%u mps0=%u",
                          ki, (unsigned)new_addr, (unsigned)uhci_dev_is_ls[ki][0], (unsigned)dev->max_packet_size);
            }
        }

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_EHCI) {
            int ki = ehci_controller_index((struct ehci_controller *)dev->ctrl);
            if (ki >= 0) {
                dev->is_low_speed = (dev->speed_id == 2) ? 1 : 0;
                ehci_dev_speed[ki][new_addr & 0x7F] = ehci_dev_speed[ki][0];
                ehci_dev_tt_hub[ki][new_addr & 0x7F] = ehci_dev_tt_hub[ki][0];
                ehci_dev_tt_port[ki][new_addr & 0x7F] = ehci_dev_tt_port[ki][0];
                ehci_dev_mps0[ki][new_addr & 0x7F] = (uint8_t)dev->max_packet_size;
            }
        }

        if (dev->ctrl && dev->ctrl->type == USB_TYPE_OHCI) {
            int ki = ohci_controller_index((struct ohci_controller *)dev->ctrl);
            if (ki >= 0) {
                dev->is_low_speed = ohci_dev_is_ls[ki][0];
                ohci_dev_is_ls[ki][new_addr & 0x7F] = ohci_dev_is_ls[ki][0];
                ohci_dev_mps0[ki][new_addr & 0x7F] = (uint8_t)dev->max_packet_size;
                LOG_DEBUG("ohci%d addr %u: low_speed=%u mps0=%u",
                          ki, (unsigned)new_addr, (unsigned)ohci_dev_is_ls[ki][0], (unsigned)dev->max_packet_size);
            }
        }

        static uint8_t desc_full[18] __attribute__((aligned(64)));
        for (int i = 0; i < 18; i++) desc_full[i] = 0;
        int gd_ret = usb_control_transfer(dev, 0x80, 0x06, 0x0100, 0, 18, desc_full);
        if (gd_ret != 0) {
            LOG_ERROR("addr %u: GET_DESCRIPTOR(18) failed (%d)", (unsigned)dev->address, gd_ret);
            goto fail;
        }
        for (int i = 0; i < 18; i++) ((uint8_t *)&dev->desc)[i] = desc_full[i];
        if (usb_validate_device_descriptor(&dev->desc) != 0) {
            LOG_ERROR("addr %u: invalid device descriptor (bLength=%u mps0=%u)",
                      (unsigned)dev->address, (unsigned)dev->desc.bLength, (unsigned)dev->desc.bMaxPacketSize0);
            goto fail;
        }

        LOG_DEBUG("addr %u: VID:PID=%04x:%04x bcdUSB=%04x mps0=%u",
                  (unsigned)dev->address, (unsigned)dev->desc.idVendor, (unsigned)dev->desc.idProduct,
                  (unsigned)dev->desc.bcdUSB, (unsigned)dev->desc.bMaxPacketSize0);
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

    static uint8_t cfg_desc[USB_CONFIG_MAX] __attribute__((aligned(4096)));
    for (int i = 0; i < 9; i++) cfg_desc[i] = 0;

    int cfg9_ret = -1;
    for (int _r = 0; _r < 3 && cfg9_ret != 0; _r++) {
        if (_r > 0) delay_ms(20);
        for (int i = 0; i < 9; i++) cfg_desc[i] = 0;
        cfg9_ret = usb_control_transfer(dev, 0x80, 0x06, (0x02 << 8), 0, 9, cfg_desc);
    }
    if (cfg9_ret != 0)
        LOG_WARNING("addr %u: GET_DESCRIPTOR(config, 9) failed (%d)", (unsigned)dev->address, cfg9_ret);
    uint8_t cfg_value_hdr = cfg_desc[5];
    if (cfg9_ret == 0) {
        uint16_t total_len = cfg_desc[2] | ((uint16_t)cfg_desc[3] << 8);
        LOG_DEBUG("addr %u: configuration total length %u", (unsigned)dev->address, (unsigned)total_len);
        if (total_len > USB_CONFIG_MAX) {
            LOG_WARNING("addr %u: configuration descriptor is %u bytes, only %u used",
                        (unsigned)dev->address, (unsigned)total_len, (unsigned)USB_CONFIG_MAX);
            total_len = USB_CONFIG_MAX;
        }
        if (total_len < 9) total_len = 9;

        delay_ms(5);
        for (uint32_t i = 0; i < total_len; i++) cfg_desc[i] = 0;
        int cfgN_ret = usb_control_transfer(dev, 0x80, 0x06,
                                            (0x02 << 8), 0, total_len, cfg_desc);
        if (cfgN_ret != 0 && total_len > 256) {
            LOG_WARNING("addr %u: GET_DESCRIPTOR(config, %u) failed (%d), retrying with 256 bytes",
                        (unsigned)dev->address, (unsigned)total_len, cfgN_ret);
            total_len = 256;
            delay_ms(5);
            cfgN_ret = usb_control_transfer(dev, 0x80, 0x06, (0x02 << 8), 0, total_len, cfg_desc);
        }
        if (cfgN_ret == 0) {
            memcpy(s_config_store[slot], cfg_desc, total_len);
            dev->config = s_config_store[slot];
            dev->config_len = total_len;
            dev->config_value = cfg_desc[5];
            usb_parse_config(dev, cfg_desc, total_len);
            usb_parse_functions(dev, cfg_desc, total_len);
            LOG_DEBUG("addr %u: class=0x%02x sub=0x%02x proto=0x%02x int_ep=0x%02x bulk_eps=%u iso_eps=%u interfaces=%u functions=%u",
                      (unsigned)dev->address, (unsigned)dev->device_class, (unsigned)dev->device_subclass,
                      (unsigned)dev->device_protocol, (unsigned)dev->endpoint_address,
                      (unsigned)dev->bulk_ep_count, (unsigned)dev->iso_ep_count,
                      (unsigned)dev->interface_count, (unsigned)dev->function_count);
        } else {
            LOG_WARNING("addr %u: GET_DESCRIPTOR(config, %u) failed (%d)",
                        (unsigned)dev->address, (unsigned)total_len, cfgN_ret);
        }
    }

    {

        uint8_t config_value = (cfg9_ret == 0 && cfg_value_hdr != 0) ? cfg_value_hdr : 1;
        dev->config_value = config_value;

        delay_ms(5);

        int sc_ret = -1;
        for (int _sc = 0; _sc < 5 && sc_ret != 0; _sc++) {
            if (_sc > 0) delay_ms(20);
            sc_ret = usb_control_transfer(dev, 0x00, 0x09, config_value, 0, 0, NULL);
        }
        if (sc_ret != 0) {
            LOG_WARNING("addr %u: SET_CONFIGURATION %u failed after retries (%d)",
                        (unsigned)dev->address, (unsigned)config_value, sc_ret);
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

    LOG_INFO("device %04x:%04x at port %u addr %u: class 0x%02x/0x%02x/0x%02x \"%s\" \"%s\"",
             (unsigned)dev->desc.idVendor, (unsigned)dev->desc.idProduct, (unsigned)port,
             (unsigned)dev->address, (unsigned)dev->device_class, (unsigned)dev->device_subclass,
             (unsigned)dev->device_protocol, dev->vendor_str, dev->product_str);
    if (dev->function_count > 1) usb_log_functions(dev);

    if (dev->device_class == 0x09) {
        LOG_INFO("hub detected at addr %u, enumerating downstream ports", (unsigned)dev->address);

        usb_hub_attach(dev, slot);

        LOG_DEBUG("hub at addr %u: downstream enumeration done", (unsigned)dev->address);
    }
    return slot;

fail:
    LOG_WARNING("enumeration of port %u failed", (unsigned)port);
    { uint64_t flags = spin_lock_irqsave(&usb_core_lock);
    dev->valid = 0;
    spin_unlock_irqrestore(&usb_core_lock, flags); }
    return -1;
}

static void usb_scan_all_locked(void) {
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
    LOG_INFO("scanning root ports (xHCI, EHCI, OHCI, UHCI)");

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

                        LOG_DEBUG("xhci%d p%u pre-reset USBSTS=0x%08x", i, (unsigned)port, xhci_read_current_usbsts());

                        if (root_hub_port_reset(&hub, port)) {

                            usb_root_port_mark(s_root_xhci, i, port, USB_ROOT_PORT_BUSY);
                            usb_init_device(ctrl, port, true);

                            int found = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                            uint32_t post = root_hub_port_status(&hub, port);
                            usb_root_port_mark(s_root_xhci, i, port, (int16_t)found);
                            usb_log_port_event(USB_PORT_LOG_XHCI,
                                (found >= 0) ? LOGGER_LEVEL_INFO : LOGGER_LEVEL_WARNING, i, port,
                                (found >= 0) ? "connect" : "enum-failed",
                                post, (uint8_t)((post >> 10) & 0xFu));
                        } else {
                            uint32_t post = root_hub_port_status(&hub, port);
                            usb_log_port_event(USB_PORT_LOG_XHCI, LOGGER_LEVEL_WARNING, i, port, "reset-failed",
                                post, (uint8_t)((post >> 10) & 0xFu));
                        }

                        LOG_DEBUG("xhci%d p%u post-reset USBSTS=0x%08x", i, (unsigned)port, xhci_read_current_usbsts());
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
                    usb_log_port_event(USB_PORT_LOG_EHCI, LOGGER_LEVEL_WARNING, i, port, "lost-after-reset", pst, 0xFF);
                    continue;
                }
                if (!(pst & (1u << 2))) {

                    root_hub_exec(&hub, HUB_CMD_PORT_OWNER_SET, port, 0);
                    usb_log_port_event(USB_PORT_LOG_EHCI, LOGGER_LEVEL_INFO, i, port, "owner->companion", pst, 0xFF);
                    continue;
                }
                usb_root_port_mark(s_root_ehci, i, port, USB_ROOT_PORT_BUSY);
                usb_init_device(ctrl, port, false);
                int after_slot = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                usb_root_port_mark(s_root_ehci, i, port, (int16_t)after_slot);
                pst = root_hub_port_status(&hub, port);
                usb_log_port_event(USB_PORT_LOG_EHCI, LOGGER_LEVEL_INFO, i, port, "connect", pst, 0xFF);
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
                    usb_log_port_event(USB_PORT_LOG_OHCI, LOGGER_LEVEL_WARNING, i, port, "lost-after-reset", pst, 0xFF);
                    continue;
                }
                usb_root_port_mark(s_root_ohci, i, port, USB_ROOT_PORT_BUSY);
                usb_init_device(ctrl, port, false);
                int after_slot = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                usb_root_port_mark(s_root_ohci, i, port, (int16_t)after_slot);
                pst = root_hub_port_status(&hub, port);
                usb_log_port_event(USB_PORT_LOG_OHCI, LOGGER_LEVEL_INFO, i, port, "connect", pst, 0xFF);
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
                    usb_log_port_event(USB_PORT_LOG_UHCI, LOGGER_LEVEL_WARNING, i, port, "lost-after-reset", pst, 0xFF);
                    continue;
                }

                usb_root_port_mark(s_root_uhci, i, port, USB_ROOT_PORT_BUSY);
                usb_init_device(ctrl, port, false);
                int after_slot = usb_root_find_device_slot((struct usb_controller *)ctrl, port);
                usb_root_port_mark(s_root_uhci, i, port, (int16_t)after_slot);
                pst = root_hub_port_status(&hub, port);
                usb_log_port_event(USB_PORT_LOG_UHCI, LOGGER_LEVEL_INFO, i, port, "connect", pst, 0xFF);
            }
        }
    }

}

void usb_scan_all(void) {
    for (int wait = 0; wait < 200; wait++) {
        if (usb_pump_enter()) {
            usb_scan_all_locked();
            usb_pump_leave();
            return;
        }
        delay_ms(10);
    }

    LOG_WARNING("scan skipped, controller busy");
}

static struct usb_core_driver core = {
    .scan_all = usb_scan_all,
    .control_transfer = usb_control_transfer,
    .interrupt_transfer = usb_interrupt_transfer,
    .enqueue_event = usb_core_enqueue_event,
    .poll_transfers = usb_core_poll_transfers,
    .poll_topology = usb_core_poll_topology,
    .bulk_transfer = usb_bulk_transfer,
    .iso_open = usb_iso_open,
    .iso_submit = usb_iso_submit,
    .iso_poll = usb_iso_poll,
    .iso_close = usb_iso_close,
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
