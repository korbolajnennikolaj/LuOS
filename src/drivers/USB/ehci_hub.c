#include "ehci_hub.h"

#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "kernel/limine.h"

#include <stdint.h>

static void ehci_real_delay_ms(uint64_t ms) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (tsc && tsc->sleep_tsc_ms) {
        tsc->sleep_tsc_ms(ms);
        return;
    }
    for (volatile uint64_t i = 0; i < ms * 2000000ull; i++) asm volatile("pause");
}

#define EHCI_HCSPARAMS 0x04
#define EHCI_CONFIGFLAG 0x40
#define EHCI_PORTSC 0x44

#define EHCI_PORTSC_RW1C ((1 << 1) | (1 << 3) | (1 << 5))

extern volatile struct limine_hhdm_request hhdm_req;

static inline uint32_t ehci_read_cap(struct ehci_controller* c, uint32_t reg) {
    uintptr_t offset = hhdm_req.response ? hhdm_req.response->offset : 0;
    return *(volatile uint32_t*)(c->cap_base + offset + reg);
}

static inline uint32_t ehci_read_op(struct ehci_controller* c, uint32_t reg) {
    uintptr_t offset = hhdm_req.response ? hhdm_req.response->offset : 0;
    return *(volatile uint32_t*)(c->op_base + offset + reg);
}

static inline void ehci_write_op(struct ehci_controller* c, uint32_t reg, uint32_t val) {
    uintptr_t offset = hhdm_req.response ? hhdm_req.response->offset : 0;
    *(volatile uint32_t*)(c->op_base + offset + reg) = val;
}

static const struct ehci_controller *s_configflag_done[MAX_EHCI_CONTROLLERS];
static int s_configflag_done_count = 0;

static void ehci_ensure_configflag(struct ehci_controller *c) {
    for (int i = 0; i < s_configflag_done_count; i++)
        if (s_configflag_done[i] == c) return;

    ehci_write_op(c, EHCI_CONFIGFLAG, 1);

    if (s_configflag_done_count < MAX_EHCI_CONTROLLERS)
        s_configflag_done[s_configflag_done_count++] = c;
}

uint32_t ehci_hub_exec(struct ehci_hub* hub, enum USB_HUB_CMD cmd, uint32_t port, uint32_t val) {
    if (!hub || !hub->ctrl) return 0;
    struct ehci_controller* c = (struct ehci_controller*)hub->ctrl;

    ehci_ensure_configflag(c);

    switch (cmd) {
        case HUB_CMD_PORT_COUNT: {
            uint32_t hcs = ehci_read_cap(c, EHCI_HCSPARAMS);
            uint32_t n_ports = hcs & 0xF;
            return (n_ports == 0) ? 1 : n_ports;
        }

        case HUB_CMD_PORT_STATUS: {
            if (port < 1) return 0;
            return ehci_read_op(c, EHCI_PORTSC + (port - 1) * 4);
        }

        case HUB_CMD_PORT_RESET: {
            uint32_t reg = EHCI_PORTSC + (port - 1) * 4;

            ehci_real_delay_ms(1);

            uint32_t status = ehci_read_op(c, reg);

            status &= ~EHCI_PORTSC_RW1C;
            ehci_write_op(c, reg, status | (1 << 8));

            ehci_real_delay_ms(50);

            status = ehci_read_op(c, reg);
            status &= ~EHCI_PORTSC_RW1C;
            ehci_write_op(c, reg, status & ~(1 << 8));

            for (int i = 0; i < 200; i++) {
                if (!(ehci_read_op(c, reg) & (1 << 8))) break;
                ehci_real_delay_ms(1);
            }

            for (int i = 0; i < 100; i++) {
                if (ehci_read_op(c, reg) & (1 << 2)) break;
                ehci_real_delay_ms(1);
            }

            ehci_real_delay_ms(20);

            status = ehci_read_op(c, reg);
            ehci_write_op(c, reg, status | EHCI_PORTSC_RW1C);

            return (ehci_read_op(c, reg) & (1 << 2)) ? 1 : 0;
        }

        case HUB_CMD_PORT_POWER: {
            uint32_t reg = EHCI_PORTSC + (port - 1) * 4;
            uint32_t status = ehci_read_op(c, reg);
            status &= ~EHCI_PORTSC_RW1C;
            ehci_write_op(c, reg, status | (1 << 12));
            return 1;
        }

        case HUB_CMD_PORT_OWNER_CLEAR: {

            uint32_t reg = EHCI_PORTSC + (port - 1) * 4;
            uint32_t status = ehci_read_op(c, reg);
            status &= ~EHCI_PORTSC_RW1C;
            status &= ~(1u << 13);
            ehci_write_op(c, reg, status);
            return 1;
        }

        case HUB_CMD_PORT_OWNER_SET: {

            uint32_t reg = EHCI_PORTSC + (port - 1) * 4;
            uint32_t status = ehci_read_op(c, reg);
            status &= ~EHCI_PORTSC_RW1C;
            status |= (1u << 13);
            ehci_write_op(c, reg, status);
            return 1;
        }

        case HUB_CMD_PORT_CLEAR_CHANGE: {
            uint32_t reg = EHCI_PORTSC + (port - 1) * 4;
            uint32_t status = ehci_read_op(c, reg);
            uint32_t clear = val & EHCI_PORTSC_RW1C;
            if (!clear) return 1;
            status &= ~EHCI_PORTSC_RW1C;
            ehci_write_op(c, reg, status | clear);
            return 1;
        }

        default:
            break;
    }
    return 0;
}
