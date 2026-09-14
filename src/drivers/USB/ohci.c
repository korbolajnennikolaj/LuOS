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

#define OHCI_TD_POOL_SIZE 32
#define OHCI_ED_POOL_SIZE 16
#define OHCI_MAX_IRQ_VECTORS 4

#define REG_CONTROL OHCI_HcControl
#define REG_COMMAND_STATUS OHCI_HcCommandStatus
#define REG_INT_STATUS OHCI_HcInterruptStatus
#define REG_INT_ENABLE OHCI_HcInterruptEnable
#define REG_INT_DISABLE OHCI_HcInterruptDisable
#define REG_HCCA OHCI_HcHCCA
#define REG_CONTROL_HEAD OHCI_HcControlHeadED
#define REG_CONTROL_CURRENT OHCI_HcControlCurrentED
#define REG_BULK_HEAD OHCI_HcBulkHeadED
#define REG_BULK_CURRENT OHCI_HcBulkCurrentED
#define REG_DONE_HEAD OHCI_HcDoneHead
#define REG_FM_INTERVAL OHCI_HcFmInterval
#define REG_FM_NUMBER OHCI_HcFmNumber
#define REG_PERIODIC_START OHCI_HcPeriodicStart
#define REG_LS_THRESHOLD OHCI_HcLSThreshold
#define REG_RH_DESCRIPTOR_A OHCI_HcRhDescriptorA
#define REG_RH_STATUS OHCI_HcRhStatus
#define REG_RH_PORT_STATUS OHCI_HcRhPortStatus

#define OHCI_ED_HEAD_FLAGS_MASK 0xFu
#define OHCI_ED_HALTED 0x1u
#define OHCI_ED_TOGGLE_CARRY 0x2u
#define OHCI_ED_SKIP (1u << 14)
#define OHCI_ED_ISO (1u << 15)
#define OHCI_ED_LOWSPEED (1u << 13)
#define OHCI_ED_DIR_TD (0u << 11)
#define OHCI_ED_DIR_OUT (1u << 11)
#define OHCI_ED_DIR_IN (2u << 11)

#define OHCI_CTRL_CBSR_MASK 0x3u
#define OHCI_CTRL_PLE (1u << 2)
#define OHCI_CTRL_IE (1u << 3)
#define OHCI_CTRL_CLE (1u << 4)
#define OHCI_CTRL_BLE (1u << 5)
#define OHCI_CTRL_HCFS_MASK (3u << 6)
#define OHCI_CTRL_HCFS_RESET (0u << 6)
#define OHCI_CTRL_HCFS_RESUME (1u << 6)
#define OHCI_CTRL_HCFS_OPERATIONAL (2u << 6)
#define OHCI_CTRL_HCFS_SUSPEND (3u << 6)
#define OHCI_CTRL_IR (1u << 8)
#define OHCI_CTRL_RWC (1u << 9)

#define OHCI_CS_HCR (1u << 0)
#define OHCI_CS_CLF (1u << 1)
#define OHCI_CS_BLF (1u << 2)
#define OHCI_CS_OCR (1u << 3)

#define OHCI_INT_SO (1u << 0)
#define OHCI_INT_WDH (1u << 1)
#define OHCI_INT_SF (1u << 2)
#define OHCI_INT_RD (1u << 3)
#define OHCI_INT_UE (1u << 4)
#define OHCI_INT_FNO (1u << 5)
#define OHCI_INT_RHSC (1u << 6)
#define OHCI_INT_OC (1u << 30)
#define OHCI_INT_MIE (1u << 31)
#define OHCI_INT_ALL 0x0000007Fu

#define OHCI_TD_R (1u << 18)
#define OHCI_TD_DP_SETUP (0u << 19)
#define OHCI_TD_DP_OUT (1u << 19)
#define OHCI_TD_DP_IN (2u << 19)
#define OHCI_TD_DI_NONE (7u << 21)
#define OHCI_TD_DI_NOW (0u << 21)
#define OHCI_TD_T_CARRY (0u << 24)
#define OHCI_TD_T_DATA0 (2u << 24)
#define OHCI_TD_T_DATA1 (3u << 24)
#define OHCI_TD_CC_NOTACCESSED 0xF0000000u

#define OHCI_ED_CONTROL 0
#define OHCI_ED_BULK_BASE 1
#define OHCI_ED_INT_BASE (OHCI_ED_BULK_BASE + MAX_OHCI_BULK_SLOTS)

#define OHCI_TD_CONTROL 0
#define OHCI_TD_BULK_BASE 4
#define OHCI_TD_INT_BASE (OHCI_TD_BULK_BASE + MAX_OHCI_BULK_SLOTS * 2)

#define OHCI_DEFAULT_FI 0x2EDFu
#define OHCI_CTRL_TIMEOUT_MS 500

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

typedef struct ohci_iso_td_hw {
    volatile uint32_t flags;
    volatile uint32_t bp0;
    volatile uint32_t next_td;
    volatile uint32_t be;
    volatile uint16_t psw[8];
} __attribute__((packed, aligned(32))) ohci_iso_td_hw;

#define OHCI_ISO_MAX_FRAMES 8

typedef struct ohci_iso_pending {
    uint8_t active;
    uint8_t n_frames;
    uint8_t endpoint;
    uint8_t direction;
    void *data;
    uint16_t total_len;
    uint16_t frame_offsets[OHCI_ISO_MAX_FRAMES];
    uint16_t frame_lens [OHCI_ISO_MAX_FRAMES];
    uint8_t frame_reaped[OHCI_ISO_MAX_FRAMES];
    void *cookie;
    struct ohci_iso_td_hw iso_td __attribute__((aligned(32)));
    struct ohci_ed_hw iso_ed __attribute__((aligned(16)));
} ohci_iso_pending;

static struct {
    struct ohci_controller ctrl;
    struct ohci_hcca hcca __attribute__((aligned(256)));
    struct ohci_ed_hw ed_pool[OHCI_ED_POOL_SIZE] __attribute__((aligned(16)));
    struct ohci_td_hw td_pool[OHCI_TD_POOL_SIZE] __attribute__((aligned(16)));
    uint32_t int_chain_head;
    uint32_t bulk_chain_head;
} ohci_resources[MAX_OHCI_CONTROLLERS] __attribute__((aligned(4096)));

static struct ohci_iso_pending s_ohci_iso[MAX_OHCI_CONTROLLERS];

uint8_t ohci_dev_is_ls[MAX_OHCI_CONTROLLERS][128];
uint8_t ohci_dev_mps0[MAX_OHCI_CONTROLLERS][128];

static int ohci_controller_count = 0;
static void (*delay_ms)(uint64_t) = NULL;

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
    uint8_t armed;
    struct usb_device *device;
} ohci_pending_xfer;

static struct ohci_pending_xfer s_ohci_pending[MAX_OHCI_CONTROLLERS][MAX_OHCI_HID_SLOTS];

static struct ohci_bulk_pending {
    struct ohci_controller *ctrl;
    struct ohci_td_hw *td;
    void *data;
    uint16_t data_len;
    uint8_t direction;
    uint8_t active;
    uint8_t result_ready;
    int8_t last_result;
    int ri;
    int slot;
    uint8_t endpoint;
    struct usb_device *device;
    void *cookie;
} s_ohci_bulk_pending[MAX_OHCI_CONTROLLERS];

static struct ohci_bulk_slot {
    uint8_t in_use;
    uint8_t addr;
    uint8_t endpoint;
} s_ohci_bulk_slots[MAX_OHCI_CONTROLLERS][MAX_OHCI_BULK_SLOTS];

static inline void ohci_write(struct ohci_controller *o, uint32_t reg, uint32_t val) {
    uint64_t off = hhdm_req.response ? hhdm_req.response->offset : 0;
    *(volatile uint32_t *)(o->base_addr + off + reg) = val;
    asm volatile("mfence" ::: "memory");
}

static inline uint32_t ohci_read(struct ohci_controller *o, uint32_t reg) {
    uint64_t off = hhdm_req.response ? hhdm_req.response->offset : 0;
    return *(volatile uint32_t *)(o->base_addr + off + reg);
}

uint32_t ohci_mmio_read(struct ohci_controller *o, uint32_t reg) {
    return o ? ohci_read(o, reg) : 0;
}

void ohci_mmio_write(struct ohci_controller *o, uint32_t reg, uint32_t val) {
    if (o) ohci_write(o, reg, val);
}

void ohci_delay_ms(uint64_t ms) {
    if (delay_ms) delay_ms(ms);
}

static int ohci_res_index(struct ohci_controller *o) {
    for (int i = 0; i < MAX_OHCI_CONTROLLERS; i++) {
        if (&ohci_resources[i].ctrl == o) return i;
    }
    return -1;
}

int ohci_controller_index(struct ohci_controller *o) {
    return ohci_res_index(o);
}

uint8_t ohci_port_count(struct ohci_controller *o) {
    if (!o) return 0;
    return o->num_ports ? o->num_ports : 1;
}

static void ohci_flush_range(const void *buf, uint32_t len) {
    if (!buf || !len) return;
    uint64_t base = (uint64_t)buf & ~63ull;
    uint64_t end = (uint64_t)buf + len;
    for (uint64_t a = base; a < end; a += 64)
        asm volatile("clflush (%0)" :: "r"(a) : "memory");
    asm volatile("mfence" ::: "memory");
}

static struct usb_device *ohci_lookup_device(struct ohci_controller *o, uint8_t addr) {
    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        struct usb_device *d = (struct usb_device *)device_table[USB_DEVICE][i];
        if (d && d->address == addr && d->ctrl == (struct usb_controller *)o)
            return d;
    }
    return NULL;
}

static int ohci_detect_ls(struct ohci_controller *o, uint8_t addr) {
    int ri = ohci_res_index(o);
    if (ri < 0) return 0;
    if (addr == 0) return ohci_dev_is_ls[ri][0] ? 1 : 0;

    struct usb_device *d = ohci_lookup_device(o, addr);
    if (d) return d->is_low_speed ? 1 : 0;
    return ohci_dev_is_ls[ri][addr & 0x7F] ? 1 : 0;
}

static inline int ohci_mps0_valid(uint16_t m) {
    return m == 8 || m == 16 || m == 32 || m == 64;
}

static uint16_t ohci_control_mps(struct ohci_controller *o, uint8_t addr, int is_ls) {
    if (addr == 0 || is_ls) return 8;

    struct usb_device *d = ohci_lookup_device(o, addr);
    if (d) {
        if (ohci_mps0_valid(d->desc.bMaxPacketSize0)) return d->desc.bMaxPacketSize0;
        if (ohci_mps0_valid(d->max_packet_size)) return d->max_packet_size;
    }

    int ri = ohci_res_index(o);
    if (ri >= 0 && ohci_mps0_valid(ohci_dev_mps0[ri][addr & 0x7F]))
        return ohci_dev_mps0[ri][addr & 0x7F];

    return 64;
}

static uint16_t ohci_endpoint_mps(struct usb_device *d, uint8_t endpoint,
                                  int is_bulk, int is_ls) {
    uint16_t mps = is_ls ? 8u : (is_bulk ? 64u : 8u);

    if (d) {
        if (is_bulk) {
            for (int i = 0; i < d->bulk_ep_count; i++) {
                if (d->bulk_ep[i].address == endpoint && d->bulk_ep[i].max_packet_size) {
                    mps = d->bulk_ep[i].max_packet_size;
                    break;
                }
            }
        } else {
            if (d->endpoint_address == endpoint && d->max_packet_size)
                mps = d->max_packet_size;
            else if (d->hub_status_ep == endpoint && d->hub_status_ep_mps)
                mps = d->hub_status_ep_mps;
        }
    }

    if (mps == 0) mps = 8;
    if (is_ls && mps > 8) mps = 8;
    if (mps > 1023) mps = 1023;
    return mps;
}

static void ohci_ed_idle(struct ohci_ed_hw *ed) {
    ed->info |= OHCI_ED_SKIP;
    asm volatile("mfence" ::: "memory");
    ed->head_p = 0;
    ed->tail_p = 0;
    asm volatile("mfence" ::: "memory");
}

static void ohci_report_interrupt(int ri, int slot, uint8_t cc, uint16_t len) {
    struct ohci_pending_xfer *pend = &s_ohci_pending[ri][slot];
    int ok = (cc == 0);

    if (ok && pend->direction)
        ohci_flush_range(pend->data, len);

    usb_event_t evt = {
        .type = ok ? USB_EVENT_TRANSFER_DONE : USB_EVENT_TRANSFER_ERR,
        .src = USB_SRC_OHCI,
        .transfer_type = USB_XFER_INTERRUPT,
        .slot_id = (uint8_t)ri,
        .endpoint = pend->endpoint,
        .completion_code = cc,
        .data = ok ? pend->data : NULL,
        .data_len = ok ? len : 0,
        .device = pend->device,
        .cookie = pend->cookie,
    };
    usb_push_event(&evt);
    pend->active = 0;
}

static uint16_t ohci_td_transferred(struct ohci_td_hw *td, void *buf, uint16_t requested) {
    uint32_t cbp = td->cbp;
    if (cbp == 0) return requested;

    uint32_t start = (uint32_t)mm_ptr_to_phys(buf);
    if (cbp < start) return 0;
    uint32_t done = cbp - start;
    if (done > requested) done = requested;
    return (uint16_t)done;
}

static int ohci_service_interrupt_slot(int ri, int slot) {
    struct ohci_pending_xfer *pend = &s_ohci_pending[ri][slot];
    if (!pend->active || !pend->td) return 0;

    uint32_t info = pend->td->info;
    uint8_t cc = (uint8_t)(info >> 28);
    if (cc == 0xF) return 0;

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[OHCI_ED_INT_BASE + slot];
    uint16_t len = (cc == 0)
                   ? ohci_td_transferred(pend->td, pend->data, pend->data_len)
                   : 0;

    if (ed->head_p & OHCI_ED_HALTED) {
        ed->head_p = ed->tail_p & ~(uint32_t)OHCI_ED_HEAD_FLAGS_MASK;
        asm volatile("mfence" ::: "memory");
    }

    ed->info |= OHCI_ED_SKIP;
    asm volatile("mfence" ::: "memory");

    ohci_report_interrupt(ri, slot, cc, len);
    return 1;
}

static int ohci_service_bulk(int ri) {
    struct ohci_bulk_pending *pend = &s_ohci_bulk_pending[ri];
    if (!pend->active || !pend->td || pend->slot < 0) return 0;

    uint32_t info = pend->td->info;
    uint8_t cc = (uint8_t)(info >> 28);
    if (cc == 0xF) return 0;

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[OHCI_ED_BULK_BASE + pend->slot];
    int ok = (cc == 0);
    uint16_t len = ok ? ohci_td_transferred(pend->td, pend->data, pend->data_len) : 0;

    if (ok && pend->direction)
        ohci_flush_range(pend->data, len);

    ed->info |= OHCI_ED_SKIP;
    asm volatile("mfence" ::: "memory");

    if (ed->head_p & OHCI_ED_HALTED) {
        ed->head_p = (ed->tail_p & ~(uint32_t)OHCI_ED_HEAD_FLAGS_MASK)
                   | (ed->head_p & OHCI_ED_TOGGLE_CARRY);
        asm volatile("mfence" ::: "memory");
    }

    if (!ok) {
        usb_logrow_begin(USB_LOG_OHCI, USB_LOG_ERROR);
        usb_logrow_str("bulk xfer addr="); usb_logrow_dec(pend->device ? pend->device->address : 0);
        usb_logrow_str(" ep="); usb_logrow_hex32(pend->endpoint);
        usb_logrow_str(" cc="); usb_logrow_dec(cc);
        usb_logrow_end();
    }

    usb_event_t evt = {
        .type = ok ? USB_EVENT_BULK_DONE : USB_EVENT_BULK_ERR,
        .src = USB_SRC_OHCI,
        .transfer_type = USB_XFER_BULK,
        .slot_id = (uint8_t)ri,
        .endpoint = pend->endpoint,
        .completion_code = cc,
        .data = ok ? pend->data : NULL,
        .data_len = ok ? len : 0,
        .device = pend->device,
        .cookie = pend->cookie,
    };
    usb_push_bulk_event(&evt);

    pend->active = 0;
    pend->result_ready = 1;
    pend->last_result = ok ? 0 : -1;
    return ok ? 1 : -1;
}

static void ohci_hc_go_operational(struct ohci_controller *o);

void ohci_irq(void) {
    for (int i = 0; i < ohci_controller_count; i++) {
        struct ohci_controller *o = &ohci_resources[i].ctrl;
        if (!o->initialized) continue;

        uint32_t s = ohci_read(o, REG_INT_STATUS);
        if (s == 0xFFFFFFFFu) continue;
        s &= OHCI_INT_ALL;
        if (!s) continue;

        if (s & OHCI_INT_WDH) {
            ohci_resources[i].hcca.hcca_done_head = 0;
            asm volatile("mfence" ::: "memory");
        }

        ohci_write(o, REG_INT_STATUS, s);

        if (s & OHCI_INT_UE) {
            usb_logrow_begin(USB_LOG_OHCI, USB_LOG_ERROR);
            usb_logrow_str("controller "); usb_logrow_dec(i);
            usb_logrow_str(" unrecoverable error, restarting");
            usb_logrow_end();
            ohci_hc_go_operational(o);
            continue;
        }

        if (!(s & OHCI_INT_WDH)) continue;

        for (int slot = 0; slot < MAX_OHCI_HID_SLOTS; slot++)
            ohci_service_interrupt_slot(i, slot);

        ohci_poll_iso(o);
    }
}

static int ohci_control_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                                      void *setup, uint16_t setup_len,
                                      void *data, uint16_t data_len, uint8_t in)
{
    (void)setup_len;
    if (!o || !o->initialized || !setup) return -1;
    if (data_len && !data) return -1;

    int ri = ohci_res_index(o);
    if (ri < 0) return -1;

    int is_ls = ohci_detect_ls(o, addr);
    uint16_t mps = ohci_control_mps(o, addr, is_ls);

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[OHCI_ED_CONTROL];
    struct ohci_td_hw *td_setup = &ohci_resources[ri].td_pool[OHCI_TD_CONTROL + 0];
    struct ohci_td_hw *td_data = &ohci_resources[ri].td_pool[OHCI_TD_CONTROL + 1];
    struct ohci_td_hw *td_status = &ohci_resources[ri].td_pool[OHCI_TD_CONTROL + 2];
    struct ohci_td_hw *td_tail = &ohci_resources[ri].td_pool[OHCI_TD_CONTROL + 3];

    ohci_ed_idle(ed);

    for (int i = 0; i < 4; i++) {
        ohci_resources[ri].td_pool[OHCI_TD_CONTROL + i].info = OHCI_TD_CC_NOTACCESSED;
        ohci_resources[ri].td_pool[OHCI_TD_CONTROL + i].cbp = 0;
        ohci_resources[ri].td_pool[OHCI_TD_CONTROL + i].next_td = 0;
        ohci_resources[ri].td_pool[OHCI_TD_CONTROL + i].be = 0;
    }

    ohci_flush_range(setup, 8);

    td_setup->info = OHCI_TD_DP_SETUP | OHCI_TD_DI_NONE | OHCI_TD_T_DATA0 | OHCI_TD_CC_NOTACCESSED;
    td_setup->cbp = (uint32_t)mm_ptr_to_phys(setup);
    td_setup->be = td_setup->cbp + 7;
    td_setup->next_td = (data_len > 0)
                      ? (uint32_t)mm_ptr_to_phys(td_data)
                      : (uint32_t)mm_ptr_to_phys(td_status);

    if (data_len > 0) {
        if (in) {
            for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        }
        ohci_flush_range(data, data_len);

        td_data->info = (in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT)
                      | (in ? OHCI_TD_R : 0u)
                      | OHCI_TD_DI_NONE | OHCI_TD_T_DATA1 | OHCI_TD_CC_NOTACCESSED;
        td_data->cbp = (uint32_t)mm_ptr_to_phys(data);
        td_data->be = td_data->cbp + data_len - 1;
        td_data->next_td = (uint32_t)mm_ptr_to_phys(td_status);
    }

    td_status->info = ((data_len > 0 && in) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN)
                    | OHCI_TD_R | OHCI_TD_DI_NOW | OHCI_TD_T_DATA1 | OHCI_TD_CC_NOTACCESSED;
    td_status->cbp = 0;
    td_status->be = 0;
    td_status->next_td = (uint32_t)mm_ptr_to_phys(td_tail);

    td_tail->info = 0;
    td_tail->cbp = 0;
    td_tail->next_td = 0;
    td_tail->be = 0;

    ed->info = (uint32_t)(addr & 0x7Fu)
             | ((uint32_t)(ep & 0xFu) << 7)
             | (is_ls ? OHCI_ED_LOWSPEED : 0u)
             | ((uint32_t)mps << 16);
    ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
    ed->next_ed = 0;
    asm volatile("mfence" ::: "memory");
    ed->head_p = (uint32_t)mm_ptr_to_phys(td_setup) & ~(uint32_t)OHCI_ED_HEAD_FLAGS_MASK;
    asm volatile("mfence" ::: "memory");

    ohci_write(o, REG_CONTROL_CURRENT, 0);
    ohci_write(o, REG_CONTROL_HEAD, (uint32_t)mm_ptr_to_phys(ed));
    ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) | OHCI_CTRL_CLE);
    ohci_write(o, REG_COMMAND_STATUS, OHCI_CS_CLF);

    int rc = -1;
    uint8_t cc = 0;
    uint16_t got = 0;

    for (int ms = 0; ms < OHCI_CTRL_TIMEOUT_MS; ms++) {
        uint32_t st = td_status->info;
        uint8_t st_cc = (uint8_t)((st >> 28) & 0xFu);

        if (st_cc != 0xF) {
            cc = st_cc;
            rc = (st_cc == 0) ? 0 : -1;
            break;
        }

        if (ed->head_p & OHCI_ED_HALTED) {
            uint32_t dinfo = td_data->info;
            uint32_t sinfo = td_setup->info;
            cc = (uint8_t)((dinfo >> 28) & 0xFu);
            if (cc == 0xF) cc = (uint8_t)((sinfo >> 28) & 0xFu);
            if (cc == 0xF) cc = 0xE;
            rc = -1;
            break;
        }

        delay_ms(1);
    }

    if (rc == 0 && data_len > 0)
        got = ohci_td_transferred(td_data, data, data_len);

    ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) & ~OHCI_CTRL_CLE);
    ohci_write(o, REG_CONTROL_HEAD, 0);
    ohci_write(o, REG_CONTROL_CURRENT, 0);
    ohci_ed_idle(ed);

    if (rc == 0 && in && data_len > 0)
        ohci_flush_range(data, got ? got : data_len);

    if (rc != 0) {
        usb_logrow_begin(USB_LOG_OHCI, USB_LOG_ERROR);
        usb_logrow_str("control xfer addr="); usb_logrow_dec(addr);
        usb_logrow_str(" ep="); usb_logrow_dec(ep);
        usb_logrow_str(cc ? ": completion code=" : ": timed out, code=");
        usb_logrow_hex32(cc);
        usb_logrow_end();
    }

    return rc;
}

static int ohci_control_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                                 void *setup, uint16_t setup_len,
                                 void *data, uint16_t data_len, uint8_t in)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_control_transfer_impl(o, addr, ep, setup, setup_len, data, data_len, in);
    spin_unlock(&o->lock);
    return ret;
}

static int ohci_interrupt_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                                        void *data, uint16_t data_len, uint8_t direction)
{
    if (!o || !o->initialized || !data || !data_len) return -1;

    int ri = ohci_res_index(o);
    if (ri < 0) return -1;

    int slot_idx = -1;
    for (int s = 0; s < MAX_OHCI_HID_SLOTS; s++) {
        struct ohci_pending_xfer *p = &s_ohci_pending[ri][s];
        if (p->armed && p->endpoint == ep && p->device &&
            (uint8_t)p->device->address == addr) { slot_idx = s; break; }
    }
    if (slot_idx < 0) {
        for (int s = 0; s < MAX_OHCI_HID_SLOTS; s++) {
            if (!s_ohci_pending[ri][s].armed) { slot_idx = s; break; }
        }
    }
    if (slot_idx < 0) return -2;

    struct ohci_pending_xfer *pend = &s_ohci_pending[ri][slot_idx];

    if (pend->active) {
        if (!ohci_service_interrupt_slot(ri, slot_idx)) return -2;
    }

    if (direction) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        ohci_flush_range(data, data_len);
    } else {
        ohci_flush_range(data, data_len);
    }

    uint8_t ep_num = ep & 0x0Fu;
    int is_ls = ohci_detect_ls(o, addr);
    struct usb_device *dev = ohci_lookup_device(o, addr);
    uint16_t mps = ohci_endpoint_mps(dev, ep, 0, is_ls);

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[OHCI_ED_INT_BASE + slot_idx];
    struct ohci_td_hw *td_data = &ohci_resources[ri].td_pool[OHCI_TD_INT_BASE + slot_idx * 2];
    struct ohci_td_hw *td_tail = &ohci_resources[ri].td_pool[OHCI_TD_INT_BASE + slot_idx * 2 + 1];

    td_tail->info = 0;
    td_tail->cbp = 0;
    td_tail->next_td = 0;
    td_tail->be = 0;

    td_data->info = (direction ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT)
                  | OHCI_TD_R | OHCI_TD_DI_NOW | OHCI_TD_T_CARRY | OHCI_TD_CC_NOTACCESSED;
    td_data->cbp = (uint32_t)mm_ptr_to_phys(data);
    td_data->be = td_data->cbp + data_len - 1;
    td_data->next_td = (uint32_t)mm_ptr_to_phys(td_tail);

    uint32_t carry = ed->head_p & OHCI_ED_TOGGLE_CARRY;
    uint32_t new_info = (uint32_t)(addr & 0x7Fu)
                      | ((uint32_t)(ep_num & 0xFu) << 7)
                      | (direction ? OHCI_ED_DIR_IN : OHCI_ED_DIR_OUT)
                      | (is_ls ? OHCI_ED_LOWSPEED : 0u)
                      | ((uint32_t)mps << 16);

    if ((ed->info & ~(uint32_t)OHCI_ED_SKIP) != new_info) carry = 0;

    ed->info = new_info | OHCI_ED_SKIP;
    asm volatile("mfence" ::: "memory");
    ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
    ed->head_p = ((uint32_t)mm_ptr_to_phys(td_data) & ~(uint32_t)OHCI_ED_HEAD_FLAGS_MASK) | carry;
    asm volatile("mfence" ::: "memory");
    ed->info = new_info;
    asm volatile("mfence" ::: "memory");

    pend->ctrl = o;
    pend->td = td_data;
    pend->data = data;
    pend->data_len = data_len;
    pend->direction = direction;
    pend->ri = ri;
    pend->endpoint = ep;
    pend->device = dev;
    pend->cookie = dev;
    pend->active = 1;
    pend->armed = 1;

    return -2;
}

static int ohci_interrupt_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                                   void *data, uint16_t data_len, uint8_t direction)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_interrupt_transfer_impl(o, addr, ep, data, data_len, direction);
    spin_unlock(&o->lock);
    return ret;
}

static int ohci_bulk_slot_get(int ri, uint8_t addr, uint8_t ep) {
    struct ohci_bulk_slot *slots = s_ohci_bulk_slots[ri];

    for (int i = 0; i < MAX_OHCI_BULK_SLOTS; i++)
        if (slots[i].in_use && slots[i].addr == addr && slots[i].endpoint == ep)
            return i;

    for (int i = 0; i < MAX_OHCI_BULK_SLOTS; i++) {
        if (slots[i].in_use) continue;
        slots[i].in_use = 1;
        slots[i].addr = addr;
        slots[i].endpoint = ep;
        ohci_resources[ri].ed_pool[OHCI_ED_BULK_BASE + i].head_p = 0;
        ohci_resources[ri].ed_pool[OHCI_ED_BULK_BASE + i].tail_p = 0;
        asm volatile("mfence" ::: "memory");
        return i;
    }

    return -1;
}

static int ohci_bulk_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                                   void *data, uint16_t data_len, uint8_t direction)
{
    if (!o || !o->initialized || !data || !data_len) return -1;

    int ri = ohci_res_index(o);
    if (ri < 0) return -1;

    struct ohci_bulk_pending *pend = &s_ohci_bulk_pending[ri];

    if (pend->active)
        ohci_service_bulk(ri);

    if (pend->result_ready) {
        pend->result_ready = 0;
        return pend->last_result;
    }

    if (pend->active) return -2;

    int slot = ohci_bulk_slot_get(ri, addr, ep);
    if (slot < 0) return -1;

    if (direction) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
    }
    ohci_flush_range(data, data_len);

    uint8_t ep_num = ep & 0x0Fu;
    int is_ls = ohci_detect_ls(o, addr);
    struct usb_device *dev = ohci_lookup_device(o, addr);
    uint16_t mps = ohci_endpoint_mps(dev, ep, 1, is_ls);

    struct ohci_ed_hw *ed = &ohci_resources[ri].ed_pool[OHCI_ED_BULK_BASE + slot];
    struct ohci_td_hw *td_data = &ohci_resources[ri].td_pool[OHCI_TD_BULK_BASE + slot * 2];
    struct ohci_td_hw *td_tail = &ohci_resources[ri].td_pool[OHCI_TD_BULK_BASE + slot * 2 + 1];

    td_tail->info = 0;
    td_tail->cbp = 0;
    td_tail->next_td = 0;
    td_tail->be = 0;

    td_data->info = (direction ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT)
                  | OHCI_TD_R | OHCI_TD_DI_NOW | OHCI_TD_T_CARRY | OHCI_TD_CC_NOTACCESSED;
    td_data->cbp = (uint32_t)mm_ptr_to_phys(data);
    td_data->be = td_data->cbp + data_len - 1u;
    td_data->next_td = (uint32_t)mm_ptr_to_phys(td_tail);

    uint32_t carry = ed->head_p & OHCI_ED_TOGGLE_CARRY;
    uint32_t new_info = (uint32_t)(addr & 0x7Fu)
                      | ((uint32_t)(ep_num & 0xFu) << 7)
                      | (direction ? OHCI_ED_DIR_IN : OHCI_ED_DIR_OUT)
                      | (is_ls ? OHCI_ED_LOWSPEED : 0u)
                      | ((uint32_t)mps << 16);

    ed->info = new_info | OHCI_ED_SKIP;
    asm volatile("mfence" ::: "memory");
    ed->tail_p = (uint32_t)mm_ptr_to_phys(td_tail);
    ed->head_p = ((uint32_t)mm_ptr_to_phys(td_data) & ~(uint32_t)OHCI_ED_HEAD_FLAGS_MASK) | carry;
    asm volatile("mfence" ::: "memory");
    ed->info = new_info;
    asm volatile("mfence" ::: "memory");

    ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) | OHCI_CTRL_BLE);
    ohci_write(o, REG_COMMAND_STATUS, OHCI_CS_BLF);

    pend->ctrl = o;
    pend->td = td_data;
    pend->data = data;
    pend->data_len = data_len;
    pend->direction = direction;
    pend->active = 1;
    pend->result_ready = 0;
    pend->ri = ri;
    pend->slot = slot;
    pend->endpoint = ep;
    pend->device = dev;
    pend->cookie = dev;

    return -2;
}

static int ohci_bulk_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                              void *data, uint16_t data_len, uint8_t direction)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_bulk_transfer_impl(o, addr, ep, data, data_len, direction);
    spin_unlock(&o->lock);
    return ret;
}

static int ohci_iso_transfer_impl(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                                  void *data, uint16_t total_len, uint8_t n_frames,
                                  const uint16_t *frame_lens, uint8_t direction)
{
    if (!o || !o->initialized || !data) return -1;
    if (!n_frames || n_frames > OHCI_ISO_MAX_FRAMES) return -1;

    int ri = ohci_res_index(o);
    if (ri < 0) return -1;
    if (s_ohci_iso[ri].active) return -2;

    uint8_t ep_num = ep & 0x0Fu;
    uint8_t is_in = direction ? 1u : 0u;

    uint32_t buf_phys = (uint32_t)mm_ptr_to_phys(data);
    if (((buf_phys + total_len - 1u) & ~0xFFFu) - (buf_phys & ~0xFFFu) > 0x1000u)
        return -1;

    uint16_t per_frame = (uint16_t)(total_len / n_frames);
    uint16_t offset = 0;
    for (uint8_t i = 0; i < n_frames; i++) {
        uint16_t flen = frame_lens ? frame_lens[i] : per_frame;
        if (offset + flen > total_len) flen = (uint16_t)(total_len - offset);
        s_ohci_iso[ri].frame_offsets[i] = offset;
        s_ohci_iso[ri].frame_lens[i] = flen;
        s_ohci_iso[ri].frame_reaped[i] = 0;
        offset = (uint16_t)(offset + flen);
    }

    uint16_t cur_frame = ohci_resources[ri].hcca.hcca_frame_number;
    uint16_t sf = (uint16_t)(cur_frame + 4u);

    struct ohci_iso_td_hw *itd = &s_ohci_iso[ri].iso_td;

    itd->bp0 = buf_phys & ~0xFFFu;
    itd->be = buf_phys + total_len - 1u;
    itd->next_td = 0u;

    for (uint8_t i = 0; i < n_frames; i++) {
        uint32_t off = (uint32_t)(buf_phys + s_ohci_iso[ri].frame_offsets[i]);
        uint16_t psw = (uint16_t)(off & 0xFFFu);
        if ((off & ~0xFFFu) != (buf_phys & ~0xFFFu)) psw |= (1u << 12);
        itd->psw[i] = (uint16_t)(psw | 0xE000u);
    }

    itd->flags = ((uint32_t)(n_frames - 1u) << 24)
               | OHCI_TD_DI_NOW
               | (uint32_t)sf
               | OHCI_TD_CC_NOTACCESSED;
    asm volatile("mfence" ::: "memory");

    struct ohci_ed_hw *ied = &s_ohci_iso[ri].iso_ed;
    ied->info = (uint32_t)(addr & 0x7Fu)
              | ((uint32_t)(ep_num & 0xFu) << 7)
              | ((uint32_t)(is_in ? 2u : 1u) << 11)
              | OHCI_ED_ISO
              | ((uint32_t)(per_frame ? per_frame : 1u) << 16);
    ied->tail_p = 0u;
    ied->head_p = (uint32_t)mm_ptr_to_phys(itd) & ~(uint32_t)OHCI_ED_HEAD_FLAGS_MASK;
    ied->next_ed = 0u;
    asm volatile("mfence" ::: "memory");

    ohci_resources[ri].ed_pool[OHCI_ED_INT_BASE + MAX_OHCI_HID_SLOTS - 1].next_ed =
        (uint32_t)mm_ptr_to_phys(ied);
    asm volatile("mfence" ::: "memory");

    ohci_write(o, REG_CONTROL, ohci_read(o, REG_CONTROL) | OHCI_CTRL_PLE | OHCI_CTRL_IE);

    s_ohci_iso[ri].active = 1;
    s_ohci_iso[ri].n_frames = n_frames;
    s_ohci_iso[ri].endpoint = ep;
    s_ohci_iso[ri].direction = is_in;
    s_ohci_iso[ri].data = data;
    s_ohci_iso[ri].total_len = total_len;
    s_ohci_iso[ri].cookie = NULL;

    return 0;
}

static int ohci_iso_transfer(struct ohci_controller *o, uint8_t addr, uint8_t ep,
                             void *data, uint16_t total_len, uint8_t n_frames,
                             const uint16_t *frame_lens, uint8_t direction)
{
    if (!o) return -1;
    spin_lock(&o->lock);
    int ret = ohci_iso_transfer_impl(o, addr, ep, data, total_len, n_frames, frame_lens, direction);
    spin_unlock(&o->lock);
    return ret;
}

void ohci_poll_iso(struct ohci_controller *o) {
    if (!o || !o->initialized) return;
    int ri = ohci_res_index(o);
    if (ri < 0 || !s_ohci_iso[ri].active) return;

    struct ohci_iso_td_hw *itd = &s_ohci_iso[ri].iso_td;

    int all_done = 1;
    for (uint8_t fi = 0; fi < s_ohci_iso[ri].n_frames; fi++) {
        if (s_ohci_iso[ri].frame_reaped[fi]) continue;

        uint16_t psw = itd->psw[fi];
        uint8_t cc = (uint8_t)((psw >> 12) & 0xFu);
        if (cc == 0xFu || cc == 0x7u) { all_done = 0; continue; }

        int ok = (cc == 0);
        uint16_t actual = (uint16_t)(psw & 0x7FFu);
        if (actual > s_ohci_iso[ri].frame_lens[fi])
            actual = s_ohci_iso[ri].frame_lens[fi];
        if (!ok) actual = 0;

        if (ok && s_ohci_iso[ri].direction)
            ohci_flush_range((uint8_t *)s_ohci_iso[ri].data +
                             s_ohci_iso[ri].frame_offsets[fi], actual);

        usb_event_t evt = {
            .type = ok ? USB_EVENT_ISO_DONE : USB_EVENT_ISO_ERR,
            .src = USB_SRC_OHCI,
            .transfer_type = USB_XFER_ISO,
            .slot_id = (uint8_t)ri,
            .endpoint = s_ohci_iso[ri].endpoint,
            .completion_code = cc,
            .data = ok ? ((uint8_t *)s_ohci_iso[ri].data
                          + s_ohci_iso[ri].frame_offsets[fi])
                       : NULL,
            .data_len = actual,
            .iso_frame_index = fi,
            .iso_expected_len = s_ohci_iso[ri].frame_lens[fi],
            .cookie = s_ohci_iso[ri].cookie,
        };
        usb_push_iso_event(&evt);
        s_ohci_iso[ri].frame_reaped[fi] = 1;
    }

    if (all_done) {
        ohci_resources[ri].ed_pool[OHCI_ED_INT_BASE + MAX_OHCI_HID_SLOTS - 1].next_ed = 0u;
        asm volatile("mfence" ::: "memory");
        s_ohci_iso[ri].active = 0;
    }
}

static void ohci_init_lists(int ri) {
    struct ohci_ed_hw *eds = ohci_resources[ri].ed_pool;

    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) {
        eds[i].info = OHCI_ED_SKIP;
        eds[i].tail_p = 0;
        eds[i].head_p = 0;
        eds[i].next_ed = 0;
    }

    for (int i = 0; i < OHCI_TD_POOL_SIZE; i++) {
        ohci_resources[ri].td_pool[i].info = 0;
        ohci_resources[ri].td_pool[i].cbp = 0;
        ohci_resources[ri].td_pool[i].next_td = 0;
        ohci_resources[ri].td_pool[i].be = 0;
    }

    for (int s = 0; s < MAX_OHCI_HID_SLOTS; s++) {
        struct ohci_ed_hw *ed = &eds[OHCI_ED_INT_BASE + s];
        ed->next_ed = (s + 1 < MAX_OHCI_HID_SLOTS)
                    ? (uint32_t)mm_ptr_to_phys(&eds[OHCI_ED_INT_BASE + s + 1])
                    : 0u;
        s_ohci_pending[ri][s].active = 0;
        s_ohci_pending[ri][s].armed = 0;
        s_ohci_pending[ri][s].device = NULL;
        s_ohci_pending[ri][s].td = NULL;
    }

    for (int b = 0; b < MAX_OHCI_BULK_SLOTS; b++) {
        struct ohci_ed_hw *ed = &eds[OHCI_ED_BULK_BASE + b];
        ed->next_ed = (b + 1 < MAX_OHCI_BULK_SLOTS)
                    ? (uint32_t)mm_ptr_to_phys(&eds[OHCI_ED_BULK_BASE + b + 1])
                    : 0u;
        s_ohci_bulk_slots[ri][b].in_use = 0;
        s_ohci_bulk_slots[ri][b].addr = 0;
        s_ohci_bulk_slots[ri][b].endpoint = 0;
    }

    s_ohci_bulk_pending[ri].active = 0;
    s_ohci_bulk_pending[ri].result_ready = 0;
    s_ohci_bulk_pending[ri].slot = -1;
    s_ohci_bulk_pending[ri].device = NULL;
    s_ohci_bulk_pending[ri].td = NULL;
    s_ohci_iso[ri].active = 0;

    ohci_resources[ri].bulk_chain_head = (uint32_t)mm_ptr_to_phys(&eds[OHCI_ED_BULK_BASE]);

    uint32_t int_head = (uint32_t)mm_ptr_to_phys(&eds[OHCI_ED_INT_BASE]);
    ohci_resources[ri].int_chain_head = int_head;

    for (int i = 0; i < 32; i++)
        ohci_resources[ri].hcca.hcca_interrupt_table[i] = int_head;
    ohci_resources[ri].hcca.hcca_frame_number = 0;
    ohci_resources[ri].hcca.hcca_done_head = 0;
    asm volatile("mfence" ::: "memory");
}

static void ohci_hc_go_operational(struct ohci_controller *o) {
    int ri = ohci_res_index(o);
    if (ri < 0) return;

    uint32_t fi = ohci_read(o, REG_FM_INTERVAL) & 0x3FFFu;
    if (fi < 0x2000u || fi > 0x3FFFu) fi = OHCI_DEFAULT_FI;

    uint32_t fsmps = ((fi - 210u) * 6u) / 7u;
    uint32_t fit = ohci_read(o, REG_FM_INTERVAL) & 0x80000000u;

    ohci_write(o, REG_HCCA, (uint32_t)mm_ptr_to_phys(&ohci_resources[ri].hcca));
    ohci_write(o, REG_CONTROL_HEAD, 0);
    ohci_write(o, REG_CONTROL_CURRENT, 0);
    ohci_write(o, REG_BULK_HEAD, ohci_resources[ri].bulk_chain_head);
    ohci_write(o, REG_BULK_CURRENT, 0);

    ohci_write(o, REG_FM_INTERVAL, (fit ^ 0x80000000u) | (fsmps << 16) | fi);
    ohci_write(o, REG_PERIODIC_START, (fi * 9u) / 10u);
    ohci_write(o, REG_LS_THRESHOLD, 0x0628u);

    ohci_write(o, REG_INT_STATUS, 0xFFFFFFFFu);
    ohci_write(o, REG_CONTROL,
               (ohci_read(o, REG_CONTROL) & ~(OHCI_CTRL_HCFS_MASK | OHCI_CTRL_IR | OHCI_CTRL_CBSR_MASK))
               | 0x3u | OHCI_CTRL_PLE | OHCI_CTRL_IE | OHCI_CTRL_CLE | OHCI_CTRL_BLE
               | OHCI_CTRL_HCFS_OPERATIONAL);

    ohci_write(o, REG_INT_ENABLE,
               OHCI_INT_WDH | OHCI_INT_UE | OHCI_INT_RHSC | OHCI_INT_MIE);
}

static void ohci_take_ownership(struct ohci_controller *o) {
    uint32_t ctrl = ohci_read(o, REG_CONTROL);

    if (ctrl & OHCI_CTRL_IR) {
        ohci_write(o, REG_INT_ENABLE, OHCI_INT_OC);
        ohci_write(o, REG_COMMAND_STATUS, OHCI_CS_OCR);
        for (int i = 0; i < 500; i++) {
            if (!(ohci_read(o, REG_CONTROL) & OHCI_CTRL_IR)) break;
            delay_ms(1);
        }
        ohci_write(o, REG_INT_DISABLE, OHCI_INT_OC);
        return;
    }

    if ((ctrl & OHCI_CTRL_HCFS_MASK) != OHCI_CTRL_HCFS_RESET) {
        if ((ctrl & OHCI_CTRL_HCFS_MASK) != OHCI_CTRL_HCFS_OPERATIONAL) {
            ohci_write(o, REG_CONTROL,
                       (ctrl & ~OHCI_CTRL_HCFS_MASK) | OHCI_CTRL_HCFS_RESUME);
            delay_ms(20);
        }
    } else {
        delay_ms(100);
    }
}

static void ohci_hc_reset(struct ohci_controller *o) {
    uint32_t saved_fm = ohci_read(o, REG_FM_INTERVAL);

    ohci_write(o, REG_INT_DISABLE, 0xFFFFFFFFu);
    ohci_write(o, REG_COMMAND_STATUS, OHCI_CS_HCR);

    for (int i = 0; i < 30; i++) {
        if (!(ohci_read(o, REG_COMMAND_STATUS) & OHCI_CS_HCR)) break;
        delay_ms(1);
    }

    ohci_write(o, REG_FM_INTERVAL, saved_fm);
}

static void ohci_power_ports(struct ohci_controller *o) {
    uint32_t desc_a = ohci_read(o, REG_RH_DESCRIPTOR_A);
    uint8_t ndp = (uint8_t)(desc_a & 0xFFu);
    uint8_t potpgt = (uint8_t)((desc_a >> 24) & 0xFFu);

    if (ndp == 0 || ndp > OHCI_MAX_PORTS) ndp = 1;
    o->num_ports = ndp;
    o->power_on_delay_ms = (uint8_t)(potpgt ? potpgt * 2u : 20u);

    if (desc_a & (1u << 9)) return;

    ohci_write(o, REG_RH_STATUS, 1u << 16);

    for (uint8_t p = 1; p <= ndp; p++)
        ohci_write(o, REG_RH_PORT_STATUS + (p - 1) * 4u, 1u << 8);

    delay_ms(o->power_on_delay_ms);
}

static void ohci_irq_handler(struct registers *regs) { (void)regs; ohci_irq(); }

static void ohci_scan_pci(void) {
    struct usb_controller *ctrls = pci_get_usb_controllers();

    for (int i = 0; i < 8; i++) {
        if (ctrls[i].type != USB_TYPE_OHCI || !ctrls[i].pci) continue;
        if (ohci_controller_count >= MAX_OHCI_CONTROLLERS) break;
        if (!ctrls[i].base_addr) continue;

        int ri = ohci_controller_count;
        struct ohci_controller *o = &ohci_resources[ri].ctrl;
        o->base_addr = ctrls[i].base_addr;
        o->type = USB_TYPE_OHCI;
        o->is_low_speed = 0;
        o->initialized = 0;

        struct pci_device *pdev = ctrls[i].pci;
        pci_enable_bus_mastering(pdev);

        uint32_t cmd = pci_read_config(pdev->bus, pdev->slot, pdev->func, 0x04);
        pci_write_config(pdev->bus, pdev->slot, pdev->func, 0x04, cmd | 0x06);

        if (ohci_read(o, OHCI_HcRevision) == 0xFFFFFFFFu) continue;

        ohci_take_ownership(o);
        ohci_hc_reset(o);
        ohci_init_lists(ri);
        ohci_hc_go_operational(o);
        ohci_power_ports(o);

        uint8_t lapic = apic_get_lapic_id();
        int vec_slot = (ri < OHCI_MAX_IRQ_VECTORS) ? ri : (OHCI_MAX_IRQ_VECTORS - 1);
        uint8_t irq_vec = (uint8_t)(MSI_VECTOR_OHCI_BASE + vec_slot);

        uint8_t irq_line = pdev->irq_line;
        if (irq_line != 0 && irq_line != 0xFF)
            ioapic_map_pci_irq(irq_line, irq_vec, lapic);
        else
            ioapic_map_pci_irq(11, irq_vec, lapic);

        irq_register_handler(irq_vec, ohci_irq_handler);

        o->initialized = 1;
        usb_logrow_begin(USB_LOG_OHCI, USB_LOG_INFO);
        usb_logrow_str("controller "); usb_logrow_dec(ri);
        usb_logrow_str(" ready, base=");
        usb_logrow_hex64(o->base_addr);
        usb_logrow_str(" ports="); usb_logrow_dec(o->num_ports);
        usb_logrow_end();
        ohci_controller_count++;
    }
}

static struct ohci_controller *ohci_get_ctrl(int idx) {
    return (idx >= 0 && idx < ohci_controller_count)
           ? &ohci_resources[idx].ctrl : NULL;
}

static int ohci_get_count(void) { return ohci_controller_count; }

static int ohci_enumerate(struct ohci_controller *o, uint8_t port) {
    (void)o; (void)port; return 0;
}

static void ohci_start(struct ohci_controller *o) {
    if (!o || !o->initialized) return;
    ohci_hc_go_operational(o);
}

static void ohci_stop(struct ohci_controller *o) {
    if (!o || !o->initialized) return;
    ohci_write(o, REG_CONTROL,
               (ohci_read(o, REG_CONTROL) & ~(OHCI_CTRL_HCFS_MASK | OHCI_CTRL_PLE |
                                              OHCI_CTRL_IE | OHCI_CTRL_CLE | OHCI_CTRL_BLE))
               | OHCI_CTRL_HCFS_SUSPEND);
}

static int ohci_reset_port(struct ohci_controller *o, uint8_t port) {
    if (!o || !o->initialized) return -1;
    if (port < 1 || port > ohci_port_count(o)) return -1;

    uint32_t reg = REG_RH_PORT_STATUS + (port - 1) * 4u;
    uint32_t status = ohci_read(o, reg);
    if (status == 0xFFFFFFFFu || !(status & 0x1u)) return -1;

    if (!(status & (1u << 8))) {
        ohci_write(o, reg, 1u << 8);
        delay_ms(o->power_on_delay_ms ? o->power_on_delay_ms : 20);
    }

    ohci_write(o, reg, (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20));
    ohci_write(o, reg, 1u << 4);

    int t = 200;
    while (t-- > 0) {
        status = ohci_read(o, reg);
        if (status == 0xFFFFFFFFu) return -1;
        if (status & (1u << 20)) break;
        delay_ms(1);
    }

    ohci_write(o, reg, 1u << 20);
    delay_ms(20);

    status = ohci_read(o, reg);
    if (!(status & 0x1u)) return -1;

    if (!(status & 0x2u)) {
        ohci_write(o, reg, 1u << 1);
        delay_ms(10);
        status = ohci_read(o, reg);
    }
    if (!(status & 0x2u)) return -1;

    int ri = ohci_res_index(o);
    uint8_t is_ls = (uint8_t)((status >> 9) & 1u);
    o->is_low_speed = is_ls;
    if (ri >= 0) ohci_dev_is_ls[ri][0] = is_ls;

    return 0;
}

void ohci_reset_endpoint_toggle(struct ohci_controller *o, uint8_t addr, uint8_t ep) {
    if (!o || !o->initialized) return;

    int ri = ohci_res_index(o);
    if (ri < 0) return;

    uint32_t match = (uint32_t)(addr & 0x7Fu) | ((uint32_t)(ep & 0xFu) << 7);
    struct ohci_ed_hw *ed_pool = ohci_resources[ri].ed_pool;

    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) {
        struct ohci_ed_hw *ed = &ed_pool[i];
        if ((ed->info & 0x7FFu) != match) continue;

        ed->head_p &= ~(uint32_t)(OHCI_ED_TOGGLE_CARRY | OHCI_ED_HALTED);
        asm volatile("mfence" ::: "memory");
    }
}

void ohci_notify_disconnect(struct usb_device *dev) {
    if (!dev) return;

    for (int ri = 0; ri < MAX_OHCI_CONTROLLERS; ri++) {
        for (int s = 0; s < MAX_OHCI_HID_SLOTS; s++) {
            struct ohci_pending_xfer *p = &s_ohci_pending[ri][s];
            if (!p->armed || p->device != dev) continue;

            ohci_ed_idle(&ohci_resources[ri].ed_pool[OHCI_ED_INT_BASE + s]);

            p->active = 0;
            p->armed = 0;
            p->device = NULL;
            p->cookie = NULL;
            p->td = NULL;
        }

        struct ohci_bulk_pending *b = &s_ohci_bulk_pending[ri];
        if (b->device == dev) {
            if (b->slot >= 0)
                ohci_ed_idle(&ohci_resources[ri].ed_pool[OHCI_ED_BULK_BASE + b->slot]);
            b->active = 0;
            b->result_ready = 0;
            b->slot = -1;
            b->device = NULL;
            b->cookie = NULL;
            b->td = NULL;
        }

        for (int bs = 0; bs < MAX_OHCI_BULK_SLOTS; bs++) {
            if (!s_ohci_bulk_slots[ri][bs].in_use) continue;
            if (s_ohci_bulk_slots[ri][bs].addr != dev->address) continue;
            ohci_ed_idle(&ohci_resources[ri].ed_pool[OHCI_ED_BULK_BASE + bs]);
            s_ohci_bulk_slots[ri][bs].in_use = 0;
        }

        if (ri < ohci_controller_count &&
            dev->ctrl == (struct usb_controller *)&ohci_resources[ri].ctrl)
        {
            ohci_dev_is_ls[ri][dev->address & 0x7F] = 0;
            ohci_dev_mps0[ri][dev->address & 0x7F] = 0;
        }
    }
}

struct ohci_driver ohci_driver_loaded = {
    .get_controller_count = ohci_get_count,
    .get_controller = ohci_get_ctrl,
    .control_transfer = ohci_control_transfer,
    .interrupt_transfer = ohci_interrupt_transfer,
    .bulk_transfer = ohci_bulk_transfer,
    .iso_transfer = ohci_iso_transfer,
    .reset_port = ohci_reset_port,
    .enumerate_device = ohci_enumerate,
    .start = ohci_start,
    .stop = ohci_stop,
    .reset_endpoint_toggle = ohci_reset_endpoint_toggle,
    .notify_disconnect = ohci_notify_disconnect,
};

struct ohci_driver *return_ohci_driver(void) {
    pci_init();

    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc || !tsc->sleep_tsc_ms) return &ohci_driver_loaded;

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
