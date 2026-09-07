#include "ohci_hub.h"

#include "kernel/limine.h"

#include <stdint.h>

#define OHCI_RH_DESCRIPTOR_A 0x48
#define OHCI_RH_PORT_STATUS 0x54
#define OHCI_INTERRUPT_STATUS 0x0C

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

extern volatile struct limine_hhdm_request hhdm_req;

static inline uint32_t ohci_read(struct ohci_controller* c, uint32_t reg) {
    uintptr_t offset = hhdm_req.response ? hhdm_req.response->offset : 0;
    return *(volatile uint32_t*)(c->base_addr + offset + reg);
}

static inline void ohci_write(struct ohci_controller* c, uint32_t reg, uint32_t val) {
    uintptr_t offset = hhdm_req.response ? hhdm_req.response->offset : 0;
    *(volatile uint32_t*)(c->base_addr + offset + reg) = val;
}

uint32_t ohci_hub_exec(struct ohci_hub* hub, enum USB_HUB_CMD cmd, uint32_t port, __attribute__((unused)) uint32_t val) {
    if (!hub || !hub->ctrl) return 0;
    struct ohci_controller* c = hub->ctrl;
    uint32_t reg = OHCI_RH_PORT_STATUS + (port - 1) * 4;

    switch (cmd) {
        case HUB_CMD_PORT_COUNT:
            return ohci_read(c, OHCI_RH_DESCRIPTOR_A) & 0xFF;

        case HUB_CMD_PORT_STATUS:
            return ohci_read(c, reg);

        case HUB_CMD_PORT_RESET: {

            ohci_write(c, reg, PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC);

            ohci_write(c, OHCI_INTERRUPT_STATUS, ohci_read(c, OHCI_INTERRUPT_STATUS));

            ohci_write(c, reg, PORT_PRS);

            int timeout = 100;
            while (timeout--) {
                uint32_t status = ohci_read(c, reg);

                if (status & PORT_PRSC) break;

                for(volatile int j = 0; j < 100000; j++) asm volatile("pause");
            }

            ohci_write(c, reg, PORT_PRSC);

            for(volatile int i = 0; i < 5000000; i++) asm volatile("pause");

            uint32_t final_status = ohci_read(c, reg);

            if (final_status & PORT_PES) {
                return 1;
            }

            return 0;
        }

        case HUB_CMD_PORT_POWER: {

            ohci_write(c, reg, PORT_PPS);
            ohci_write(c, reg, PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC);
            return 1;
        }

        case HUB_CMD_PORT_CLEAR_CHANGE: {
            uint32_t change_mask = PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC;
            uint32_t clear = val & change_mask;
            if (clear) ohci_write(c, reg, clear);
            return 1;
        }
    }
    return 0;
}
