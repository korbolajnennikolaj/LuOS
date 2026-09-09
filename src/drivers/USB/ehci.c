#include "ehci.h"

#include "components/drivers.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/Memory/mm.h"
#include "components/pci.h"
#include "drivers/Timer/apic_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "drivers/USB/usb_log.h"
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
    struct usb_device *device;
} ehci_pending_xfer;

#define MAX_EHCI_HID_SLOTS 4
#define MAX_EHCI_PENDING (MAX_EHCI_CONTROLLERS * MAX_EHCI_HID_SLOTS)
static struct ehci_pending_xfer s_ehci_pending[MAX_EHCI_CONTROLLERS][MAX_EHCI_HID_SLOTS];

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
        usb_log(USB_LOG_EHCI, USB_LOG_TRACE, "no legacy support capability, skip BIOS handoff");
        return;
    }

    uint32_t legsup;
    while (eecp) {
        legsup = ehci_cap_read(e, eecp);
        if ((legsup & 0xFF) == 1) break;
        eecp = (legsup >> 8) & 0xFF;
    }
    if (!eecp) {
        usb_log(USB_LOG_EHCI, USB_LOG_TRACE, "no USB Legacy Support cap found in ext cap list");
        return;
    }

    ehci_cap_write(e, eecp, legsup | (1 << 24));
    int t = 100;
    while (1) {
        legsup = ehci_cap_read(e, eecp);
        if (!(legsup & (1 << 16)) && (legsup & (1 << 24))) {
            usb_log(USB_LOG_EHCI, USB_LOG_INFO, "BIOS handoff: OS now owns controller");
            break;
        }
        if (t-- <= 0) {

            usb_log(USB_LOG_EHCI, USB_LOG_WARN, "BIOS handoff timed out, forcing OS ownership");
            ehci_cap_write(e, eecp + 4, 0);
            return;
        }
        delay_ms(10);
    }

    uint32_t legctlsts = ehci_cap_read(e, eecp + 4);
    ehci_cap_write(e, eecp + 4, legctlsts & 0xFFFF0000u);
    usb_log(USB_LOG_EHCI, USB_LOG_INFO, "BIOS SMI sources disabled for this controller");
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
        usb_log(USB_LOG_EHCI, USB_LOG_ERROR, "controller reset (HCRESET) timed out");
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
    usb_logrow_begin(USB_LOG_EHCI, USB_LOG_INFO);
    usb_logrow_str("controller "); usb_logrow_dec(ri);
    usb_logrow_str(" ready, ports="); usb_logrow_dec(e->num_ports);
    usb_logrow_end();
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

    uint32_t mps = 8u;
    if (dev_addr != 0) {
        for (int _ci = 0; _ci < MAX_USB_DEVICES; _ci++) {
            struct usb_device *_cd = (struct usb_device *)device_table[USB_DEVICE][_ci];
            if (_cd && (uint8_t)_cd->address == dev_addr && _cd->max_packet_size > 0) {
                mps = _cd->max_packet_size;
                break;
            }
        }
        if (mps == 8u) mps = 64u;
    }

    struct ehci_qh *qh = &ehci_resources[ri].async_head;
    qh->characteristics = (dev_addr & 0x7F)
                        | ((endpoint & 0xF) << 8)
                        | (2u << 12)
                        | (1u << 14)
                        | (1u << 15)
                        | (mps << 16);
    ehci_reset_qh_overlay(qh);
    qh->next_link = (uint32_t)mm_ptr_to_phys(qtd_setup);
    asm volatile("mfence" ::: "memory");

    ehci_write(e, EHCI_ASYNCLISTADDR,
               (uint32_t)mm_ptr_to_phys(&ehci_resources[ri].async_head));

    {
        uint32_t cmd = ehci_read(e, EHCI_USBCMD);
        if (!(cmd & (1 << 5)))
            ehci_write(e, EHCI_USBCMD, cmd | (1 << 5));
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
            usb_logrow_begin(USB_LOG_EHCI, USB_LOG_ERROR);
            usb_logrow_str("control xfer addr="); usb_logrow_dec(dev_addr);
            usb_logrow_str(" ep="); usb_logrow_dec(endpoint);
            usb_logrow_str(": setup qTD halted, token=");
            usb_logrow_hex32(tok_s);
            usb_logrow_end();
            return -5;
        }
        volatile uint32_t token = qtd_status->token;
        if (token & 0x40) {
            qh->next_link = 1;
            free_qtd(ri, qtd_setup);
            if (qtd_data) free_qtd(ri, qtd_data);
            free_qtd(ri, qtd_status);
            usb_logrow_begin(USB_LOG_EHCI, USB_LOG_ERROR);
            usb_logrow_str("control xfer addr="); usb_logrow_dec(dev_addr);
            usb_logrow_str(" ep="); usb_logrow_dec(endpoint);
            usb_logrow_str(": status qTD halted, token=");
            usb_logrow_hex32(token);
            usb_logrow_end();
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
        usb_logrow_begin(USB_LOG_EHCI, USB_LOG_ERROR);
        usb_logrow_str("control xfer addr="); usb_logrow_dec(dev_addr);
        usb_logrow_str(" ep="); usb_logrow_dec(endpoint);
        usb_logrow_str(": timed out");
        usb_logrow_end();
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
        if (_p->active && _p->device) {
            if ((uint8_t)_p->device->address == dev_addr) { slot_idx = _s; break; }
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
        } else {

            return -2;
        }
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
        if (_md && (uint8_t)_md->address == dev_addr) {

            if (_md->device_class == 0x09 && _md->hub_status_ep == endpoint) {
                if (_md->hub_status_ep_mps) ehci_mps = _md->hub_status_ep_mps;
            } else if (_md->max_packet_size) {
                ehci_mps = _md->max_packet_size;
            }
            break;
        }
    }
    iqh->characteristics = (dev_addr & 0x7F)
                         | ((ep_num & 0xF) << 8)
                         | (2 << 12)

                         | (ehci_mps << 16);
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

    struct ehci_qh *qh = &ehci_resources[ri].async_head;
    qh->characteristics = (dev_addr & 0x7Fu)
                        | ((uint32_t)(ep_num & 0xFu) << 8)
                        | (2u << 12)
                        | ((uint32_t)bulk_mps << 16)
                        | (1u << 15);

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

#define EHCI_ISO_MAX_FRAMES 8
#define EHCI_ITD_POOL_SIZE 32

struct ehci_iso_slot {
    struct ehci_itd itd_pool[EHCI_ITD_POOL_SIZE] __attribute__((aligned(32)));
    uint8_t itd_used[EHCI_ITD_POOL_SIZE];

    struct {
        uint8_t active;
        uint8_t n_frames;
        uint8_t frames_done;
        uint8_t endpoint;
        uint8_t direction;
        void *data;
        uint16_t total_len;
        uint16_t frame_offsets[EHCI_ISO_MAX_FRAMES];
        uint16_t frame_lens [EHCI_ISO_MAX_FRAMES];
        uint16_t frame_slots [EHCI_ISO_MAX_FRAMES];
        void *cookie;
    } pending;
} s_ehci_iso[MAX_EHCI_CONTROLLERS];

static struct ehci_itd *alloc_itd(int ri) {
    for (int i = 0; i < EHCI_ITD_POOL_SIZE; i++) {
        if (!s_ehci_iso[ri].itd_used[i]) {
            s_ehci_iso[ri].itd_used[i] = 1;
            struct ehci_itd *itd = &s_ehci_iso[ri].itd_pool[i];
            for (int j = 0; j < 8; j++) { itd->transaction[j] = 0; itd->buffer_page[j % 7] = 0; }
            itd->next_link = 1;
            return itd;
        }
    }
    return NULL;
}

static void free_itd(int ri, struct ehci_itd *itd) {
    uintptr_t start = (uintptr_t)s_ehci_iso[ri].itd_pool;
    int idx = (int)(((uintptr_t)itd - start) / sizeof(struct ehci_itd));
    if (idx >= 0 && idx < EHCI_ITD_POOL_SIZE)
        s_ehci_iso[ri].itd_used[idx] = 0;
}

static int ehci_iso_transfer_impl(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *data, uint16_t total_len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction)
{
    if (!e || !e->initialized) return -1;
    if (!n_frames || n_frames > EHCI_ISO_MAX_FRAMES) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (&ehci_resources[i].ctrl == e) { ri = i; break; }
    }
    if (ri == -1) return -1;

    if (s_ehci_iso[ri].pending.active) return -2;

    uint8_t ep_num = endpoint & 0x0Fu;
    uint8_t is_in = (endpoint & 0x80u) ? 1u : 0u;
    (void)direction;

    uint16_t flen[EHCI_ISO_MAX_FRAMES];
    uint16_t offset = 0;
    uint16_t per_frame = (n_frames > 0) ? (total_len / n_frames) : total_len;
    for (uint8_t i = 0; i < n_frames; i++) {
        flen[i] = frame_lens ? frame_lens[i] : per_frame;
        s_ehci_iso[ri].pending.frame_offsets[i] = offset;
        s_ehci_iso[ri].pending.frame_lens[i] = flen[i];
        offset = (uint16_t)(offset + flen[i]);
    }

    uint32_t cur_frame = ehci_read(e, 0x0C) & 0x3FFu;

    for (uint8_t i = 0; i < n_frames; i++) {
        struct ehci_itd *itd = alloc_itd(ri);
        if (!itd) {

            for (uint8_t j = 0; j < i; j++) {
                uint16_t si = s_ehci_iso[ri].pending.frame_slots[j];
                ehci_resources[ri].frame_list[(cur_frame + 1 + j) % EHCI_FRAME_LIST_SIZE] = 1;
                free_itd(ri, &s_ehci_iso[ri].itd_pool[si]);
                s_ehci_iso[ri].itd_used[si] = 0;
            }
            return -1;
        }

        int itd_idx = (int)((uintptr_t)(itd - s_ehci_iso[ri].itd_pool));
        s_ehci_iso[ri].pending.frame_slots[i] = (uint16_t)itd_idx;

        uint8_t *frame_buf = (uint8_t *)data + s_ehci_iso[ri].pending.frame_offsets[i];
        uint32_t buf_phys = (uint32_t)mm_ptr_to_phys(frame_buf);

        itd->buffer_page[0] = (buf_phys & ~0xFFFu)
                            | ((uint32_t)(dev_addr & 0x7Fu))
                            | ((uint32_t)(ep_num & 0xFu) << 8);
        itd->buffer_page[1] = ((buf_phys + 0x1000u) & ~0xFFFu)
                            | ((uint32_t)(flen[i] & 0xFFFu) << 0)
                            | ((uint32_t)is_in << 11);

        itd->transaction[0] = (buf_phys & 0xFFFu)
                            | ((uint32_t)flen[i] << 12)
                            | (1u << 25)
                            | (1u << 31);

        uint32_t frame_idx = (cur_frame + 1u + i) % EHCI_FRAME_LIST_SIZE;
        uint32_t itd_phys = (uint32_t)mm_ptr_to_phys(itd);

        itd->next_link = ehci_resources[ri].frame_list[frame_idx];
        asm volatile("mfence" ::: "memory");
        ehci_resources[ri].frame_list[frame_idx] = itd_phys;
        asm volatile("mfence" ::: "memory");
    }

    s_ehci_iso[ri].pending.active = 1;
    s_ehci_iso[ri].pending.n_frames = n_frames;
    s_ehci_iso[ri].pending.frames_done = 0;
    s_ehci_iso[ri].pending.endpoint = endpoint;
    s_ehci_iso[ri].pending.direction = is_in;
    s_ehci_iso[ri].pending.data = data;
    s_ehci_iso[ri].pending.total_len = total_len;
    s_ehci_iso[ri].pending.cookie = NULL;

    return 0;
}

static int ehci_iso_transfer(struct ehci_controller *e, uint8_t dev_addr, uint8_t endpoint, void *data, uint16_t total_len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction)
{
    if (!e) return -1;
    spin_lock(&e->lock);
    int ret = ehci_iso_transfer_impl(e, dev_addr, endpoint, data, total_len, n_frames, frame_lens, direction);
    spin_unlock(&e->lock);
    return ret;
}

void ehci_poll_iso(struct ehci_controller *e) {
    if (!e || !e->initialized) return;
    int ri = -1;
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (&ehci_resources[i].ctrl == e) { ri = i; break; }
    }
    if (ri == -1 || !s_ehci_iso[ri].pending.active) return;

    for (uint8_t fi = 0; fi < s_ehci_iso[ri].pending.n_frames; fi++) {
        uint16_t itd_idx = s_ehci_iso[ri].pending.frame_slots[fi];
        struct ehci_itd *itd = &s_ehci_iso[ri].itd_pool[itd_idx];

        volatile uint32_t trans = itd->transaction[0];
        if (trans & (1u << 31)) continue;

        uint8_t active_bit = (trans & (1u << 31)) ? 1u : 0u;
        uint8_t err = (trans >> 28) & 0x3u;
        int ok = !active_bit && !(err & 0x3u);
        (void)active_bit;

        usb_event_t evt = {
            .type = ok ? USB_EVENT_ISO_DONE : USB_EVENT_ISO_ERR,
            .src = USB_SRC_EHCI,
            .slot_id = (uint8_t)ri,
            .endpoint = s_ehci_iso[ri].pending.endpoint,
            .completion_code = ok ? 0 : (uint8_t)err,
            .data = ok ? ((uint8_t *)s_ehci_iso[ri].pending.data
                                      + s_ehci_iso[ri].pending.frame_offsets[fi])
                                   : NULL,
            .data_len = ok ? s_ehci_iso[ri].pending.frame_lens[fi] : 0,
            .iso_frame_index = fi,
            .iso_expected_len = s_ehci_iso[ri].pending.frame_lens[fi],
            .cookie = s_ehci_iso[ri].pending.cookie,
        };
        usb_push_iso_event(&evt);

        itd->transaction[0] = 0;
        s_ehci_iso[ri].pending.frames_done++;
    }

    if (s_ehci_iso[ri].pending.frames_done >= s_ehci_iso[ri].pending.n_frames) {
        for (uint8_t fi = 0; fi < s_ehci_iso[ri].pending.n_frames; fi++) {
            uint16_t idx = s_ehci_iso[ri].pending.frame_slots[fi];
            free_itd(ri, &s_ehci_iso[ri].itd_pool[idx]);
        }
        s_ehci_iso[ri].pending.active = 0;
    }
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
    }
}

struct ehci_driver ehci_driver_loaded = {
    .get_controller_count = ehci_get_count,
    .get_controller = ehci_get_ctrl,
    .control_transfer = ehci_control_transfer,
    .interrupt_transfer = ehci_interrupt_transfer,
    .bulk_transfer = ehci_bulk_transfer,
    .iso_transfer = ehci_iso_transfer,
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

    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) { s_ehci_bulk_pending[i].active = 0; s_ehci_iso[i].pending.active = 0; }

    struct usb_controller *u = pci_get_usb_controllers();
    for (int i = 0; i < MAX_EHCI_CONTROLLERS; i++) {
        if (u[i].type != USB_TYPE_EHCI || !u[i].pci) continue;

        struct ehci_controller *e = &ehci_resources[ehci_controller_count].ctrl;
        e->type = USB_TYPE_EHCI;
        e->cap_base = (uint32_t)u[i].base_addr;
        e->initialized = 0;

        pci_enable_bus_mastering(u[i].pci);

        if (ehci_init_controller(e, ehci_controller_count) != 0) continue;

        struct pci_device *pdev = u[i].pci;
        uint8_t lapic = apic_get_lapic_id();
        uint8_t vec = (uint8_t)(MSI_VECTOR_EHCI_BASE + ehci_controller_count);

        if (msi_enable(pdev->bus, pdev->slot, pdev->func, vec, lapic, ehci_irq)) {

            ehci_write(e, EHCI_USBINTR, 0x07);
        } else {

            uint8_t irq_vec = (uint8_t)(0x20 + pdev->irq_line);
            ioapic_map_pci_irq(pdev->irq_line, irq_vec, lapic);
            irq_register_handler(irq_vec, ehci_irq_legacy);
            ehci_write(e, EHCI_USBINTR, 0x07);
        }

        ehci_controller_count++;
    }

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
