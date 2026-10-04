#include "ehci.h"

#include "components/drivers.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/logger.h"
#include "components/Memory/mm.h"
#include "components/pci.h"
#include "drivers/Timer/apic_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "kernel/limine.h"

#include <stddef.h>

#define EHCI_QTD_POOL_SIZE 128

#define EHCI_FRAME_LIST_SIZE 1024

extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_kernel_address_request kernel_address_request;

static struct {
    struct ehci_controller ctrl;
    struct ehci_qh async_head __attribute__((aligned(32)));
    struct ehci_qh intr_qh_pool[4] __attribute__((aligned(32)));
    uint32_t frame_list[EHCI_FRAME_LIST_SIZE] __attribute__((aligned(4096)));
    struct ehci_qtd qtd_pool[EHCI_QTD_POOL_SIZE];
    uint8_t qtd_used[EHCI_QTD_POOL_SIZE];
} ehci_resources[MAX_EHCI_CONTROLLERS] __attribute__((aligned(4096)));

static int ehci_controller_count = 0;
static void (*delay_ms)(uint64_t) = NULL;

typedef struct ehci_pending_xfer {
    struct ehci_controller *ctrl;
    struct ehci_qtd *qtd;
    void *data;
    uint16_t data_len;
    uint8_t direction;
    uint8_t active;
    int ri;
    void *cookie;
    uint8_t endpoint;
    uint8_t addr;
    uint8_t result_ready;
    int8_t last_result;
    struct usb_device *device;
} ehci_pending_xfer;

#define MAX_EHCI_HID_SLOTS 4
#define MAX_EHCI_PENDING (MAX_EHCI_CONTROLLERS * MAX_EHCI_HID_SLOTS)
static struct ehci_pending_xfer s_ehci_pending[MAX_EHCI_CONTROLLERS][MAX_EHCI_HID_SLOTS];

uint8_t ehci_dev_speed[MAX_EHCI_CONTROLLERS][128];
uint8_t ehci_dev_tt_hub[MAX_EHCI_CONTROLLERS][128];
uint8_t ehci_dev_tt_port[MAX_EHCI_CONTROLLERS][128];
uint8_t ehci_dev_mps0[MAX_EHCI_CONTROLLERS][128];

int ehci_controller_index(struct ehci_controller *e) {
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++)
        if (&ehci_resources[i].ctrl == e) return i;
    return -1;
}

static uint32_t ehci_endpoint_chars(int ri, uint8_t addr, uint8_t ep_num, uint32_t mps,
                                    int is_control, int periodic, uint32_t *caps_out) {
    uint8_t a = addr & 0x7Fu;
    uint8_t speed = ehci_dev_speed[ri][a];
    uint32_t eps = (speed == 1) ? 0u : (speed == 2) ? 1u : 2u;

    uint32_t chars = (uint32_t)a
                   | ((uint32_t)(ep_num & 0xFu) << 8)
                   | (eps << 12)
                   | (mps << 16);
    uint32_t caps = (1u << 30);

    if (eps != 2u) {
        if (is_control) chars |= (1u << 27);
        caps |= ((uint32_t)(ehci_dev_tt_hub[ri][a] & 0x7Fu) << 16)
              | ((uint32_t)(ehci_dev_tt_port[ri][a] & 0x7Fu) << 23);
        if (periodic) caps |= 0x01u | (0x1Cu << 8);
    } else if (periodic) {
        caps |= 0x01u;
    }

    if (caps_out) *caps_out = caps;
    return chars;
}

static uint32_t ehci_control_mps(struct ehci_controller *e, int ri, uint8_t addr) {
    if (addr == 0) return 8u;

    uint8_t m = ehci_dev_mps0[ri][addr & 0x7Fu];
    if (m == 8 || m == 16 || m == 32 || m == 64) return m;

    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        struct usb_device *d = (struct usb_device *)device_table[USB_DEVICE][i];
        if (!d || d->address != addr || d->ctrl != (struct usb_controller *)e) continue;
        uint8_t dm = d->desc.bMaxPacketSize0;
        if (dm == 8 || dm == 16 || dm == 32 || dm == 64) return dm;
        break;
    }

    return (ehci_dev_speed[ri][addr & 0x7Fu] == 2) ? 8u : 64u;
}

static inline uint64_t get_hhdm_offset(void) {
    return hhdm_req.response ? hhdm_req.response->offset : 0xffff800000000000ULL;
}

static inline uint32_t ehci_read(struct ehci_controller *e, uint32_t r) {
    return *(volatile uint32_t *)(uintptr_t)(e->op_base + get_hhdm_offset() + r);
}
static inline void ehci_write(struct ehci_controller *e, uint32_t r, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(e->op_base + get_hhdm_offset() + r) = v;
}
static inline uint32_t ehci_cap_read(struct ehci_controller *e, uint32_t r) {
    return *(volatile uint32_t *)(uintptr_t)(e->cap_base + get_hhdm_offset() + r);
}
static inline void ehci_cap_write(struct ehci_controller *e, uint32_t r, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(e->cap_base + get_hhdm_offset() + r) = v;
}

static struct ehci_qtd *alloc_qtd(int ri) {
    for (int i = 0; i < EHCI_QTD_POOL_SIZE; i++) {
        if (!ehci_resources[ri].qtd_used[i]) {
            ehci_resources[ri].qtd_used[i] = 1;
            struct ehci_qtd *q = &ehci_resources[ri].qtd_pool[i];
            q->next_qtd = 1;
            q->alt_next_qtd = 1;
            q->token = 0;
            for (int b = 0; b < 5; b++) q->buffer[b] = 0;
            return q;
        }
    }
    return NULL;
}

static void free_qtd(int ri, struct ehci_qtd *qtd) {
    uintptr_t start = (uintptr_t)ehci_resources[ri].qtd_pool;
    int idx = (int)(((uintptr_t)qtd - start) / sizeof(struct ehci_qtd));
    if (idx >= 0 && idx < EHCI_QTD_POOL_SIZE)
        ehci_resources[ri].qtd_used[idx] = 0;
}

static void ehci_bios_handoff(struct ehci_controller *e) {
    uint32_t hccparams = ehci_cap_read(e, EHCI_HCSPARAMS + 4);
    uint8_t eecp = (hccparams >> 8) & 0xFF;
    if (eecp < 0x40) {
        LOG_DEBUG("no legacy support capability, skip BIOS handoff");
        return;
    }

    uint32_t legsup;
    while (eecp) {
        legsup = ehci_cap_read(e, eecp);
        if ((legsup & 0xFF) == 1) break;
        eecp = (legsup >> 8) & 0xFF;
    }
    if (!eecp) {
        LOG_DEBUG("no USB Legacy Support cap found in ext cap list");
        return;
    }

    ehci_cap_write(e, eecp, legsup | (1 << 24));
    int t = 100;
    while (1) {
        legsup = ehci_cap_read(e, eecp);
        if (!(legsup & (1 << 16)) && (legsup & (1 << 24))) {
            LOG_DEBUG("BIOS handoff: OS now owns controller");
            break;
        }
        if (t-- <= 0) {

            LOG_WARNING("BIOS handoff timed out, forcing OS ownership");
            ehci_cap_write(e, eecp + 4, 0);
            return;
        }
        delay_ms(10);
    }

    uint32_t legctlsts = ehci_cap_read(e, eecp + 4);
    ehci_cap_write(e, eecp + 4, legctlsts & 0xFFFF0000u);
    LOG_DEBUG("BIOS SMI sources disabled for this controller");
}

static int ehci_init_controller(struct ehci_controller *e, int ri) {
    if (!e || e->initialized) return -1;

    uint32_t cap_len = *(volatile uint32_t *)(uintptr_t)(e->cap_base + get_hhdm_offset()) & 0xFF;
    e->op_base = e->cap_base + cap_len;

    ehci_bios_handoff(e);

    ehci_write(e, EHCI_USBCMD, (1 << 1));
    int t = 500;
    while ((ehci_read(e, EHCI_USBCMD) & (1 << 1)) && t > 0) { delay_ms(1); t--; }
    if (!t) {
        LOG_ERROR("controller %d reset (HCRESET) timed out", ri);
        return -1;
    }

    ehci_write(e, EHCI_USBINTR, 0x37);
    uint32_t hcs = ehci_cap_read(e, EHCI_HCSPARAMS);
    e->num_ports = hcs & 0xF;

    struct ehci_qh *qh = &ehci_resources[ri].async_head;
    uint32_t phys_qh = (uint32_t)mm_ptr_to_phys(qh);

    qh->horiz_link = phys_qh | 2;
    qh->characteristics = (1 << 15) | (2 << 12);
    qh->cur_link = 0;
    qh->next_link = 1;
    qh->alt_link = 1;
    qh->token = 0;
    for (int b = 0; b < 5; b++) qh->buffer[b] = 0;

    ehci_write(e, EHCI_ASYNCLISTADDR, phys_qh);

    for (int _s = 0; _s < MAX_EHCI_HID_SLOTS; _s++) {
        struct ehci_qh *iqh = &ehci_resources[ri].intr_qh_pool[_s];
        iqh->horiz_link = (_s < MAX_EHCI_HID_SLOTS - 1)
                               ? ((uint32_t)mm_ptr_to_phys(&ehci_resources[ri].intr_qh_pool[_s + 1]) | 0x2)
                               : 1u;
        iqh->characteristics = (2 << 12);
        iqh->cur_link = 0;
        iqh->next_link = 1;
        iqh->alt_link = 1;
        iqh->token = 0;
        for (int b = 0; b < 5; b++) iqh->buffer[b] = 0;
    }
    uint32_t phys_iqh = (uint32_t)mm_ptr_to_phys(&ehci_resources[ri].intr_qh_pool[0]);
    asm volatile("mfence" ::: "memory");

    for (int i = 0; i < EHCI_FRAME_LIST_SIZE; i++)
        ehci_resources[ri].frame_list[i] = phys_iqh | 0x2;
    asm volatile("mfence" ::: "memory");

    ehci_write(e, EHCI_PERIODICLISTBASE, (uint32_t)mm_ptr_to_phys(ehci_resources[ri].frame_list));

    ehci_write(e, EHCI_USBCMD, (1 << 0) | (1 << 4) | (1 << 5));

    t = 1000;
    while ((ehci_read(e, EHCI_USBSTS) & (1 << 12)) && t > 0) { delay_ms(1); t--; }
    if (!t) LOG_WARNING("controller %d still halted after run, USBSTS=0x%08x", ri, ehci_read(e, EHCI_USBSTS));

#ifndef EHCI_CONFIGFLAG
#define EHCI_CONFIGFLAG 0x40
#endif
    ehci_write(e, EHCI_CONFIGFLAG, 1u);
    delay_ms(200);

    if (hcs & (1u << 4)) {
        for (uint8_t p = 1; p <= e->num_ports; p++) {
            uint32_t preg = EHCI_PORTSC + (uint32_t)(p - 1) * 4u;
            uint32_t pv = ehci_read(e, preg);
            if (!(pv & (1u << 12))) {
                pv &= ~((1u << 1) | (1u << 3) | (1u << 5));
                ehci_write(e, preg, pv | (1u << 12));
            }
        }
        delay_ms(100);
    }

    e->initialized = 1;
    LOG_INFO("controller %d ready, cap base 0x%x, %u ports%s", ri, e->cap_base,
             (unsigned)e->num_ports, (hcs & (1u << 4)) ? ", port power control" : "");
    return 0;
}

static int ehci_reset_port(struct ehci_controller *e, uint8_t port) {
    if (!e || port == 0) return -1;
    uint32_t reg = EHCI_PORTSC + (port - 1) * 4;
    uint32_t v = ehci_read(e, reg);

    if (!(v & (1 << 0))) return -1;

    v &= ~(1u << 2);
    v |= (1u << 8);
    ehci_write(e, reg, v);
    delay_ms(50);

    v = ehci_read(e, reg);
    v &= ~(1u << 8);
    ehci_write(e, reg, v);

    int t = 100;
    while ((ehci_read(e, reg) & (1u << 8)) && t > 0) { delay_ms(1); t--; }

    t = 50;
    while (t > 0) { delay_ms(2); if (ehci_read(e, reg) & (1u << 2)) break; t--; }

    delay_ms(10);
    v = ehci_read(e, reg);

    if (!(v & (1u << 2))) {

        return -1;
    }

    return 0;
}

static void ehci_reset_qh_overlay(struct ehci_qh *qh) {

    uint32_t dt_preserve = qh->token & (1u << 31);
    qh->cur_link = 0;
    qh->alt_link = 1;
    qh->token = dt_preserve;
    for (int b = 0; b < 5; b++) qh->buffer[b] = 0;
    asm volatile("mfence" ::: "memory");
}

static int ehci_control_transfer_impl(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *setup_packet, uint16_t setup_len, void *data, uint16_t data_len, uint8_t direction)
{
    (void)setup_len;
    if (!e || !e->initialized) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (&ehci_resources[i].ctrl == e) { ri = i; break; }
    }
    if (ri == -1) return -1;

    struct ehci_qtd *qtd_setup = alloc_qtd(ri);
    if (!qtd_setup) return -2;
    struct ehci_qtd *qtd_status = alloc_qtd(ri);
    if (!qtd_status) { free_qtd(ri, qtd_setup); return -2; }
    struct ehci_qtd *qtd_data = NULL;

    qtd_setup->alt_next_qtd = 1;
    qtd_setup->token = 0x80 | (EHCI_PID_SETUP << 8) | (3 << 10) | (8 << 16);
    qtd_setup->buffer[0] = (uint32_t)mm_ptr_to_phys(setup_packet);

    if (data_len > 0) {
        qtd_data = alloc_qtd(ri);
        if (!qtd_data) {
            free_qtd(ri, qtd_setup);
            free_qtd(ri, qtd_status);
            return -2;
        }
        qtd_setup->next_qtd = (uint32_t)mm_ptr_to_phys(qtd_data);
        qtd_data->next_qtd = (uint32_t)mm_ptr_to_phys(qtd_status);
        qtd_data->alt_next_qtd = 1;
        uint8_t pid = direction ? EHCI_PID_IN : EHCI_PID_OUT;
        qtd_data->token = 0x80 | (pid << 8) | (3 << 10) | (data_len << 16) | (1u << 31);
        if (direction) {
            for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
                asm volatile("clflush (%0)" : : "r"(a) : "memory");
            asm volatile("mfence" ::: "memory");
        }
        qtd_data->buffer[0] = (uint32_t)mm_ptr_to_phys(data);

        {
            uint32_t page0_bytes = 0x1000u - (qtd_data->buffer[0] & 0xFFFu);
            uint32_t base_phys = qtd_data->buffer[0] & ~0xFFFu;
            for (int pg = 1; pg < 5; pg++) {
                if (page0_bytes + (uint32_t)(pg - 1) * 0x1000u >= (uint32_t)data_len)
                    break;
                qtd_data->buffer[pg] = base_phys + (uint32_t)pg * 0x1000u;
            }
        }
    } else {
        qtd_setup->next_qtd = (uint32_t)mm_ptr_to_phys(qtd_status);
    }

    uint8_t pid_s = (data_len > 0 && direction) ? EHCI_PID_OUT : EHCI_PID_IN;
    qtd_status->next_qtd = 1;
    qtd_status->alt_next_qtd = 1;
    qtd_status->token = 0x80 | (pid_s << 8) | (3 << 10) | (1u << 31);

    uint32_t mps = ehci_control_mps(e, ri, dev_addr);

    struct ehci_qh *qh = &ehci_resources[ri].async_head;
    uint32_t ctl_caps = 0;
    qh->characteristics = ehci_endpoint_chars(ri, dev_addr, endpoint, mps, 1, 0, &ctl_caps)
                        | (1u << 14)
                        | (1u << 15);
    qh->caps_overlay = ctl_caps;
    ehci_reset_qh_overlay(qh);
    qh->next_link = (uint32_t)mm_ptr_to_phys(qtd_setup);
    asm volatile("mfence" ::: "memory");

    {
        uint32_t cmd = ehci_read(e, EHCI_USBCMD);
        if (!(cmd & (1 << 5))) {
            ehci_write(e, EHCI_ASYNCLISTADDR,
                       (uint32_t)mm_ptr_to_phys(&ehci_resources[ri].async_head));
            ehci_write(e, EHCI_USBCMD, cmd | (1 << 5));
        }
    }
    asm volatile("mfence" ::: "memory");

    int timeout = 2000;
    while (timeout > 0) {

        volatile uint32_t tok_s = qtd_setup->token;
        if ((tok_s & 0x40) && (tok_s & 0x80) == 0) {

            qh->next_link = 1;
            free_qtd(ri, qtd_setup);
            if (qtd_data) free_qtd(ri, qtd_data);
            free_qtd(ri, qtd_status);
            LOG_ERROR("control xfer addr=%u ep=%u: setup qTD halted, token=0x%08x",
                      (unsigned)dev_addr, (unsigned)endpoint, tok_s);
            return -5;
        }
        volatile uint32_t token = qtd_status->token;
        if (token & 0x40) {
            qh->next_link = 1;
            free_qtd(ri, qtd_setup);
            if (qtd_data) free_qtd(ri, qtd_data);
            free_qtd(ri, qtd_status);
            LOG_ERROR("control xfer addr=%u ep=%u: status qTD halted, token=0x%08x",
                      (unsigned)dev_addr, (unsigned)endpoint, token);
            return -3;
        }
        if (!(token & 0x80)) {
            if (direction && data_len > 0) {
                for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
                    asm volatile("clflush (%0)" : : "r"(a) : "memory");
                asm volatile("mfence" ::: "memory");
                asm volatile("lfence" ::: "memory");
            }
            break;
        }
        delay_ms(1);
        timeout--;
    }

    qh->next_link = 1;
    free_qtd(ri, qtd_setup);
    if (qtd_data) free_qtd(ri, qtd_data);
    free_qtd(ri, qtd_status);

    if (timeout <= 0) {
        LOG_ERROR("control xfer addr=%u ep=%u: timed out", (unsigned)dev_addr, (unsigned)endpoint);
    }
    return (timeout <= 0) ? -4 : 0;
}

static int ehci_control_transfer(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *setup_packet, uint16_t setup_len, void *data, uint16_t data_len, uint8_t direction)
{
    if (!e) return -1;
    spin_lock(&e->lock);
    int ret = ehci_control_transfer_impl(e, dev_addr, endpoint, setup_packet, setup_len, data, data_len, direction);
    spin_unlock(&e->lock);
    return ret;
}

static int ehci_interrupt_transfer_impl(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!e || !e->initialized) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (&ehci_resources[i].ctrl == e) { ri = i; break; }
    }
    if (ri == -1) return -1;

    int slot_idx = -1;
    for (int _s = 0; _s < MAX_EHCI_HID_SLOTS; _s++) {
        struct ehci_pending_xfer *_p = &s_ehci_pending[ri][_s];
        if ((_p->active || _p->result_ready) &&
            _p->addr == dev_addr && _p->endpoint == endpoint) { slot_idx = _s; break; }
    }
    if (slot_idx < 0) {

        for (int _s = 0; _s < MAX_EHCI_HID_SLOTS; _s++) {
            if (!s_ehci_pending[ri][_s].active && !s_ehci_pending[ri][_s].result_ready) { slot_idx = _s; break; }
        }
    }
    if (slot_idx < 0) {
        for (int _s = 0; _s < MAX_EHCI_HID_SLOTS; _s++) {
            if (!s_ehci_pending[ri][_s].active) { slot_idx = _s; break; }
        }
    }
    if (slot_idx < 0) return -2;

    struct ehci_pending_xfer *pend = &s_ehci_pending[ri][slot_idx];
    struct ehci_qh *iqh = &ehci_resources[ri].intr_qh_pool[slot_idx];

    if (pend->active) {
        volatile uint32_t token = pend->qtd->token;
        if (!(token & 0x80)) {

            int ok = !(token & 0x40);
            if (ok && pend->direction) {
                for (uint64_t a = (uint64_t)pend->data;
                     a < (uint64_t)pend->data + pend->data_len; a += 64)
                    asm volatile("clflush (%0)" :: "r"(a) : "memory");
                asm volatile("mfence" ::: "memory");
            }

            iqh->next_link = 1;
            free_qtd(ri, pend->qtd);

            usb_event_t evt = {
                .type = ok ? USB_EVENT_TRANSFER_DONE
                                      : USB_EVENT_TRANSFER_ERR,
                .src = USB_SRC_EHCI,
                .transfer_type = USB_XFER_INTERRUPT,
                .slot_id = (uint8_t)ri,
                .endpoint = pend->endpoint,
                .completion_code = ok ? 0 : 0xFF,
                .data = ok ? pend->data : NULL,
                .data_len = ok ? pend->data_len : 0,
                .device = pend->device,
                .cookie = pend->cookie,
            };
            usb_push_event(&evt);
            pend->active = 0;
            pend->result_ready = 1;
            pend->last_result = ok ? 0 : -1;
        } else {

            return -2;
        }
    }

    if (pend->result_ready) {
        pend->result_ready = 0;
        if (pend->data == data && pend->addr == dev_addr && pend->endpoint == endpoint)
            return pend->last_result;
    }

    if (direction) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
            asm volatile("clflush (%0)" :: "r"(a) : "memory");
        asm volatile("mfence" ::: "memory");
    }

    struct ehci_qtd *qtd = alloc_qtd(ri);
    if (!qtd) return -2;

    uint8_t ep_num = endpoint & 0x0F;
    uint8_t pid = direction ? EHCI_PID_IN : EHCI_PID_OUT;

    qtd->next_qtd = 1;
    qtd->alt_next_qtd = 1;

    qtd->token = 0x80 | (pid << 8) | (3 << 10) | (data_len << 16) | (1 << 15);
    qtd->buffer[0] = (uint32_t)mm_ptr_to_phys(data);

    uint32_t ehci_mps = 8u;
    for (int _mi = 0; _mi < MAX_USB_DEVICES; _mi++) {
        struct usb_device *_md = (struct usb_device *)device_table[USB_DEVICE][_mi];
        if (_md && (uint8_t)_md->address == dev_addr && _md->ctrl == (struct usb_controller *)e) {

            if (_md->device_class == 0x09 && _md->hub_status_ep == endpoint) {
                if (_md->hub_status_ep_mps) ehci_mps = _md->hub_status_ep_mps;
            } else if (_md->max_packet_size) {
                ehci_mps = _md->max_packet_size;
            }
            break;
        }
    }
    uint32_t int_caps = 0;
    iqh->characteristics = ehci_endpoint_chars(ri, dev_addr, ep_num, ehci_mps, 0, 1, &int_caps);
    iqh->caps_overlay = int_caps;
    ehci_reset_qh_overlay(iqh);
    iqh->next_link = (uint32_t)mm_ptr_to_phys(qtd);
    asm volatile("mfence" ::: "memory");

    pend->ctrl = e;
    pend->qtd = qtd;
    pend->data = data;
    pend->data_len = data_len;
    pend->direction = direction;
    pend->ri = ri;
    pend->endpoint = endpoint;
    pend->addr = dev_addr;
    pend->result_ready = 0;

    pend->device = NULL;
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && _d->address == dev_addr &&
            _d->ctrl == (struct usb_controller *)e) {
            pend->device = _d;
            break;
        }
    }
    pend->cookie = pend->device;
    pend->active = 1;

    return -2;
}

static int ehci_interrupt_transfer(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!e) return -1;
    spin_lock(&e->lock);
    int ret = ehci_interrupt_transfer_impl(e, dev_addr, endpoint, data, data_len, direction);
    spin_unlock(&e->lock);
    return ret;
}

void ehci_irq(void) {
    for (int i = 0; i < ehci_controller_count; i++) {
        struct ehci_controller *e = &ehci_resources[i].ctrl;

        uint32_t status = ehci_read(e, EHCI_USBSTS);
        if (!status) continue;
        ehci_write(e, EHCI_USBSTS, status & 0x3F);

        if (status & (1u << 4))
            LOG_ERROR("controller %d: host system error, USBSTS=0x%08x", i, status);

        if (!(status & 0x03)) continue;

        for (int _s = 0; _s < MAX_EHCI_HID_SLOTS; _s++) {
        struct ehci_pending_xfer *pend = &s_ehci_pending[i][_s];
        if (!pend->active) continue;

        volatile uint32_t token = pend->qtd->token;
        if (token & 0x80) continue;

        int ok = !(token & 0x40);
        if (ok && pend->direction) {
            for (uint64_t a = (uint64_t)pend->data;
                 a < (uint64_t)pend->data + pend->data_len; a += 64)
                asm volatile("clflush (%0)" :: "r"(a) : "memory");
            asm volatile("mfence" ::: "memory");
        }

        struct ehci_qh *iqh = &ehci_resources[i].intr_qh_pool[_s];
        iqh->next_link = 1;
        free_qtd(i, pend->qtd);

        usb_event_t evt = {
            .type = ok ? USB_EVENT_TRANSFER_DONE
                                  : USB_EVENT_TRANSFER_ERR,
            .src = USB_SRC_EHCI,
            .transfer_type = USB_XFER_INTERRUPT,
            .slot_id = (uint8_t)i,
            .endpoint = pend->endpoint,
            .completion_code = ok ? 0 : 0xFF,
            .data = ok ? pend->data : NULL,
            .data_len = ok ? pend->data_len : 0,
            .device = pend->device,
            .cookie = pend->cookie,
        };
        usb_push_event(&evt);
        pend->active = 0;
        pend->result_ready = 1;
        pend->last_result = ok ? 0 : -1;
        }
    }
}

static void ehci_scan_pci(void) {
    struct usb_controller *ctrls = pci_get_usb_controllers();
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (ctrls[i].type != USB_TYPE_EHCI) continue;

        struct ehci_controller *e = &ehci_resources[ehci_controller_count].ctrl;
        e->type = USB_TYPE_EHCI;
        e->cap_base = (uint32_t)ctrls[i].base_addr;
        e->initialized = 0;

        pci_enable_bus_mastering(ctrls[i].pci);

        if (ehci_init_controller(e, ehci_controller_count) == 0)
            ehci_controller_count++;
    }
}

static struct ehci_controller *ehci_get_ctrl(int idx) {
    return (idx >= 0 && idx < ehci_controller_count)
           ? &ehci_resources[idx].ctrl : NULL;
}
static int ehci_get_count(void) { return ehci_controller_count; }

static int ehci_enumerate(struct ehci_controller *e, uint8_t p) {
    if (!e || p == 0) return -1;
    return ehci_reset_port(e, p);
}
static void ehci_start(struct ehci_controller *e) {
    if (e) ehci_write(e, EHCI_USBCMD, ehci_read(e, EHCI_USBCMD) | (1 << 0) | (1 << 4) | (1 << 5));
}
static void ehci_stop(struct ehci_controller *e) {
    if (e) ehci_write(e, EHCI_USBCMD, ehci_read(e, EHCI_USBCMD) & ~((1 << 0) | (1 << 4) | (1 << 5)));
}

typedef struct ehci_bulk_pending {
    struct ehci_controller *ctrl;
    struct ehci_qtd *qtd;
    void *data;
    uint16_t data_len;
    uint8_t direction;
    uint8_t active;
    int ri;
    uint8_t endpoint;
    uint8_t last_ep_num;
    uint8_t last_dir;
    struct usb_device *device;
    void *cookie;
} ehci_bulk_pending;

static struct ehci_bulk_pending s_ehci_bulk_pending[MAX_EHCI_CONTROLLERS];

#define EHCI_DT_EP_DIRS 32
static uint8_t ehci_bulk_dt[MAX_EHCI_CONTROLLERS][128][EHCI_DT_EP_DIRS];

static int ehci_bulk_transfer_impl(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!e || !e->initialized) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (&ehci_resources[i].ctrl == e) { ri = i; break; }
    }
    if (ri == -1) return -1;

    struct ehci_bulk_pending *pend = &s_ehci_bulk_pending[ri];

    if (pend->active) {
        volatile uint32_t token = pend->qtd->token;
        if (!(token & 0x80)) {

            int ok = !(token & 0x40);

            struct ehci_qh *qh = &ehci_resources[ri].async_head;

            if (ok) {
                uint8_t sv_ep = pend->last_ep_num & 0x0Fu;
                uint8_t sv_dir = pend->last_dir & 0x01u;
                uint8_t sv_idx = (uint8_t)((sv_ep * 2u) + sv_dir);
                uint8_t sv_da = pend->device
                                 ? (uint8_t)(pend->device->address & 0x7Fu)
                                 : (uint8_t)(dev_addr & 0x7Fu);
                if (sv_idx < EHCI_DT_EP_DIRS && sv_da < 128u)
                    ehci_bulk_dt[ri][sv_da][sv_idx] =
                        (uint8_t)((qh->token >> 31) & 1u);
            }

            qh->next_link = 1;

            if (ok && pend->direction) {
                for (uint64_t a = (uint64_t)pend->data;
                     a < (uint64_t)pend->data + pend->data_len; a += 64)
                    asm volatile("clflush (%0)" :: "r"(a) : "memory");
                asm volatile("mfence" ::: "memory");
                asm volatile("lfence" ::: "memory");
            }
            free_qtd(ri, pend->qtd);

            usb_event_t evt = {
                .type = ok ? USB_EVENT_BULK_DONE : USB_EVENT_BULK_ERR,
                .src = USB_SRC_EHCI,
                .transfer_type = USB_XFER_BULK,
                .slot_id = (uint8_t)ri,
                .endpoint = pend->endpoint,
                .completion_code = ok ? 0 : 0xFF,
                .data = ok ? pend->data : NULL,
                .data_len = ok ? pend->data_len : 0,
                .device = pend->device,
                .cookie = pend->cookie,
            };
            usb_push_bulk_event(&evt);
            pend->active = 0;
            return ok ? 0 : -1;
        }

        return -2;
    }

    if (direction) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
            asm volatile("clflush (%0)" :: "r"(a) : "memory");
        asm volatile("mfence" ::: "memory");
    }

    struct ehci_qtd *qtd = alloc_qtd(ri);
    if (!qtd) return -2;

    uint8_t ep_num = endpoint & 0x0F;
    uint8_t pid = direction ? EHCI_PID_IN : EHCI_PID_OUT;

    qtd->next_qtd = 1;
    qtd->alt_next_qtd = 1;

    qtd->token = 0x80u | ((uint32_t)pid << 8) | (3u << 10)
                   | ((uint32_t)data_len << 16) | (1u << 15);
    qtd->buffer[0] = (uint32_t)mm_ptr_to_phys(data);

    {
        uint32_t page0_bytes = 0x1000u - (qtd->buffer[0] & 0xFFFu);
        uint32_t base_phys = qtd->buffer[0] & ~0xFFFu;
        for (int pg = 1; pg < 5; pg++) {
            if (page0_bytes + (uint32_t)(pg - 1) * 0x1000u >= (uint32_t)data_len)
                break;
            qtd->buffer[pg] = base_phys + (uint32_t)pg * 0x1000u;
        }
    }

    uint16_t bulk_mps = 512u;
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && _d->address == dev_addr &&
            _d->ctrl == (struct usb_controller *)e) {
            for (int _bi = 0; _bi < _d->bulk_ep_count; _bi++) {
                uint8_t ba = _d->bulk_ep[_bi].address;
                if ((ba & 0x0Fu) != ep_num) continue;
                if (((ba & 0x80u) ? 1u : 0u) != (direction ? 1u : 0u)) continue;
                if (_d->bulk_ep[_bi].max_packet_size)
                    bulk_mps = _d->bulk_ep[_bi].max_packet_size;
                break;
            }
            break;
        }
    }
    if (bulk_mps == 0u || bulk_mps > 1024u) bulk_mps = 512u;
    if (ehci_dev_speed[ri][dev_addr & 0x7Fu] == 1 && bulk_mps > 64u) bulk_mps = 64u;

    struct ehci_qh *qh = &ehci_resources[ri].async_head;
    uint32_t bulk_caps = 0;
    qh->characteristics = ehci_endpoint_chars(ri, dev_addr, ep_num, bulk_mps, 0, 0, &bulk_caps)
                        | (1u << 15);
    qh->caps_overlay = bulk_caps;

    {
        uint8_t dt_da = (uint8_t)(dev_addr & 0x7Fu);
        uint8_t dt_idx = (uint8_t)((ep_num * 2u) + (direction ? 1u : 0u));
        uint8_t dt_val = (dt_da < 128u && dt_idx < EHCI_DT_EP_DIRS)
                         ? ehci_bulk_dt[ri][dt_da][dt_idx] : 0u;
        uint32_t dt_preserve = (uint32_t)dt_val << 31;
        qh->cur_link = 0;
        qh->alt_link = 1;
        qh->token = dt_preserve;
        for (int _b = 0; _b < 5; _b++) qh->buffer[_b] = 0;
    }
    asm volatile("mfence" ::: "memory");
    qh->next_link = (uint32_t)mm_ptr_to_phys(qtd);
    asm volatile("mfence" ::: "memory");

    pend->ctrl = e;
    pend->qtd = qtd;
    pend->data = data;
    pend->data_len = data_len;
    pend->direction = direction;
    pend->active = 1;
    pend->ri = ri;
    pend->endpoint = endpoint;
    pend->last_ep_num = ep_num;
    pend->last_dir = direction ? 1u : 0u;

    pend->device = NULL;
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && _d->address == dev_addr &&
            _d->ctrl == (struct usb_controller *)e) {
            pend->device = _d;
            break;
        }
    }
    pend->cookie = pend->device;

    return -2;
}

static int ehci_bulk_transfer(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!e) return -1;
    spin_lock(&e->lock);
    int ret = ehci_bulk_transfer_impl(e, dev_addr, endpoint, data, data_len, direction);
    spin_unlock(&e->lock);
    return ret;
}

#define EHCI_ISO_STREAMS 4
#define EHCI_ISO_ITDS 128
#define EHCI_ISO_SITDS 128
#define EHCI_ISO_LEAD_FRAMES 4
#define EHCI_FRINDEX 0x0C

typedef struct ehci_sitd {
    uint32_t next_link;
    uint32_t ep_chars;
    uint32_t uframe_ctrl;
    uint32_t state;
    uint32_t buffer[2];
    uint32_t back_link;
    uint32_t ext_buffer[2];
} __attribute__((packed, aligned(32))) ehci_sitd;

typedef struct ehci_iso_desc_meta {
    struct usb_iso_request *req;
    uint16_t first_pkt;
    uint16_t frame;
    uint8_t count;
    uint8_t used;
    uint8_t linked;
    uint8_t slot_of[8];
} ehci_iso_desc_meta;

typedef struct ehci_iso_stream {
    uint8_t in_use;
    uint8_t addr;
    uint8_t endpoint;
    uint8_t hs;
    uint8_t started;
    uint8_t mult;
    uint8_t interval_uf;
    uint8_t per_frame;
    uint16_t mps;
    uint16_t frame_step;
    uint16_t next_frame;
    uint16_t q_head;
    uint16_t q_tail;
    uint8_t q_kind[EHCI_ISO_SITDS];
    uint8_t queue[EHCI_ISO_SITDS];
} ehci_iso_stream;

static struct ehci_iso_state {
    struct ehci_itd itd[EHCI_ISO_ITDS] __attribute__((aligned(32)));
    struct ehci_sitd sitd[EHCI_ISO_SITDS] __attribute__((aligned(32)));
    ehci_iso_desc_meta itd_meta[EHCI_ISO_ITDS];
    ehci_iso_desc_meta sitd_meta[EHCI_ISO_SITDS];
    ehci_iso_stream st[EHCI_ISO_STREAMS];
    spinlock_t lock;
} s_ehci_iso[MAX_EHCI_CONTROLLERS];

static int ehci_res_of(struct usb_device *dev) {
    if (!dev || !dev->ctrl || dev->ctrl->type != USB_TYPE_EHCI) return -1;
    int ri = ehci_controller_index((struct ehci_controller *)dev->ctrl);
    if (ri < 0 || !ehci_resources[ri].ctrl.initialized) return -1;
    return ri;
}

static inline uint16_t ehci_cur_frame(int ri) {
    return (uint16_t)((ehci_read(&ehci_resources[ri].ctrl, EHCI_FRINDEX) >> 3) & 0x7FFu);
}

static void ehci_iso_flush(const void *buf, uint32_t len) {
    if (!buf || !len) return;
    for (uint64_t a = (uint64_t)buf & ~63ull; a < (uint64_t)buf + len; a += 64)
        asm volatile("clflush (%0)" :: "r"(a) : "memory");
    asm volatile("mfence" ::: "memory");
}

static void ehci_iso_unlink(int ri, uint8_t kind, int di) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    ehci_iso_desc_meta *m = kind ? &is->sitd_meta[di] : &is->itd_meta[di];
    if (!m->linked) return;
    uint32_t target = kind ? (uint32_t)mm_ptr_to_phys(&is->sitd[di]) : (uint32_t)mm_ptr_to_phys(&is->itd[di]);
    uint32_t itd_lo = (uint32_t)mm_ptr_to_phys(&is->itd[0]);
    uint32_t itd_hi = itd_lo + (uint32_t)sizeof(struct ehci_itd) * EHCI_ISO_ITDS;
    uint32_t sitd_lo = (uint32_t)mm_ptr_to_phys(&is->sitd[0]);
    uint32_t sitd_hi = sitd_lo + (uint32_t)sizeof(struct ehci_sitd) * EHCI_ISO_SITDS;
    uint32_t next = kind ? is->sitd[di].next_link : is->itd[di].next_link;
    uint16_t slot = m->frame & (EHCI_FRAME_LIST_SIZE - 1u);

    int prev_kind = -1;
    int prev = -1;
    for (int guard = 0; guard < EHCI_ISO_ITDS + EHCI_ISO_SITDS; guard++) {
        uint32_t v = (prev_kind < 0) ? ehci_resources[ri].frame_list[slot]
                   : (prev_kind ? is->sitd[prev].next_link : is->itd[prev].next_link);
        if (v & 1u) break;
        uint32_t type = (v >> 1) & 3u;
        uint32_t ph = v & ~0x1Fu;
        if (ph == target) {
            if (prev_kind < 0) ehci_resources[ri].frame_list[slot] = next;
            else if (prev_kind) is->sitd[prev].next_link = next;
            else is->itd[prev].next_link = next;
            asm volatile("mfence" ::: "memory");
            break;
        }
        if (type == 0 && ph >= itd_lo && ph < itd_hi) {
            prev_kind = 0;
            prev = (int)((ph - itd_lo) / sizeof(struct ehci_itd));
        } else if (type == 2 && ph >= sitd_lo && ph < sitd_hi) {
            prev_kind = 1;
            prev = (int)((ph - sitd_lo) / sizeof(struct ehci_sitd));
        } else {
            break;
        }
    }
    m->linked = 0;
}

static void ehci_iso_link(int ri, uint8_t kind, int di, uint16_t frame) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    uint16_t slot = frame & (EHCI_FRAME_LIST_SIZE - 1u);
    uint32_t phys = kind ? ((uint32_t)mm_ptr_to_phys(&is->sitd[di]) | (2u << 1)) : (uint32_t)mm_ptr_to_phys(&is->itd[di]);
    if (kind) is->sitd[di].next_link = ehci_resources[ri].frame_list[slot];
    else is->itd[di].next_link = ehci_resources[ri].frame_list[slot];
    asm volatile("mfence" ::: "memory");
    ehci_resources[ri].frame_list[slot] = phys;
    asm volatile("mfence" ::: "memory");
    if (kind) is->sitd_meta[di].linked = 1;
    else is->itd_meta[di].linked = 1;
}

static bool ehci_iso_desc_active(int ri, uint8_t kind, int di) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    if (kind) return (is->sitd[di].state & 0x80u) != 0;
    ehci_iso_desc_meta *m = &is->itd_meta[di];
    for (uint8_t k = 0; k < 8; k++)
        if (m->slot_of[k] != 0xFF && (is->itd[di].transaction[k] & (1u << 31))) return true;
    return false;
}

static void ehci_iso_complete(int ri, uint8_t kind, int di, bool cancelled) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    ehci_iso_desc_meta *m = kind ? &is->sitd_meta[di] : &is->itd_meta[di];
    ehci_iso_unlink(ri, kind, di);
    struct usb_iso_request *req = m->req;
    if (req && !req->done) {
        bool in = (req->endpoint & 0x80u) != 0;
        if (kind) {
            uint32_t st = is->sitd[di].state;
            uint16_t pi = m->first_pkt;
            if (pi < req->n_packets) {
                bool ok = !cancelled && !(st & 0xFCu);
                uint16_t remain = (uint16_t)((st >> 16) & 0x3FFu);
                uint16_t actual = ok ? (in ? (uint16_t)(remain >= req->lens[pi] ? 0 : req->lens[pi] - remain) : req->lens[pi]) : 0;
                req->actual[pi] = actual;
                if (!ok) req->errors++;
                req->completed++;
            }
            is->sitd[di].state = 0;
        } else {
            for (uint8_t k = 0; k < 8; k++) {
                uint8_t rel = m->slot_of[k];
                if (rel == 0xFF) continue;
                uint16_t pi = (uint16_t)(m->first_pkt + rel);
                if (pi >= req->n_packets) continue;
                uint32_t t = is->itd[di].transaction[k];
                bool ok = !cancelled && !(t & (0xFu << 28));
                uint16_t actual = 0;
                if (ok) actual = in ? (uint16_t)((t >> 16) & 0xFFFu) : req->lens[pi];
                if (actual > req->lens[pi]) actual = req->lens[pi];
                req->actual[pi] = actual;
                if (!ok) req->errors++;
                req->completed++;
                is->itd[di].transaction[k] = 0;
            }
        }
        if (cancelled) req->status = USB_ISO_CANCELLED;
        if (cancelled || req->completed >= req->n_packets) {
            if (in) ehci_iso_flush(req->data, req->length);
            req->queued = 0;
            req->done = 1;
        }
    }
    m->req = NULL;
}

static void ehci_iso_reap(int ri, int si, bool force) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    ehci_iso_stream *st = &is->st[si];
    uint16_t cur = ehci_cur_frame(ri);
    while (st->q_tail != st->q_head) {
        uint8_t kind = st->q_kind[st->q_tail % EHCI_ISO_SITDS];
        int di = st->queue[st->q_tail % EHCI_ISO_SITDS];
        ehci_iso_desc_meta *m = kind ? &is->sitd_meta[di] : &is->itd_meta[di];
        uint16_t behind = (uint16_t)((cur - m->frame) & 0x7FFu);
        bool passed = behind != 0 && behind < 1024u;
        if (!force) {
            if (!passed) break;
            if (ehci_iso_desc_active(ri, kind, di) && behind <= 2u) break;
        }
        st->q_tail++;
        ehci_iso_complete(ri, kind, di, force);
        m->used = (behind >= 2u && behind < 1024u) ? 0 : 2;
    }
    for (int i = 0; i < EHCI_ISO_ITDS; i++) {
        ehci_iso_desc_meta *m = &is->itd_meta[i];
        uint16_t behind = (uint16_t)((cur - m->frame) & 0x7FFu);
        if (m->used == 2 && behind >= 2u && behind < 1024u) m->used = 0;
    }
    for (int i = 0; i < EHCI_ISO_SITDS; i++) {
        ehci_iso_desc_meta *m = &is->sitd_meta[i];
        uint16_t behind = (uint16_t)((cur - m->frame) & 0x7FFu);
        if (m->used == 2 && behind >= 2u && behind < 1024u) m->used = 0;
    }
}

static int ehci_iso_find_stream(int ri, uint8_t addr, uint8_t ep) {
    for (int i = 0; i < EHCI_ISO_STREAMS; i++) {
        ehci_iso_stream *st = &s_ehci_iso[ri].st[i];
        if (st->in_use && st->addr == addr && st->endpoint == ep) return i;
    }
    return -1;
}

static int ehci_iso_open(struct usb_device *dev, const struct usb_endpoint_info *ep) {
    int ri = ehci_res_of(dev);
    if (ri < 0 || !ep) return USB_ISO_ERR;
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    uint8_t a = dev->address & 0x7Fu;
    uint8_t spd = ehci_dev_speed[ri][a];
    if (spd == 2) return USB_ISO_ERR;

    uint64_t fl = spin_lock_irqsave(&is->lock);
    int si = ehci_iso_find_stream(ri, a, ep->address);
    if (si >= 0) ehci_iso_reap(ri, si, true);
    else {
        for (int i = 0; i < EHCI_ISO_STREAMS; i++)
            if (!is->st[i].in_use) { si = i; break; }
    }
    if (si < 0) { spin_unlock_irqrestore(&is->lock, fl); return USB_ISO_BUSY; }

    ehci_iso_stream *st = &is->st[si];
    st->in_use = 1;
    st->addr = a;
    st->endpoint = ep->address;
    st->hs = (spd != 1);
    st->mps = ep->max_packet_size & 0x7FFu;
    st->mult = (uint8_t)(((ep->max_packet_size >> 11) & 3u) + 1u);
    st->started = 0;
    st->q_head = st->q_tail = 0;
    if (st->hs) {
        uint8_t bi = ep->interval ? ep->interval : 1u;
        if (bi > 16) bi = 16;
        uint32_t uf = 1u << (bi - 1u);
        st->interval_uf = (uint8_t)(uf > 8u ? 8u : uf);
        st->per_frame = (uint8_t)(8u / st->interval_uf);
        st->frame_step = (uint16_t)(uf > 8u ? uf / 8u : 1u);
    } else {
        st->interval_uf = 8;
        st->per_frame = 1;
        st->frame_step = 1;
    }
    spin_unlock_irqrestore(&is->lock, fl);
    return USB_ISO_OK;
}

static int ehci_iso_alloc_desc(int ri, uint8_t kind) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    int n = kind ? EHCI_ISO_SITDS : EHCI_ISO_ITDS;
    ehci_iso_desc_meta *meta = kind ? is->sitd_meta : is->itd_meta;
    for (int i = 0; i < n; i++) {
        if (meta[i].used) continue;
        meta[i].used = 1;
        meta[i].linked = 0;
        meta[i].req = NULL;
        for (int k = 0; k < 8; k++) meta[i].slot_of[k] = 0xFF;
        return i;
    }
    return -1;
}

static int ehci_iso_free_count(int ri, uint8_t kind) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    int n = kind ? EHCI_ISO_SITDS : EHCI_ISO_ITDS;
    ehci_iso_desc_meta *meta = kind ? is->sitd_meta : is->itd_meta;
    int c = 0;
    for (int i = 0; i < n; i++) if (!meta[i].used) c++;
    return c;
}

static void ehci_iso_fill_itd(int ri, ehci_iso_stream *st, int di, struct usb_iso_request *req, uint16_t first, uint8_t count) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    struct ehci_itd *itd = &is->itd[di];
    ehci_iso_desc_meta *m = &is->itd_meta[di];
    bool in = (req->endpoint & 0x80u) != 0;
    uint32_t base = (uint32_t)mm_ptr_to_phys((uint8_t *)req->data + req->offsets[first]) & ~0xFFFu;

    for (int k = 0; k < 8; k++) itd->transaction[k] = 0;
    for (int j = 0; j < 7; j++) {
        itd->buffer_page[j] = base + (uint32_t)j * 0x1000u;
        itd->ext_buffer[j] = 0;
    }
    itd->buffer_page[0] |= (uint32_t)(st->addr & 0x7Fu) | ((uint32_t)(st->endpoint & 0x0Fu) << 8);
    itd->buffer_page[1] |= (in ? (1u << 11) : 0u) | (uint32_t)(st->mps & 0x7FFu);
    itd->buffer_page[2] |= (uint32_t)(st->mult & 3u);

    for (uint8_t i = 0; i < count; i++) {
        uint16_t pi = (uint16_t)(first + i);
        uint8_t uf = (uint8_t)(i * st->interval_uf);
        if (uf > 7) break;
        uint32_t ph = (uint32_t)mm_ptr_to_phys((uint8_t *)req->data + req->offsets[pi]);
        uint32_t pg = (ph - base) >> 12;
        bool last = (pi == req->n_packets - 1u);
        itd->transaction[uf] = (1u << 31)
                             | ((uint32_t)(req->lens[pi] & 0xFFFu) << 16)
                             | (last ? (1u << 15) : 0u)
                             | ((pg & 7u) << 12)
                             | (ph & 0xFFFu);
        m->slot_of[uf] = i;
    }
    m->req = req;
    m->first_pkt = first;
    m->count = count;
}

static void ehci_iso_fill_sitd(int ri, ehci_iso_stream *st, int di, struct usb_iso_request *req, uint16_t pi) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    struct ehci_sitd *sd = &is->sitd[di];
    ehci_iso_desc_meta *m = &is->sitd_meta[di];
    bool in = (req->endpoint & 0x80u) != 0;
    uint16_t len = req->lens[pi];
    uint32_t ph = (uint32_t)mm_ptr_to_phys((uint8_t *)req->data + req->offsets[pi]);
    uint32_t end = ph + (len ? len - 1u : 0u);

    sd->ep_chars = (in ? (1u << 31) : 0u)
                 | ((uint32_t)(ehci_dev_tt_port[ri][st->addr] & 0x7Fu) << 24)
                 | ((uint32_t)(ehci_dev_tt_hub[ri][st->addr] & 0x7Fu) << 16)
                 | ((uint32_t)(st->endpoint & 0x0Fu) << 8)
                 | (uint32_t)(st->addr & 0x7Fu);

    uint32_t tcount = 1;
    uint32_t smask, cmask;
    if (in) {
        smask = 0x01u;
        cmask = 0xFCu;
    } else {
        tcount = (len + 187u) / 188u;
        if (tcount == 0) tcount = 1;
        if (tcount > 6) tcount = 6;
        smask = (1u << tcount) - 1u;
        cmask = 0;
    }
    sd->uframe_ctrl = (cmask << 8) | smask;
    bool last = (pi == req->n_packets - 1u);
    sd->buffer[0] = ph;
    sd->buffer[1] = (end & ~0xFFFu) | (in ? 0u : (((tcount > 1) ? 1u : 0u) << 3) | tcount);
    sd->ext_buffer[0] = 0;
    sd->ext_buffer[1] = 0;
    sd->back_link = 1u;
    asm volatile("mfence" ::: "memory");
    sd->state = (last ? (1u << 31) : 0u) | ((uint32_t)(len & 0x3FFu) << 16) | 0x80u;

    m->req = req;
    m->first_pkt = pi;
    m->count = 1;
}

static int ehci_iso_submit(struct usb_device *dev, struct usb_iso_request *req) {
    int ri = ehci_res_of(dev);
    if (ri < 0 || !req) return USB_ISO_ERR;
    struct ehci_iso_state *is = &s_ehci_iso[ri];

    uint64_t fl = spin_lock_irqsave(&is->lock);
    int si = ehci_iso_find_stream(ri, dev->address & 0x7Fu, req->endpoint);
    if (si < 0) { spin_unlock_irqrestore(&is->lock, fl); return USB_ISO_ERR; }
    ehci_iso_stream *st = &is->st[si];
    ehci_iso_reap(ri, si, false);

    uint8_t kind = st->hs ? 0 : 1;
    uint16_t frames = st->hs ? (uint16_t)((req->n_packets + st->per_frame - 1u) / st->per_frame) : req->n_packets;
    if (ehci_iso_free_count(ri, kind) < frames) { spin_unlock_irqrestore(&is->lock, fl); return USB_ISO_BUSY; }
    if ((uint16_t)(st->q_head - st->q_tail) + frames >= EHCI_ISO_SITDS) { spin_unlock_irqrestore(&is->lock, fl); return USB_ISO_BUSY; }

    uint16_t cur = ehci_cur_frame(ri);
    uint16_t ahead = (uint16_t)((st->next_frame - cur) & 0x7FFu);
    if (!st->started || ahead < EHCI_ISO_LEAD_FRAMES || ahead > 900u) {
        st->next_frame = (uint16_t)((cur + EHCI_ISO_LEAD_FRAMES + (st->started ? 1u : 4u)) & 0x7FFu);
        st->started = 1;
    } else if (ahead + (uint32_t)frames * st->frame_step > 1000u) {
        spin_unlock_irqrestore(&is->lock, fl);
        return USB_ISO_BUSY;
    }

    ehci_iso_flush(req->data, req->length);
    req->queued = 1;
    req->start_frame = st->next_frame;

    uint16_t pi = 0;
    while (pi < req->n_packets) {
        int di = ehci_iso_alloc_desc(ri, kind);
        if (di < 0) break;
        uint8_t count = 1;
        if (kind == 0) {
            count = st->per_frame;
            if (pi + count > req->n_packets) count = (uint8_t)(req->n_packets - pi);
            ehci_iso_fill_itd(ri, st, di, req, pi, count);
        } else {
            ehci_iso_fill_sitd(ri, st, di, req, pi);
        }
        ehci_iso_desc_meta *m = kind ? &is->sitd_meta[di] : &is->itd_meta[di];
        m->frame = st->next_frame;
        ehci_iso_link(ri, kind, di, st->next_frame);
        st->q_kind[st->q_head % EHCI_ISO_SITDS] = kind;
        st->queue[st->q_head % EHCI_ISO_SITDS] = (uint8_t)di;
        st->q_head++;
        st->next_frame = (uint16_t)((st->next_frame + st->frame_step) & 0x7FFu);
        pi = (uint16_t)(pi + count);
    }
    spin_unlock_irqrestore(&is->lock, fl);
    return USB_ISO_OK;
}

static void ehci_iso_poll_ri(int ri) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    uint64_t fl = spin_lock_irqsave(&is->lock);
    for (int si = 0; si < EHCI_ISO_STREAMS; si++)
        if (is->st[si].in_use) ehci_iso_reap(ri, si, false);
    spin_unlock_irqrestore(&is->lock, fl);
}

static void ehci_iso_poll(struct usb_device *dev) {
    int ri = ehci_res_of(dev);
    if (ri >= 0) ehci_iso_poll_ri(ri);
}

static void ehci_iso_close(struct usb_device *dev, uint8_t endpoint) {
    int ri = ehci_res_of(dev);
    if (ri < 0) return;
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    uint64_t fl = spin_lock_irqsave(&is->lock);
    int si = ehci_iso_find_stream(ri, dev->address & 0x7Fu, endpoint);
    if (si >= 0) {
        ehci_iso_reap(ri, si, true);
        is->st[si].in_use = 0;
    }
    spin_unlock_irqrestore(&is->lock, fl);
}

static void ehci_iso_drop_device(int ri, uint8_t addr) {
    struct ehci_iso_state *is = &s_ehci_iso[ri];
    uint64_t fl = spin_lock_irqsave(&is->lock);
    for (int si = 0; si < EHCI_ISO_STREAMS; si++) {
        if (!is->st[si].in_use || is->st[si].addr != addr) continue;
        ehci_iso_reap(ri, si, true);
        is->st[si].in_use = 0;
    }
    spin_unlock_irqrestore(&is->lock, fl);
}

void ehci_reset_endpoint_toggle(struct usb_device *dev, uint8_t dev_addr, uint8_t endpoint){
    if (!dev) return;

    int _ri = -1;
    for (int _i = 0; _i < MAX_EHCI_CONTROLLERS; _i++) {
        if (&ehci_resources[_i].ctrl ==
            (struct ehci_controller *)dev->ctrl) { _ri = _i; break; }
    }
    if (_ri < 0) return;

    uint8_t da = (uint8_t)((dev_addr ? dev_addr : dev->address) & 0x7Fu);
    uint8_t ep_num = (uint8_t)(endpoint & 0x0Fu);

    for (uint8_t d = 0; d < 2u; d++) {
        uint8_t idx = (uint8_t)(ep_num * 2u + d);
        if (da < 128u && idx < EHCI_DT_EP_DIRS)
            ehci_bulk_dt[_ri][da][idx] = 0u;
    }

    struct ehci_bulk_pending *bp = &s_ehci_bulk_pending[_ri];
    struct ehci_qh *_qh = &ehci_resources[_ri].async_head;

    if (bp->active && (bp->endpoint & 0x0Fu) == ep_num) {
        _qh->next_link = 1u;
        asm volatile("mfence" ::: "memory");
        free_qtd(_ri, bp->qtd);
        bp->active = 0;
        bp->device = NULL;
        bp->cookie = NULL;
    }

    _qh->token = 0u;
    asm volatile("mfence" ::: "memory");
}

void ehci_notify_disconnect(struct usb_device *dev) {
    if (!dev) return;
    for (int ri = 0; ri < MAX_EHCI_CONTROLLERS; ri++) {
        for (int s = 0; s < MAX_EHCI_HID_SLOTS; s++) {
            struct ehci_pending_xfer *p = &s_ehci_pending[ri][s];
            if (!p->active || p->device != dev) continue;

            struct ehci_qh *iqh = &ehci_resources[ri].intr_qh_pool[s];
            iqh->next_link = 1;
            asm volatile("mfence" ::: "memory");

            free_qtd(ri, p->qtd);
            p->active = 0;
            p->device = NULL;
            p->cookie = NULL;
        }

        for (int s = 0; s < MAX_EHCI_HID_SLOTS; s++) {
            struct ehci_pending_xfer *p = &s_ehci_pending[ri][s];
            if (p->result_ready && p->addr == dev->address &&
                dev->ctrl == (struct usb_controller *)&ehci_resources[ri].ctrl)
                p->result_ready = 0;
        }

        struct ehci_bulk_pending *b = &s_ehci_bulk_pending[ri];
        if (b->active && b->device == dev) {
            ehci_resources[ri].async_head.next_link = 1;
            asm volatile("mfence" ::: "memory");
            free_qtd(ri, b->qtd);
            b->active = 0;
            b->device = NULL;
            b->cookie = NULL;
        }

        if (ri < ehci_controller_count &&
            dev->ctrl == (struct usb_controller *)&ehci_resources[ri].ctrl) {
            ehci_iso_drop_device(ri, dev->address & 0x7Fu);
        }

        if (ri < ehci_controller_count &&
            dev->ctrl == (struct usb_controller *)&ehci_resources[ri].ctrl) {
            uint8_t a = dev->address & 0x7Fu;
            ehci_dev_speed[ri][a] = 0;
            ehci_dev_tt_hub[ri][a] = 0;
            ehci_dev_tt_port[ri][a] = 0;
            ehci_dev_mps0[ri][a] = 0;
            for (int i = 0; i < EHCI_DT_EP_DIRS; i++) ehci_bulk_dt[ri][a][i] = 0;
        }
    }
}

struct ehci_driver ehci_driver_loaded = {
    .get_controller_count = ehci_get_count,
    .get_controller = ehci_get_ctrl,
    .control_transfer = ehci_control_transfer,
    .interrupt_transfer = ehci_interrupt_transfer,
    .bulk_transfer = ehci_bulk_transfer,
    .iso_open = ehci_iso_open,
    .iso_submit = ehci_iso_submit,
    .iso_poll = ehci_iso_poll,
    .iso_close = ehci_iso_close,
    .reset_port = ehci_reset_port,
    .reset_endpoint_toggle = ehci_reset_endpoint_toggle,
    .notify_disconnect = ehci_notify_disconnect,
    .enumerate_device = ehci_enumerate,
    .start = ehci_start,
    .stop = ehci_stop,
};

static void ehci_irq_legacy(struct registers *regs) { (void)regs; ehci_irq(); }

struct ehci_driver *return_ehci_driver(void) {
    pci_init();

    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);

    delay_ms = tsc->sleep_tsc_ms;

    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++)
        for (int j = 0; j < MAX_EHCI_HID_SLOTS; j++) s_ehci_pending[i][j].active = 0;

    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) { s_ehci_bulk_pending[i].active = 0; }

    struct usb_controller *u = pci_get_usb_controllers();
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (u[i].type != USB_TYPE_EHCI || !u[i].pci) continue;

        LOG_INFO("found EHCI controller at %02x:%02x.%x, base 0x%llx",
                 (unsigned)u[i].pci->bus, (unsigned)u[i].pci->slot, (unsigned)u[i].pci->func,
                 (unsigned long long)u[i].base_addr);

        struct ehci_controller *e = &ehci_resources[ehci_controller_count].ctrl;
        e->type = USB_TYPE_EHCI;
        e->cap_base = (uint32_t)u[i].base_addr;
        e->initialized = 0;

        pci_enable_bus_mastering(u[i].pci);

        if (ehci_init_controller(e, ehci_controller_count) != 0) {
            LOG_ERROR("controller at %02x:%02x.%x failed to initialize",
                      (unsigned)u[i].pci->bus, (unsigned)u[i].pci->slot, (unsigned)u[i].pci->func);
            continue;
        }

        struct pci_device *pdev = u[i].pci;
        uint8_t lapic = apic_get_lapic_id();
        uint8_t vec = (uint8_t)(MSI_VECTOR_EHCI_BASE + ehci_controller_count);

        if (msi_enable(pdev->bus, pdev->slot, pdev->func, vec, lapic, ehci_irq)) {

            LOG_INFO("controller %d: MSI enabled, vector 0x%02x", ehci_controller_count, (unsigned)vec);
            ehci_write(e, EHCI_USBINTR, 0x07);
        } else {

            uint8_t irq_vec = (uint8_t)(0x20 + pdev->irq_line);
            LOG_INFO("controller %d: legacy IRQ %u on vector 0x%02x", ehci_controller_count,
                     (unsigned)pdev->irq_line, (unsigned)irq_vec);
            ioapic_map_pci_irq(pdev->irq_line, irq_vec, lapic);
            irq_register_handler(irq_vec, ehci_irq_legacy);
            ehci_write(e, EHCI_USBINTR, 0x07);
        }

        ehci_controller_count++;
    }

    LOG_INFO("%d EHCI controller(s) ready", ehci_controller_count);
    return &ehci_driver_loaded;
}

struct driver *return_meta_ehci_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };

    static struct driver meta = {
        .name = "EHCI USB Controller Driver",
        .type = USB_DRIVER,
        .sub_type = USB_TYPE_EHCI,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &ehci_driver_loaded,
        .init = (void *)return_ehci_driver,
    };
    return &meta;
}
