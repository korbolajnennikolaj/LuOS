#include "drivers/USB/uhci.h"

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

#include <ports.h>
#include <stddef.h>

#define UHCI_DBG 0
#if UHCI_DBG
#define DBG_COM1 0x3F8
static int dbg_inited = 0;
static void dbg_init(void) {
    if (dbg_inited) return;
    dbg_inited = 1;
    outb(DBG_COM1 + 1, 0x00);
    outb(DBG_COM1 + 3, 0x80);
    outb(DBG_COM1 + 0, 0x03);
    outb(DBG_COM1 + 1, 0x00);
    outb(DBG_COM1 + 3, 0x03);
    outb(DBG_COM1 + 2, 0xC7);
    outb(DBG_COM1 + 4, 0x0B);
}
static void dbg_putc(char c) {
    dbg_init();
    for (int spin = 0; spin < 100000 && !(inb(DBG_COM1 + 5) & 0x20); spin++) {}
    outb(DBG_COM1, (uint8_t)c);
}
static void dbg_str(const char *s) { while (*s) dbg_putc(*s++); }
static void dbg_hex32(uint32_t v) {
    dbg_str("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        dbg_putc(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
    }
}
static void dbg_dec(int v) {
    char buf[12]; int n = 0; int neg = v < 0;
    unsigned int uv = neg ? (unsigned int)(-v) : (unsigned int)v;
    if (uv == 0) { dbg_putc('0'); return; }
    while (uv) { buf[n++] = '0' + (uv % 10); uv /= 10; }
    if (neg) dbg_putc('-');
    while (n) dbg_putc(buf[--n]);
}
#else
static void dbg_str(const char *s) { (void)s; }
static void dbg_hex32(uint32_t v) { (void)v; }
static void dbg_dec(int v) { (void)v; }
#endif

#define TD_PID_SETUP 0x2D
#define TD_PID_IN 0x69
#define TD_PID_OUT 0xE1

#define UHCI_ISO_MAX_FRAMES 8
#define UHCI_CTRL_TIMEOUT_MS 500
#define UHCI_MAX_IRQ_VECTORS 4

#define TD_CS_ACTLEN_MASK 0x7FFu
#define TD_CS_BITSTUFF (1u << 17)
#define TD_CS_TIMEOUTCRC (1u << 18)
#define TD_CS_NAK (1u << 19)
#define TD_CS_BABBLE (1u << 20)
#define TD_CS_BUFERR (1u << 21)
#define TD_CS_STALL (1u << 22)
#define TD_CS_ACTIVE (1u << 23)
#define TD_CS_IOC (1u << 24)
#define TD_CS_IOS (1u << 25)
#define TD_CS_LS (1u << 26)
#define TD_CS_CERR3 (3u << 27)
#define TD_CS_SPD (1u << 29)
#define TD_CS_ERROR_MASK (TD_CS_STALL | TD_CS_BUFERR | TD_CS_BABBLE | TD_CS_NAK | TD_CS_TIMEOUTCRC | TD_CS_BITSTUFF)
#define TD_CS_FATAL_MASK (TD_CS_STALL | TD_CS_BUFERR | TD_CS_BABBLE | TD_CS_TIMEOUTCRC | TD_CS_BITSTUFF)

#define TD_F_IOC 0x01u
#define TD_F_SPD 0x02u

#define LP_TERMINATE 0x01u
#define LP_QH 0x02u
#define LP_DEPTH 0x04u

uint8_t uhci_dev_is_ls[MAX_UHCI_CONTROLLERS][128];
uint8_t uhci_dev_mps0[MAX_UHCI_CONTROLLERS][128];

typedef struct {
    volatile uint32_t link;
    volatile uint32_t element;
    uint32_t _reserved0;
    uint32_t _reserved1;
} __attribute__((aligned(16))) uhci_qh_hw_t;

#define UHCI_QH_SLOTS 48
#define UHCI_QH_TD_RING 24

enum {
    UHCI_XFER_BULK = 0,
    UHCI_XFER_INTERRUPT = 1,
};

typedef struct {
    uint8_t in_use;
    uint8_t linked;
    uint8_t dev_addr;
    uint8_t ep_num;
    uint8_t direction;
    uint8_t xfer_kind;
    uint8_t is_ls;
    uint16_t mps;

    uint8_t data_toggle;
    uint8_t active;
    uint8_t batch_count;
    uint8_t stalled;

    uint8_t result_ready;
    int8_t last_result;

    uint8_t *cursor;
    uint16_t remaining;
    uint16_t total_len;
    uint16_t xfer_ok_len;
    void *xfer_buf;
    uint8_t endpoint_byte;

    struct usb_device *device;
    void *cookie;
} uhci_qh_meta_t;

typedef struct {
    volatile uint32_t frames[1024];
} __attribute__((aligned(4096))) uhci_frame_list_t;

static struct {
    struct uhci_controller ctrl;
    uhci_frame_list_t fl;

    uhci_qh_hw_t async_anchor;
    uhci_qh_hw_t *async_tail;

    uhci_qh_hw_t qh_pool[UHCI_QH_SLOTS];
    uhci_qh_meta_t qh_meta[UHCI_QH_SLOTS];
    struct uhci_td td_ring[UHCI_QH_SLOTS][UHCI_QH_TD_RING] __attribute__((aligned(16)));

    struct uhci_td ctrl_ring[UHCI_QH_TD_RING] __attribute__((aligned(16)));
    uhci_qh_hw_t ctrl_qh;

    struct uhci_td iso_td_pool[UHCI_ISO_MAX_FRAMES];
} uhci_resources[MAX_UHCI_CONTROLLERS] __attribute__((aligned(4096)));

static int uhci_controller_count = 0;
static void (*delay_ms)(uint64_t) = NULL;

void uhci_delay_ms(uint64_t ms) {
    if (delay_ms) delay_ms(ms);
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    if (flags & (1u << 9))
        asm volatile("sti" ::: "memory");
}

static inline uint16_t uhci_readw(struct uhci_controller *u, uint16_t r) {
    return inw((uint16_t)(u->io_base + r));
}
static inline void uhci_writew(struct uhci_controller *u, uint16_t r, uint16_t v) {
    outw((uint16_t)(u->io_base + r), v);
}
static inline void uhci_writel(struct uhci_controller *u, uint16_t r, uint32_t v) {
    outl((uint16_t)(u->io_base + r), v);
}

static inline uint16_t uhci_portsc_reg(uint8_t port) {
    return (uint16_t)(UHCI_PORTSC1 + (port - 1) * 2);
}

static inline int uhci_res_index(struct uhci_controller *u) {
    for (int i = 0; i < MAX_UHCI_CONTROLLERS; i++) {
        if (&uhci_resources[i].ctrl == u) return i;
    }
    return -1;
}

int uhci_controller_index(struct uhci_controller *u) {
    return uhci_res_index(u);
}

uint8_t uhci_port_count(struct uhci_controller *u) {
    if (!u) return 0;
    return u->num_ports ? u->num_ports : 2;
}

static void uhci_flush_range(const void *buf, uint32_t len) {
    if (!buf || !len) return;
    uint64_t base = (uint64_t)buf & ~63ull;
    uint64_t end = (uint64_t)buf + len;
    for (uint64_t a = base; a < end; a += 64)
        asm volatile("clflush (%0)" :: "r"(a) : "memory");
    asm volatile("mfence" ::: "memory");
}

static int uhci_detect_ls(struct uhci_controller *u, uint8_t dev_addr) {
    int ri = uhci_res_index(u);
    if (ri < 0) return 0;
    if (dev_addr == 0) return uhci_dev_is_ls[ri][0] ? 1 : 0;
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && (uint8_t)_d->address == dev_addr &&
            _d->ctrl == (struct usb_controller *)u)
            return _d->is_low_speed ? 1 : 0;
    }
    return uhci_dev_is_ls[ri][dev_addr & 0x7F] ? 1 : 0;
}

static struct usb_device *uhci_lookup_device(struct uhci_controller *u, uint8_t dev_addr) {
    for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
        struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
        if (_d && _d->address == dev_addr && _d->ctrl == (struct usb_controller *)u)
            return _d;
    }
    return NULL;
}

static inline int uhci_mps0_valid(uint16_t m) {
    return m == 8 || m == 16 || m == 32 || m == 64;
}

static uint16_t uhci_control_mps(struct uhci_controller *u, uint8_t dev_addr, int is_ls) {
    if (dev_addr == 0 || is_ls) return 8;

    struct usb_device *d = uhci_lookup_device(u, dev_addr);
    if (d) {
        if (uhci_mps0_valid(d->desc.bMaxPacketSize0)) return d->desc.bMaxPacketSize0;
        if (uhci_mps0_valid(d->max_packet_size)) return d->max_packet_size;
    }

    int ri = uhci_res_index(u);
    if (ri >= 0 && uhci_mps0_valid(uhci_dev_mps0[ri][dev_addr & 0x7F]))
        return uhci_dev_mps0[ri][dev_addr & 0x7F];

    return 64;
}

static uint16_t uhci_endpoint_mps(struct usb_device *d, uint8_t endpoint,
                                  uint8_t xfer_kind, int is_ls) {
    uint16_t mps = is_ls ? 8u : (xfer_kind == UHCI_XFER_BULK ? 64u : 8u);

    if (d) {
        if (xfer_kind == UHCI_XFER_INTERRUPT) {
            if (d->endpoint_address == endpoint && d->max_packet_size)
                mps = d->max_packet_size;
            else if (d->hub_status_ep == endpoint && d->hub_status_ep_mps)
                mps = d->hub_status_ep_mps;
        } else {
            for (int i = 0; i < d->bulk_ep_count; i++) {
                if (d->bulk_ep[i].address == endpoint && d->bulk_ep[i].max_packet_size) {
                    mps = d->bulk_ep[i].max_packet_size;
                    break;
                }
            }
        }
    }

    if (mps == 0) mps = 8;
    if (is_ls && mps > 8) mps = 8;
    if (mps > 64) mps = 64;
    return mps;
}

static inline void uhci_td_build(struct uhci_td *td, uint32_t link, uint8_t pid,
                                 uint8_t dev_addr, uint8_t ep_num, uint8_t toggle,
                                 int is_ls, uint32_t flags, uint16_t len, void *buf)
{
    uint32_t maxlen_field = (len == 0) ? 0x7FFu : (uint32_t)(len - 1);
    uint32_t cs = TD_CS_CERR3 | TD_CS_ACTIVE;

    if (is_ls) cs |= TD_CS_LS;
    if (flags & TD_F_IOC) cs |= TD_CS_IOC;
    if (flags & TD_F_SPD) cs |= TD_CS_SPD;

    td->link_ptr = link;
    td->buffer_ptr = buf ? (uint32_t)mm_ptr_to_phys(buf) : 0;
    td->token = (maxlen_field << 21) | ((uint32_t)ep_num << 15) |
    ((uint32_t)toggle << 19) | ((uint32_t)dev_addr << 8) | pid;
    asm volatile("mfence" ::: "memory");
    td->control_status = cs;
}

static inline uint16_t uhci_td_requested_len(const struct uhci_td *td) {
    uint32_t f = (td->token >> 21) & 0x7FFu;
    return (f == 0x7FFu) ? 0 : (uint16_t)(f + 1);
}
static inline uint16_t uhci_td_actual_len(uint32_t control_status) {
    uint32_t f = control_status & TD_CS_ACTLEN_MASK;
    return (f == 0x7FFu) ? 0 : (uint16_t)(f + 1);
}

static void uhci_qh_link_into_async(int res_idx, uhci_qh_hw_t *hw) {
    hw->link = LP_TERMINATE;
    hw->element = LP_TERMINATE;
    asm volatile("mfence" ::: "memory");

    uhci_qh_hw_t *tail = uhci_resources[res_idx].async_tail;
    uint32_t newphys = (uint32_t)mm_ptr_to_phys(hw) | LP_QH;
    tail->link = newphys;
    asm volatile("mfence" ::: "memory");
    uhci_resources[res_idx].async_tail = hw;
}

static int uhci_qh_find_or_alloc(int res_idx, uint8_t dev_addr, uint8_t ep_num,
                                 uint8_t direction, uint8_t xfer_kind)
{
    uhci_qh_meta_t *meta = uhci_resources[res_idx].qh_meta;

    for (int i = 0; i < UHCI_QH_SLOTS; i++) {
        if (meta[i].in_use && meta[i].dev_addr == dev_addr &&
            meta[i].ep_num == ep_num && meta[i].direction == direction &&
            meta[i].xfer_kind == xfer_kind)
            return i;
    }

    for (int i = 0; i < UHCI_QH_SLOTS; i++) {
        if (meta[i].in_use) continue;

        meta[i].in_use = 1;
        meta[i].dev_addr = dev_addr;
        meta[i].ep_num = ep_num;
        meta[i].direction = direction;
        meta[i].xfer_kind = xfer_kind;
        meta[i].data_toggle = 0;
        meta[i].active = 0;
        meta[i].result_ready = 0;
        meta[i].batch_count = 0;
        meta[i].stalled = 0;
        meta[i].device = NULL;
        meta[i].cookie = NULL;

        uhci_resources[res_idx].qh_pool[i].element = LP_TERMINATE;
        asm volatile("mfence" ::: "memory");

        dbg_str("[uhci] find_or_alloc NEW slot qi="); dbg_dec(i);
        dbg_str(" dev="); dbg_dec(dev_addr);
        dbg_str(" ep="); dbg_dec(ep_num);
        dbg_str(" dir="); dbg_dec(direction);
        dbg_str(" kind="); dbg_dec(xfer_kind);
        dbg_str("\r\n");

        if (!meta[i].linked) {
            uhci_qh_link_into_async(res_idx, &uhci_resources[res_idx].qh_pool[i]);
            meta[i].linked = 1;
        }
        return i;
    }

    return -1;
}

static void uhci_qh_halt(int res_idx, int qi) {
    uhci_resources[res_idx].qh_pool[qi].element = LP_TERMINATE;
    asm volatile("mfence" ::: "memory");
}

static void uhci_qh_submit_batch(int res_idx, int qi) {
    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];
    struct uhci_td *ring = uhci_resources[res_idx].td_ring[qi];
    uhci_qh_hw_t *hw = &uhci_resources[res_idx].qh_pool[qi];

    uint8_t pid = m->direction ? TD_PID_IN : TD_PID_OUT;
    uint8_t n = 0;
    uint16_t rem = m->remaining;
    uint8_t *cur = m->cursor;

    while (rem > 0 && n < UHCI_QH_TD_RING) {
        uint16_t chunk = (rem > m->mps) ? m->mps : rem;
        uhci_td_build(&ring[n], LP_TERMINATE, pid, m->dev_addr, m->ep_num,
                      m->data_toggle, m->is_ls, m->direction ? TD_F_SPD : 0,
                      chunk, cur);
        m->data_toggle ^= 1;
        cur += chunk;
        rem -= chunk;
        n++;
    }

    for (uint8_t i = 0; i < n; i++) {
        ring[i].link_ptr = (i + 1 < n)
        ? ((uint32_t)mm_ptr_to_phys(&ring[i + 1]) | LP_DEPTH)
        : LP_TERMINATE;
    }
    if (n) ring[n - 1].control_status |= TD_CS_IOC;
    asm volatile("mfence" ::: "memory");

    m->batch_count = n;
    m->cursor = cur;
    m->remaining = rem;

    hw->element = (n == 0) ? LP_TERMINATE : (uint32_t)mm_ptr_to_phys(&ring[0]);
    asm volatile("mfence" ::: "memory");
}

static void uhci_qh_arm_interrupt(int res_idx, int qi) {
    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];
    struct uhci_td *td = &uhci_resources[res_idx].td_ring[qi][0];
    uhci_qh_hw_t *hw = &uhci_resources[res_idx].qh_pool[qi];

    uint16_t chunk = (m->remaining > m->mps) ? m->mps : m->remaining;
    uint8_t pid = m->direction ? TD_PID_IN : TD_PID_OUT;

    uhci_td_build(td, LP_TERMINATE, pid, m->dev_addr, m->ep_num,
                  m->data_toggle, m->is_ls,
                  TD_F_IOC | (m->direction ? TD_F_SPD : 0), chunk, m->cursor);
    asm volatile("mfence" ::: "memory");

    m->data_toggle ^= 1;
    m->cursor += chunk;
    m->remaining -= chunk;
    m->batch_count = 1;

    hw->element = (uint32_t)mm_ptr_to_phys(td);
    asm volatile("mfence" ::: "memory");
}

static int uhci_qh_poll_batch(int res_idx, int qi, uint16_t *added) {
    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];
    struct uhci_td *ring = uhci_resources[res_idx].td_ring[qi];
    uint16_t total = 0;

    if (m->batch_count == 0) { *added = 0; return 1; }

    for (uint8_t i = 0; i < m->batch_count; i++) {
        uint32_t cs = ring[i].control_status;

        if (cs & TD_CS_ACTIVE) {
            *added = total;
            return 0;
        }
        if (cs & TD_CS_FATAL_MASK) {
            m->stalled = !!(cs & TD_CS_STALL);
            uhci_qh_halt(res_idx, qi);
            *added = total;
            return -1;
        }
        if (cs & TD_CS_NAK) {
            uhci_qh_halt(res_idx, qi);
            *added = total;
            return -1;
        }

        uint16_t requested = uhci_td_requested_len(&ring[i]);
        uint16_t actual = uhci_td_actual_len(cs);
        total += actual;

        if (actual < requested) {
            m->remaining = 0;
            uhci_qh_halt(res_idx, qi);
            *added = total;
            return 1;
        }
    }

    *added = total;
    return 1;
}

static void uhci_qh_report(int res_idx, int qi, int ok) {
    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];

    if (ok && m->direction)
        uhci_flush_range(m->xfer_buf, m->xfer_ok_len);

    if (!ok && m->stalled)
        m->data_toggle = 0;

    usb_event_t evt = {
        .type = ok ? (m->xfer_kind == UHCI_XFER_BULK ? USB_EVENT_BULK_DONE : USB_EVENT_TRANSFER_DONE)
        : (m->xfer_kind == UHCI_XFER_BULK ? USB_EVENT_BULK_ERR : USB_EVENT_TRANSFER_ERR),
        .src = USB_SRC_UHCI,
        .transfer_type = (m->xfer_kind == UHCI_XFER_BULK) ? USB_XFER_BULK : USB_XFER_INTERRUPT,
        .slot_id = (uint8_t)res_idx,
        .endpoint = m->endpoint_byte,
        .completion_code = ok ? 0 : 0xFF,
        .data = ok ? m->xfer_buf : NULL,
        .data_len = ok ? m->xfer_ok_len : 0,
        .device = m->device,
        .cookie = m->cookie,
    };
    if (m->xfer_kind == UHCI_XFER_BULK) usb_push_bulk_event(&evt);
    else usb_push_event(&evt);

    m->active = 0;
    m->result_ready = 1;
    m->last_result = ok ? 0 : -1;
}

static void uhci_qh_rearm_periodic(int res_idx, int qi) {
    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];
    m->cursor = m->xfer_buf;
    m->remaining = m->total_len;
    if (m->remaining > m->mps) m->remaining = m->mps;
    m->xfer_ok_len = 0;
    m->stalled = 0;
    m->active = 1;
    uhci_qh_arm_interrupt(res_idx, qi);
    dbg_str("[uhci] rearm_periodic res="); dbg_dec(res_idx);
    dbg_str(" qi="); dbg_dec(qi);
    dbg_str(" toggle="); dbg_dec(m->data_toggle);
    dbg_str(" active="); dbg_dec(m->active);
    dbg_str("\r\n");
}

static void uhci_qh_service(int res_idx, int qi) {
    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];
    if (!m->active) return;

    uint16_t added = 0;
    int r = uhci_qh_poll_batch(res_idx, qi, &added);
    if (r == 0) return;

    m->xfer_ok_len += added;

    dbg_str("[uhci] service res="); dbg_dec(res_idx);
    dbg_str(" qi="); dbg_dec(qi);
    dbg_str(" r="); dbg_dec(r);
    dbg_str(" added="); dbg_dec(added);
    dbg_str(" remaining="); dbg_dec(m->remaining);
    dbg_str(" stalled="); dbg_dec(m->stalled);
    dbg_str(" xfer_kind="); dbg_dec(m->xfer_kind);
    dbg_str("\r\n");

    if (r < 0) {
        uhci_qh_report(res_idx, qi, 0);
        if (m->xfer_kind == UHCI_XFER_INTERRUPT && !m->stalled)
            uhci_qh_rearm_periodic(res_idx, qi);
        else
            dbg_str("[uhci] NOT rearmed after error (stalled or not interrupt)\r\n");
        return;
    }

    if (m->remaining > 0) {
        if (m->xfer_kind == UHCI_XFER_INTERRUPT) uhci_qh_arm_interrupt(res_idx, qi);
        else uhci_qh_submit_batch(res_idx, qi);
        return;
    }

    uhci_qh_report(res_idx, qi, 1);
    if (m->xfer_kind == UHCI_XFER_INTERRUPT)
        uhci_qh_rearm_periodic(res_idx, qi);
    else
        dbg_str("[uhci] NOT rearmed after success (not interrupt xfer_kind)\r\n");
}

static int uhci_queued_transfer(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                                void *data, uint16_t data_len, uint8_t direction, uint8_t xfer_kind)
{
    if (!u || !u->initialized) return -1;
    if (!data || !data_len) return -1;
    int res_idx = uhci_res_index(u);
    if (res_idx < 0) return -1;

    uint8_t ep_num = endpoint & 0x0Fu;
    int qi = uhci_qh_find_or_alloc(res_idx, dev_addr, ep_num, direction, xfer_kind);
    if (qi < 0) return -1;

    uhci_qh_meta_t *m = &uhci_resources[res_idx].qh_meta[qi];

    uint64_t fl = irq_save();

    if (m->active)
        uhci_qh_service(res_idx, qi);

    if (m->result_ready) {
        m->result_ready = 0;
        int last_result = m->last_result;
        irq_restore(fl);
        return last_result;
    }

    if (!m->active) {
        struct usb_device *dev = uhci_lookup_device(u, dev_addr);
        int is_ls = uhci_detect_ls(u, dev_addr);

        if (direction) {
            for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
            uhci_flush_range(data, data_len);
        }

        m->is_ls = (uint8_t)is_ls;
        m->mps = uhci_endpoint_mps(dev, endpoint, xfer_kind, is_ls);

        m->cursor = (uint8_t *)data;
        m->remaining = data_len;
        if (xfer_kind == UHCI_XFER_INTERRUPT && m->remaining > m->mps)
            m->remaining = m->mps;

        m->total_len = data_len;
        m->xfer_ok_len = 0;
        m->xfer_buf = data;
        m->endpoint_byte = endpoint;
        m->stalled = 0;
        m->device = dev;
        m->cookie = dev;
        m->active = 1;

        if (xfer_kind == UHCI_XFER_INTERRUPT) uhci_qh_arm_interrupt(res_idx, qi);
        else uhci_qh_submit_batch(res_idx, qi);
    }

    irq_restore(fl);
    return -2;
}

static int uhci_bulk_transfer(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                              void *data, uint16_t data_len, uint8_t direction)
{
    if (!u) return -1;
    spin_lock(&u->lock);
    int ret = uhci_queued_transfer(u, dev_addr, endpoint, data, data_len, direction, UHCI_XFER_BULK);
    spin_unlock(&u->lock);
    return ret;
}

static int uhci_interrupt_transfer(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                                   void *data, uint16_t data_len, uint8_t direction)
{
    if (!u) return -1;
    spin_lock(&u->lock);
    int ret = uhci_queued_transfer(u, dev_addr, endpoint, data, data_len, direction, UHCI_XFER_INTERRUPT);
    spin_unlock(&u->lock);
    return ret;
}

#define UHCI_RUN_COMPLETE 0
#define UHCI_RUN_SHORT 1
#define UHCI_RUN_ERROR (-1)
#define UHCI_RUN_TIMEOUT (-2)

static int uhci_ctrl_run(int res_idx, struct uhci_td *ring, uint8_t nt, int *stop_idx) {
    uhci_qh_hw_t *hw = &uhci_resources[res_idx].ctrl_qh;

    *stop_idx = -1;
    if (nt == 0) return UHCI_RUN_COMPLETE;

    for (uint8_t i = 0; i + 1 < nt; i++)
        ring[i].link_ptr = (uint32_t)mm_ptr_to_phys(&ring[i + 1]) | LP_DEPTH;
    ring[nt - 1].link_ptr = LP_TERMINATE;
    ring[nt - 1].control_status |= TD_CS_IOC;
    asm volatile("mfence" ::: "memory");

    hw->element = (uint32_t)mm_ptr_to_phys(&ring[0]);
    asm volatile("mfence" ::: "memory");

    int result = UHCI_RUN_TIMEOUT;
    for (int ms = 0; ms < UHCI_CTRL_TIMEOUT_MS; ms++) {
        int pending = 0;

        for (uint8_t i = 0; i < nt; i++) {
            uint32_t cs = ring[i].control_status;

            if (cs & TD_CS_ACTIVE) { pending = 1; break; }
            if (cs & TD_CS_ERROR_MASK) { *stop_idx = i; result = UHCI_RUN_ERROR; break; }

            if (uhci_td_actual_len(cs) < uhci_td_requested_len(&ring[i])) {
                *stop_idx = i;
                result = UHCI_RUN_SHORT;
                break;
            }
            if (i + 1 == nt) result = UHCI_RUN_COMPLETE;
        }

        if (result != UHCI_RUN_TIMEOUT) break;
        if (!pending) { result = UHCI_RUN_COMPLETE; break; }
        delay_ms(1);
    }

    hw->element = LP_TERMINATE;
    asm volatile("mfence" ::: "memory");
    return result;
}

static void uhci_ctrl_log_error(uint8_t dev_addr, uint8_t ep_num, int run,
                                const struct uhci_td *td)
{
    usb_logrow_begin(USB_LOG_UHCI, USB_LOG_ERROR);
    usb_logrow_str("control xfer addr="); usb_logrow_dec(dev_addr);
    usb_logrow_str(" ep="); usb_logrow_dec(ep_num);
    usb_logrow_str(run == UHCI_RUN_TIMEOUT ? ": timed out, status=" : ": TD error, status=");
    usb_logrow_hex32(td ? td->control_status : 0);
    if (td) {
        usb_logrow_str(" token=");
        usb_logrow_hex32(td->token);
    }
    usb_logrow_end();
}

static int uhci_control_transfer_impl(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                                 void *setup_packet, uint16_t setup_len, void *data,
                                 uint16_t data_len, uint8_t direction)
{
    (void)setup_len;
    if (!u || !u->initialized || !setup_packet) return -1;
    int res_idx = uhci_res_index(u);
    if (res_idx < 0) return -1;
    if (data_len && !data) return -1;

    struct uhci_td *ring = uhci_resources[res_idx].ctrl_ring;
    uint8_t ep_num = endpoint & 0x0Fu;

    int is_ls = uhci_detect_ls(u, dev_addr);
    uint16_t mps = uhci_control_mps(u, dev_addr, is_ls);
    uint16_t max_data_tds = UHCI_QH_TD_RING - 2;

    if (direction && data_len) {
        for (uint16_t i = 0; i < data_len; i++) ((uint8_t *)data)[i] = 0;
        uhci_flush_range(data, data_len);
    } else if (!direction && data_len) {
        uhci_flush_range(data, data_len);
    }
    uhci_flush_range(setup_packet, 8);

    uint16_t queued = 0;
    uint16_t actual = 0;
    uint8_t toggle = 1;
    int first_stage = 1;
    int short_seen = 0;
    int failed = 0;

    while (!failed && !short_seen && queued < data_len) {
        uint8_t nt = 0;
        uint8_t data_first = 0;

        if (first_stage) {
            uhci_td_build(&ring[nt], LP_TERMINATE, TD_PID_SETUP, dev_addr, ep_num,
                          0, is_ls, 0, 8, setup_packet);
            nt++;
        }
        data_first = nt;

        uint16_t stage_bytes = 0;
        while (queued + stage_bytes < data_len && (nt - data_first) < max_data_tds) {
            uint16_t left = (uint16_t)(data_len - queued - stage_bytes);
            uint16_t chunk = (left > mps) ? mps : left;
            uhci_td_build(&ring[nt], LP_TERMINATE, direction ? TD_PID_IN : TD_PID_OUT,
                          dev_addr, ep_num, toggle, is_ls,
                          direction ? TD_F_SPD : 0, chunk,
                          (uint8_t *)data + queued + stage_bytes);
            toggle ^= 1;
            stage_bytes = (uint16_t)(stage_bytes + chunk);
            nt++;
        }

        int stop_idx = -1;
        int run = uhci_ctrl_run(res_idx, ring, nt, &stop_idx);

        for (uint8_t i = data_first; i < nt; i++) {
            uint32_t cs = ring[i].control_status;
            if (cs & (TD_CS_ACTIVE | TD_CS_ERROR_MASK)) break;
            actual = (uint16_t)(actual + uhci_td_actual_len(cs));
            if (uhci_td_actual_len(cs) < uhci_td_requested_len(&ring[i])) break;
        }

        if (run == UHCI_RUN_ERROR || run == UHCI_RUN_TIMEOUT) {
            uhci_ctrl_log_error(dev_addr, ep_num, run,
                                (stop_idx >= 0) ? &ring[stop_idx] : &ring[nt - 1]);
            failed = 1;
            break;
        }
        if (run == UHCI_RUN_SHORT) short_seen = 1;

        queued = (uint16_t)(queued + stage_bytes);
        first_stage = 0;
    }

    if (!failed) {
        uint8_t nt = 0;

        if (first_stage) {
            uhci_td_build(&ring[nt], LP_TERMINATE, TD_PID_SETUP, dev_addr, ep_num,
                          0, is_ls, 0, 8, setup_packet);
            nt++;
        }

        uint8_t status_pid = (data_len > 0 && direction) ? TD_PID_OUT : TD_PID_IN;
        uhci_td_build(&ring[nt], LP_TERMINATE, status_pid, dev_addr, ep_num,
                      1, is_ls, 0, 0, NULL);
        nt++;

        int stop_idx = -1;
        int run = uhci_ctrl_run(res_idx, ring, nt, &stop_idx);
        if (run == UHCI_RUN_ERROR || run == UHCI_RUN_TIMEOUT) {
            uhci_ctrl_log_error(dev_addr, ep_num, run,
                                (stop_idx >= 0) ? &ring[stop_idx] : &ring[nt - 1]);
            failed = 1;
        }
    }

    if (!failed && direction && actual)
        uhci_flush_range(data, actual);

    return failed ? -1 : 0;
}

static int uhci_control_transfer(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                                 void *setup_packet, uint16_t setup_len, void *data,
                                 uint16_t data_len, uint8_t direction)
{
    if (!u) return -1;
    spin_lock(&u->lock);
    int ret = uhci_control_transfer_impl(u, dev_addr, endpoint, setup_packet, setup_len, data, data_len, direction);
    spin_unlock(&u->lock);
    return ret;
}

typedef struct {
    uint8_t active;
    uint8_t n_frames;
    uint8_t frames_done;
    uint8_t endpoint;
    uint8_t direction;
    void *data;
    uint16_t total_len;
    uint16_t frame_offsets[UHCI_ISO_MAX_FRAMES];
    uint16_t frame_lens [UHCI_ISO_MAX_FRAMES];
    uint16_t frame_slots [UHCI_ISO_MAX_FRAMES];
    uint8_t td_indices [UHCI_ISO_MAX_FRAMES];
    uint8_t frame_reaped[UHCI_ISO_MAX_FRAMES];
    uint32_t saved_frame [UHCI_ISO_MAX_FRAMES];
    void *cookie;
} uhci_iso_pending;

static struct uhci_iso_pending_holder { uhci_iso_pending v; } s_uhci_iso_holder[MAX_UHCI_CONTROLLERS];
#define s_uhci_iso(idx) (s_uhci_iso_holder[(idx)].v)

static void uhci_iso_pool_init(int res_idx) {
    for (int i = 0; i < UHCI_ISO_MAX_FRAMES; i++) {
        uhci_resources[res_idx].iso_td_pool[i].control_status = 0;
        uhci_resources[res_idx].iso_td_pool[i].token = 0;
        uhci_resources[res_idx].iso_td_pool[i].buffer_ptr = 0;
        uhci_resources[res_idx].iso_td_pool[i].link_ptr = LP_TERMINATE;
    }
    s_uhci_iso(res_idx).active = 0;
    asm volatile("mfence" ::: "memory");
}

static int uhci_iso_transfer_impl(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                             void *data, uint16_t total_len, uint8_t n_frames,
                             const uint16_t *frame_lens, uint8_t direction)
{
    if (!u || !u->initialized || !data) return -1;
    if (!n_frames || n_frames > UHCI_ISO_MAX_FRAMES) return -1;

    int res_idx = uhci_res_index(u);
    if (res_idx < 0) return -1;
    if (s_uhci_iso(res_idx).active) return -2;

    uint8_t ep_num = endpoint & 0x0Fu;
    uint8_t pid = direction ? TD_PID_IN : TD_PID_OUT;

    uint16_t cur = uhci_readw(u, UHCI_FRNUM) & 0x3FFu;
    uint16_t start = (uint16_t)((cur + 2u) & 0x3FFu);

    uint16_t per_frame = (uint16_t)(total_len / n_frames);
    uint16_t offset = 0;

    for (uint8_t i = 0; i < n_frames; i++) {
        uint16_t flen = frame_lens ? frame_lens[i] : per_frame;
        if (offset + flen > total_len) flen = (uint16_t)(total_len - offset);
        s_uhci_iso(res_idx).frame_offsets[i] = offset;
        s_uhci_iso(res_idx).frame_lens[i] = flen;
        s_uhci_iso(res_idx).frame_reaped[i] = 0;
        offset = (uint16_t)(offset + flen);
    }

    int td_free_list[UHCI_ISO_MAX_FRAMES];
    int found = 0;
    for (int ti = 0; ti < UHCI_ISO_MAX_FRAMES && found < (int)n_frames; ti++) {
        if (!(uhci_resources[res_idx].iso_td_pool[ti].control_status & TD_CS_ACTIVE))
            td_free_list[found++] = ti;
    }
    if (found < (int)n_frames) return -1;

    for (uint8_t i = 0; i < n_frames; i++) {
        struct uhci_td *td = &uhci_resources[res_idx].iso_td_pool[td_free_list[i]];
        uint8_t *frame_buf = (uint8_t *)data + s_uhci_iso(res_idx).frame_offsets[i];
        uint16_t flen = s_uhci_iso(res_idx).frame_lens[i];
        uint16_t fslot = (uint16_t)((start + i) & 0x3FFu);

        uint32_t continue_link = (uint32_t)mm_ptr_to_phys(&uhci_resources[res_idx].async_anchor) | LP_QH;

        td->token = (uint32_t)pid
        | ((uint32_t)(dev_addr & 0x7Fu) << 8)
        | ((uint32_t)(ep_num & 0xFu) << 15)
        | ((uint32_t)((flen > 0 ? (uint32_t)(flen - 1) : 0x7FFu) & 0x7FFu) << 21);
        td->buffer_ptr = (uint32_t)mm_ptr_to_phys(frame_buf);
        td->link_ptr = continue_link;
        asm volatile("mfence" ::: "memory");
        td->control_status = TD_CS_ACTIVE | TD_CS_IOS | TD_CS_IOC;

        s_uhci_iso(res_idx).frame_slots[i] = fslot;
        s_uhci_iso(res_idx).td_indices[i] = (uint8_t)td_free_list[i];
        s_uhci_iso(res_idx).saved_frame[i] = u->frame_list[fslot];
        u->frame_list[fslot] = (uint32_t)mm_ptr_to_phys(td);
    }
    asm volatile("mfence" ::: "memory");

    s_uhci_iso(res_idx).active = 1;
    s_uhci_iso(res_idx).n_frames = n_frames;
    s_uhci_iso(res_idx).frames_done = 0;
    s_uhci_iso(res_idx).endpoint = endpoint;
    s_uhci_iso(res_idx).direction = direction;
    s_uhci_iso(res_idx).data = data;
    s_uhci_iso(res_idx).total_len = total_len;
    s_uhci_iso(res_idx).cookie = NULL;

    return 0;
}

static int uhci_iso_transfer(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint,
                             void *data, uint16_t total_len, uint8_t n_frames,
                             const uint16_t *frame_lens, uint8_t direction)
{
    if (!u) return -1;
    spin_lock(&u->lock);
    int ret = uhci_iso_transfer_impl(u, dev_addr, endpoint, data, total_len, n_frames, frame_lens, direction);
    spin_unlock(&u->lock);
    return ret;
}

void uhci_poll_iso(struct uhci_controller *u) {
    if (!u || !u->initialized) return;
    int res_idx = uhci_res_index(u);
    if (res_idx < 0 || !s_uhci_iso(res_idx).active) return;

    for (uint8_t fi = 0; fi < s_uhci_iso(res_idx).n_frames; fi++) {
        if (s_uhci_iso(res_idx).frame_reaped[fi]) continue;

        uint8_t td_idx = s_uhci_iso(res_idx).td_indices[fi];
        uint32_t cs = uhci_resources[res_idx].iso_td_pool[td_idx].control_status;

        if (cs & TD_CS_ACTIVE) continue;

        uint16_t fslot = s_uhci_iso(res_idx).frame_slots[fi];
        u->frame_list[fslot] = s_uhci_iso(res_idx).saved_frame[fi];
        uhci_resources[res_idx].iso_td_pool[td_idx].link_ptr = LP_TERMINATE;
        s_uhci_iso(res_idx).frame_reaped[fi] = 1;

        int ok = !(cs & TD_CS_ERROR_MASK);
        uint16_t got = ok ? uhci_td_actual_len(cs) : 0;
        if (got > s_uhci_iso(res_idx).frame_lens[fi])
            got = s_uhci_iso(res_idx).frame_lens[fi];

        if (ok && s_uhci_iso(res_idx).direction)
            uhci_flush_range((uint8_t *)s_uhci_iso(res_idx).data +
                             s_uhci_iso(res_idx).frame_offsets[fi], got);

        usb_event_t evt = {
            .type = ok ? USB_EVENT_ISO_DONE : USB_EVENT_ISO_ERR,
            .src = USB_SRC_UHCI,
            .transfer_type = USB_XFER_ISO,
            .slot_id = (uint8_t)res_idx,
            .endpoint = s_uhci_iso(res_idx).endpoint,
            .completion_code = ok ? 0 : 0xFF,
            .data = ok ? ((uint8_t *)s_uhci_iso(res_idx).data + s_uhci_iso(res_idx).frame_offsets[fi]) : NULL,
            .data_len = got,
            .iso_frame_index = fi,
            .iso_expected_len = s_uhci_iso(res_idx).frame_lens[fi],
            .cookie = s_uhci_iso(res_idx).cookie,
        };
        usb_push_iso_event(&evt);
        s_uhci_iso(res_idx).frames_done++;
    }

    if (s_uhci_iso(res_idx).frames_done >= s_uhci_iso(res_idx).n_frames) {
        for (uint8_t fi = 0; fi < s_uhci_iso(res_idx).n_frames; fi++) {
            uint8_t ti = s_uhci_iso(res_idx).td_indices[fi];
            uhci_resources[res_idx].iso_td_pool[ti].control_status = 0;
            uhci_resources[res_idx].iso_td_pool[ti].token = 0;
            uhci_resources[res_idx].iso_td_pool[ti].buffer_ptr = 0;
            uhci_resources[res_idx].iso_td_pool[ti].link_ptr = LP_TERMINATE;
        }
        asm volatile("mfence" ::: "memory");
        s_uhci_iso(res_idx).active = 0;
    }
}

static void uhci_hc_start(struct uhci_controller *u) {
    int res_idx = uhci_res_index(u);
    if (res_idx < 0) return;

    uhci_writel(u, UHCI_FRBASEADD,
                (uint32_t)mm_ptr_to_phys((void *)uhci_resources[res_idx].fl.frames));
    uhci_writew(u, UHCI_FRNUM, 0);
    uhci_writew(u, UHCI_SOFMOD, 0x40);
    uhci_writew(u, UHCI_STS, UHCI_STS_MASK);
    uhci_writew(u, UHCI_INTR, UHCI_INTR_TIMEOUT | UHCI_INTR_RESUME |
                              UHCI_INTR_IOC | UHCI_INTR_SP);
    uhci_writew(u, UHCI_CMD, UHCI_CMD_RUN | UHCI_CMD_CF | UHCI_CMD_MAXP);
}

void uhci_irq(void) {
    for (int i = 0; i < uhci_controller_count; i++) {
        struct uhci_controller *u = &uhci_resources[i].ctrl;
        if (!u->initialized) continue;

        uint16_t status = uhci_readw(u, UHCI_STS);
        if (status == 0xFFFF) continue;
        status &= UHCI_STS_MASK;
        if (!status) continue;

        uhci_writew(u, UHCI_STS, status);

        if (status & (UHCI_STS_HSE | UHCI_STS_HCPE)) {
            usb_logrow_begin(USB_LOG_UHCI, USB_LOG_ERROR);
            usb_logrow_str("controller "); usb_logrow_dec(i);
            usb_logrow_str(" fatal, status=");
            usb_logrow_hex32(status);
            usb_logrow_end();
            uhci_hc_start(u);
            continue;
        }

        if (status & UHCI_STS_HCH) {
            usb_logrow_begin(USB_LOG_UHCI, USB_LOG_WARN);
            usb_logrow_str("controller "); usb_logrow_dec(i);
            usb_logrow_str(" halted, restarting");
            usb_logrow_end();
            uhci_hc_start(u);
        }

        if (!(status & (UHCI_STS_USBINT | UHCI_STS_USBERR))) continue;

        dbg_str("[uhci] IRQ fired ctrl="); dbg_dec(i);
        dbg_str(" status="); dbg_hex32(status);
        dbg_str("\r\n");

        for (int qi = 0; qi < UHCI_QH_SLOTS; qi++) {
            if (!uhci_resources[i].qh_meta[qi].in_use) continue;
            if (!uhci_resources[i].qh_meta[qi].active) continue;
            dbg_str("[uhci] IRQ servicing qi="); dbg_dec(qi); dbg_str("\r\n");
            uhci_qh_service(i, qi);
        }

        uhci_poll_iso(u);
    }
}

static void uhci_irq_handler(struct registers *regs) { (void)regs; uhci_irq(); }

static uint8_t uhci_probe_ports(struct uhci_controller *u) {
    uint8_t n = 0;
    for (uint8_t p = 1; p <= UHCI_MAX_PORTS; p++) {
        uint16_t v = uhci_readw(u, uhci_portsc_reg(p));
        if (v == 0xFFFF || !(v & PORTSC_ALWAYS1)) break;
        n++;
    }
    if (n < 2) n = 2;
    return n;
}

static void uhci_hc_reset(struct uhci_controller *u) {
    uhci_writew(u, UHCI_INTR, 0);
    uhci_writew(u, UHCI_CMD, UHCI_CMD_GRESET);
    delay_ms(50);
    uhci_writew(u, UHCI_CMD, 0);
    delay_ms(10);

    uhci_writew(u, UHCI_CMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(uhci_readw(u, UHCI_CMD) & UHCI_CMD_HCRESET)) break;
        delay_ms(1);
    }
    uhci_writew(u, UHCI_CMD, 0);
    uhci_writew(u, UHCI_STS, UHCI_STS_MASK);
}

static int uhci_init_controller(struct uhci_controller *uhci, int res_index, struct pci_device *pdev) {
    if (!uhci || uhci->initialized) return -1;
    if (!uhci->io_base) return -1;

    if (pdev)
        pci_write_config(pdev->bus, pdev->slot, pdev->func, UHCI_LEGSUP, 0x8F00);

    uhci_hc_reset(uhci);

    uhci->num_ports = uhci_probe_ports(uhci);

    volatile uint32_t *fl_ptr = uhci_resources[res_index].fl.frames;
    uhci->frame_list = (uint32_t *)fl_ptr;

    uhci_qh_hw_t *anchor = &uhci_resources[res_index].async_anchor;
    anchor->link = LP_TERMINATE;
    anchor->element = LP_TERMINATE;
    uhci_resources[res_index].async_tail = anchor;

    for (int qi = 0; qi < UHCI_QH_SLOTS; qi++) {
        uhci_resources[res_index].qh_meta[qi].in_use = 0;
        uhci_resources[res_index].qh_meta[qi].linked = 0;
        uhci_resources[res_index].qh_meta[qi].active = 0;
        uhci_resources[res_index].qh_meta[qi].result_ready = 0;
        uhci_resources[res_index].qh_pool[qi].link = LP_TERMINATE;
        uhci_resources[res_index].qh_pool[qi].element = LP_TERMINATE;
    }

    uhci_qh_hw_t *ctrl_qh = &uhci_resources[res_index].ctrl_qh;
    uhci_qh_link_into_async(res_index, ctrl_qh);

    uhci_iso_pool_init(res_index);

    uint32_t anchor_phys = (uint32_t)mm_ptr_to_phys(anchor) | LP_QH;
    for (int i = 0; i < 1024; i++)
        uhci->frame_list[i] = anchor_phys;
    asm volatile("mfence" ::: "memory");

    uhci_hc_start(uhci);

    uhci->initialized = 1;
    usb_logrow_begin(USB_LOG_UHCI, USB_LOG_INFO);
    usb_logrow_str("controller "); usb_logrow_dec(res_index);
    usb_logrow_str(" ready, io_base=0x");
    usb_logrow_hex32(uhci->io_base);
    usb_logrow_str(" ports="); usb_logrow_dec(uhci->num_ports);
    usb_logrow_end();
    return 0;
}

static int uhci_reset_port(struct uhci_controller *uhci, uint8_t port) {
    if (!uhci || port < 1 || port > uhci_port_count(uhci)) return -1;

    uint16_t reg = uhci_portsc_reg(port);
    uint16_t status = uhci_readw(uhci, reg);
    if (status == 0xFFFF || !(status & PORTSC_CCS)) return -1;

    int ri = uhci_res_index(uhci);
    if (ri >= 0) uhci_dev_is_ls[ri][0] = (status & PORTSC_LSDA) ? 1 : 0;

    uhci_writew(uhci, reg, PORTSC_RWC);
    delay_ms(1);

    uhci_writew(uhci, reg, PORTSC_PR);
    delay_ms((status & PORTSC_LSDA) ? 60 : 50);

    uhci_writew(uhci, reg, 0);
    delay_ms(10);

    for (int i = 0; i < 20; i++) {
        uint16_t s = uhci_readw(uhci, reg);
        if (!(s & PORTSC_CCS)) return -1;

        uhci_writew(uhci, reg, (uint16_t)((s & ~(PORTSC_PR | PORTSC_RWC)) | PORTSC_PES | PORTSC_RWC));
        delay_ms(5);
        if (uhci_readw(uhci, reg) & PORTSC_PES) {
            delay_ms(20);
            return 0;
        }
    }
    return -1;
}

static void uhci_scan_pci(void) {
    struct usb_controller *ctrls = pci_get_usb_controllers();
    for (int i = 0; i < 8; i++) {
        if (ctrls[i].type != USB_TYPE_UHCI || !ctrls[i].pci) continue;
        if (uhci_controller_count >= MAX_UHCI_CONTROLLERS) break;

        struct uhci_controller *u = &uhci_resources[uhci_controller_count].ctrl;
        u->type = USB_TYPE_UHCI;
        u->io_base = (uint16_t)(ctrls[i].base_addr & ~3u);
        u->initialized = 0;

        struct pci_device *pdev = ctrls[i].pci;
        pci_enable_bus_mastering(pdev);

        if (uhci_init_controller(u, uhci_controller_count, pdev) != 0) continue;

        uint8_t lapic = apic_get_lapic_id();
        int vec_slot = (uhci_controller_count < UHCI_MAX_IRQ_VECTORS)
                       ? uhci_controller_count : (UHCI_MAX_IRQ_VECTORS - 1);
        uint8_t irq_vec = (uint8_t)(MSI_VECTOR_UHCI_BASE + vec_slot);

        uint8_t irq_line = pdev->irq_line;
        if (irq_line != 0 && irq_line != 0xFF)
            ioapic_map_pci_irq(irq_line, irq_vec, lapic);
        else
            ioapic_map_pci_irq(11, irq_vec, lapic);

        irq_register_handler(irq_vec, uhci_irq_handler);

        uhci_controller_count++;
    }
}

static struct uhci_controller *uhci_get_controller_internal(int index) {
    return (index >= 0 && index < uhci_controller_count)
    ? &uhci_resources[index].ctrl : NULL;
}

static int uhci_get_controller_count(void) { return uhci_controller_count; }

static int uhci_enumerate(struct uhci_controller *u, uint8_t port) {
    (void)u; (void)port; return 0;
}

static void uhci_start(struct uhci_controller *u) {
    if (!u || !u->initialized) return;
    uhci_hc_start(u);
}

static void uhci_stop(struct uhci_controller *u) {
    if (!u || !u->initialized) return;
    uhci_writew(u, UHCI_CMD, 0);
    for (int i = 0; i < 100; i++) {
        if (uhci_readw(u, UHCI_STS) & UHCI_STS_HCH) break;
        delay_ms(1);
    }
}

void uhci_reset_endpoint_toggle(struct uhci_controller *u, uint8_t dev_addr, uint8_t endpoint) {
    if (!u || !u->initialized) return;
    int res_idx = uhci_res_index(u);
    if (res_idx < 0) return;

    uint8_t ep_num = endpoint & 0x0Fu;
    uint8_t direction = (endpoint & 0x80u) ? 1 : 0;

    uhci_qh_meta_t *meta = uhci_resources[res_idx].qh_meta;
    for (int i = 0; i < UHCI_QH_SLOTS; i++) {
        if (meta[i].in_use && meta[i].dev_addr == dev_addr &&
            meta[i].ep_num == ep_num && meta[i].direction == direction) {
            meta[i].data_toggle = 0;
            meta[i].stalled = 0;
        }
    }
}

void uhci_notify_disconnect(struct usb_device *dev) {
    if (!dev) return;
    for (int ri = 0; ri < MAX_UHCI_CONTROLLERS; ri++) {
        uhci_qh_meta_t *meta = uhci_resources[ri].qh_meta;
        for (int i = 0; i < UHCI_QH_SLOTS; i++) {
            if (!meta[i].in_use || meta[i].device != dev) continue;

            uhci_resources[ri].qh_pool[i].element = LP_TERMINATE;
            asm volatile("mfence" ::: "memory");

            meta[i].in_use = 0;
            meta[i].active = 0;
            meta[i].result_ready = 0;
            meta[i].batch_count = 0;
            meta[i].stalled = 0;
            meta[i].device = NULL;
            meta[i].cookie = NULL;
            meta[i].xfer_buf = NULL;
            meta[i].cursor = NULL;
            meta[i].remaining = 0;
        }

        if (ri < uhci_controller_count &&
            dev->ctrl == (struct usb_controller *)&uhci_resources[ri].ctrl) {
            uhci_dev_is_ls[ri][dev->address & 0x7F] = 0;
            uhci_dev_mps0[ri][dev->address & 0x7F] = 0;
        }
    }
}

struct uhci_driver uhci_driver_loaded = {
    .get_controller_count = uhci_get_controller_count,
    .get_controller = uhci_get_controller_internal,
    .control_transfer = uhci_control_transfer,
    .interrupt_transfer = uhci_interrupt_transfer,
    .bulk_transfer = uhci_bulk_transfer,
    .iso_transfer = uhci_iso_transfer,
    .reset_port = uhci_reset_port,
    .enumerate_device = uhci_enumerate,
    .start = uhci_start,
    .stop = uhci_stop,
    .reset_endpoint_toggle = uhci_reset_endpoint_toggle,
    .notify_disconnect = uhci_notify_disconnect,
};

struct uhci_driver *return_uhci_driver(void) {
    pci_init();

    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc || !tsc->sleep_tsc_ms) return &uhci_driver_loaded;

    delay_ms = tsc->sleep_tsc_ms;

    uhci_scan_pci();
    return &uhci_driver_loaded;
}

struct driver *return_meta_uhci_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };

    static struct driver meta = {
        .name = "UHCI USB Controller Driver",
        .type = USB_DRIVER,
        .sub_type = USB_TYPE_UHCI,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &uhci_driver_loaded,
        .init = (void *)return_uhci_driver,
    };
    return &meta;
}
