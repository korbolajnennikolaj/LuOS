#include "uhci_hub.h"

#include "drivers/Timer/timer.h"

#include <ports.h>

extern void delay_ms(uint64_t ms);

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
