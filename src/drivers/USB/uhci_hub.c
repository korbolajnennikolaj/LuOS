#include "uhci_hub.h"

#include "drivers/Timer/timer.h"
#include "drivers/USB/uhci.h"

#include <ports.h>

uint32_t uhci_hub_exec(struct uhci_hub* hub, enum USB_HUB_CMD cmd, uint32_t port, uint32_t val) {
    if (!hub || !hub->ctrl) return 0;
    struct uhci_controller* c = (struct uhci_controller*)hub->ctrl;

    if (cmd != HUB_CMD_PORT_COUNT) {
        if (port < 1 || port > uhci_port_count(c)) return 0;
    }

    uint16_t addr = (uint16_t)(c->io_base + UHCI_PORTSC1 + (port - 1) * 2);

    switch (cmd) {
        case HUB_CMD_PORT_COUNT:
            return uhci_port_count(c);

        case HUB_CMD_PORT_STATUS: {
            uint16_t s = inw(addr);
            return (s == 0xFFFF) ? 0 : s;
        }

        case HUB_CMD_PORT_RESET: {
            uint16_t s = inw(addr);
            if (s == 0xFFFF || !(s & PORTSC_CCS)) return 0;

            int is_ls_port = (s & PORTSC_LSDA) ? 1 : 0;

            int ci = uhci_controller_index(c);
            if (ci >= 0) uhci_dev_is_ls[ci][0] = (uint8_t)is_ls_port;

            outw(addr, PORTSC_RWC);
            uhci_delay_ms(1);

            outw(addr, PORTSC_PR);
            uhci_delay_ms(is_ls_port ? 60 : 50);

            outw(addr, 0);
            uhci_delay_ms(10);

            for (int tries = 0; tries < 20; tries++) {
                uint16_t r = inw(addr);
                if (r == 0xFFFF || !(r & PORTSC_CCS)) return 0;

                outw(addr, (uint16_t)((r & ~(PORTSC_PR | PORTSC_RWC)) | PORTSC_PES | PORTSC_RWC));
                uhci_delay_ms(5);

                r = inw(addr);
                if (r & PORTSC_PES) {
                    if (ci >= 0)
                        uhci_dev_is_ls[ci][0] = (r & PORTSC_LSDA) ? 1 : 0;
                    uhci_delay_ms(20);
                    return 1;
                }
            }
            return 0;
        }

        case HUB_CMD_PORT_POWER:
            return 1;

        case HUB_CMD_PORT_CLEAR_CHANGE: {
            uint16_t clear = (uint16_t)(val & PORTSC_RWC);
            if (!clear) return 1;
            uint16_t s = inw(addr);
            if (s == 0xFFFF) return 0;
            outw(addr, (uint16_t)((s & ~(PORTSC_RWC | PORTSC_PR)) | clear));
            return 1;
        }

        default:
            break;
    }
    return 0;
}
