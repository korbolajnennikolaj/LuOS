#include "root_hub.h"

#include "ehci_hub.h"
#include "ohci_hub.h"
#include "uhci_hub.h"
#include "xhci_hub.h"

#include <stddef.h>

uint32_t root_hub_exec(struct root_hub* hub, enum USB_HUB_CMD cmd, uint32_t a, uint32_t b) {
    if (!hub || !hub->backend) return 0;

    switch (hub->host_type) {
        case HUB_TYPE_XHCI: return xhci_hub_exec((struct xhci_hub*)hub->backend, cmd, a, b);
        case HUB_TYPE_EHCI: return ehci_hub_exec((struct ehci_hub*)hub->backend, cmd, a, b);
        case HUB_TYPE_OHCI: return ohci_hub_exec((struct ohci_hub*)hub->backend, cmd, a, b);
        case HUB_TYPE_UHCI: return uhci_hub_exec((struct uhci_hub*)hub->backend, cmd, a, b);
    }
    return 0;
}

#define ROOT_HUB_MAX_PER_TYPE 8
bool root_hub_init(struct root_hub* hub, int type, void* backend) {
    if (!hub || !backend) return false;
    hub->host_type = type;

    if (type == HUB_TYPE_XHCI) {
        static struct xhci_hub x_backends[ROOT_HUB_MAX_PER_TYPE];
        static int x_idx = 0;
        struct xhci_hub *b = &x_backends[x_idx % ROOT_HUB_MAX_PER_TYPE];
        x_idx++;
        b->ctrl = backend;
        hub->backend = b;
    } else if (type == HUB_TYPE_EHCI) {
        static struct ehci_hub e_backends[ROOT_HUB_MAX_PER_TYPE];
        static int e_idx = 0;
        struct ehci_hub *b = &e_backends[e_idx % ROOT_HUB_MAX_PER_TYPE];
        e_idx++;
        b->ctrl = backend;
        hub->backend = b;
    } else if (type == HUB_TYPE_OHCI) {
        static struct ohci_hub o_backends[ROOT_HUB_MAX_PER_TYPE];
        static int o_idx = 0;
        struct ohci_hub *b = &o_backends[o_idx % ROOT_HUB_MAX_PER_TYPE];
        o_idx++;
        b->ctrl = backend;
        hub->backend = b;
    } else if (type == HUB_TYPE_UHCI) {
        static struct uhci_hub u_backends[ROOT_HUB_MAX_PER_TYPE];
        static int u_idx = 0;
        struct uhci_hub *b = &u_backends[u_idx % ROOT_HUB_MAX_PER_TYPE];
        u_idx++;
        b->ctrl = backend;
        hub->backend = b;
    }

    hub->port_count = root_hub_exec(hub, HUB_CMD_PORT_COUNT, 0, 0);
    return hub->port_count > 0;
}

uint32_t root_hub_port_status(struct root_hub* hub, uint8_t port) {
    return root_hub_exec(hub, HUB_CMD_PORT_STATUS, port, 0);
}

bool root_hub_port_power(struct root_hub* hub, uint8_t port, bool on) {
    return root_hub_exec(hub, HUB_CMD_PORT_POWER, port, on);
}

bool root_hub_port_reset(struct root_hub* hub, uint8_t port) {
    return root_hub_exec(hub, HUB_CMD_PORT_RESET, port, 0);
}

void root_hub_clear_port_change(struct root_hub* hub, uint8_t port, uint32_t change_bits) {
    root_hub_exec(hub, HUB_CMD_PORT_CLEAR_CHANGE, port, change_bits);
}
