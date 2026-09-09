#include "ohci.h"

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
#include "drivers/Video/limine_video_driver.h"
#include "kernel/limine.h"

#include <stddef.h>

#define OHCI_TD_POOL_SIZE 128

#define REG_CONTROL 0x04
#define REG_COMMAND_STATUS 0x08
#define REG_INT_STATUS 0x0C
#define REG_INT_ENABLE 0x10
#define REG_HCCA 0x18
#define REG_CONTROL_HEAD 0x20
#define REG_BULK_HEAD 0x28
#define REG_FM_INTERVAL 0x34
#define REG_PERIODIC_START 0x3C
#define REG_RH_DESCRIPTOR_A 0x48
#define REG_RH_PORT_STATUS 0x54

#define OHCI_ED_HEAD_FLAGS_MASK 0xFu

#define OHCI_CTRL_CLE (1u << 4)
#define OHCI_CTRL_BLE (1u << 5)
#define OHCI_CTRL_HCFS_OPERATIONAL (2u << 6)

#define OHCI_CS_CLF (1u << 1)
#define OHCI_CS_HCR (1u << 0)

extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_kernel_address_request kernel_address_request;

typedef struct ohci_ed_hw {
    volatile uint32_t info;
    volatile uint32_t tail_p;
    volatile uint32_t head_p;
    volatile uint32_t next_ed;
} __attribute__((aligned(16))) ohci_ed_hw;

typedef struct ohci_td_hw {
    volatile uint32_t info;
    volatile uint32_t cbp;
    volatile uint32_t next_td;
    volatile uint32_t be;
} __attribute__((aligned(16))) ohci_td_hw;

static struct {
    struct ohci_controller ctrl;
    struct ohci_hcca hcca __attribute__((aligned(256)));
    struct ohci_ed_hw ed_pool[16] __attribute__((aligned(16)));
    struct ohci_td_hw td_pool[OHCI_TD_POOL_SIZE] __attribute__((aligned(16)));
} ohci_resources[MAX_OHCI_CONTROLLERS] __attribute__((aligned(4096)));

static int ohci_controller_count = 0;
static void (*delay_ms)(uint64_t) = NULL;

#define OHCI_INT_WDH (1u << 1)
#define OHCI_INT_UE (1u << 4)
#define OHCI_INT_MIE (1u << 31)

typedef struct ohci_pending_xfer {
    struct ohci_controller *ctrl;
    struct ohci_td_hw *td;
    void *data;
    uint16_t data_len;
    uint8_t direction;
    uint8_t active;
    int ri;
    uint8_t endpoint;
    void *cookie;
    uint8_t in_hcca;
    struct usb_device *device;
} ohci_pending_xfer;

#define MAX_OHCI_HID_SLOTS 4
#define MAX_OHCI_PENDING (MAX_OHCI_CONTROLLERS * MAX_OHCI_HID_SLOTS)
static struct ohci_pending_xfer s_ohci_pending[MAX_OHCI_CONTROLLERS][MAX_OHCI_HID_SLOTS];

static inline void ohci_write(struct ohci_controller *o, uint32_t reg, uint32_t val) {
    uint64_t off = hhdm_req.response ? hhdm_req.response->offset : 0;
    *(volatile uint32_t *)(o->base_addr + off + reg) = val;
    asm volatile("mfence" ::: "memory");
}

static inline uint32_t ohci_read(struct ohci_controller *o, uint32_t reg) {
    uint64_t off = hhdm_req.response ? hhdm_req.response->offset : 0;
    return *(volatile uint32_t *)(o->base_addr + off + reg);
}

void ohci_irq(void) {
    for (int i = 0; i < ohci_controller_count; i++) {
        struct ohci_controller *o = &ohci_resources[i].ctrl;

        uint32_t s = ohci_read(o, REG_INT_STATUS);
        if (!s) continue;
        ohci_write(o, REG_INT_STATUS, s);

        if (!(s & OHCI_INT_WDH)) continue;

        for (int _s = 0; _s < MAX_OHCI_HID_SLOTS; _s++) {
        struct ohci_pending_xfer *pend = &s_ohci_pending[i][_s];
        if (!pend->active) continue;

        volatile uint32_t info = pend->td->info;
        if ((info >> 28) == 0xF) continue;

        uint8_t cc = (uint8_t)(info >> 28);
        int ok = (cc == 0);

        if (ok && pend->direction) {
            for (uint64_t a = (uint64_t)pend->data;
                 a < (uint64_t)pend->data + pend->data_len; a += 64)
                asm volatile("clflush (%0)" :: "r"(a) : "memory");
            asm volatile("mfence" ::: "memory");
        }

        volatile uint32_t *hcca_intr_irq = (volatile uint32_t *)(uintptr_t)
            mm_phys_to_virt((uint64_t)ohci_read(o, REG_HCCA));
        hcca_intr_irq[_s] = 0;
        asm volatile("mfence" ::: "memory");

        usb_event_t evt = {
            .type = ok ? USB_EVENT_TRANSFER_DONE
                                  : USB_EVENT_TRANSFER_ERR,
            .src = USB_SRC_OHCI,
            .transfer_type = USB_XFER_INTERRUPT,
            .slot_id = (uint8_t)i,
            .endpoint = pend->endpoint,
            .completion_code = cc,
            .data = ok ? pend->data : NULL,
            .data_len = ok ? pend->data_len : 0,
            .device = pend->device,
            .cookie = pend->cookie,
        };
        usb_push_event(&evt);
        pend->active = 0;
        pend->in_hcca = 0;
        }
    }
}

static int ohci_control_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *setup, uint16_t setup_len, void *data, uint16_t data_len, uint8_t in)
{
    (void)setup_len;
    int res_idx = -1;
    for (int i = 0; i < ohci_controller_count; i++) {
        if (&ohci_resources[i].ctrl == o) { res_idx = i; break; }
    }
    if (res_idx == -1) return -1;

    uint32_t is_low_speed = o->is_low_speed;

    struct ohci_ed_hw *ed = &ohci_resources[res_idx].ed_pool[0];
    struct ohci_td_hw *td_setup = &ohci_resources[res_idx].td_pool[0];
    struct ohci_td_hw *td_data = &ohci_resources[res_idx].td_pool[1];
    struct ohci_td_hw *td_status= &ohci_resources[res_idx].td_pool[2];
    struct ohci_td_hw *td_tail = &ohci_resources[res_idx].td_pool[3];

    for (int i = 0; i < 4; i++) {
        ohci_resources[res_idx].td_pool[i].info = 0xF0000000u;
        ohci_resources[res_idx].td_pool[i].cbp = 0;
        ohci_resources[res_idx].td_pool[i].next_td = 0;
        ohci_resources[res_idx].td_pool[i].be = 0;
    }

    td_setup->info = (0u << 19) | (7u << 21) | (2u << 24) | 0xF0000000u;
    td_setup->cbp = (uint32_t)mm_ptr_to_phys(setup);
    td_setup->be = td_setup->cbp + 7;
    td_setup->next_td = (data_len > 0)
                      ? (uint32_t)mm_ptr_to_phys(td_data)
                      : (uint32_t)mm_ptr_to_phys(td_status);

    if (data_len > 0) {
        uint32_t dp = in ? 2u : 1u;
        if (in) {
            for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
                asm volatile("clflush (%0)" : : "r"(a) : "memory");
            asm volatile("mfence" ::: "memory");
        }
        td_data->info = (dp << 19) | (7u << 21) | (3u << 24) | 0xF0000000u;
        td_data->cbp = (uint32_t)mm_ptr_to_phys(data);
        td_data->be = td_data->cbp + data_len - 1;
        td_data->next_td = (uint32_t)mm_ptr_to_phys(td_status);
    }

    uint32_t status_dp = (data_len > 0 && in) ? 1u : 2u;
    td_status->info = (status_dp << 19) | (0u << 21) | (3u << 24) | 0xF0000000u;
    td_status->cbp = 0;
    td_status->be = 0;
    td_status->next_td = (uint32_t)mm_ptr_to_phys(td_tail);

    td_tail->info = 0;
    td_tail->cbp = 0;
    td_tail->next_td = 0;
    td_tail->be = 0;

    uint32_t mps = (addr == 0 || is_low_speed) ? 8u : 64u;

    ed->info = (addr & 0x7Fu) | ((ep & 0xFu) << 7) | (mps << 16) | (is_low_speed << 13);
    ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
    ed->next_ed = 0;
    asm volatile("mfence" ::: "memory");
    ed->head_p = (uint32_t)mm_ptr_to_phys(td_setup) & ~OHCI_ED_HEAD_FLAGS_MASK;
    asm volatile("mfence" ::: "memory");

    uint32_t ctrl_save = ohci_read(o, REG_CONTROL);
    ohci_write(o, REG_CONTROL_HEAD, (uint32_t)mm_ptr_to_phys(ed));
    ohci_write(o, REG_CONTROL, ctrl_save | OHCI_CTRL_CLE);
    ohci_write(o, REG_COMMAND_STATUS, OHCI_CS_CLF);

    int timeout = 500;
    while (timeout--) {
        uint32_t cc = (td_status->info >> 28) & 0xF;
        if (cc != 0xF) {
            ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) & ~OHCI_CTRL_CLE);
            if (in && data_len > 0) {
                for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
                    asm volatile("clflush (%0)" : : "r"(a) : "memory");
                asm volatile("mfence" ::: "memory");
                asm volatile("lfence" ::: "memory");
            }
            if (cc != 0) {
                usb_logrow_begin(USB_LOG_OHCI, USB_LOG_ERROR);
                usb_logrow_str("control xfer addr="); usb_logrow_dec(addr);
                usb_logrow_str(" ep="); usb_logrow_dec(ep);
                usb_logrow_str(": completion code=");
                usb_logrow_hex32(cc);
                usb_logrow_end();
            }
            return (cc == 0) ? 0 : -1;
        }
        if (ed->head_p & (1u << 0)) {
            ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) & ~OHCI_CTRL_CLE);
            usb_logrow_begin(USB_LOG_OHCI, USB_LOG_ERROR);
            usb_logrow_str("control xfer addr="); usb_logrow_dec(addr);
            usb_logrow_str(" ep="); usb_logrow_dec(ep);
            usb_logrow_str(": ED halted");
            usb_logrow_end();
            return -1;
        }
        delay_ms(1);
    }

    ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) & ~OHCI_CTRL_CLE);
    usb_logrow_begin(USB_LOG_OHCI, USB_LOG_ERROR);
    usb_logrow_str("control xfer addr="); usb_logrow_dec(addr);
    usb_logrow_str(" ep="); usb_logrow_dec(ep);
    usb_logrow_str(": timed out");
    usb_logrow_end();
    return -1;
}

static int ohci_control_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *setup, uint16_t setup_len, void *data, uint16_t data_len, uint8_t in)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_control_transfer_impl(o, addr, ep, setup, setup_len, data, data_len, in);
    spin_unlock(&o->lock);
    return ret;
}

static int ohci_interrupt_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *data, uint16_t data_len, uint8_t direction)
{
    if (!o || !o->initialized) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_OHCI_CONTROLLERS; i++) {
        if (&ohci_resources[i].ctrl == o) { ri = i; break; }
    }
    if (ri == -1) return -1;

    int slot_idx = -1;
    for (int _s = 0; _s < MAX_OHCI_HID_SLOTS; _s++) {
        struct ohci_pending_xfer *_p = &s_ohci_pending[ri][_s];
        if (_p->active && _p->device && (uint8_t)_p->device->address == addr)
            { slot_idx = _s; break; }
    }
    if (slot_idx < 0) {
        for (int _s = 0; _s < MAX_OHCI_HID_SLOTS; _s++) {
            if (!s_ohci_pending[ri][_s].active) { slot_idx = _s; break; }
        }
    }
    if (slot_idx < 0) return -2;

    struct ohci_pending_xfer *pend = &s_ohci_pending[ri][slot_idx];

    if (pend->active) {
        volatile uint32_t info = pend->td->info;

        if ((info >> 28) == 0xF) return -2;

        uint8_t cc = (uint8_t)(info >> 28);
        int ok = (cc == 0);

        if (ok && pend->direction) {
            for (uint64_t a = (uint64_t)pend->data;
                 a < (uint64_t)pend->data + pend->data_len; a += 64)
                asm volatile("clflush (%0)" :: "r"(a) : "memory");
            asm volatile("mfence" ::: "memory");
        }

        {
            volatile uint32_t *hcca_poll = (volatile uint32_t *)(uintptr_t)
                mm_phys_to_virt((uint64_t)ohci_read(o, REG_HCCA));
            hcca_poll[slot_idx] = 0;
            asm volatile("mfence" ::: "memory");
        }

        usb_event_t evt = {
            .type = ok ? USB_EVENT_TRANSFER_DONE
                                  : USB_EVENT_TRANSFER_ERR,
            .src = USB_SRC_OHCI,
            .transfer_type = USB_XFER_INTERRUPT,
            .slot_id = (uint8_t)ri,
            .endpoint = pend->endpoint,
            .completion_code = cc,
            .data = ok ? pend->data : NULL,
            .data_len = ok ? pend->data_len : 0,
            .device = pend->device,
            .cookie = pend->cookie,
        };
        usb_push_event(&evt);
        pend->active = 0;
        pend->in_hcca = 0;
    }

    if (direction) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
            asm volatile("clflush (%0)" :: "r"(a) : "memory");
        asm volatile("mfence" ::: "memory");
    }

    uint8_t ep_num = ep & 0x0F;

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[4 + slot_idx];
    struct ohci_td_hw *td_data = &ohci_resources[ri].td_pool[8 + slot_idx * 2];
    struct ohci_td_hw *td_tail = &ohci_resources[ri].td_pool[8 + slot_idx * 2 + 1];

    td_tail->info = 0;
    td_tail->cbp = 0;
    td_tail->next_td = 0;
    td_tail->be = 0;

    uint32_t dp = direction ? 2u : 1u;

    td_data->info = (dp << 19) | (0u << 21) | (3u << 24) | 0xF0000000u | (1u << 18);
    td_data->cbp = (uint32_t)mm_ptr_to_phys(data);
    td_data->be = td_data->cbp + data_len - 1;
    td_data->next_td = (uint32_t)mm_ptr_to_phys(td_tail);

    uint32_t ed_mps = 8u;
    for (int _mi = 0; _mi < MAX_USB_DEVICES; _mi++) {
        struct usb_device *_md = (struct usb_device *)device_table[USB_DEVICE][_mi];
        if (_md && _md->address == addr && _md->max_packet_size > 0) {
            ed_mps = _md->max_packet_size;
            break;
        }
    }
    ed->info = (addr & 0x7F)
               | ((ep_num & 0xF) << 7)
               | (0u << 11)
               | (0u << 13)
               | (ed_mps << 16);
    ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
    ed->head_p = (uint32_t)mm_ptr_to_phys(td_data);
    ed->next_ed = 0;

    asm volatile("mfence" ::: "memory");

    uint32_t ed_phys = (uint32_t)mm_ptr_to_phys(ed);
    if (!pend->in_hcca) {

        volatile uint32_t *hcca_intr = (volatile uint32_t *)(uintptr_t)
            mm_phys_to_virt((uint64_t)ohci_read(o, REG_HCCA));
        ed->next_ed = hcca_intr[slot_idx];
        hcca_intr[slot_idx] = ed_phys;
        asm volatile("mfence" ::: "memory");

        ohci_write(o, REG_CONTROL,
                   ohci_read(o, REG_CONTROL) | (1u << 2) | OHCI_CTRL_HCFS_OPERATIONAL);
        pend->in_hcca = 1;
    } else {

        uint32_t carry = ed->head_p & 0x2u;
        ed->head_p = (uint32_t)mm_ptr_to_phys(td_data) | carry;
        ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
        asm volatile("mfence" ::: "memory");
    }

    pend->ctrl = o;
    pend->td = td_data;
    pend->data = data;
    pend->data_len = data_len;
    pend->direction = direction;
    pend->ri = ri;
    pend->endpoint = ep;

    pend->device = NULL;
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && _d->address == addr &&
            _d->ctrl == (struct usb_controller *)o) {
            pend->device = _d;
            break;
        }
    }
    pend->cookie = pend->device;
    pend->active = 1;

    return -2;
}

static int ohci_interrupt_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *data, uint16_t data_len, uint8_t direction)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_interrupt_transfer_impl(o, addr, ep, data, data_len, direction);
    spin_unlock(&o->lock);
    return ret;
}

static int ohci_reset_port(struct ohci_controller *o, uint8_t port) {
    uint32_t reg = REG_RH_PORT_STATUS + (port - 1) * 4;

    uint32_t status = ohci_read(o, reg);
    if (!(status & 0x01)) return -1;

    ohci_write(o, reg, 0x10);
    delay_ms(50);

    int t = 200;
    while (t-- > 0) {
        status = ohci_read(o, reg);
        if (status & (1u << 20)) break;
        delay_ms(1);
    }

    ohci_write(o, reg, (1u << 20));
    delay_ms(10);

    status = ohci_read(o, reg);
    if (!(status & 0x01)) return -1;

    if (!(status & 0x02)) {
        ohci_write(o, reg, 0x02);
        delay_ms(10);
    }

    status = ohci_read(o, reg);
    if (!(status & 0x02)) return -1;

    o->is_low_speed = (status >> 9) & 1u;
    return 0;
}

static void ohci_irq_handler(struct registers *regs) { (void)regs; ohci_irq(); }

static void ohci_scan_pci(void) {
    struct usb_controller *ctrls = pci_get_usb_controllers();
    for (int i = 0; i < 8; i++) {
        if (ctrls[i].type != USB_TYPE_OHCI) continue;

        struct ohci_controller *o = &ohci_resources[ohci_controller_count].ctrl;
        o->base_addr = ctrls[i].base_addr;
        o->type = USB_TYPE_OHCI;
        o->is_low_speed = 0;

        pci_enable_bus_mastering(ctrls[i].pci);

        uint32_t b = ctrls[i].pci->bus;
        uint32_t s = ctrls[i].pci->slot;
        uint32_t f = ctrls[i].pci->func;
        uint32_t cmd = pci_read_config(b, s, f, 0x04);
        pci_write_config(b, s, f, 0x04, cmd | 0x06);

        ohci_write(o, REG_COMMAND_STATUS, OHCI_CS_HCR);
        delay_ms(10);

        ohci_write(o, REG_HCCA,
            (uint32_t)mm_ptr_to_phys(&ohci_resources[ohci_controller_count].hcca));
        ohci_write(o, REG_FM_INTERVAL, (0x7FFFu << 16) | 0x2EDFu);
        ohci_write(o, REG_PERIODIC_START, 0x2A2Fu);
        ohci_write(o, REG_CONTROL, OHCI_CTRL_HCFS_OPERATIONAL);
        delay_ms(10);

        ohci_write(o, REG_INT_ENABLE,
                   OHCI_INT_WDH | OHCI_INT_UE | OHCI_INT_MIE);

        struct pci_device *pdev = ctrls[i].pci;
        uint8_t lapic = apic_get_lapic_id();
        uint8_t irq_vec = (uint8_t)(MSI_VECTOR_OHCI_BASE + ohci_controller_count);
        ioapic_map_pci_irq(pdev->irq_line, irq_vec, lapic);
        irq_register_handler(irq_vec, ohci_irq_handler);

        o->initialized = 1;
        usb_logrow_begin(USB_LOG_OHCI, USB_LOG_INFO);
        usb_logrow_str("controller "); usb_logrow_dec(ohci_controller_count);
        usb_logrow_str(" ready, base=");
        usb_logrow_hex64(o->base_addr);
        usb_logrow_end();
        ohci_controller_count++;
    }
}

static struct ohci_controller *ohci_get_ctrl(int idx) {
    return (idx >= 0 && idx < ohci_controller_count)
           ? &ohci_resources[idx].ctrl : NULL;
}
static int ohci_get_count(void) { return ohci_controller_count; }
static void ohci_start(struct ohci_controller *o) { (void)o; }
static void ohci_stop(struct ohci_controller *o) { (void)o; }

typedef struct ohci_bulk_pending {
    struct ohci_controller *ctrl;
    struct ohci_td_hw *td;
    void *data;
    uint16_t data_len;
    uint8_t direction;
    uint8_t active;
    int ri;
    uint8_t endpoint;
    struct usb_device *device;
    void *cookie;
} ohci_bulk_pending;

static struct ohci_bulk_pending s_ohci_bulk_pending[MAX_OHCI_CONTROLLERS];

static int ohci_bulk_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *data, uint16_t data_len, uint8_t direction)
{
    if (!o || !o->initialized) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_OHCI_CONTROLLERS; i++) {
        if (&ohci_resources[i].ctrl == o) { ri = i; break; }
    }
    if (ri == -1) return -1;

    struct ohci_bulk_pending *pend = &s_ohci_bulk_pending[ri];

    if (pend->active) {
        volatile uint32_t info = pend->td->info;
        if ((info >> 28) == 0xF) return -2;

        uint8_t cc = (uint8_t)(info >> 28);
        int ok = (cc == 0);

        if (ok && pend->direction) {
            for (uint64_t a = (uint64_t)pend->data;
                 a < (uint64_t)pend->data + pend->data_len; a += 64)
                asm volatile("clflush (%0)" :: "r"(a) : "memory");
            asm volatile("mfence" ::: "memory");
        }

        ohci_write(o, REG_BULK_HEAD, 0);
        ohci_write(o, 0x08, (1u << 2));

        usb_event_t evt = {
            .type = ok ? USB_EVENT_BULK_DONE : USB_EVENT_BULK_ERR,
            .src = USB_SRC_OHCI,
            .transfer_type = USB_XFER_BULK,
            .slot_id = (uint8_t)ri,
            .endpoint = pend->endpoint,
            .completion_code = cc,
            .data = ok ? pend->data : NULL,
            .data_len = ok ? pend->data_len : 0,
            .device = pend->device,
            .cookie = pend->cookie,
        };
        usb_push_bulk_event(&evt);
        pend->active = 0;
        return ok ? 0 : -1;
    }

    if (direction) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
            asm volatile("clflush (%0)" :: "r"(a) : "memory");
        asm volatile("mfence" ::: "memory");
    }

    uint8_t ep_num = ep & 0x0Fu;

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[5];
    struct ohci_td_hw *td_data = &ohci_resources[ri].td_pool[10];
    struct ohci_td_hw *td_tail = &ohci_resources[ri].td_pool[11];

    td_tail->info = 0;
    td_tail->cbp = 0;
    td_tail->next_td = 0;
    td_tail->be = 0;

    uint32_t dp = direction ? 2u : 1u;
    td_data->info = (dp << 19) | (0u << 21) | (3u << 24) | 0xF0000000u | (1u << 18);
    td_data->cbp = (uint32_t)mm_ptr_to_phys(data);
    td_data->be = td_data->cbp + data_len - 1u;
    td_data->next_td = (uint32_t)mm_ptr_to_phys(td_tail);

    uint32_t ed_mps = 64u;
    ed->info = (uint32_t)(addr & 0x7Fu)
                | ((uint32_t)(ep_num & 0xFu) << 7)
                | (0u << 11)
                | ((uint32_t)o->is_low_speed << 13)
                | (0u << 14)
                | (ed_mps << 16);
    ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
    ed->head_p = (uint32_t)mm_ptr_to_phys(td_data);
    ed->next_ed = 0;
    asm volatile("mfence" ::: "memory");

    ohci_write(o, REG_BULK_HEAD, (uint32_t)mm_ptr_to_phys(ed));

    uint32_t ctrl = ohci_read(o, REG_CONTROL);
    ohci_write(o, REG_CONTROL, ctrl | (1u << 5));

    ohci_write(o, REG_COMMAND_STATUS, (1u << 2));

    pend->ctrl = o;
    pend->td = td_data;
    pend->data = data;
    pend->data_len = data_len;
    pend->direction = direction;
    pend->active = 1;
    pend->ri = ri;
    pend->endpoint = ep;

    pend->device = NULL;
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && _d->address == addr &&
            _d->ctrl == (struct usb_controller *)o) {
            pend->device = _d;
            break;
        }
    }
    pend->cookie = pend->device;

    return -2;
}

static int ohci_bulk_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *data, uint16_t data_len, uint8_t direction)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_bulk_transfer_impl(o, addr, ep, data, data_len, direction);
    spin_unlock(&o->lock);
    return ret;
}

#define OHCI_ISO_MAX_FRAMES 8

typedef struct ohci_iso_td_hw {
    volatile uint32_t flags;
    volatile uint32_t bp0;
    volatile uint32_t next_td;
    volatile uint32_t be;
    volatile uint16_t psw[8];
} __attribute__((packed, aligned(32))) ohci_iso_td_hw;

typedef struct ohci_iso_pending {
    uint8_t active;
    uint8_t n_frames;
    uint8_t endpoint;
    uint8_t direction;
    void *data;
    uint16_t total_len;
    uint16_t frame_offsets[OHCI_ISO_MAX_FRAMES];
    uint16_t frame_lens [OHCI_ISO_MAX_FRAMES];
    void *cookie;
    struct ohci_iso_td_hw iso_td __attribute__((aligned(32)));
    struct ohci_ed_hw iso_ed __attribute__((aligned(16)));
} ohci_iso_pending;

static struct ohci_iso_pending s_ohci_iso[MAX_OHCI_CONTROLLERS];

static int ohci_iso_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep, void *data, uint16_t total_len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction)
{
    if (!o || !o->initialized) return -1;
    if (!n_frames || n_frames > OHCI_ISO_MAX_FRAMES) return -1;

    int ri = -1;
    for (int i = 0; i < MAX_OHCI_CONTROLLERS; i++) {
        if (&ohci_resources[i].ctrl == o) { ri = i; break; }
    }
    if (ri == -1) return -1;

    if (s_ohci_iso[ri].active) return -2;

    uint8_t ep_num = ep & 0x0Fu;
    uint8_t is_in = (ep & 0x80u) ? 1u : 0u;
    (void)direction;

    uint16_t per_frame = (n_frames > 0) ? (total_len / n_frames) : total_len;
    uint16_t offset = 0;
    for (uint8_t i = 0; i < n_frames; i++) {
        uint16_t flen = frame_lens ? frame_lens[i] : per_frame;
        s_ohci_iso[ri].frame_offsets[i] = offset;
        s_ohci_iso[ri].frame_lens[i] = flen;
        offset = (uint16_t)(offset + flen);
    }

    uint16_t cur_frame = ohci_resources[ri].hcca.hcca_frame_number;
    uint16_t sf = (uint16_t)((cur_frame + 2u) & 0xFFFFu);

    struct ohci_iso_td_hw *itd = &s_ohci_iso[ri].iso_td;
    uint8_t *base_buf = (uint8_t *)data;
    uint32_t buf_phys = (uint32_t)mm_ptr_to_phys(base_buf);

    itd->flags = ((uint32_t)(n_frames - 1u) << 24)
                  | (0u << 21)
                  | (uint32_t)sf;
    itd->bp0 = buf_phys & ~0xFFFu;
    itd->be = buf_phys + total_len - 1u;
    itd->next_td = 0u;

    for (uint8_t i = 0; i < n_frames; i++) {
        uint16_t off = s_ohci_iso[ri].frame_offsets[i];
        itd->psw[i] = (uint16_t)(0xE000u | (off & 0x1FFFu));
    }
    asm volatile("mfence" ::: "memory");

    struct ohci_ed_hw *ied = &s_ohci_iso[ri].iso_ed;
    ied->info = (uint32_t)(addr & 0x7Fu)
                 | ((uint32_t)(ep_num & 0xFu) << 7)
                 | ((uint32_t)(is_in ? 2u : 1u) << 11)
                 | (0u << 13)
                 | (1u << 15)
                 | ((uint32_t)per_frame << 16);
    ied->tail_p = 0u;
    ied->head_p = (uint32_t)mm_ptr_to_phys(itd);
    ied->next_ed = 0u;
    asm volatile("mfence" ::: "memory");

    ohci_resources[ri].hcca.hcca_interrupt_table[0] = (uint32_t)mm_ptr_to_phys(ied);
    asm volatile("mfence" ::: "memory");

    uint32_t ctrl = ohci_read(o, REG_CONTROL);
    ohci_write(o, REG_CONTROL, ctrl | (1u << 2) | (1u << 3));

    s_ohci_iso[ri].active = 1;
    s_ohci_iso[ri].n_frames = n_frames;
    s_ohci_iso[ri].endpoint = ep;
    s_ohci_iso[ri].direction = is_in;
    s_ohci_iso[ri].data = data;
    s_ohci_iso[ri].total_len = total_len;
    s_ohci_iso[ri].cookie = NULL;

    return 0;
}

void ohci_poll_iso(struct ohci_controller *o) {
    if (!o || !o->initialized) return;
    int ri = -1;
    for (int i = 0; i < MAX_OHCI_CONTROLLERS; i++) {
        if (&ohci_resources[i].ctrl == o) { ri = i; break; }
    }
    if (ri == -1 || !s_ohci_iso[ri].active) return;

    struct ohci_iso_td_hw *itd = &s_ohci_iso[ri].iso_td;

    int all_done = 1;
    for (uint8_t fi = 0; fi < s_ohci_iso[ri].n_frames; fi++) {
        volatile uint16_t psw = itd->psw[fi];
        uint8_t cc = (uint8_t)((psw >> 12) & 0xFu);
        if (cc == 0xFu || cc == 0x7u) { all_done = 0; continue; }

        int ok = (cc == 0);
        uint16_t actual_len = ok ? s_ohci_iso[ri].frame_lens[fi] : 0u;

        usb_event_t evt = {
            .type = ok ? USB_EVENT_ISO_DONE : USB_EVENT_ISO_ERR,
            .src = USB_SRC_OHCI,
            .slot_id = (uint8_t)ri,
            .endpoint = s_ohci_iso[ri].endpoint,
            .completion_code = cc,
            .data = ok ? ((uint8_t *)s_ohci_iso[ri].data
                                      + s_ohci_iso[ri].frame_offsets[fi])
                                   : NULL,
            .data_len = actual_len,
            .iso_frame_index = fi,
            .iso_expected_len = s_ohci_iso[ri].frame_lens[fi],
            .cookie = s_ohci_iso[ri].cookie,
        };
        usb_push_iso_event(&evt);

        itd->psw[fi] = (uint16_t)((psw & 0x0FFFu) | 0xE000u);
    }

    if (all_done) {

        ohci_resources[ri].hcca.hcca_interrupt_table[0] = 0u;
        asm volatile("mfence" ::: "memory");
        s_ohci_iso[ri].active = 0;
    }
}

void ohci_reset_endpoint_toggle(struct ohci_controller *o, uint8_t addr, uint8_t ep) {
    if (!o || !o->initialized) return;

    int ri = -1;
    for (int i = 0; i < MAX_OHCI_CONTROLLERS; i++) {
        if (&ohci_resources[i].ctrl == o) { ri = i; break; }
    }
    if (ri == -1) return;

    struct ohci_ed_hw *ed_pool = ohci_resources[ri].ed_pool;
    for (int i = 0; i < 16; i++) {
        struct ohci_ed_hw *ed = &ed_pool[i];
        if ((ed->info & 0x7FFFu) == ((addr & 0x7Fu) | ((ep & 0xFu) << 7))) {
            ed->head_p &= ~0x2u;
            asm volatile("mfence" ::: "memory");
        }
    }
}

void ohci_notify_disconnect(struct usb_device *dev) {
    if (!dev) return;
    for (int ri = 0; ri < MAX_OHCI_CONTROLLERS; ri++) {
        for (int s = 0; s < MAX_OHCI_HID_SLOTS; s++) {
            struct ohci_pending_xfer *p = &s_ohci_pending[ri][s];
            if (!p->active || p->device != dev) continue;

            struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[4 + s];
            ed->info |= (1u << 14);
            asm volatile("mfence" ::: "memory");

            if (p->in_hcca) {
                volatile uint32_t *hcca_poll = (volatile uint32_t *)(uintptr_t)
                    mm_phys_to_virt((uint64_t)ohci_read(&ohci_resources[ri].ctrl, REG_HCCA));
                hcca_poll[s] = 0;
                asm volatile("mfence" ::: "memory");
            }

            p->active = 0;
            p->in_hcca = 0;
            p->device = NULL;
            p->cookie = NULL;
        }
    }
}

struct ohci_driver ohci_driver_loaded = {
    .get_controller_count = ohci_get_count,
    .get_controller = ohci_get_ctrl,
    .control_transfer = (void *)ohci_control_transfer,
    .interrupt_transfer = (void *)ohci_interrupt_transfer,
    .bulk_transfer = ohci_bulk_transfer,
    .reset_port = (void *)ohci_reset_port,
    .start = ohci_start,
    .stop = ohci_stop,
    .reset_endpoint_toggle = ohci_reset_endpoint_toggle,
    .notify_disconnect = ohci_notify_disconnect,
};

struct ohci_driver *return_ohci_driver(void) {
    pci_init();

    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);

    delay_ms = tsc->sleep_tsc_ms;
    ohci_scan_pci();

    return &ohci_driver_loaded;
}

struct driver *return_meta_ohci_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };

    static struct driver meta = {
        .name = "OHCI USB Controller Driver",
        .type = USB_DRIVER,
        .sub_type = USB_TYPE_OHCI,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &ohci_driver_loaded,
        .init = (void *)return_ohci_driver,
    };
    return &meta;
}
