#include "drivers/USB/usb_hub.h"

#include "components/drivers.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core_internal.h"
#include "drivers/USB/usb_log.h"
#include "drivers/USB/xhci.h"

#include <stdbool.h>
#include <stddef.h>

#define HUB_BMREQ_GET_HUB_DESC 0xA0u
#define HUB_BMREQ_GET_PORT_STS 0xA3u
#define HUB_BMREQ_SET_PORT_FEAT 0x23u
#define HUB_BMREQ_CLR_PORT_FEAT 0x23u

#define HUB_CBIT_CONNECTION 0
#define HUB_CBIT_ENABLE 1
#define HUB_CBIT_SUSPEND 2
#define HUB_CBIT_OVER_CURRENT 3
#define HUB_CBIT_RESET 4

#define HUB_STATUS_BUF_SIZE 8

typedef struct usb_hub_state {
    uint8_t in_use;
    uint8_t port_count;
    uint32_t pwron_ms;

    int8_t child_slot[USB_HUB_MAX_PORTS + 1];

    uint16_t fail_streak;
} usb_hub_state;

static usb_hub_state s_hub[MAX_USB_DEVICES];

static uint8_t s_hub_status_buf[MAX_USB_DEVICES][HUB_STATUS_BUF_SIZE]
__attribute__((aligned(64)));

void usb_hub_reset_state(void) {
    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        s_hub[i].in_use = 0;
        s_hub[i].port_count = 0;
        s_hub[i].pwron_ms = 0;
        s_hub[i].fail_streak = 0;
        for (int p = 0; p <= USB_HUB_MAX_PORTS; p++) s_hub[i].child_slot[p] = -1;
        for (int b = 0; b < HUB_STATUS_BUF_SIZE; b++) s_hub_status_buf[i][b] = 0;
    }
}

static uint16_t hub_get_port_status(struct usb_device *hub, uint8_t port, uint16_t *change_out, bool *fail_out) {
    static uint8_t buf[4] __attribute__((aligned(64)));
    for (int i = 0; i < 4; i++) buf[i] = 0;

    if (usb_control_transfer(hub, HUB_BMREQ_GET_PORT_STS, USB_REQ_GET_STATUS,
                              0, port, 4, buf) != 0) {
        if (change_out) *change_out = 0;
        if (fail_out) *fail_out = true;
        return 0;
    }

    if (fail_out) *fail_out = false;
    uint16_t status = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    if (change_out) *change_out = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    return status;
}

static void hub_set_port_feature(struct usb_device *hub, uint8_t port, uint16_t feature) {
    usb_control_transfer(hub, HUB_BMREQ_SET_PORT_FEAT, USB_REQ_SET_FEATURE,
                          feature, port, 0, NULL);
}

static void hub_clear_port_feature(struct usb_device *hub, uint8_t port, uint16_t feature) {
    usb_control_transfer(hub, HUB_BMREQ_CLR_PORT_FEAT, USB_REQ_CLEAR_FEATURE,
                          feature, port, 0, NULL);
}

static int hub_bringup_port(struct usb_device *hub, int hub_slot, uint8_t port, uint32_t pwron_ms) {
    hub_set_port_feature(hub, port, USB_HUB_FEAT_PORT_POWER);
    delay_ms(pwron_ms);

    uint16_t status = hub_get_port_status(hub, port, NULL, NULL);
    if (!(status & USB_HUB_PORTSTS_CONNECTION)) return -1;

    hub_clear_port_feature(hub, port, USB_HUB_FEAT_C_PORT_CONNECTION);
    hub_set_port_feature(hub, port, USB_HUB_FEAT_PORT_RESET);

    int reset_ok = 0;
    for (int t = 0; t < 50; t++) {
        delay_ms(10);
        uint16_t change = 0;
        hub_get_port_status(hub, port, &change, NULL);
        if (change & (1u << HUB_CBIT_RESET)) { reset_ok = 1; break; }
    }
    if (!reset_ok) return -1;
    hub_clear_port_feature(hub, port, USB_HUB_FEAT_C_PORT_RESET);

    status = hub_get_port_status(hub, port, NULL, NULL);
    if (!(status & USB_HUB_PORTSTS_CONNECTION)) return -1;

    uint8_t psiv;
    if (status & USB_HUB_PORTSTS_LOW_SPEED) psiv = 2;
    else if (status & USB_HUB_PORTSTS_HIGH_SPEED) psiv = 3;
    else psiv = 1;

    bool child_is_xhci = (hub->ctrl && hub->ctrl->type == USB_TYPE_XHCI);

    int before = usb_get_device_count();
    usb_init_device_topo(hub->ctrl, port, child_is_xhci,
                          hub->root_port, (uint8_t)(hub->hub_depth + 1),
                          hub->route_string, hub->address, psiv);
    int after = usb_get_device_count();

    if (after > before) {
        int child_slot = after - 1;
        if (port <= USB_HUB_MAX_PORTS) s_hub[hub_slot].child_slot[port] = (int8_t)child_slot;
        return child_slot;
    }
    return -1;
}

void usb_hub_attach(struct usb_device *hub_dev, int hub_slot) {
    if (!hub_dev) return;
    if (hub_slot < 0 || hub_slot >= MAX_USB_DEVICES) return;

    static uint8_t hub_desc[16] __attribute__((aligned(64)));
    for (int i = 0; i < 16; i++) hub_desc[i] = 0;

    int hd_ret = usb_control_transfer(hub_dev, HUB_BMREQ_GET_HUB_DESC,
                                       USB_REQ_GET_DESCRIPTOR, 0x2900, 0, 8, hub_desc);

    uint8_t port_count = (hd_ret == 0 && hub_desc[2] > 0 && hub_desc[2] <= USB_HUB_MAX_PORTS)
        ? hub_desc[2] : 4;
    uint32_t pwron_ms = (hd_ret == 0) ? (uint32_t)hub_desc[5] * 2 : 100;
    if (pwron_ms < 20) pwron_ms = 20;

    s_hub[hub_slot].in_use = 1;
    s_hub[hub_slot].port_count = port_count;
    s_hub[hub_slot].pwron_ms = pwron_ms;
    for (int p = 0; p <= USB_HUB_MAX_PORTS; p++) s_hub[hub_slot].child_slot[p] = -1;

    if (hub_dev->ctrl && hub_dev->ctrl->type == USB_TYPE_XHCI) {
        struct xhci_driver *x_drv = get_self_driver(USB_DRIVER, USB_TYPE_XHCI);
        if (x_drv && x_drv->evaluate_hub_slot) {
            x_drv->evaluate_hub_slot((struct xhci_controller *)hub_dev->ctrl,
                                      hub_dev->address, port_count);
        }
    }

    usb_logrow_begin(USB_LOG_HUB, USB_LOG_INFO);
    usb_logrow_str("hub slot "); usb_logrow_dec(hub_slot);
    usb_logrow_str(": "); usb_logrow_dec(port_count);
    usb_logrow_str(" ports, pwron="); usb_logrow_dec((int32_t)pwron_ms);
    usb_logrow_str("ms");
    usb_logrow_end();

    for (uint8_t port = 1; port <= port_count; port++) {
        int child = hub_bringup_port(hub_dev, hub_slot, port, pwron_ms);
        if (child >= 0) {
            uint16_t change = 0;
            uint16_t status = hub_get_port_status(hub_dev, port, &change, NULL);
            usb_log_port_event(USB_LOG_HUB, hub_slot, port, "connect",
                ((uint32_t)change << 16) | status, 0xFF);
        }
    }
}

static void hub_remove_subtree(int slot) {
    if (slot < 0 || slot >= MAX_USB_DEVICES) return;

    if (s_hub[slot].in_use) {
        for (uint8_t p = 1; p <= s_hub[slot].port_count; p++) {
            int child = s_hub[slot].child_slot[p];
            if (child >= 0) {
                hub_remove_subtree(child);
                s_hub[slot].child_slot[p] = -1;
            }
        }
        s_hub[slot].in_use = 0;
        s_hub[slot].port_count = 0;
    }

    struct usb_device *dev = usb_get_device(slot);
    if (dev) usb_core_remove_device(dev);
}

void usb_hub_detach(int slot) {
    hub_remove_subtree(slot);
}

void usb_hub_poll(void) {
    int count = usb_get_device_count();

    for (int i = 0; i < count; i++) {
        if (!s_hub[i].in_use) continue;

        struct usb_device *dev = usb_get_device(i);
        if (!dev || dev->device_class != 0x09 || dev->hub_status_ep == 0) continue;

        if (s_hub[i].fail_streak > 0) {
            uint32_t backoff = (uint32_t)s_hub[i].fail_streak * 50u;
            if (backoff > 500u) backoff = 500u;
            delay_ms(backoff);
        }

        uint8_t *buf = s_hub_status_buf[i];
        int ret = usb_interrupt_transfer(dev, dev->hub_status_ep, buf,
                                          HUB_STATUS_BUF_SIZE, 1);
        if (ret != 0) continue;

        bool any_get_status_failed = false;
        for (uint8_t port = 1; port <= s_hub[i].port_count; port++) {
            uint8_t byte = port >> 3;
            uint8_t bit = port & 7;
            if (byte >= HUB_STATUS_BUF_SIZE) break;
            if (!(buf[byte] & (1u << bit))) continue;

            uint16_t change = 0;
            bool failed = false;
            uint16_t status = hub_get_port_status(dev, port, &change, &failed);
            if (failed) {
                any_get_status_failed = true;
                continue;

            }
            uint32_t packed = ((uint32_t)change << 16) | status;

            if (change & (1u << HUB_CBIT_CONNECTION)) {
                hub_clear_port_feature(dev, port, USB_HUB_FEAT_C_PORT_CONNECTION);

                if (status & USB_HUB_PORTSTS_CONNECTION) {

                    int child = hub_bringup_port(dev, i, port, s_hub[i].pwron_ms);
                    usb_log_port_event(USB_LOG_HUB, i, port,
                        (child >= 0) ? "connect" : "enum-failed", packed, 0xFF);
                } else if (port <= USB_HUB_MAX_PORTS) {

                    int child = s_hub[i].child_slot[port];
                    if (child >= 0) hub_remove_subtree(child);
                    s_hub[i].child_slot[port] = -1;
                    usb_log_port_event(USB_LOG_HUB, i, port, "disconnect", packed, 0xFF);
                }
            }

            if (change & (1u << HUB_CBIT_ENABLE))
                hub_clear_port_feature(dev, port, USB_HUB_FEAT_C_PORT_ENABLE);

            if (change & (1u << HUB_CBIT_OVER_CURRENT))
                hub_clear_port_feature(dev, port, USB_HUB_FEAT_C_PORT_OVER_CURRENT);

            if (change & (1u << HUB_CBIT_RESET))
                hub_clear_port_feature(dev, port, USB_HUB_FEAT_C_PORT_RESET);

            if (change & (1u << HUB_CBIT_SUSPEND))
                hub_clear_port_feature(dev, port, USB_HUB_FEAT_C_PORT_SUSPEND);
        }

        if (any_get_status_failed) {
            if (s_hub[i].fail_streak < 0xFFFFu) s_hub[i].fail_streak++;
        } else {
            s_hub[i].fail_streak = 0;
        }
    }
}
