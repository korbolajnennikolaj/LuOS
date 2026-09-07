#ifndef UHCI_HUB_H
#define UHCI_HUB_H

#include "drivers/USB/uhci.h"
#include "root_hub.h"

#include <stdint.h>

typedef struct uhci_hub {
    struct uhci_controller* ctrl;
} uhci_hub;

uint32_t uhci_hub_exec(struct uhci_hub* hub, enum USB_HUB_CMD cmd, uint32_t a, uint32_t b);

#endif
