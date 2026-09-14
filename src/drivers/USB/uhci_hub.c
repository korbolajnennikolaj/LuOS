#include "uhci_hub.h"

#include "drivers/Timer/timer.h"
#include "drivers/USB/uhci.h"

#include <ports.h>

extern void delay_ms(uint64_t ms);
extern uint8_t uhci_dev_is_ls[4][128];

uint32_t uhci_hub_exec(struct uhci_hub* hub, enum USB_HUB_CMD cmd, uint32_t port, uint32_t val) {
    if (!hub || !hub->ctrl) return 0;
    struct uhci_controller* c = (struct uhci_controller*)hub->ctrl;

    switch (cmd) {
        case HUB_CMD_PORT_COUNT: {

            return 2;
        }
        case HUB_CMD_PORT_STATUS:
            return inw(c->io_base + 0x10 + (port-1)*2);
        case HUB_CMD_PORT_RESET: {
            uint16_t addr = c->io_base + 0x10 + (port-1)*2;

            int is_ls_port = (inw(addr) & 0x0100) ? 1 : 0;

            int ci = uhci_controller_index(c);
            if (ci >= 0) uhci_dev_is_ls[ci][0] = (uint8_t)is_ls_port;

            outw(addr, (inw(addr) & ~0x000A) | 0x000A);
            delay_ms(1);

            outw(addr, (inw(addr) & ~0x000A) | 0x0200);
            delay_ms(is_ls_port ? 60 : 15);

            outw(addr, inw(addr) & ~(0x0200 | 0x000A));
            delay_ms(15);

            for (int tries = 0; tries < 20; tries++) {
                uint16_t s = inw(addr);

                outw(addr, (s & ~(0x0200)) | 0x0004 | 0x000A);
                delay_ms(5);
                uint16_t r = inw(addr);
                if (r & 0x0004) return 1;
                if (!(r & 0x0001)) return 0;
            }
            return 1;
        }
        case HUB_CMD_PORT_POWER: return 1;

        case HUB_CMD_PORT_CLEAR_CHANGE: {
            uint16_t addr = c->io_base + 0x10 + (port-1)*2;
            uint16_t clear = (uint16_t)(val & (PORTSC_CSC | PORTSC_PEC));
            if (clear) outw(addr, inw(addr) | clear);
            return 1;
        }
    }
    return 0;
}
