#ifndef XHCI_HUB_H
#define XHCI_HUB_H

#include "drivers/USB/xhci.h"
#include "root_hub.h"

#include <stdint.h>

typedef struct xhci_hub {
    struct xhci_controller* ctrl;
} xhci_hub;

uint32_t xhci_hub_exec(struct xhci_hub* hub, enum USB_HUB_CMD cmd, uint32_t a, uint32_t b);

void xhci_hub_clear_port_change(struct xhci_controller* ctrl, uint8_t port, uint32_t change_bits);

#endif
