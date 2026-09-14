#include "ohci_hub.h"

#include "drivers/USB/ohci.h"
#include "kernel/limine.h"

#include <stdint.h>

#define PORT_CCS (1 << 0)
#define PORT_PES (1 << 1)
#define PORT_PSS (1 << 2)
#define PORT_POCI (1 << 3)
#define PORT_PRS (1 << 4)
#define PORT_PPS (1 << 8)
#define PORT_LSDA (1 << 9)

#define PORT_CSC (1 << 16)
#define PORT_PESC (1 << 17)
#define PORT_PSSC (1 << 18)
#define PORT_OCIC (1 << 19)
#define PORT_PRSC (1 << 20)

#define PORT_CHANGE_MASK (PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC)

uint32_t ohci_hub_exec(struct ohci_hub* hub, enum USB_HUB_CMD cmd, uint32_t port, uint32_t val) {
    if (!hub || !hub->ctrl) return 0;
    struct ohci_controller* c = hub->ctrl;

    if (cmd == HUB_CMD_PORT_COUNT)
        return ohci_port_count(c);

    if (port < 1 || port > ohci_port_count(c)) return 0;

    uint32_t reg = OHCI_HcRhPortStatus + (port - 1) * 4;

    switch (cmd) {
        case HUB_CMD_PORT_STATUS: {
            uint32_t s = ohci_mmio_read(c, reg);
            return (s == 0xFFFFFFFFu) ? 0 : s;
        }

        case HUB_CMD_PORT_RESET: {
            uint32_t s = ohci_mmio_read(c, reg);
            if (s == 0xFFFFFFFFu || !(s & PORT_CCS)) return 0;

            if (!(s & PORT_PPS)) {
                ohci_mmio_write(c, reg, PORT_PPS);
                ohci_delay_ms(c->power_on_delay_ms ? c->power_on_delay_ms : 20);
            }

            ohci_mmio_write(c, reg, PORT_CHANGE_MASK);
            ohci_mmio_write(c, reg, PORT_PRS);

            int timeout = 200;
            while (timeout-- > 0) {
                s = ohci_mmio_read(c, reg);
                if (s == 0xFFFFFFFFu) return 0;
                if (s & PORT_PRSC) break;
                ohci_delay_ms(1);
            }

            ohci_mmio_write(c, reg, PORT_PRSC);
            ohci_delay_ms(20);

            s = ohci_mmio_read(c, reg);
            if (s == 0xFFFFFFFFu || !(s & PORT_CCS)) return 0;

            if (!(s & PORT_PES)) {
                ohci_mmio_write(c, reg, PORT_PES);
                ohci_delay_ms(10);
                s = ohci_mmio_read(c, reg);
            }
            if (!(s & PORT_PES)) return 0;

            int ci = ohci_controller_index(c);
            uint8_t is_ls = (uint8_t)((s & PORT_LSDA) ? 1 : 0);
            c->is_low_speed = is_ls;
            if (ci >= 0) ohci_dev_is_ls[ci][0] = is_ls;

            return 1;
        }

        case HUB_CMD_PORT_POWER: {
            ohci_mmio_write(c, reg, val ? PORT_PPS : (1u << 9));
            ohci_delay_ms(c->power_on_delay_ms ? c->power_on_delay_ms : 20);
            return 1;
        }

        case HUB_CMD_PORT_CLEAR_CHANGE: {
            uint32_t clear = val & PORT_CHANGE_MASK;
            if (clear) ohci_mmio_write(c, reg, clear);
            return 1;
        }

        default:
            break;
    }
    return 0;
}
