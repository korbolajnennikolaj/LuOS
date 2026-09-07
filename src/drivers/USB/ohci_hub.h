#ifndef OHCI_HUB_H
#define OHCI_HUB_H

#include "drivers/USB/ohci.h"
#include "root_hub.h"

#include <stdint.h>

typedef struct ohci_hub {
    struct ohci_controller* ctrl;
} ohci_hub;

uint32_t ohci_hub_exec(struct ohci_hub* hub, enum USB_HUB_CMD cmd, uint32_t a, uint32_t b);

#endif
