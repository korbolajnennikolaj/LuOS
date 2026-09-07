#include "xhci_hub.h"

#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "kernel/limine.h"

extern volatile struct limine_hhdm_request hhdm_req;

static inline uint32_t x_read(uintptr_t base, uint32_t reg) {
    uintptr_t offset = hhdm_req.response ? hhdm_req.response->offset : 0;
    return *(volatile uint32_t*)(base + offset + reg);
}

static void xhci_real_delay_ms(uint64_t ms) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (tsc && tsc->sleep_tsc_ms) {
        tsc->sleep_tsc_ms(ms);
        return;
    }
    for (volatile uint64_t i = 0; i < ms * 2000000ull; i++) asm volatile("pause");
}

uint32_t xhci_hub_exec(struct xhci_hub* hub, enum USB_HUB_CMD cmd, uint32_t port, uint32_t val) {
    if (!hub || !hub->ctrl) return 0;
    struct xhci_controller* c = (struct xhci_controller*)hub->ctrl;

    switch (cmd) {
        case HUB_CMD_PORT_COUNT: {
            uint32_t hcs1 = x_read(c->base_addr, 0x04);
            return (hcs1 >> 24) & 0xFF;
        }

        case HUB_CMD_PORT_STATUS: {

            if (!c->op_base) return 0;
            return x_read(c->op_base, 0x400 + (port - 1) * 0x10);
        }

        case HUB_CMD_PORT_POWER: {

            if (!c->op_base) return 0;
            uint32_t ps = x_read(c->op_base, 0x400 + (port - 1) * 0x10);
            if (!(ps & XHCI_PORTSC_PP)) {

                volatile uint32_t *reg = (volatile uint32_t *)(
                    (c->op_base + (hhdm_req.response ? hhdm_req.response->offset : 0))
                    + 0x400 + (port - 1) * 0x10);
                *reg = xhci_portsc_neutral(ps) | XHCI_PORTSC_PP;
            }
            return 1;
        }

        case HUB_CMD_PORT_RESET: {

            if (!c->op_base) return 0;
            uint32_t reg_off = 0x400 + (port - 1) * 0x10;
            uint32_t ps = x_read(c->op_base, reg_off);
            xhci_debug_port("PORTSC before reset", port, ps);
            if (!(ps & 1u)) { xhci_debug_port("no CCS, aborting reset", port, ps); return 0; }

            volatile uint32_t *preg = (volatile uint32_t *)(
                (c->op_base + (hhdm_req.response ? hhdm_req.response->offset : 0))
                + reg_off);

            if (!(ps & XHCI_PORTSC_PP)) {
                *preg = xhci_portsc_neutral(ps) | XHCI_PORTSC_PP;

                xhci_real_delay_ms(1);
                ps = x_read(c->op_base, reg_off);
            }

            *preg = xhci_portsc_neutral(ps) | XHCI_PORTSC_PR;

            for (int _t = 0; _t < 2000; _t++) {
                xhci_real_delay_ms(1);
                if (!(x_read(c->op_base, reg_off) & XHCI_PORTSC_PR)) break;
            }

            xhci_real_delay_ms(20);

            ps = x_read(c->op_base, reg_off);
            xhci_debug_port("PORTSC after Hot Reset", port, ps);

            if (ps & XHCI_PORTSC_CHANGE_MASK) {
                *preg = xhci_portsc_neutral(ps) | (ps & XHCI_PORTSC_CHANGE_MASK);
                ps = x_read(c->op_base, reg_off);
            }

            if ((ps & 1u) && !(ps & (1u << 1))) {
                uint32_t speed = (ps >> 10) & 0xFu;
                xhci_debug_port("connected but not Enabled after Hot Reset, speed id", port, speed);
                if (speed >= 4u) {
                    for (int attempt = 0;
                         attempt < 3 && (ps & XHCI_PORTSC_CCS) && !(ps & XHCI_PORTSC_PED);
                         attempt++) {
                        *preg = xhci_portsc_neutral(ps) | XHCI_PORTSC_WPR;

                        for (int _t = 0; _t < 2000; _t++) {
                            xhci_real_delay_ms(1);
                            if (!(x_read(c->op_base, reg_off) & XHCI_PORTSC_WPR)) break;
                        }
                        xhci_real_delay_ms(20);

                        ps = x_read(c->op_base, reg_off);
                        if (ps & XHCI_PORTSC_CHANGE_MASK) {
                            *preg = xhci_portsc_neutral(ps) | (ps & XHCI_PORTSC_CHANGE_MASK);
                            ps = x_read(c->op_base, reg_off);
                        }
                        xhci_debug_port("Warm Reset attempt #", port, (uint32_t)attempt);
                        xhci_debug_port("PORTSC after that attempt", port, ps);
                    }
                } else {
                    xhci_debug_port("not a SuperSpeed-class port, no Warm Reset attempted", port, speed);
                }
            }

            xhci_debug_port(((ps & 1u) && (ps & (1u << 1))) ? "reset result: ENABLED" : "reset result: FAILED (final PORTSC)", port, ps);
            return ((ps & 1u) && (ps & (1u << 1))) ? 1u : 0u;
        }

        case HUB_CMD_PORT_CLEAR_CHANGE:
            xhci_hub_clear_port_change(c, (uint8_t)port, val);
            return 1;

        default:
            break;
    }

    return 0;
}

void xhci_hub_clear_port_change(struct xhci_controller* ctrl, uint8_t port, uint32_t change_bits) {
    if (!ctrl || !ctrl->op_base || port == 0) return;

    uint32_t reg_off = 0x400 + (uint32_t)(port - 1) * 0x10;
    change_bits &= XHCI_PORTSC_CHANGE_MASK;
    if (!change_bits) return;

    uint32_t ps = x_read(ctrl->op_base, reg_off);

    volatile uint32_t *preg = (volatile uint32_t *)(
        (ctrl->op_base + (hhdm_req.response ? hhdm_req.response->offset : 0))
        + reg_off);
    *preg = xhci_portsc_neutral(ps) | change_bits;
}
