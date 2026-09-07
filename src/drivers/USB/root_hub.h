#ifndef ROOT_HUB_H
#define ROOT_HUB_H

#include <stdbool.h>
#include <stdint.h>

enum USB_HUB_CMD {
    HUB_CMD_PORT_COUNT = 0,
    HUB_CMD_PORT_STATUS,
    HUB_CMD_PORT_POWER,
    HUB_CMD_PORT_RESET,
    HUB_CMD_PORT_OWNER_CLEAR,
    HUB_CMD_PORT_OWNER_SET,

    HUB_CMD_PORT_CLEAR_CHANGE,
};

enum HUB_CONTROLLER_TYPE {
    HUB_TYPE_UHCI = 0,
    HUB_TYPE_OHCI = 1,
    HUB_TYPE_EHCI = 2,
    HUB_TYPE_XHCI = 3
};

typedef struct root_hub {
    int host_type;
    void* backend;
    uint8_t port_count;
} root_hub;

bool root_hub_init(struct root_hub* hub, int type, void* backend);
uint32_t root_hub_port_status(struct root_hub* hub, uint8_t port);
bool root_hub_port_power(struct root_hub* hub, uint8_t port, bool on);
bool root_hub_port_reset(struct root_hub* hub, uint8_t port);
uint32_t root_hub_exec(struct root_hub* hub, enum USB_HUB_CMD cmd, uint32_t a, uint32_t b);

void root_hub_clear_port_change(struct root_hub* hub, uint8_t port, uint32_t change_bits);

#endif
