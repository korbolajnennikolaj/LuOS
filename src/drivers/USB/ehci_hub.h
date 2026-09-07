#ifndef EHCI_HUB_H
#define EHCI_HUB_H

#include "drivers/USB/ehci.h"
#include "root_hub.h"

#include <stdint.h>

typedef struct ehci_hub {
    struct ehci_controller* ctrl;
} ehci_hub;

uint32_t ehci_hub_exec(struct ehci_hub* hub, enum USB_HUB_CMD cmd, uint32_t a, uint32_t b);

#endif
