#include "xhci.h"

#include "components/drivers.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/logger.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/pci.h"
#include "drivers/Timer/apic_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/limine.h"
#include "kernel/scheduler/scheduler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CACHE_FLUSH(addr) asm volatile("clflush (%0)" : : "r"(addr) : "memory")
#define FULL_BARRIER() asm volatile("mfence" ::: "memory")
#define STORE_BARRIER() asm volatile("sfence" ::: "memory")
#define LOAD_BARRIER() asm volatile("lfence" ::: "memory")
#define COMPILER_BARRIER() asm volatile("" ::: "memory")

#define MAX_XHCI_CONTROLLERS 8
#define TRB_RING_SIZE 256
#define MAX_SLOTS 64
#define ERDP_WITH_EHB(pa) ((pa) | (1ULL << 3))

#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP_STAGE 2
#define TRB_TYPE_DATA_STAGE 3
#define TRB_TYPE_STATUS_STAGE 4
#define TRB_TYPE_LINK 6
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_COMMAND_COMPL 33
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIG_EP 12
#define TRB_TYPE_STOP_ENDPOINT 15
#define TRB_TYPE_RESET_ENDPOINT 14
#define TRB_TYPE_SET_TR_DEQUEUE 16

#define EP_TYPE_ISO_OUT 1
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_INTR_OUT 3

#define EP_TYPE_ISO_IN 5
#define EP_TYPE_BULK_IN 6
#define EP_TYPE_INTR_IN 7

#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_CRCR 0x18
#define XHCI_OP_DCBAAP 0x30
#define XHCI_OP_CONFIG 0x38
#define XHCI_OP_PORTSC(n) (0x400 + ((n)-1) * 0x10)

extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_kernel_address_request kernel_address_request;

typedef struct xhci_slot_context {
    uint32_t dw0, dw1, dw2, dw3;
    uint32_t reserved[4];
} __attribute__((packed)) xhci_slot_context;

typedef struct xhci_endpoint_context {
    uint32_t dw0, dw1;
    uint64_t tr_dequeue_ptr;
    uint32_t dw4;
    uint32_t reserved[3];
} __attribute__((packed)) xhci_endpoint_context;

#define XHCI_CTX_SIZE_32 32u
#define XHCI_CTX_SIZE_64 64u
#define XHCI_MAX_CTX_SIZE XHCI_CTX_SIZE_64
#define XHCI_DEV_CTX_BYTES (32u * XHCI_MAX_CTX_SIZE)
#define XHCI_INPUT_CTX_BYTES (33u * XHCI_MAX_CTX_SIZE)

static inline uint8_t *xhci_dev_ctx_slot(uint8_t *dev_ctx) {
    return dev_ctx;
}
static inline uint8_t *xhci_dev_ctx_ep(uint8_t *dev_ctx, uint32_t ctx_size, uint8_t dci) {

    return dev_ctx + (size_t)dci * ctx_size;
}
static inline uint8_t *xhci_input_ctx_ctrl(uint8_t *input_ctx) {
    return input_ctx;
}
static inline uint8_t *xhci_input_ctx_slot(uint8_t *input_ctx, uint32_t ctx_size) {
    return input_ctx + (size_t)ctx_size;
}
static inline uint8_t *xhci_input_ctx_ep(uint8_t *input_ctx, uint32_t ctx_size, uint8_t dci) {
    return input_ctx + (size_t)(1u + dci) * ctx_size;
}

#define XHCI_INPUT_CTX_STRIDE 4096u
#define XHCI_DEV_CTX_STRIDE 2048u

static struct {
    uint64_t dcbaa[MAX_SLOTS + 1] __attribute__((aligned(4096)));
    struct xhci_trb cmd_ring[TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_trb evt_ring[TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_erst_entry erst[1] __attribute__((aligned(64)));
    uint8_t dev_ctx[MAX_SLOTS][XHCI_DEV_CTX_STRIDE] __attribute__((aligned(4096)));
    uint8_t input_ctx[MAX_SLOTS][XHCI_INPUT_CTX_STRIDE] __attribute__((aligned(4096)));
    struct xhci_trb transfer_rings[MAX_SLOTS][TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_trb intr_rings[MAX_SLOTS][TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_trb bulk_rings[MAX_SLOTS][2][TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_trb iso_rings [MAX_SLOTS][TRB_RING_SIZE] __attribute__((aligned(4096)));
} __attribute__((aligned(4096))) xhci_mem[MAX_XHCI_CONTROLLERS];

_Static_assert(sizeof(struct xhci_trb) * TRB_RING_SIZE == 4096,
               "TRB ring segment must be exactly one 4K page");
_Static_assert(XHCI_INPUT_CTX_STRIDE >= XHCI_INPUT_CTX_BYTES,
               "input context stride too small");
_Static_assert(XHCI_DEV_CTX_STRIDE >= XHCI_DEV_CTX_BYTES,
               "device context stride too small");
_Static_assert(65536u % XHCI_INPUT_CTX_STRIDE == 0 &&
               65536u % XHCI_DEV_CTX_STRIDE == 0,
               "context stride must divide 64K");

static uint32_t intr_tr_idx[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static void *intr_data_ptr[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint16_t intr_data_len[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t intr_cycle[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t intr_active[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t intr_slot_dci[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t ep_configured[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t ep_needs_reset[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint32_t ctrl_tr_idx[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t ctrl_tr_cycle[MAX_XHCI_CONTROLLERS][MAX_SLOTS];

static uint32_t bulk_tr_idx [MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static uint8_t bulk_cycle [MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static uint8_t bulk_active [MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static uint8_t bulk_error [MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static uint8_t bulk_ep_configured[MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static uint8_t bulk_slot_dci [MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];

static volatile uint8_t bulk_submitted[MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static volatile uint8_t bulk_done [MAX_XHCI_CONTROLLERS][MAX_SLOTS][2];
static volatile uint8_t intr_submitted[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static volatile uint8_t intr_done [MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static volatile uint8_t intr_error [MAX_XHCI_CONTROLLERS][MAX_SLOTS];

static uint32_t iso_tr_idx [MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t iso_cycle [MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t iso_ep_configured[MAX_XHCI_CONTROLLERS][MAX_SLOTS];
static uint8_t iso_slot_dci [MAX_XHCI_CONTROLLERS][MAX_SLOTS];

#define XHCI_ISO_MAX_FRAMES 8
typedef struct xhci_iso_ctx {
    uint8_t active;
    uint8_t n_frames;
    uint8_t frames_done;
    uint8_t slot_id;
    uint8_t endpoint;
    void *data;
    uint16_t frame_offsets[XHCI_ISO_MAX_FRAMES];
    uint16_t frame_lens [XHCI_ISO_MAX_FRAMES];
    void *cookie;
} xhci_iso_ctx_t;
static xhci_iso_ctx_t xhci_iso_ctx[MAX_XHCI_CONTROLLERS][MAX_SLOTS];

static struct xhci_controller ctrls[MAX_XHCI_CONTROLLERS];
static int ctrl_count = 0;
static uint8_t g_last_xfer_error_code = 0xFF;
static uint32_t g_last_usbsts = 0;
static void (*delay_ms)(uint64_t);

static inline uint64_t virt_to_phys(void *v) {
    return mm_ptr_to_phys(v);
}

static inline uint64_t mmio_base(uint64_t phys) {
    return phys + (hhdm_req.response ? hhdm_req.response->offset : 0);
}

static inline uint32_t rd32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(mmio_base(base) + off);
}

static inline void wr32(uint64_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(mmio_base(base) + off) = v;
}

static inline void wr64(uint64_t base, uint32_t off, uint64_t v) {
    wr32(base, off, (uint32_t)v);
    wr32(base, off + 4, (uint32_t)(v >> 32));
}

static inline void *xhci_self(struct xhci_controller *x) {
    void *me = (void *)current_task();
    return me ? me : (void *)x;
}

static void xhci_wait_ms(uint64_t ms) {
    if (current_task()) scheduler_sleep_ms(ms);
    else if (delay_ms) delay_ms(ms);
}

#define XHCI_HOT_WINDOW_US 2000u
#define XHCI_CTRL_TIMEOUT_MS 5000u
#define XHCI_CMD_TIMEOUT_MS 5000u

static uint64_t (*xhci_now_us_fn)(void) = NULL;

static inline uint64_t xhci_now_us(void) {
    return xhci_now_us_fn ? xhci_now_us_fn() : 0;
}

typedef struct {
    uint64_t start_us;
    uint32_t polls;
} xhci_waiter_t;

static inline void xhci_waiter_init(xhci_waiter_t *w) {
    w->start_us = xhci_now_us();
    w->polls = 0;
}

static inline uint64_t xhci_waiter_elapsed_us(const xhci_waiter_t *w) {
    if (!xhci_now_us_fn) return (uint64_t)w->polls * 1000u;
    return xhci_now_us() - w->start_us;
}

static inline int xhci_waiter_expired(const xhci_waiter_t *w, uint32_t timeout_ms) {
    return xhci_waiter_elapsed_us(w) >= (uint64_t)timeout_ms * 1000u;
}

static void xhci_waiter_backoff(xhci_waiter_t *w) {
    w->polls++;
    if (xhci_now_us_fn && xhci_waiter_elapsed_us(w) < XHCI_HOT_WINDOW_US) {
        asm volatile("pause");
        return;
    }
    xhci_wait_ms(1);
}

#define XHCI_LOCK_SPINS 4096
#define XHCI_BULK_SPIN_POLLS 256

static inline void xhci_relax(unsigned *spins) {
    unsigned s = ++(*spins);
    if (s < XHCI_LOCK_SPINS || !current_task()) {
        asm volatile("pause");
        return;
    }
    if (s < XHCI_LOCK_SPINS + 16u) scheduler_yield();
    else scheduler_sleep_ms(1);
}

static void xhci_ctrl_enter(struct xhci_controller *x) {
    void *me = xhci_self(x);
    unsigned spins = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&x->lock);
        if (x->lock_depth == 0) {
            x->lock_owner = me;
            x->lock_depth = 1;
            spin_unlock_irqrestore(&x->lock, flags);
            return;
        }
        if (x->lock_owner == me) {
            x->lock_depth++;
            spin_unlock_irqrestore(&x->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&x->lock, flags);
        xhci_relax(&spins);
    }
}

static void xhci_ctrl_leave(struct xhci_controller *x) {
    uint64_t flags = spin_lock_irqsave(&x->lock);
    if (x->lock_depth > 0) x->lock_depth--;
    if (x->lock_depth == 0) x->lock_owner = NULL;
    spin_unlock_irqrestore(&x->lock, flags);
}

static void xhci_event_enter(struct xhci_controller *x) {
    void *me = xhci_self(x);
    unsigned spins = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&x->event_lock);
        if (x->event_depth == 0) {
            x->event_owner = me;
            x->event_depth = 1;
            spin_unlock_irqrestore(&x->event_lock, flags);
            return;
        }
        if (x->event_owner == me) {
            x->event_depth++;
            spin_unlock_irqrestore(&x->event_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&x->event_lock, flags);
        xhci_relax(&spins);
    }
}

static int xhci_send_command_impl(struct xhci_controller *x, struct xhci_trb *cmd);
static int xhci_reset_port_impl(struct xhci_controller *x, uint8_t port);
static int xhci_enable_slot_impl(struct xhci_controller *x);
static int xhci_disable_slot_impl(struct xhci_controller *x, uint8_t slot_id);
static int xhci_address_device_impl(struct xhci_controller *x, uint8_t slot_id, const struct xhci_topology *topo);
static int xhci_evaluate_hub_slot_impl(struct xhci_controller *x, uint8_t slot_id, uint8_t port_count);

static void xhci_event_leave(struct xhci_controller *x) {
    uint64_t flags = spin_lock_irqsave(&x->event_lock);
    if (x->event_depth > 0) x->event_depth--;
    if (x->event_depth == 0) x->event_owner = NULL;
    spin_unlock_irqrestore(&x->event_lock, flags);
}

static void xhci_poll_event_ring_locked(struct xhci_controller *x) {

    volatile struct xhci_trb *ev = &x->event_ring[x->event_ring_idx];
    CACHE_FLUSH((void *)ev);
    FULL_BARRIER();

    int cnt = 0;

    while ((ev->control & 1) == x->event_cycle && cnt < 64) {
        uint8_t type = (ev->control >> 10) & 0x3F;
        uint8_t code = (ev->status >> 24) & 0xFF;
        uint8_t ev_slot = (ev->control >> 24) & 0xFF;
        uint8_t ev_ep = (ev->control >> 16) & 0x1F;

        if (type == TRB_TYPE_COMMAND_COMPL) {
            x->last_slot_id = ev_slot;
            x->last_completion_code = code;

        } else if (type == TRB_TYPE_TRANSFER_EVENT) {

            if (ev_slot != 0 && ev_slot == x->pending_xfer_slot && ev_ep <= 1) {
                if ((code != 1 && code != 13) || ev->param == x->pending_xfer_trb)
                    x->last_completion_code = code;
            }

            if ((code == 1 || code == 13) && (ev_ep > 1)) {

                {
                    int ci_e = (int)(x - ctrls);
                    int ki_e = (int)ev_slot - 1;
                    uint8_t is_bulk = 0, is_iso = 0;
                    uint8_t bulk_dir = 0;
                    if (ki_e >= 0 && ki_e < MAX_SLOTS) {

                        uint8_t dir = ev_ep & 1u;
                        if (bulk_active[ci_e][ki_e][dir] &&
                            bulk_slot_dci[ci_e][ki_e][dir] == ev_ep) {
                            is_bulk = 1;
                        bulk_dir = dir;
                            } else if (iso_slot_dci[ci_e][ki_e] == ev_ep &&
                                xhci_iso_ctx[ci_e][ki_e].active)
                                is_iso = 1;
                    }

                    if (is_bulk) {

                        if (ki_e >= 0 && ki_e < MAX_SLOTS) {
                            bulk_active[ci_e][ki_e][bulk_dir] = 0;
                            bulk_done  [ci_e][ki_e][bulk_dir] = 1;
                        }
                        usb_event_t uevt = {
                            .type = USB_EVENT_BULK_DONE,
                            .src = USB_SRC_XHCI,
                            .slot_id = ev_slot,
                            .endpoint = ev_ep,
                            .port = 0,
                            .completion_code = code,
                            .data = NULL,
                            .data_len = 0,
                            .cookie = NULL,
                        };
                        usb_push_bulk_event(&uevt);
                    } else if (is_iso) {

                        xhci_iso_ctx_t *ictx = (ki_e >= 0 && ki_e < MAX_SLOTS)
                        ? &xhci_iso_ctx[ci_e][ki_e] : NULL;
                        if (ictx) {
                            uint8_t fi = ictx->frames_done;
                            usb_event_t uevt = {
                                .type = (code == 1 || code == 13) ? USB_EVENT_ISO_DONE : USB_EVENT_ISO_ERR,
                                .src = USB_SRC_XHCI,
                                .slot_id = ev_slot,
                                .endpoint = ev_ep,
                                .completion_code = code,
                                .data = NULL,
                                .data_len = 0,
                                .iso_frame_index = fi,
                                .iso_expected_len = (fi < XHCI_ISO_MAX_FRAMES) ? ictx->frame_lens[fi] : 0,
                                .cookie = ictx->cookie,
                            };
                            usb_push_iso_event(&uevt);
                            ictx->frames_done++;
                            if (ictx->frames_done >= ictx->n_frames)
                                ictx->active = 0;
                        }
                    } else {

                        struct usb_device *ev_dev = NULL;
                        for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
                            struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
                            if (_d && (uint8_t)_d->address == ev_slot) { ev_dev = _d; break; }
                        }
                        uint8_t ep_num = ev_ep >> 1;
                        uint8_t is_in = (ev_ep & 1);
                        uint8_t ep_addr = ep_num | (is_in ? 0x80u : 0x00u);

                        if (ki_e >= 0 && ki_e < MAX_SLOTS) {
                            intr_active[ci_e][ki_e] = 0;
                            intr_done  [ci_e][ki_e] = 1;
                        }

                        void *evt_buf = (ki_e >= 0 && ki_e < MAX_SLOTS)
                        ? intr_data_ptr[ci_e][ki_e] : NULL;
                        uint16_t evt_len = (ki_e >= 0 && ki_e < MAX_SLOTS)
                        ? intr_data_len[ci_e][ki_e] : 0;
                        if (evt_buf && evt_len) {
                            for (uint64_t _a = (uint64_t)evt_buf;
                                 _a < (uint64_t)evt_buf + evt_len; _a += 64)
                                 CACHE_FLUSH((void *)_a);
                            FULL_BARRIER();
                        }

                        usb_event_t uevt = {
                            .type = USB_EVENT_TRANSFER_DONE,
                            .src = USB_SRC_XHCI,
                            .transfer_type = USB_XFER_INTERRUPT,
                            .slot_id = ev_slot,
                            .endpoint = ep_addr,
                            .port = 0,
                            .completion_code = code,
                            .device = ev_dev,
                            .cookie = ev_dev,
                            .data = evt_buf,
                            .data_len = evt_len,
                        };
                        usb_push_event(&uevt);
                    }
                }
            }

            if (code != 1 && code != 13 && code != 0 && (ev_ep > 1)) {

                {
                    int ci_p = (int)(x - ctrls);
                    int ki_p = (int)ev_slot - 1;
                    if (ki_p >= 0 && ki_p < MAX_SLOTS) {
                        if (intr_slot_dci[ci_p][ki_p] == ev_ep &&
                            intr_submitted[ci_p][ki_p]) {
                            intr_error[ci_p][ki_p] = 1;
                        }
                        intr_active[ci_p][ki_p] = 0;

                        for (uint8_t _bdir = 0; _bdir < 2; _bdir++) {
                            if (bulk_slot_dci[ci_p][ki_p][_bdir] == ev_ep &&
                                (bulk_active[ci_p][ki_p][_bdir] ||
                                 bulk_submitted[ci_p][ki_p][_bdir])) {
                                bulk_error [ci_p][ki_p][_bdir] = 1;
                            g_last_xfer_error_code = code;
                            bulk_active[ci_p][ki_p][_bdir] = 0;
                                }
                        }
                    }
                }
                usb_event_t uevt = {
                    .type = USB_EVENT_TRANSFER_ERR,
                    .src = USB_SRC_XHCI,
                    .slot_id = ev_slot,
                    .endpoint = ev_ep,
                    .completion_code = code,
                    .data = NULL,
                    .data_len = 0,
                    .cookie = NULL,
                };
                usb_push_event(&uevt);
            }
        }

        x->event_ring_idx = (x->event_ring_idx + 1) % TRB_RING_SIZE;
        if (x->event_ring_idx == 0) x->event_cycle = !x->event_cycle;

        ev = &x->event_ring[x->event_ring_idx];
        CACHE_FLUSH((void *)ev);
        FULL_BARRIER();

        wr64(x->rt_base, 0x38, ERDP_WITH_EHB(virt_to_phys((void *)ev)));
        (void)rd32(x->rt_base, 0x38);
        FULL_BARRIER();
        cnt++;
    }
}

void xhci_poll_event_ring(struct xhci_controller *x) {
    if (!x || !x->initialized) return;

    xhci_event_enter(x);
    xhci_poll_event_ring_locked(x);
    xhci_event_leave(x);
}

void xhci_irq(void) {
    for (int i = 0; i < ctrl_count; i++) {
        struct xhci_controller *x = &ctrls[i];
        if (!x->initialized) continue;

        uint32_t st = rd32(x->op_base, XHCI_OP_USBSTS);
        if (!(st & (1 << 3))) continue;

        wr32(x->op_base, XHCI_OP_USBSTS, st | (1 << 3));

        uint32_t iman = rd32(x->rt_base, 0x20);
        wr32(x->rt_base, 0x20, iman | 3u);

    }
}

int xhci_send_command(struct xhci_controller *x, struct xhci_trb *cmd)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int _r = xhci_send_command_impl(x, cmd);
    xhci_ctrl_leave(x);
    return _r;
}

static int xhci_send_command_impl(struct xhci_controller *x, struct xhci_trb *cmd) {
    if (!x || !x->initialized) return -1;

    struct xhci_trb *trb = &x->cmd_ring[x->cmd_ring_idx];
    trb->param = cmd->param;
    trb->status = cmd->status;
    COMPILER_BARRIER();
    trb->control = (cmd->control & ~1u) | x->cmd_cycle;
    STORE_BARRIER();
    CACHE_FLUSH(trb);
    FULL_BARRIER();

    int next = x->cmd_ring_idx + 1;
    if (next >= TRB_RING_SIZE - 1) {
        x->cmd_ring[TRB_RING_SIZE - 1].param = virt_to_phys(x->cmd_ring);
        COMPILER_BARRIER();
        x->cmd_ring[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | (1 << 1) | x->cmd_cycle;
        STORE_BARRIER();
        CACHE_FLUSH(&x->cmd_ring[TRB_RING_SIZE - 1]);
        FULL_BARRIER();
        x->cmd_ring_idx = 0;
        x->cmd_cycle = !x->cmd_cycle;
    } else {
        x->cmd_ring_idx = next;
    }

    FULL_BARRIER();
    wr32(x->db_base, 0, 0);
    (void)rd32(x->db_base, 0);
    FULL_BARRIER();
    delay_ms(1);
    return 0;
}

static void xhci_wait_command(struct xhci_controller *x, int max_ms) {
    for (int i = 0; i < max_ms; i++) {
        int k = x->event_ring_idx;
        volatile struct xhci_trb *ev = &x->event_ring[k];
        CACHE_FLUSH((void *)ev);
        FULL_BARRIER();

        if ((ev->control & 1) == x->event_cycle) {
            uint8_t t = (ev->control >> 10) & 0x3F;
            uint8_t sl = (ev->control >> 24) & 0xFF;
            uint8_t c = (ev->status >> 24) & 0xFF;

            x->event_ring_idx = (k + 1) % TRB_RING_SIZE;
            if (k + 1 >= TRB_RING_SIZE) x->event_cycle ^= 1;

            volatile struct xhci_trb *next_ev = &x->event_ring[x->event_ring_idx];
            wr64(x->rt_base, 0x38, ERDP_WITH_EHB(virt_to_phys((void *)next_ev)));
            (void)rd32(x->rt_base, 0x38);
            FULL_BARRIER();

            if (t == TRB_TYPE_COMMAND_COMPL) {
                x->last_slot_id = sl;
                x->last_completion_code = c;
                return;
            }
        }

        asm volatile("pause");
        delay_ms(1);

        if (x->last_completion_code != 0xFF) return;
    }
}

static void xhci_handoff(struct xhci_controller *x) {
    LOG_DEBUG("Handoff: Checking ExtCaps");
    uint32_t hcc = rd32(x->base_addr, 0x10);
    uint32_t xecp = (hcc >> 16) << 2;
    if (!xecp) { LOG_DEBUG("No ExtCaps"); return; }

    uint32_t off = xecp;
    for (int guard = 0; guard < 32 && off != 0; guard++) {
        uint32_t v = rd32(x->base_addr, off);
        LOG_DEBUG("ExtCap 0x%08x", v);

        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                LOG_INFO("BIOS owns HC, requesting handoff");
                wr32(x->base_addr, off, v | (1u << 24));
                for (int i = 0; i < 500; i++) {
                    delay_ms(10);
                    uint32_t cur = rd32(x->base_addr, off);

                    if (!(cur & (1u << 16))) {

                        uint32_t ctrl = rd32(x->base_addr, off + 4);
                        wr32(x->base_addr, off + 4, ctrl & ~0x1FFF0000u);
                        LOG_INFO("BIOS handoff OK after %d ms", i * 10);
                        delay_ms(50);
                        return;
                    }
                    if (i % 50 == 0) LOG_DEBUG("Handoff: waiting...");
                }

                LOG_WARNING("BIOS handoff timed out, forcing OS ownership");
                uint32_t cur = rd32(x->base_addr, off);
                wr32(x->base_addr, off, (cur & ~(1u << 16)) | (1u << 24));
                uint32_t ctrl = rd32(x->base_addr, off + 4);
                wr32(x->base_addr, off + 4, ctrl & ~0x1FFF0000u);
                delay_ms(50);
            } else {
                LOG_DEBUG("No BIOS ownership (OS already owns or no semaphore)");
            }
            return;
        }

        uint32_t next = (v >> 8) & 0xFF;
        if (next == 0) break;
        off += next << 2;
    }
    LOG_DEBUG("No USB Legacy Support ExtCap found");
}

int xhci_reset_port(struct xhci_controller *x, uint8_t port)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int _r = xhci_reset_port_impl(x, port);
    xhci_ctrl_leave(x);
    return _r;
}

static int xhci_reset_port_impl(struct xhci_controller *x, uint8_t port) {
    if (!x || port == 0) return -1;
    LOG_DEBUG("resetting port %u", (unsigned)port);

    uint32_t reg = XHCI_OP_PORTSC(port);

    uint32_t st = rd32(x->op_base, reg);
    LOG_DEBUG("port %u PORTSC=0x%08x", (unsigned)port, st);

    if (!(st & XHCI_PORTSC_CCS)) { LOG_WARNING("no device on port %u", (unsigned)port); return -1; }

    if (!(st & XHCI_PORTSC_PP)) {
        wr32(x->op_base, reg, xhci_portsc_neutral(st) | XHCI_PORTSC_PP);
        delay_ms(100);
        st = rd32(x->op_base, reg);
    }

    wr32(x->op_base, reg, xhci_portsc_neutral(st) | XHCI_PORTSC_PR);
    delay_ms(200);

    for (int t = 100; t-- > 0;) {
        if (!(rd32(x->op_base, reg) & XHCI_PORTSC_PR)) break;
        delay_ms(10);
    }
    delay_ms(50);

    st = rd32(x->op_base, reg);
    LOG_DEBUG("port %u PORTSC=0x%08x", (unsigned)port, st);

    if (st & XHCI_PORTSC_CHANGE_MASK) {
        wr32(x->op_base, reg,
             xhci_portsc_neutral(st) | (st & XHCI_PORTSC_CHANGE_MASK));
        st = rd32(x->op_base, reg);
    }

    if ((st & XHCI_PORTSC_CCS) && !(st & XHCI_PORTSC_PED)) {
        uint32_t speed = (st & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
        if (speed >= 4u) {
            for (int attempt = 0;
                 attempt < 3 && (st & XHCI_PORTSC_CCS) && !(st & XHCI_PORTSC_PED);
                 attempt++) {
                wr32(x->op_base, reg, xhci_portsc_neutral(st) | XHCI_PORTSC_WPR);
                delay_ms(200);
                for (int t = 100; t-- > 0;) {
                    if (!(rd32(x->op_base, reg) & XHCI_PORTSC_WPR)) break;
                    delay_ms(10);
                }
                delay_ms(50);
                st = rd32(x->op_base, reg);
                if (st & XHCI_PORTSC_CHANGE_MASK) {
                    wr32(x->op_base, reg,
                         xhci_portsc_neutral(st) | (st & XHCI_PORTSC_CHANGE_MASK));
                    st = rd32(x->op_base, reg);
                }
            }
        }
    }

    if ((st & 1u) && (st & (1u << 1))) { LOG_DEBUG("port %u reset OK", (unsigned)port); delay_ms(100); return 0; }

    LOG_WARNING("port %u did not reach Enabled after reset, PORTSC=0x%08x", (unsigned)port, st);
    return -1;
}

int xhci_enable_slot(struct xhci_controller *x)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int _r = xhci_enable_slot_impl(x);
    xhci_ctrl_leave(x);
    return _r;
}

static int xhci_enable_slot_impl(struct xhci_controller *x) {
    LOG_DEBUG("Enable Slot");
    struct xhci_trb cmd = {0};
    cmd.control = (TRB_TYPE_ENABLE_SLOT << 10);
    x->last_slot_id = 0;
    x->last_completion_code = 0xFF;
    xhci_send_command(x, &cmd);

    for (int i = 0; i < 5000; i++) {
        if (i % 100 == 0) LOG_DEBUG("poll %d, USBSTS=0x%08x", i, rd32(x->op_base, XHCI_OP_USBSTS));
        xhci_poll_event_ring(x);
        if (x->last_completion_code != 0xFF) {
            if (x->last_completion_code == 1 && x->last_slot_id > 0) {
                LOG_DEBUG("slot %u enabled", (unsigned)x->last_slot_id);
                delay_ms(10);
                return x->last_slot_id;
            }
            LOG_ERROR("Enable Slot failed, completion code %u", (unsigned)x->last_completion_code);
            return -1;
        }
        delay_ms(2);
    }
    LOG_ERROR("Enable Slot timed out");
    return -1;
}

int xhci_disable_slot(struct xhci_controller *x, uint8_t slot_id)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int _r = xhci_disable_slot_impl(x, slot_id);
    xhci_ctrl_leave(x);
    return _r;
}

static int xhci_disable_slot_impl(struct xhci_controller *x, uint8_t slot_id) {
    if (!x || !x->initialized || slot_id == 0 || slot_id > x->max_slots) return -1;

    LOG_DEBUG("Disable Slot %u", (unsigned)slot_id);

    struct xhci_trb cmd = {0};
    cmd.control = ((uint32_t)slot_id << 24) | (TRB_TYPE_DISABLE_SLOT << 10);
    x->last_completion_code = 0xFF;

    if (xhci_send_command(x, &cmd) != 0) { LOG_ERROR("DISABLE_SLOT send fail, code 0x%x", slot_id); return -1; }

    for (int i = 0; i < 5000; i++) {
        xhci_poll_event_ring(x);
        if (x->last_completion_code != 0xFF) break;
        delay_ms(2);
    }

    if (x->last_completion_code != 1) {
        LOG_ERROR("DISABLE_SLOT FAILED, code 0x%x", x->last_completion_code);

    }

    int idx = x - ctrls;
    xhci_mem[idx].dcbaa[slot_id] = 0;
    CACHE_FLUSH(&xhci_mem[idx].dcbaa[slot_id]);
    intr_active[idx][slot_id - 1] = 0;
    intr_submitted[idx][slot_id - 1] = 0;
    intr_done[idx][slot_id - 1] = 0;
    intr_error[idx][slot_id - 1] = 0;
    intr_slot_dci[idx][slot_id - 1] = 0;
    ep_configured[idx][slot_id - 1] = 0;
    ep_needs_reset[idx][slot_id - 1] = 0;
    bulk_ep_configured[idx][slot_id - 1][0] = 0;
    bulk_ep_configured[idx][slot_id - 1][1] = 0;
    bulk_active[idx][slot_id - 1][0] = 0;
    bulk_active[idx][slot_id - 1][1] = 0;
    bulk_error[idx][slot_id - 1][0] = 0;
    bulk_error[idx][slot_id - 1][1] = 0;
    bulk_submitted[idx][slot_id - 1][0] = 0;
    bulk_submitted[idx][slot_id - 1][1] = 0;
    bulk_done[idx][slot_id - 1][0] = 0;
    bulk_done[idx][slot_id - 1][1] = 0;
    intr_submitted[idx][slot_id - 1] = 0;
    intr_done[idx][slot_id - 1] = 0;
    intr_error[idx][slot_id - 1] = 0;

    return 0;
}

int xhci_address_device(struct xhci_controller *x, uint8_t slot_id, const struct xhci_topology *topo)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int _r = xhci_address_device_impl(x, slot_id, topo);
    xhci_ctrl_leave(x);
    return _r;
}

static int xhci_address_device_impl(struct xhci_controller *x, uint8_t slot_id, const struct xhci_topology *topo) {
    LOG_DEBUG("Address Device, slot %u", (unsigned)slot_id);
    if (slot_id == 0 || slot_id > MAX_SLOTS) { LOG_ERROR("Bad slot, code 0x%x", slot_id); return -1; }

    static const struct xhci_topology root_topo_default = {0, 0, 0, 0, 0};
    if (!topo) topo = &root_topo_default;

    int idx = x - ctrls;
    uint32_t ctx_size = x->csz ? (uint32_t)x->csz : XHCI_CTX_SIZE_32;
    uint32_t W = ctx_size / 4;
    uint32_t *ic = (uint32_t *)xhci_mem[idx].input_ctx[slot_id - 1];
    uint8_t *dc = (uint8_t *)xhci_mem[idx].dev_ctx[slot_id - 1];
    uint8_t *tr = (uint8_t *)&xhci_mem[idx].transfer_rings[slot_id - 1];

    memset(ic, 0, XHCI_INPUT_CTX_BYTES);
    memset(dc, 0, XHCI_DEV_CTX_BYTES);
    memset(tr, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);
    intr_active[idx][slot_id - 1] = 0;
    intr_submitted[idx][slot_id - 1] = 0;
    intr_done[idx][slot_id - 1] = 0;
    intr_error[idx][slot_id - 1] = 0;
    intr_slot_dci[idx][slot_id - 1] = 0;
    ep_configured[idx][slot_id - 1] = 0;
    ep_needs_reset[idx][slot_id - 1] = 0;
    ctrl_tr_idx[idx][slot_id - 1] = 0;
    ctrl_tr_cycle[idx][slot_id - 1] = 1;

    uint32_t ss, mps;
    if (topo->parent_hub_slot == 0) {

        uint32_t portsc = rd32(x->op_base, XHCI_OP_PORTSC(topo->root_port));
        LOG_DEBUG("port %u PORTSC=0x%08x", (unsigned)topo->root_port, portsc);
        uint32_t speed = (portsc >> 10) & 0xF;
        switch (speed) {
            case 1: ss = 1; mps = 64; LOG_DEBUG("FS (64)"); break;
            case 2: ss = 2; mps = 8; LOG_DEBUG("LS (8)"); break;
            case 3: ss = 3; mps = 64; LOG_DEBUG("HS (64)"); break;
            case 4: ss = 4; mps = 512; LOG_DEBUG("SS (512)"); break;
            default:

                ss = speed;
                mps = 512;
                LOG_DEBUG("SS/SSP (raw PSIV) 0x%x", speed);
                break;
        }
    } else {

        ss = topo->speed_id ? topo->speed_id : 1u;
        switch (ss) {
            case 2: mps = 8; LOG_DEBUG("hub child: LS (8)"); break;
            case 3: mps = 64; LOG_DEBUG("hub child: HS (64)"); break;
            case 4: mps = 512; LOG_DEBUG("hub child: SS (512)"); break;
            default: ss = 1; mps = 64; LOG_DEBUG("hub child: FS (64)"); break;
        }
    }

    ic[1] = 0x3;

    ic[W + 0] = (topo->route_string & 0xFFFFFu) | (1u << 27) | (ss << 20);
    ic[W + 1] = ((uint32_t)topo->root_port << 16);
    ic[W + 2] = ((uint32_t)topo->parent_hub_slot) | ((uint32_t)topo->parent_port << 8);

    ic[2 * W + 1] = (mps << 16) | (4 << 3) | (3 << 1);
    uint64_t tr_phys = virt_to_phys(tr);
    ic[2 * W + 2] = (uint32_t)(tr_phys | 1);
    ic[2 * W + 3] = (uint32_t)(tr_phys >> 32);
    ic[2 * W + 4] = (mps << 16);

    uint32_t *dw = (uint32_t *)dc;
    for (uint32_t i = 0; i < 2 * W; i++) dw[i] = ic[W + i];

    xhci_mem[idx].dcbaa[slot_id] = virt_to_phys(dc);
    CACHE_FLUSH(&xhci_mem[idx].dcbaa[slot_id]);
    for (size_t i = 0; i < XHCI_INPUT_CTX_BYTES; i += 64) CACHE_FLUSH((uint8_t *)ic + i);
    for (size_t i = 0; i < XHCI_DEV_CTX_BYTES; i += 64) CACHE_FLUSH(dc + i);
    for (size_t i = 0; i < sizeof(struct xhci_trb) * TRB_RING_SIZE; i += 64) CACHE_FLUSH(tr + i);
    FULL_BARRIER();
    delay_ms(5);

    struct xhci_trb cmd = {0};
    cmd.param = virt_to_phys(ic);
    cmd.control = (TRB_TYPE_ADDRESS_DEVICE << 10) | (slot_id << 24);
    x->last_completion_code = 0xFF;
    xhci_send_command(x, &cmd);

    for (int i = 0; i < 5000; i++) {
        if (i % 100 == 0) LOG_DEBUG("poll %d, USBSTS=0x%08x", i, rd32(x->op_base, XHCI_OP_USBSTS));
        if (rd32(x->op_base, XHCI_OP_USBSTS) & 1) { LOG_ERROR("HALTED!"); return -1; }
        xhci_poll_event_ring(x);
        if (x->last_completion_code != 0xFF) {
            if (x->last_completion_code == 1) {
                LOG_DEBUG("slot %u addressed, USBSTS=0x%08x", (unsigned)slot_id, rd32(x->op_base, XHCI_OP_USBSTS));
                for (size_t i2 = 0; i2 < XHCI_DEV_CTX_BYTES; i2 += 64) CACHE_FLUSH(dc + i2);
                FULL_BARRIER();
                delay_ms(100);
                return 0;
            }
            LOG_ERROR("Address FAILED, code 0x%x", x->last_completion_code);
            return -1;
        }
        delay_ms(2);
    }
    LOG_ERROR("Address TIMEOUT");
    return -1;
}

int xhci_evaluate_hub_slot(struct xhci_controller *x, uint8_t slot_id, uint8_t port_count)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int _r = xhci_evaluate_hub_slot_impl(x, slot_id, port_count);
    xhci_ctrl_leave(x);
    return _r;
}

static int xhci_evaluate_hub_slot_impl(struct xhci_controller *x, uint8_t slot_id, uint8_t port_count) {
    if (!x || !x->initialized || slot_id == 0 || slot_id > x->max_slots) return -1;

    int idx = x - ctrls;
    uint32_t ctx_size = x->csz ? (uint32_t)x->csz : XHCI_CTX_SIZE_32;
    uint32_t W = ctx_size / 4;
    uint32_t *ic = (uint32_t *)xhci_mem[idx].input_ctx[slot_id - 1];
    uint8_t *dc = (uint8_t *)xhci_mem[idx].dev_ctx[slot_id - 1];

    memset(ic, 0, XHCI_INPUT_CTX_BYTES);

    for (size_t i = 0; i < XHCI_DEV_CTX_BYTES; i += 64) CACHE_FLUSH(dc + i);
    FULL_BARRIER();

    struct xhci_slot_context *dev_slot = (struct xhci_slot_context *)xhci_dev_ctx_slot(dc);

    ic[1] = 0x1;

    uint32_t *slot_w = ic + W;
    slot_w[0] = dev_slot->dw0 | (1u << 26);
    slot_w[1] = (dev_slot->dw1 & 0x0000FFFFu) | ((uint32_t)port_count << 24);
    slot_w[2] = dev_slot->dw2;
    slot_w[3] = dev_slot->dw3;

    for (size_t i = 0; i < XHCI_INPUT_CTX_BYTES; i += 64) CACHE_FLUSH((uint8_t *)ic + i);
    FULL_BARRIER();

    struct xhci_trb cmd = {0};
    cmd.param = virt_to_phys(ic);
    cmd.control = ((uint32_t)slot_id << 24) | (TRB_TYPE_EVALUATE_CONTEXT << 10);
    x->last_completion_code = 0xFF;

    if (xhci_send_command(x, &cmd) != 0) { LOG_ERROR("EVAL_HUB_SLOT send fail"); return -1; }

    for (int i = 0; i < 5000; i++) {
        xhci_poll_event_ring(x);
        if (x->last_completion_code != 0xFF) break;
        delay_ms(2);
    }

    if (x->last_completion_code != 1) {
        LOG_ERROR("EVAL_HUB_SLOT FAILED, code 0x%x", x->last_completion_code);
        return -1;
    }

    for (size_t i = 0; i < XHCI_DEV_CTX_BYTES; i += 64) CACHE_FLUSH(dc + i);
    FULL_BARRIER();
    LOG_DEBUG("Hub slot marked (Hub=1) 0x%x", slot_id);
    return 0;
}

static int xhci_configure_endpoint(struct xhci_controller *x, uint8_t slot_id, uint8_t dci, uint8_t ep_type, uint16_t mps, uint8_t interval)
{


    LOG_DEBUG("Configure EP slot %u dci %u type %u mps %u interval %u",
              (unsigned)slot_id, (unsigned)dci, (unsigned)ep_type, (unsigned)mps, (unsigned)interval);

    if (!x || !x->initialized || slot_id == 0 || slot_id > x->max_slots) {
        LOG_ERROR("CFG_EP: bad params, code 0x%x", slot_id);
        return -1;
    }

    int ci = x - ctrls;
    uint32_t ctx_size = x->csz ? (uint32_t)x->csz : XHCI_CTX_SIZE_32;
    uint8_t *ictx = (uint8_t *)xhci_mem[ci].input_ctx[slot_id - 1];
    uint8_t *dctx = (uint8_t *)xhci_mem[ci].dev_ctx[slot_id - 1];

    uint8_t is_bulk_ep = (ep_type == EP_TYPE_BULK_IN || ep_type == EP_TYPE_BULK_OUT);
    uint8_t bulk_dir = (ep_type == EP_TYPE_BULK_IN) ? 1u : 0u;
    int ki = (int)slot_id - 1;

    struct xhci_trb *tr_ring;
    if (is_bulk_ep) {
        tr_ring = xhci_mem[ci].bulk_rings[ki][bulk_dir];
    } else {
        tr_ring = xhci_mem[ci].intr_rings[slot_id - 1];
    }
    memset(tr_ring, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);
    tr_ring[TRB_RING_SIZE - 1].param = virt_to_phys(tr_ring);
    tr_ring[TRB_RING_SIZE - 1].status = 0;
    tr_ring[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | (1u << 1) | 1u;
    for (int i = 0; i < TRB_RING_SIZE; i++) CACHE_FLUSH(&tr_ring[i]);
    FULL_BARRIER();

    if (!is_bulk_ep) {
        intr_tr_idx[ci][slot_id - 1] = 0;
        intr_cycle[ci][slot_id - 1] = 1;
        intr_active[ci][slot_id - 1] = 0;
        intr_slot_dci[ci][slot_id - 1] = dci;
    } else {

        bulk_tr_idx[ci][ki][bulk_dir] = 0;
        bulk_cycle [ci][ki][bulk_dir] = 1;
        bulk_active[ci][ki][bulk_dir] = 0;
        bulk_submitted[ci][ki][bulk_dir] = 0;
        bulk_done [ci][ki][bulk_dir] = 0;
    }

    uint64_t tr_phys = virt_to_phys(tr_ring);
    LOG_DEBUG("intr ring phys 0x%016llx", (unsigned long long)tr_phys);

    memset(ictx, 0, XHCI_INPUT_CTX_BYTES);
    {
        uint32_t *ictrl = (uint32_t *)xhci_input_ctx_ctrl(ictx);
        ictrl[0] = 0;
        ictrl[1] = (1u << 0) | (1u << dci);
    }

    for (size_t fi = 0; fi < XHCI_DEV_CTX_BYTES; fi += 64)
        CACHE_FLUSH(dctx + fi);
    FULL_BARRIER();

    struct xhci_slot_context *dev_slot = (struct xhci_slot_context *)xhci_dev_ctx_slot(dctx);
    struct xhci_slot_context *in_slot = (struct xhci_slot_context *)xhci_input_ctx_slot(ictx, ctx_size);

    in_slot->dw0 = dev_slot->dw0;
    in_slot->dw1 = dev_slot->dw1;
    in_slot->dw2 = dev_slot->dw2;
    in_slot->dw3 = dev_slot->dw3;

    {
        uint32_t cur_entries = (in_slot->dw0 >> 27) & 0x1Fu;
        uint32_t new_entries = (cur_entries > (uint32_t)dci) ? cur_entries : (uint32_t)dci;
        in_slot->dw0 = (in_slot->dw0 & ~(0x1Fu << 27)) | (new_entries << 27);
    }

    bool ep_is_periodic = (ep_type == EP_TYPE_INTR_IN || ep_type == EP_TYPE_INTR_OUT ||
                           ep_type == EP_TYPE_ISO_IN || ep_type == EP_TYPE_ISO_OUT);
    uint8_t xhci_interval = 0;
    {
        uint8_t dev_speed = (uint8_t)((in_slot->dw0 >> 20) & 0xFu);
        bool is_fs_ls = (dev_speed == 1u || dev_speed == 2u);

        if (ep_is_periodic) {
            if (is_fs_ls) {

                uint32_t bi = interval ? (uint32_t)interval : 1u;
                uint8_t log2val = 0;
                while ((bi >> (log2val + 1u)) != 0u) log2val++;
                int v = (int)log2val + 3;
                if (v < 3) v = 3;
                if (v > 10) v = 10;
                xhci_interval = (uint8_t)v;
            } else {

                int v = (int)interval - 1;
                if (v < 0) v = 0;
                if (v > 15) v = 15;
                xhci_interval = (uint8_t)v;
            }
        }
    }
    LOG_DEBUG("CFG_EP interval(xhci) 0x%x", xhci_interval);

    uint32_t max_burst = 0u;
    uint32_t max_esit = ep_is_periodic ? ((uint32_t)mps * (max_burst + 1u)) : 0u;
    uint32_t avg_trb_len = mps ? (uint32_t)mps : 8u;

    struct xhci_endpoint_context *epctx =
        (struct xhci_endpoint_context *)xhci_input_ctx_ep(ictx, ctx_size, dci);
    epctx->dw0 = (uint32_t)xhci_interval << 16;
    epctx->dw1 = ((uint32_t)mps << 16)
    | (max_burst << 8)
    | ((uint32_t)ep_type << 3)
    | (3u << 1);
    epctx->tr_dequeue_ptr = tr_phys | 1u;
    epctx->dw4 = (avg_trb_len & 0xFFFFu) | ((max_esit & 0xFFFFu) << 16);

    for (size_t fi = 0; fi < XHCI_INPUT_CTX_BYTES; fi += 64)
        CACHE_FLUSH(ictx + fi);
    FULL_BARRIER();

    struct xhci_trb cmd = {0};
    cmd.param = virt_to_phys(ictx);
    cmd.control = ((uint32_t)slot_id << 24) | (TRB_TYPE_CONFIG_EP << 10);
    x->last_completion_code = 0xFF;

    if (xhci_send_command(x, &cmd) != 0) { LOG_ERROR("CFG_EP send fail"); return -1; }

    for (int i = 0; i < 5000; i++) {
        if (i % 100 == 0) LOG_DEBUG("CFG_EP poll %d, USBSTS=0x%08x", i, rd32(x->op_base, XHCI_OP_USBSTS));
        xhci_poll_event_ring(x);
        if (x->last_completion_code != 0xFF) break;
        delay_ms(2);
    }

    if (x->last_completion_code != 1) {
        LOG_ERROR("CFG_EP FAILED, code 0x%x", x->last_completion_code);
        return -1;
    }
    LOG_DEBUG("Configure EP OK slot %u, USBSTS=0x%08x", (unsigned)slot_id, rd32(x->op_base, XHCI_OP_USBSTS));
    delay_ms(10);

    struct xhci_endpoint_context *dev_ep =
        (struct xhci_endpoint_context *)xhci_dev_ctx_ep(dctx, ctx_size, dci);

    for (size_t fi = 0; fi < ctx_size; fi += 64)
        CACHE_FLUSH((uint8_t *)dev_ep + fi);
    FULL_BARRIER();

    uint8_t ep_state = dev_ep->dw0 & 0x7u;
    uint32_t dw1_check = dev_ep->dw1;

    if (((dw1_check >> 3) & 0x7u) == 0) {

        dev_ep->dw0 = epctx->dw0 | (ep_state ? ep_state : 1u);
        dev_ep->dw1 = epctx->dw1;
        dev_ep->tr_dequeue_ptr = epctx->tr_dequeue_ptr;
        dev_ep->dw4 = epctx->dw4;
        for (size_t fi = 0; fi < ctx_size; fi += 64)
            CACHE_FLUSH((uint8_t *)dev_ep + fi);
        FULL_BARRIER();
    }

    ep_configured[ci][slot_id - 1] = 1;
    LOG_DEBUG("EP configured");
    return 0;
}

static int xhci_reset_endpoint(struct xhci_controller *x, uint8_t slot_id, uint8_t dci);
static int xhci_set_tr_dequeue(struct xhci_controller *x, uint8_t slot_id, uint8_t dci, uint64_t ring_base);
static void xhci_control_ep0_recover(struct xhci_controller *x, uint8_t slot_id, int idx, struct xhci_trb *tr);

static int xhci_control_transfer_impl(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *setup, uint16_t setup_len, void *data, uint16_t data_len, uint8_t direction)
{
    (void)endpoint; (void)setup_len;
    if (!x || slot_id == 0) return -1;

    int idx = x - ctrls;
    struct xhci_trb *tr = (struct xhci_trb *)xhci_mem[idx].transfer_rings[slot_id - 1];

    int i = (int)ctrl_tr_idx[idx][slot_id - 1];
    uint8_t cyc = ctrl_tr_cycle[idx][slot_id - 1];

    int needed = (data_len > 0) ? 3 : 2;
    if (i + needed >= TRB_RING_SIZE) {
        tr[i].param = virt_to_phys(tr);
        tr[i].status = 0;
        COMPILER_BARRIER();
        tr[i].control = (TRB_TYPE_LINK << 10) | (1u << 1) | (uint32_t)cyc;
        STORE_BARRIER();
        CACHE_FLUSH(&tr[i]);
        FULL_BARRIER();
        cyc ^= 1;
        ctrl_tr_cycle[idx][slot_id - 1] = cyc;
        i = 0;
    }

    for (int k = 0; k < 16; k++) CACHE_FLUSH(&tr[(i + k) % TRB_RING_SIZE]);
    FULL_BARRIER();

    tr[i].param = *(uint64_t *)setup;
    tr[i].status = 8;
    uint32_t trt = (data_len > 0) ? (direction ? 3 : 2) : 0;
    COMPILER_BARRIER();
    tr[i].control = (TRB_TYPE_SETUP_STAGE << 10) | (1 << 6) | (trt << 16) | cyc;
    STORE_BARRIER(); CACHE_FLUSH(&tr[i]); i = (i + 1) % TRB_RING_SIZE; FULL_BARRIER();

    if (data_len > 0) {
        tr[i].param = virt_to_phys(data);
        tr[i].status = data_len;
        for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64) CACHE_FLUSH((void *)a);
        FULL_BARRIER(); COMPILER_BARRIER();
        tr[i].control = (TRB_TYPE_DATA_STAGE << 10) | (direction << 16) | (1 << 2) | cyc;
        STORE_BARRIER(); CACHE_FLUSH(&tr[i]); i = (i + 1) % TRB_RING_SIZE; FULL_BARRIER();
    }

    uint32_t sd = (data_len > 0) ? (direction ? 0 : 1) : 1;
    tr[i].param = 0;
    tr[i].status = 0;
    COMPILER_BARRIER();
    tr[i].control = (TRB_TYPE_STATUS_STAGE << 10) | (1 << 5) | (sd << 16) | cyc;
    STORE_BARRIER(); CACHE_FLUSH(&tr[i]); i = (i + 1) % TRB_RING_SIZE; FULL_BARRIER();

    ctrl_tr_idx[idx][slot_id - 1] = i;

    {
        int st_idx = (i == 0) ? (TRB_RING_SIZE - 1) : (i - 1);
        x->pending_xfer_trb = virt_to_phys(&tr[st_idx]);
    }

    x->pending_xfer_slot = 0;
    xhci_poll_event_ring(x);

    x->pending_xfer_slot = slot_id;
    x->last_completion_code = 0xFF;
    FULL_BARRIER();
    wr32(x->db_base, slot_id * 4, 1);
    (void)rd32(x->db_base, slot_id * 4);
    FULL_BARRIER();

    xhci_waiter_t w;
    xhci_waiter_init(&w);

    for (;;) {
        if ((w.polls & 1023u) == 0 && w.polls) LOG_DEBUG("control transfer slot %u still waiting, %u polls", (unsigned)slot_id, (unsigned)w.polls);
        {
            uint32_t _sts = rd32(x->op_base, XHCI_OP_USBSTS);
            if (_sts & ((1u << 0) | (1u << 2) | (1u << 14))) {
                LOG_ERROR("USBSTS went fatal (0x%08x) during control transfer on slot %u, request %02x/%02x",
                          _sts, (unsigned)slot_id, (unsigned)((uint8_t *)setup)[0], (unsigned)((uint8_t *)setup)[1]);
                g_last_xfer_error_code = x->last_completion_code;
                g_last_usbsts = _sts;
                x->pending_xfer_slot = 0;
                xhci_control_ep0_recover(x, slot_id, idx, tr);
                return -1;
            }
        }
        xhci_poll_event_ring(x);
        if (x->last_completion_code != 0xFF) {
            uint8_t cc = x->last_completion_code;
            x->pending_xfer_slot = 0;
            if (cc == 1 || cc == 13) {
                if (direction == 1 && data_len > 0) {
                    for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
                        CACHE_FLUSH((void *)a);
                    FULL_BARRIER();
                }
                delay_ms(2);
                return 0;
            }
            LOG_ERROR("control transfer on slot %u failed, completion code %u, request %02x/%02x",
                      (unsigned)slot_id, (unsigned)cc, (unsigned)((uint8_t *)setup)[0], (unsigned)((uint8_t *)setup)[1]);
            g_last_xfer_error_code = cc;
            g_last_usbsts = rd32(x->op_base, XHCI_OP_USBSTS);
            xhci_control_ep0_recover(x, slot_id, idx, tr);
            return -1;
        }
        if (xhci_waiter_expired(&w, XHCI_CTRL_TIMEOUT_MS)) break;
        xhci_waiter_backoff(&w);
    }
    x->pending_xfer_slot = 0;
    LOG_ERROR("control transfer on slot %u timed out after %u polls, request %02x/%02x",
              (unsigned)slot_id, (unsigned)w.polls, (unsigned)((uint8_t *)setup)[0], (unsigned)((uint8_t *)setup)[1]);
    g_last_xfer_error_code = 0xFF;
    g_last_usbsts = rd32(x->op_base, XHCI_OP_USBSTS);
    xhci_control_ep0_recover(x, slot_id, idx, tr);
    return -1;
}

int xhci_control_transfer(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *setup, uint16_t setup_len, void *data, uint16_t data_len, uint8_t direction)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int ret = xhci_control_transfer_impl(x, slot_id, endpoint, setup, setup_len, data, data_len, direction);
    xhci_ctrl_leave(x);
    return ret;
}

static int xhci_init(struct xhci_controller *x, int idx) {
    LOG_INFO("initializing controller %d at 0x%llx", idx, (unsigned long long)x->base_addr);
    x->type = 3;

    uint32_t cap_lo = rd32(x->base_addr, 0);
    uint32_t cap = cap_lo & 0xFF;
    x->cap_len = cap;
    x->op_base = x->base_addr + cap;
    x->rt_base = x->base_addr + (rd32(x->base_addr, 0x18) & ~0x1F);
    x->db_base = x->base_addr + (rd32(x->base_addr, 0x14) & ~0x3);

    xhci_handoff(x);

    uint32_t cmd_val = rd32(x->op_base, XHCI_OP_USBCMD);
    if (cmd_val & 1) {
        LOG_DEBUG("HC running, stopping...");
        wr32(x->op_base, XHCI_OP_USBCMD, cmd_val & ~1u);

        int th = 500;
        while (!(rd32(x->op_base, XHCI_OP_USBSTS) & 1) && --th) delay_ms(1);
        if (!th) {
            LOG_WARNING("HC stop timeout, continuing to software reset");

        } else {
            LOG_DEBUG("HC stopped");
        }
    } else {
        LOG_DEBUG("HC already stopped");
    }

    wr32(x->op_base, XHCI_OP_USBCMD, (1 << 1));
    delay_ms(10);
    int tr = 5000;
    while ((rd32(x->op_base, XHCI_OP_USBCMD) & (1 << 1)) && --tr) delay_ms(1);
    if (!tr) { LOG_ERROR("Reset FAILED"); return -1; }
    LOG_DEBUG("Reset OK");

    int tcnr = 2000;
    while ((rd32(x->op_base, XHCI_OP_USBSTS) & (1 << 11)) && --tcnr) delay_ms(1);
    if (!tcnr) { LOG_ERROR("CNR stuck"); return -1; }
    LOG_DEBUG("CNR cleared");
    delay_ms(50);

    {
        uint32_t hccp1 = rd32(x->base_addr, 0x10);
        if (hccp1 & (1u << 2)) {
            x->csz = XHCI_CTX_SIZE_64;
            LOG_DEBUG("CSZ=1, using 64-byte contexts 0x%x", hccp1);
        } else {
            x->csz = XHCI_CTX_SIZE_32;
            LOG_DEBUG("CSZ=0, using 32-byte contexts 0x%x", hccp1);
        }
    }

    uint32_t hcs1 = rd32(x->base_addr, 0x04);
    x->max_slots = hcs1 & 0xFF;
    uint32_t maxp = (hcs1 >> 24) & 0xFF;
    x->max_ports = maxp;

    uint32_t hcs2 = rd32(x->base_addr, 0x08);
    uint32_t max_scratch = ((hcs2 >> 27) & 0x1Fu) | (((hcs2 >> 21) & 0x1Fu) << 5);

    memset(&xhci_mem[idx], 0, sizeof(xhci_mem[idx]));
    for (int s = 0; s < MAX_SLOTS; s++) {
        intr_tr_idx[idx][s] = 0;
        intr_cycle[idx][s] = 1;
        intr_active[idx][s] = 0;
        intr_submitted[idx][s] = 0;
        intr_done[idx][s] = 0;
        intr_error[idx][s] = 0;
        intr_slot_dci[idx][s] = 0;
        ep_configured[idx][s] = 0;
        ep_needs_reset[idx][s] = 0;
        ctrl_tr_idx[idx][s] = 0;
        ctrl_tr_cycle[idx][s] = 1;

        for (int d = 0; d < 2; d++) {
            bulk_tr_idx [idx][s][d] = 0;
            bulk_cycle [idx][s][d] = 1;
            bulk_active [idx][s][d] = 0;
            bulk_error [idx][s][d] = 0;
            bulk_submitted[idx][s][d] = 0;
            bulk_done [idx][s][d] = 0;
            bulk_ep_configured[idx][s][d] = 0;
            bulk_slot_dci [idx][s][d] = 0;
        }
    }

    x->cmd_ring = xhci_mem[idx].cmd_ring;
    x->cmd_ring_idx = 0;
    x->cmd_cycle = 1;
    x->event_ring = xhci_mem[idx].evt_ring;
    x->event_ring_idx = 0;
    x->event_cycle = 1;
    x->dcbaa = xhci_mem[idx].dcbaa;
    x->erst = xhci_mem[idx].erst;

    uint64_t dcbaa_phys = virt_to_phys(x->dcbaa);
    uint64_t cring_phys = virt_to_phys(x->cmd_ring);
    uint64_t ering_phys = virt_to_phys(x->event_ring);

    if (!dcbaa_phys || !cring_phys || !ering_phys) {
        LOG_ERROR("virt_to_phys returned 0!");
        return -1;
    }

    uint64_t evp = ering_phys;
    uint64_t erst_phys = virt_to_phys(x->erst);
    xhci_mem[idx].erst[0].base = evp;
    xhci_mem[idx].erst[0].size = TRB_RING_SIZE;

    if (max_scratch > 0) {
        LOG_DEBUG("Scratchpad buffers required 0x%x", max_scratch);

        uint32_t sp_arr_bytes = max_scratch * (uint32_t)sizeof(uint64_t);
        uint32_t sp_arr_pages = (sp_arr_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        if (sp_arr_pages < 1) sp_arr_pages = 1;

        uint64_t sp_arr_phys = pmm_alloc_pages(sp_arr_pages);
        if (!sp_arr_phys) { LOG_ERROR("scratchpad array alloc FAILED, code 0x%x", max_scratch); return -1; }

        uint64_t *sp_arr = (uint64_t *)mm_phys_to_virt(sp_arr_phys);
        memset(sp_arr, 0, (size_t)sp_arr_pages * PAGE_SIZE);

        for (uint32_t s = 0; s < max_scratch; s++) {
            uint64_t buf_phys = pmm_alloc_pages(1);
            if (!buf_phys) { LOG_ERROR("scratchpad buffer alloc FAILED, code 0x%x", s); return -1; }
            void *buf_virt = (void *)mm_phys_to_virt(buf_phys);
            memset(buf_virt, 0, PAGE_SIZE);
            sp_arr[s] = buf_phys;
        }

        for (size_t i = 0; i < (size_t)sp_arr_pages * PAGE_SIZE; i += 64)
            CACHE_FLUSH((uint8_t *)sp_arr + i);
        FULL_BARRIER();

        xhci_mem[idx].dcbaa[0] = sp_arr_phys;
        CACHE_FLUSH(&xhci_mem[idx].dcbaa[0]);
        FULL_BARRIER();
    } else {
        LOG_DEBUG("No scratchpad buffers required");
    }

    wr32(x->op_base, XHCI_OP_CONFIG, x->max_slots);
    wr64(x->op_base, XHCI_OP_DCBAAP, dcbaa_phys);
    wr64(x->op_base, XHCI_OP_CRCR, cring_phys | 1);

    wr32(x->rt_base, 0x20, 0x00000003);
    wr32(x->rt_base, 0x24, 0x00000000);
    wr32(x->rt_base, 0x28, 1);
    wr64(x->rt_base, 0x38, ERDP_WITH_EHB(evp));
    wr64(x->rt_base, 0x30, erst_phys);

    CACHE_FLUSH(x->erst);
    for (size_t i = 0; i < TRB_RING_SIZE; i++) {
        CACHE_FLUSH(&x->event_ring[i]);
        CACHE_FLUSH(&x->cmd_ring[i]);
    }
    for (int i = 0; i < MAX_SLOTS + 1; i++) CACHE_FLUSH(&x->dcbaa[i]);
    FULL_BARRIER();
    delay_ms(10);

    wr32(x->op_base, XHCI_OP_USBCMD, 0x05);
    (void)rd32(x->op_base, XHCI_OP_USBCMD);
    FULL_BARRIER();
    delay_ms(100);

    uint32_t sts = rd32(x->op_base, XHCI_OP_USBSTS);
    if (sts & 1) { LOG_ERROR("HC HALTED after run!, code 0x%x", sts); return -1; }

    for (uint32_t p = 1; p <= maxp; p++) {
        uint32_t ps = rd32(x->op_base, XHCI_OP_PORTSC(p));
        if (!(ps & XHCI_PORTSC_PP)) {

            wr32(x->op_base, XHCI_OP_PORTSC(p),
                 xhci_portsc_neutral(ps) | XHCI_PORTSC_PP);
            delay_ms(20);
        }
    }
    delay_ms(200);

    if (rd32(x->op_base, XHCI_OP_USBSTS) & 1) {
        LOG_ERROR("HC HALTED after port power!");
        return -1;
    }

    x->initialized = 1;
    LOG_DEBUG("controller %d init OK", idx);
    return 0;
}

static int xhci_reset_endpoint(struct xhci_controller *x, uint8_t slot_id, uint8_t dci) {
    struct xhci_trb r = {0};
    r.control = (TRB_TYPE_RESET_ENDPOINT << 10)
    | ((uint32_t)slot_id << 24)
    | ((uint32_t)dci << 16);
    x->last_completion_code = 0xFF;
    xhci_send_command(x, &r);
    xhci_wait_command(x, 15);
    return (x->last_completion_code == 1) ? 0 : -1;
}

static int xhci_set_tr_dequeue(struct xhci_controller *x, uint8_t slot_id, uint8_t dci, uint64_t ring_base)
{
    struct xhci_trb sd = {0};
    sd.param = ring_base | 1ULL;
    sd.control = (TRB_TYPE_SET_TR_DEQUEUE << 10)
    | ((uint32_t)slot_id << 24)
    | ((uint32_t)dci << 16);
    x->last_completion_code = 0xFF;
    xhci_send_command(x, &sd);
    xhci_wait_command(x, 15);

    if (x->last_completion_code == 19u ) {
        struct xhci_trb stop = {0};
        stop.control = (TRB_TYPE_STOP_ENDPOINT << 10)
        | ((uint32_t)slot_id << 24)
        | ((uint32_t)dci << 16);
        x->last_completion_code = 0xFF;
        xhci_send_command(x, &stop);
        xhci_wait_command(x, 50);

        struct xhci_trb sd2 = {0};
        sd2.param = ring_base | 1ULL;
        sd2.control = (TRB_TYPE_SET_TR_DEQUEUE << 10)
        | ((uint32_t)slot_id << 24)
        | ((uint32_t)dci << 16);
        x->last_completion_code = 0xFF;
        xhci_send_command(x, &sd2);
        xhci_wait_command(x, 15);
    }

    return (x->last_completion_code == 1 || x->last_completion_code == 25) ? 0 : -1;
}

static void xhci_control_ep0_recover(struct xhci_controller *x, uint8_t slot_id, int idx, struct xhci_trb *tr)
{
    if (!x || slot_id == 0) return;

    xhci_reset_endpoint(x, slot_id, 1);

    uint64_t ring_base = virt_to_phys(tr);
    xhci_set_tr_dequeue(x, slot_id, 1, ring_base);

    ctrl_tr_idx[idx][slot_id - 1] = 0;
    ctrl_tr_cycle[idx][slot_id - 1] = 1;
}

static void xhci_reset_bulk_toggle_locked(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint)
{
    if (!x || !x->initialized || slot_id == 0) return;

    int ci = (int)(x - ctrls);
    int ki = (int)slot_id - 1;
    if (ci < 0 || ci >= MAX_XHCI_CONTROLLERS) return;
    if (ki < 0 || ki >= MAX_SLOTS) return;

    uint8_t ep_num = endpoint & 0x0Fu;
    uint8_t is_in = (endpoint & 0x80u) ? 1u : 0u;
    uint8_t dci = (uint8_t)((ep_num * 2u) + is_in);
    if (dci == 0 || dci > 31) return;

    uint8_t dir = is_in;

    if (!bulk_ep_configured[ci][ki][dir]) return;

    uint64_t ring_base = virt_to_phys(xhci_mem[ci].bulk_rings[ki][dir]);

    {
        uint32_t ctx_size = x->csz ? (uint32_t)x->csz : XHCI_CTX_SIZE_32;
        uint8_t *dctx = (uint8_t *)xhci_mem[ci].dev_ctx[ki];
        struct xhci_endpoint_context *ep_ctx =
            (struct xhci_endpoint_context *)xhci_dev_ctx_ep(dctx, ctx_size, dci);

        for (size_t _fi = 0; _fi < ctx_size; _fi += 64)
            CACHE_FLUSH((uint8_t *)ep_ctx + _fi);

        FULL_BARRIER();
        LOAD_BARRIER();
        uint8_t ep_state = ep_ctx->dw0 & 0x7u;

        if (ep_state == 3u || ep_state == 2u ) {

            int _rr = xhci_reset_endpoint(x, slot_id, dci);
            if (_rr != 0 && ep_state == 2u) {

                struct xhci_trb stop = {0};
                stop.control = (TRB_TYPE_STOP_ENDPOINT << 10)
                | ((uint32_t)slot_id << 24)
                | ((uint32_t)dci << 16);
                x->last_completion_code = 0xFF;
                xhci_send_command(x, &stop);
                xhci_wait_command(x, 50);

                for (size_t _fi2 = 0; _fi2 < ctx_size; _fi2 += 64)
                    CACHE_FLUSH((uint8_t *)ep_ctx + _fi2);
                FULL_BARRIER();
            }

        } else if (bulk_active[ci][ki][dir]) {

            struct xhci_trb stop = {0};
            stop.control = (TRB_TYPE_STOP_ENDPOINT << 10)
            | ((uint32_t)slot_id << 24)
            | ((uint32_t)dci << 16);
            x->last_completion_code = 0xFF;
            xhci_send_command(x, &stop);
            xhci_wait_command(x, 50);

            xhci_poll_event_ring(x);

            for (size_t _fi2 = 0; _fi2 < ctx_size; _fi2 += 64)
                CACHE_FLUSH((uint8_t *)ep_ctx + _fi2);
            FULL_BARRIER();
        }

    }

    {
        struct xhci_trb *tr = xhci_mem[ci].bulk_rings[ki][dir];
        memset(tr, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);
        tr[TRB_RING_SIZE - 1].param = virt_to_phys(tr);
        tr[TRB_RING_SIZE - 1].status = 0;
        tr[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | (1u << 1) | 1u;
        for (int _i = 0; _i < TRB_RING_SIZE; _i++) CACHE_FLUSH(&tr[_i]);
        FULL_BARRIER();
    }

    xhci_set_tr_dequeue(x, slot_id, dci, ring_base);

    bulk_tr_idx[ci][ki][dir] = 0u;
    bulk_cycle [ci][ki][dir] = 1u;
    bulk_active[ci][ki][dir] = 0u;
    bulk_error [ci][ki][dir] = 0u;
    bulk_submitted[ci][ki][dir] = 0u;
    bulk_done [ci][ki][dir] = 0u;
}

static void xhci_reset_bulk_toggle(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint)
{
    if (!x) return;
    xhci_ctrl_enter(x);
    xhci_reset_bulk_toggle_locked(x, slot_id, endpoint);
    xhci_ctrl_leave(x);
}

static int xhci_interrupt_transfer_impl(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!x || !x->initialized || slot_id == 0 || slot_id > x->max_slots) {
        LOG_ERROR("interrupt transfer: bad controller or slot %u", (unsigned)slot_id);
        return -1;
    }

    int ci = x - ctrls;
    int ki = slot_id - 1;

    uint8_t ep_num = endpoint & 0x0F;
    uint8_t is_in = (endpoint & 0x80) ? 1 : 0;
    uint8_t dci = (ep_num * 2) + is_in;
    if (dci == 0 || dci > 31) { LOG_ERROR("bad dci %u", (unsigned)dci); return -1; }
    (void)direction;

    struct xhci_trb *tr_ring = xhci_mem[ci].intr_rings[ki];
    uint64_t ring_base = virt_to_phys(tr_ring);
    uint64_t buf_phys = virt_to_phys(data);

    if (intr_submitted[ci][ki]) {

        if (!intr_done[ci][ki] && !intr_error[ci][ki])
            xhci_poll_event_ring(x);

        if (intr_error[ci][ki]) {
            intr_error[ci][ki] = 0;
            intr_done[ci][ki] = 0;
            intr_active[ci][ki] = 0;
            intr_submitted[ci][ki] = 0;
            ep_needs_reset[ci][ki] = 1;
            return -1;
        }

        if (!intr_done[ci][ki]) {
            uint32_t s = rd32(x->op_base, XHCI_OP_USBSTS);
            if (s & (1 << 3)) wr32(x->op_base, XHCI_OP_USBSTS, (1 << 3));
            wr32(x->rt_base, 0x20, rd32(x->rt_base, 0x20) | 3u);
            FULL_BARRIER();
            return -2;
        }

        intr_done[ci][ki] = 0;
        intr_active[ci][ki] = 0;
        intr_submitted[ci][ki] = 0;

        if (is_in) {
            for (uint64_t ca = (uint64_t)data;
                 ca < (uint64_t)data + data_len; ca += 64)
                 CACHE_FLUSH((void *)ca);
            FULL_BARRIER();
        }

        intr_data_ptr[ci][ki] = NULL;
        intr_data_len[ci][ki] = 0;
        return 0;
    }

    if (ep_needs_reset[ci][ki]) {

        {
            uint32_t ctx_size = x->csz ? (uint32_t)x->csz : XHCI_CTX_SIZE_32;
            uint8_t *_dctx = (uint8_t *)xhci_mem[ci].dev_ctx[ki];
            struct xhci_endpoint_context *_ep_ctx =
                (struct xhci_endpoint_context *)xhci_dev_ctx_ep(_dctx, ctx_size, dci);
            for (size_t _fi = 0; _fi < ctx_size; _fi += 64)
                CACHE_FLUSH((uint8_t *)_ep_ctx + _fi);
            FULL_BARRIER();
            if ((_ep_ctx->dw0 & 0x7u) == 3u )
                xhci_reset_endpoint(x, slot_id, dci);
        }
        xhci_set_tr_dequeue(x, slot_id, dci, ring_base);
        ep_needs_reset[ci][ki] = 0;
    }

    if (!ep_configured[ci][ki]) {
        uint8_t ep_type = is_in ? EP_TYPE_INTR_IN : EP_TYPE_INTR_OUT;
        uint16_t ep_mps = 8;
        uint8_t ep_interval = 4;
        for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
            struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
            if (_d && (uint8_t)_d->address == slot_id) {
                if (_d->device_class == 0x09 && _d->hub_status_ep == endpoint) {

                    if (_d->hub_status_ep_mps) ep_mps = _d->hub_status_ep_mps;
                    if (_d->hub_status_interval) ep_interval = _d->hub_status_interval;
                } else {
                    if (_d->max_packet_size) ep_mps = _d->max_packet_size;
                    if (_d->hid_interval) ep_interval = _d->hid_interval;
                }
                break;
            }
        }
        if (ep_interval < 1) ep_interval = 1;
        if (ep_interval > 16) ep_interval = 16;
        if (xhci_configure_endpoint(x, slot_id, dci, ep_type, ep_mps, ep_interval) != 0) {
            LOG_ERROR("Configure EP failed");
            return -1;
        }

        xhci_poll_event_ring(x);

        uint32_t s2 = rd32(x->op_base, XHCI_OP_USBSTS);
        if (s2 & (1 << 3)) wr32(x->op_base, XHCI_OP_USBSTS, (1 << 3));
        wr32(x->rt_base, 0x20, rd32(x->rt_base, 0x20) | 3u);
        FULL_BARRIER();
    }

    volatile uint8_t *dd = (volatile uint8_t *)data;
    for (int i = 0; i < (int)data_len; i++) dd[i] = 0;
    FULL_BARRIER();
    for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
        CACHE_FLUSH((void *)a);
    FULL_BARRIER();

    uint32_t cur_idx = intr_tr_idx[ci][ki];
    uint8_t cur_cycle = intr_cycle[ci][ki];

    memset(&tr_ring[cur_idx], 0, sizeof(struct xhci_trb));
    CACHE_FLUSH(&tr_ring[cur_idx]);
    FULL_BARRIER();

    tr_ring[cur_idx].param = buf_phys;
    tr_ring[cur_idx].status = data_len;
    COMPILER_BARRIER();

    tr_ring[cur_idx].control = (uint32_t)cur_cycle
    | (1u << 2)
    | (1u << 5)
    | (TRB_TYPE_NORMAL << 10);
    STORE_BARRIER();
    CACHE_FLUSH(&tr_ring[cur_idx]);
    FULL_BARRIER();

    uint32_t nxt = cur_idx + 1;
    if (nxt >= (uint32_t)(TRB_RING_SIZE - 1)) {
        tr_ring[TRB_RING_SIZE - 1].control =
        (TRB_TYPE_LINK << 10) | (1u << 1) | (uint32_t)cur_cycle;
        CACHE_FLUSH(&tr_ring[TRB_RING_SIZE - 1]);
        FULL_BARRIER();
        intr_tr_idx[ci][ki] = 0;
        intr_cycle[ci][ki] = cur_cycle ^ 1;
    } else {
        intr_tr_idx[ci][ki] = nxt;
    }

    ep_configured[ci][ki] = 1;
    intr_done[ci][ki] = 0;
    intr_error[ci][ki] = 0;
    intr_slot_dci[ci][ki] = dci;
    intr_data_ptr[ci][ki] = data;
    intr_data_len[ci][ki] = data_len;
    intr_active[ci][ki] = 1;
    intr_submitted[ci][ki] = 1;

    FULL_BARRIER();
    wr32(x->db_base, (uint32_t)slot_id * 4, (uint32_t)dci);
    (void)rd32(x->db_base, (uint32_t)slot_id * 4);
    FULL_BARRIER();

    return -2;
}

static int xhci_interrupt_transfer(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int ret = xhci_interrupt_transfer_impl(x, slot_id, endpoint, data, data_len, direction);
    xhci_ctrl_leave(x);
    return ret;
}

static int xhci_bulk_transfer_impl(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!x || !x->initialized || slot_id == 0 || slot_id > x->max_slots)
        return -1;

    int ci = x - ctrls;
    int ki = slot_id - 1;

    uint8_t ep_num = endpoint & 0x0Fu;
    uint8_t is_in = (endpoint & 0x80u) ? 1u : 0u;
    uint8_t dci = (ep_num * 2u) + is_in;
    if (dci == 0 || dci > 31) return -1;
    uint8_t dir = is_in;
    (void)direction;

    struct xhci_trb *tr_ring = xhci_mem[ci].bulk_rings[ki][dir];
    uint64_t ring_base = virt_to_phys(tr_ring);
    uint64_t buf_phys = virt_to_phys(data);
    (void)ring_base;

    if (bulk_submitted[ci][ki][dir]) {

        for (int _sp = 0; _sp < XHCI_BULK_SPIN_POLLS; _sp++) {
            if (bulk_done[ci][ki][dir] || bulk_error[ci][ki][dir]) break;
            xhci_poll_event_ring(x);
            if (bulk_done[ci][ki][dir] || bulk_error[ci][ki][dir]) break;
            asm volatile("pause");
        }

        if (bulk_error[ci][ki][dir]) {
            bulk_error[ci][ki][dir] = 0;
            bulk_active[ci][ki][dir] = 0;
            bulk_submitted[ci][ki][dir] = 0;
            bulk_done[ci][ki][dir] = 0;
            xhci_reset_bulk_toggle_locked(x, slot_id, endpoint);
            return -1;
        }

        if (!bulk_done[ci][ki][dir]) {
            uint32_t s = rd32(x->op_base, XHCI_OP_USBSTS);
            if (s & (1u << 3)) wr32(x->op_base, XHCI_OP_USBSTS, (1u << 3));
            wr32(x->rt_base, 0x20, rd32(x->rt_base, 0x20) | 3u);
            FULL_BARRIER();
            return -2;
        }

        bulk_done[ci][ki][dir] = 0;
        bulk_active[ci][ki][dir] = 0;
        bulk_submitted[ci][ki][dir] = 0;

        if (is_in) {

            for (uint64_t ca = (uint64_t)data;
                 ca < (uint64_t)data + data_len; ca += 64)
                 CACHE_FLUSH((void *)ca);
            FULL_BARRIER();
            LOAD_BARRIER();
        }

        intr_data_ptr[ci][ki] = NULL;
        intr_data_len[ci][ki] = 0;
        return 0;
    }

    if (bulk_error[ci][ki][dir]) {
        bulk_error[ci][ki][dir] = 0;
        bulk_active[ci][ki][dir] = 0;
        bulk_done[ci][ki][dir] = 0;
        xhci_reset_bulk_toggle_locked(x, slot_id, endpoint);
        return -1;
    }

    if (!bulk_ep_configured[ci][ki][dir]) {

        uint16_t mps_out = 512u, mps_in = 512u;
        uint8_t ep_out_addr = 0, ep_in_addr = 0;
        for (int _di = 0; _di < MAX_USB_DEVICES; _di++) {
            struct usb_device *_d = (struct usb_device *)device_table[USB_DEVICE][_di];
            if (!_d || (uint8_t)_d->address != slot_id) continue;
            for (int _bi = 0; _bi < _d->bulk_ep_count; _bi++) {
                uint8_t ba = _d->bulk_ep[_bi].address;
                uint16_t bm = _d->bulk_ep[_bi].max_packet_size ? _d->bulk_ep[_bi].max_packet_size : 512u;
                if (ba & 0x80u) { mps_in = bm; ep_in_addr = ba; }
                else { mps_out = bm; ep_out_addr = ba; }
            }
            break;
        }

        uint8_t ep_out_num = ep_out_addr & 0x0Fu;
        uint8_t ep_in_num = ep_in_addr & 0x0Fu;
        uint8_t dci_out = ep_out_num ? (uint8_t)(ep_out_num * 2u) : (uint8_t)((ep_num * 2u));
        uint8_t dci_in = ep_in_num ? (uint8_t)(ep_in_num * 2u + 1u) : (uint8_t)((ep_num * 2u + 1u));

        if (!ep_out_addr) { dci_out = (ep_num * 2u); ep_out_addr = (uint8_t)(ep_num); }
        if (!ep_in_addr) { dci_in = (ep_num * 2u + 1u); ep_in_addr = (uint8_t)(ep_num | 0x80u); }

        struct xhci_trb *tr_out = xhci_mem[ci].bulk_rings[ki][0];
        memset(tr_out, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);
        tr_out[TRB_RING_SIZE-1].param = virt_to_phys(tr_out);
        tr_out[TRB_RING_SIZE-1].status = 0;
        tr_out[TRB_RING_SIZE-1].control = (TRB_TYPE_LINK << 10) | (1u << 1) | 1u;
        for (int _i = 0; _i < TRB_RING_SIZE; _i++) CACHE_FLUSH(&tr_out[_i]);
        FULL_BARRIER();

        struct xhci_trb *tr_in = xhci_mem[ci].bulk_rings[ki][1];
        memset(tr_in, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);
        tr_in[TRB_RING_SIZE-1].param = virt_to_phys(tr_in);
        tr_in[TRB_RING_SIZE-1].status = 0;
        tr_in[TRB_RING_SIZE-1].control = (TRB_TYPE_LINK << 10) | (1u << 1) | 1u;
        for (int _i = 0; _i < TRB_RING_SIZE; _i++) CACHE_FLUSH(&tr_in[_i]);
        FULL_BARRIER();

        uint32_t ctx_size = x->csz ? (uint32_t)x->csz : XHCI_CTX_SIZE_32;
        uint8_t *ictx = (uint8_t *)xhci_mem[ci].input_ctx[ki];
        uint8_t *dctx = (uint8_t *)xhci_mem[ci].dev_ctx[ki];

        for (size_t _fi = 0; _fi < XHCI_DEV_CTX_BYTES; _fi += 64)
            CACHE_FLUSH(dctx + _fi);
        FULL_BARRIER();

        struct xhci_slot_context *dev_slot = (struct xhci_slot_context *)xhci_dev_ctx_slot(dctx);

        {
            memset(ictx, 0, XHCI_INPUT_CTX_BYTES);
            uint32_t *ictrl = (uint32_t *)xhci_input_ctx_ctrl(ictx);
            ictrl[0] = 0;
            ictrl[1] = (1u << 0) | (1u << dci_in);

            struct xhci_slot_context *in_slot =
                (struct xhci_slot_context *)xhci_input_ctx_slot(ictx, ctx_size);
            in_slot->dw0 = dev_slot->dw0;
            in_slot->dw1 = dev_slot->dw1;
            in_slot->dw2 = dev_slot->dw2;
            in_slot->dw3 = dev_slot->dw3;
            uint8_t _max_dci_in = (dci_in > dci_out) ? dci_in : dci_out;
            in_slot->dw0 = (in_slot->dw0 & ~(0x1Fu << 27))
            | ((uint32_t)_max_dci_in << 27);

            struct xhci_endpoint_context *in_ep_in =
                (struct xhci_endpoint_context *)xhci_input_ctx_ep(ictx, ctx_size, dci_in);
            in_ep_in->dw0 = 0;
            in_ep_in->dw1 = ((uint32_t)mps_in << 16)
            | ((uint32_t)EP_TYPE_BULK_IN << 3)
            | (3u << 1);
            in_ep_in->tr_dequeue_ptr = virt_to_phys(tr_in) | 1u;
            in_ep_in->dw4 = (uint32_t)mps_in;
            for (size_t _fi = 0; _fi < XHCI_INPUT_CTX_BYTES; _fi += 64)
                CACHE_FLUSH(ictx + _fi);
            FULL_BARRIER();

            struct xhci_trb _cmd_in = {0};
            _cmd_in.param = virt_to_phys(ictx);
            _cmd_in.control = ((uint32_t)slot_id << 24) | (TRB_TYPE_CONFIG_EP << 10);
            x->last_completion_code = 0xFF;
            if (xhci_send_command(x, &_cmd_in) != 0) return -1;
            {
                xhci_waiter_t _w;
                xhci_waiter_init(&_w);
                for (;;) {
                    xhci_poll_event_ring(x);
                    if (x->last_completion_code != 0xFF) break;
                    if (xhci_waiter_expired(&_w, XHCI_CMD_TIMEOUT_MS)) break;
                    xhci_waiter_backoff(&_w);
                }
            }
            if (x->last_completion_code != 1) {
                g_last_xfer_error_code = x->last_completion_code;
                g_last_usbsts = rd32(x->op_base, XHCI_OP_USBSTS);
                return -1;
            }
        }

        {
            memset(ictx, 0, XHCI_INPUT_CTX_BYTES);
            uint32_t *ictrl = (uint32_t *)xhci_input_ctx_ctrl(ictx);
            ictrl[0] = 0;
            ictrl[1] = (1u << 0) | (1u << dci_out);

            struct xhci_slot_context *in_slot =
                (struct xhci_slot_context *)xhci_input_ctx_slot(ictx, ctx_size);
            in_slot->dw0 = dev_slot->dw0;
            in_slot->dw1 = dev_slot->dw1;
            in_slot->dw2 = dev_slot->dw2;
            in_slot->dw3 = dev_slot->dw3;
            uint8_t _max_dci_out = (dci_out > dci_in) ? dci_out : dci_in;
            in_slot->dw0 = (in_slot->dw0 & ~(0x1Fu << 27))
            | ((uint32_t)_max_dci_out << 27);

            struct xhci_endpoint_context *in_ep_out =
                (struct xhci_endpoint_context *)xhci_input_ctx_ep(ictx, ctx_size, dci_out);
            in_ep_out->dw0 = 0;
            in_ep_out->dw1 = ((uint32_t)mps_out << 16)
            | ((uint32_t)EP_TYPE_BULK_OUT << 3)
            | (3u << 1);
            in_ep_out->tr_dequeue_ptr = virt_to_phys(tr_out) | 1u;
            in_ep_out->dw4 = (uint32_t)mps_out;
            for (size_t _fi = 0; _fi < XHCI_INPUT_CTX_BYTES; _fi += 64)
                CACHE_FLUSH(ictx + _fi);
            FULL_BARRIER();

            struct xhci_trb _cmd_out = {0};
            _cmd_out.param = virt_to_phys(ictx);
            _cmd_out.control = ((uint32_t)slot_id << 24) | (TRB_TYPE_CONFIG_EP << 10);
            x->last_completion_code = 0xFF;
            if (xhci_send_command(x, &_cmd_out) != 0) return -1;
            {
                xhci_waiter_t _w;
                xhci_waiter_init(&_w);
                for (;;) {
                    xhci_poll_event_ring(x);
                    if (x->last_completion_code != 0xFF) break;
                    if (xhci_waiter_expired(&_w, XHCI_CMD_TIMEOUT_MS)) break;
                    xhci_waiter_backoff(&_w);
                }
            }
            if (x->last_completion_code != 1) {
                g_last_xfer_error_code = x->last_completion_code;
                g_last_usbsts = rd32(x->op_base, XHCI_OP_USBSTS);
                return -1;
            }
        }

        xhci_poll_event_ring(x);
        uint32_t s = rd32(x->op_base, XHCI_OP_USBSTS);
        if (s & (1u << 3)) wr32(x->op_base, XHCI_OP_USBSTS, (1u << 3));
        wr32(x->rt_base, 0x20, rd32(x->rt_base, 0x20) | 3u);
        FULL_BARRIER();

        delay_ms(10);

        bulk_tr_idx[ci][ki][0] = 0; bulk_cycle[ci][ki][0] = 1;
        bulk_tr_idx[ci][ki][1] = 0; bulk_cycle[ci][ki][1] = 1;
        bulk_error [ci][ki][0] = 0; bulk_error [ci][ki][1] = 0;
        bulk_active[ci][ki][0] = 0; bulk_active[ci][ki][1] = 0;
        bulk_submitted[ci][ki][0] = 0; bulk_submitted[ci][ki][1] = 0;
        bulk_done [ci][ki][0] = 0; bulk_done [ci][ki][1] = 0;
        bulk_ep_configured[ci][ki][0] = 1; bulk_slot_dci[ci][ki][0] = dci_out;
        bulk_ep_configured[ci][ki][1] = 1; bulk_slot_dci[ci][ki][1] = dci_in;

        tr_ring = xhci_mem[ci].bulk_rings[ki][dir];
        ring_base = virt_to_phys(tr_ring);
        buf_phys = virt_to_phys(data);
    }

    volatile uint8_t *dd = (volatile uint8_t *)data;
    if (is_in) { for (int i = 0; i < (int)data_len; i++) dd[i] = 0; }
    FULL_BARRIER();
    for (uint64_t a = (uint64_t)data; a < (uint64_t)data + data_len; a += 64)
        CACHE_FLUSH((void *)a);
    FULL_BARRIER();

    uint32_t cur_idx = bulk_tr_idx[ci][ki][dir];
    uint8_t cur_cycle = bulk_cycle [ci][ki][dir];

    memset(&tr_ring[cur_idx], 0, sizeof(struct xhci_trb));
    CACHE_FLUSH(&tr_ring[cur_idx]);
    FULL_BARRIER();

    tr_ring[cur_idx].param = buf_phys;
    tr_ring[cur_idx].status = data_len;
    COMPILER_BARRIER();

    tr_ring[cur_idx].control = (uint32_t)cur_cycle
    | (1u << 2)
    | (1u << 5)
    | (TRB_TYPE_NORMAL << 10);
    STORE_BARRIER();
    CACHE_FLUSH(&tr_ring[cur_idx]);
    FULL_BARRIER();

    uint32_t nxt = cur_idx + 1u;
    if (nxt >= (uint32_t)(TRB_RING_SIZE - 1)) {
        tr_ring[TRB_RING_SIZE - 1].control =
        (TRB_TYPE_LINK << 10) | (1u << 1) | (uint32_t)cur_cycle;
        CACHE_FLUSH(&tr_ring[TRB_RING_SIZE - 1]);
        FULL_BARRIER();
        bulk_tr_idx[ci][ki][dir] = 0;
        bulk_cycle [ci][ki][dir] = cur_cycle ^ 1u;
    } else {
        bulk_tr_idx[ci][ki][dir] = nxt;
    }

    uint32_t actual_dci = bulk_slot_dci[ci][ki][dir];
    if (actual_dci == 0) actual_dci = (uint32_t)dci;

    bulk_done[ci][ki][dir] = 0;
    bulk_error[ci][ki][dir] = 0;
    bulk_active[ci][ki][dir] = 1;
    bulk_submitted[ci][ki][dir] = 1;

    FULL_BARRIER();
    wr32(x->db_base, (uint32_t)slot_id * 4u, actual_dci);
    (void)rd32(x->db_base, (uint32_t)slot_id * 4u);

    FULL_BARRIER();

    for (int _sp = 0; _sp < XHCI_BULK_SPIN_POLLS; _sp++) {
        if (bulk_done[ci][ki][dir] || bulk_error[ci][ki][dir]) break;
        xhci_poll_event_ring(x);
        if (bulk_done[ci][ki][dir] || bulk_error[ci][ki][dir]) break;
        asm volatile("pause");
    }

    if (bulk_error[ci][ki][dir]) {
        bulk_error[ci][ki][dir] = 0;
        bulk_active[ci][ki][dir] = 0;
        bulk_submitted[ci][ki][dir] = 0;
        bulk_done[ci][ki][dir] = 0;
        xhci_reset_bulk_toggle_locked(x, slot_id, endpoint);
        return -1;
    }

    if (bulk_done[ci][ki][dir]) {
        bulk_done[ci][ki][dir] = 0;
        bulk_active[ci][ki][dir] = 0;
        bulk_submitted[ci][ki][dir] = 0;

        if (is_in) {
            for (uint64_t ca = (uint64_t)data;
                 ca < (uint64_t)data + data_len; ca += 64)
                 CACHE_FLUSH((void *)ca);
            FULL_BARRIER();
            LOAD_BARRIER();
        }

        intr_data_ptr[ci][ki] = NULL;
        intr_data_len[ci][ki] = 0;
        return 0;
    }

    return -2;
}

static int xhci_bulk_transfer(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *data, uint16_t data_len, uint8_t direction)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int ret = xhci_bulk_transfer_impl(x, slot_id, endpoint, data, data_len, direction);
    xhci_ctrl_leave(x);
    return ret;
}

static int xhci_iso_transfer_impl(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *data, uint16_t total_len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction)
{
    if (!x || !x->initialized || slot_id == 0 || slot_id > x->max_slots)
        return -1;
    if (!n_frames || n_frames > XHCI_ISO_MAX_FRAMES) return -1;

    int ci = x - ctrls;
    int ki = slot_id - 1;

    uint8_t ep_num = endpoint & 0x0Fu;
    uint8_t is_in = (endpoint & 0x80u) ? 1u : 0u;
    uint8_t dci = (ep_num * 2u) + is_in;
    if (dci == 0 || dci > 31) return -1;
    (void)direction;

    if (xhci_iso_ctx[ci][ki].active) {
        xhci_poll_event_ring(x);
        if (xhci_iso_ctx[ci][ki].active) return -2;
    }

    struct xhci_trb *tr_ring = xhci_mem[ci].iso_rings[ki];

    if (!iso_ep_configured[ci][ki]) {
        uint8_t ep_type = is_in ? EP_TYPE_ISO_IN : EP_TYPE_ISO_OUT;

        uint16_t mps = 1024u;

        if (xhci_configure_endpoint(x, slot_id, dci, ep_type, mps, 3) != 0)
            return -1;
        xhci_poll_event_ring(x);
        uint32_t s = rd32(x->op_base, XHCI_OP_USBSTS);
        if (s & (1u << 3)) wr32(x->op_base, XHCI_OP_USBSTS, (1u << 3));
        wr32(x->rt_base, 0x20, rd32(x->rt_base, 0x20) | 3u);
        FULL_BARRIER();
        iso_ep_configured[ci][ki] = 1;
        iso_slot_dci[ci][ki] = dci;
    }

    uint16_t per_frame = (n_frames > 0) ? (total_len / n_frames) : total_len;
    uint16_t offset = 0;
    for (uint8_t i = 0; i < n_frames; i++) {
        xhci_iso_ctx[ci][ki].frame_lens[i] = frame_lens ? frame_lens[i] : per_frame;
        xhci_iso_ctx[ci][ki].frame_offsets[i] = offset;
        offset = (uint16_t)(offset + xhci_iso_ctx[ci][ki].frame_lens[i]);
    }

    for (uint64_t a = (uint64_t)data; a < (uint64_t)data + total_len; a += 64)
        CACHE_FLUSH((void *)a);
    FULL_BARRIER();

    uint32_t cur_idx = iso_tr_idx[ci][ki];
    uint8_t cur_cycle = iso_cycle[ci][ki];

    for (uint8_t fi = 0; fi < n_frames; fi++) {
        uint16_t flen = xhci_iso_ctx[ci][ki].frame_lens[fi];
        uint16_t foffset = xhci_iso_ctx[ci][ki].frame_offsets[fi];
        uint64_t buf_phys = virt_to_phys((uint8_t *)data + foffset);

        memset(&tr_ring[cur_idx], 0, sizeof(struct xhci_trb));
        CACHE_FLUSH(&tr_ring[cur_idx]);
        FULL_BARRIER();

        tr_ring[cur_idx].param = buf_phys;
        tr_ring[cur_idx].status = (uint32_t)flen;
        COMPILER_BARRIER();

        tr_ring[cur_idx].control = (uint32_t)cur_cycle
        | (1u << 5)
        | (TRB_TYPE_ISOCH << 10);
        STORE_BARRIER();
        CACHE_FLUSH(&tr_ring[cur_idx]);
        FULL_BARRIER();

        uint32_t nxt = cur_idx + 1u;
        if (nxt >= (uint32_t)(TRB_RING_SIZE - 1)) {
            tr_ring[TRB_RING_SIZE - 1].control =
            (TRB_TYPE_LINK << 10) | (1u << 1) | (uint32_t)cur_cycle;
            CACHE_FLUSH(&tr_ring[TRB_RING_SIZE - 1]);
            FULL_BARRIER();
            cur_idx = 0;
            cur_cycle = cur_cycle ^ 1u;
        } else {
            cur_idx = nxt;
        }
    }

    iso_tr_idx[ci][ki] = cur_idx;
    iso_cycle[ci][ki] = cur_cycle;

    FULL_BARRIER();
    wr32(x->db_base, (uint32_t)slot_id * 4u, (uint32_t)dci);
    (void)rd32(x->db_base, (uint32_t)slot_id * 4u);
    FULL_BARRIER();

    xhci_iso_ctx[ci][ki].active = 1;
    xhci_iso_ctx[ci][ki].n_frames = n_frames;
    xhci_iso_ctx[ci][ki].frames_done = 0;
    xhci_iso_ctx[ci][ki].slot_id = slot_id;
    xhci_iso_ctx[ci][ki].endpoint = endpoint;
    xhci_iso_ctx[ci][ki].data = data;
    xhci_iso_ctx[ci][ki].cookie = NULL;

    return 0;
}

static int xhci_iso_transfer(struct xhci_controller *x, uint8_t slot_id, uint8_t endpoint, void *data, uint16_t total_len, uint8_t n_frames, const uint16_t *frame_lens, uint8_t direction)
{
    if (!x) return -1;
    xhci_ctrl_enter(x);
    int ret = xhci_iso_transfer_impl(x, slot_id, endpoint, data, total_len, n_frames, frame_lens, direction);
    xhci_ctrl_leave(x);
    return ret;
}

static int get_cnt(void) { return ctrl_count; }

static struct xhci_controller *get_ctrl(int i) {
    return (i < ctrl_count) ? &ctrls[i] : NULL;
}

int xhci_get_last_error_code(void)
{
    return (int)g_last_xfer_error_code;
}

uint32_t xhci_get_last_usbsts(void)
{
    return g_last_usbsts;
}

uint32_t xhci_read_current_usbsts(void)
{
    if (ctrl_count <= 0 || !ctrls[0].initialized) return 0xFFFFFFFFu;
    return rd32(ctrls[0].op_base, XHCI_OP_USBSTS);
}

static void xhci_yield_to_host(struct xhci_controller *x)
{
    if (!x) return;
    struct tsc_driver *_tsc =
    (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (_tsc && _tsc->get_tsc_uptime_ms) {
        uint64_t _t0 = _tsc->get_tsc_uptime_ms();
        while (_tsc->get_tsc_uptime_ms() - _t0 < 2ULL) {
            (void)rd32(x->op_base, 0x04);
            (void)rd32(x->rt_base, 0x38);
            (void)rd32(x->rt_base, 0x3c);
        }
    } else {
        for (int _i = 0; _i < 200; _i++)
            (void)rd32(x->op_base, 0x04);
    }
}

struct xhci_driver xhci_driver_loaded = {
    .get_controller_count = get_cnt,
    .get_controller = get_ctrl,
    .reset_port = xhci_reset_port,
    .send_command = xhci_send_command,
    .enable_slot = xhci_enable_slot,
    .address_device = xhci_address_device,
    .evaluate_hub_slot = xhci_evaluate_hub_slot,
    .disable_slot = xhci_disable_slot,
    .interrupt_transfer = xhci_interrupt_transfer,
    .bulk_transfer = xhci_bulk_transfer,
    .iso_transfer = xhci_iso_transfer,
    .control_transfer = xhci_control_transfer,
    .poll_event_ring = xhci_poll_event_ring,
    .reset_bulk_toggle = xhci_reset_bulk_toggle,
    .yield_to_host = xhci_yield_to_host
};

static void xhci_irq_legacy_handler(struct registers *regs) {
    (void)regs;
    xhci_irq();
}

struct xhci_driver *return_xhci_driver(void) {
    pci_init();

    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);

    delay_ms = tsc->sleep_tsc_ms;
    xhci_now_us_fn = tsc->get_tsc_uptime_us;
    LOG_DEBUG("scanning PCI for xHCI controllers");

    struct usb_controller *u = pci_get_usb_controllers();
    for (int i = 0; i < 8; i++) {
        if (u[i].type != USB_TYPE_XHCI || !u[i].pci) continue;
        if (ctrl_count >= MAX_XHCI_CONTROLLERS) break;

        LOG_INFO("found xHCI controller at %02x:%02x.%x (USB slot %d)",
                 (unsigned)u[i].pci->bus, (unsigned)u[i].pci->slot, (unsigned)u[i].pci->func, i);
        pci_enable_bus_mastering(u[i].pci);
        delay_ms(10);

        struct pci_device *pdev = u[i].pci;
        uint32_t b0 = pci_read_config(pdev->bus, pdev->slot, pdev->func, 0x10);
        uint32_t b1 = pci_read_config(pdev->bus, pdev->slot, pdev->func, 0x14);

        uint64_t addr;
        if (((b0 >> 1) & 0x3) == 0x2)
            addr = ((uint64_t)b1 << 32) | (b0 & 0xFFFFFFF0);
        else
            addr = b0 & 0xFFFFFFF0;

        if (!addr) { LOG_ERROR("controller %d: BAR0 is zero, skipping", i); continue; }

        {
            uint64_t hhdm_off = hhdm_req.response ? hhdm_req.response->offset : 0;
            uint64_t pml4 = vmm_kernel_pml4();
            uint64_t mmio_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
            VMM_FLAG_GLOBAL | VMM_FLAG_PWT |
            VMM_FLAG_PCD;

            uint64_t map_size = 256 * 1024;
            uint64_t pages = (map_size + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint64_t p = 0; p < pages; p++) {
                uint64_t phys_page = (addr & ~(uint64_t)(PAGE_SIZE - 1)) + p * PAGE_SIZE;
                uint64_t virt_page = phys_page + hhdm_off;

                if (!vmm_is_mapped(pml4, virt_page)) {
                    vmm_map_page(pml4, virt_page, phys_page, mmio_flags);
                }
            }
        }

        ctrls[ctrl_count].base_addr = addr;
        if (xhci_init(&ctrls[ctrl_count], ctrl_count) != 0) {
            LOG_ERROR("controller %d at 0x%llx failed to initialize", ctrl_count, (unsigned long long)addr);
            continue;
        }
        LOG_INFO("controller %d ready: %u slots, %u ports", ctrl_count,
                 (unsigned)ctrls[ctrl_count].max_slots, (unsigned)ctrls[ctrl_count].max_ports);

        uint8_t msi_vec = (uint8_t)(MSI_VECTOR_XHCI_BASE + ctrl_count);
        uint8_t lapic = apic_get_lapic_id();

        bool msi_ok = msi_enable(pdev->bus, pdev->slot, pdev->func,
                                 msi_vec, lapic, xhci_irq);
        if (msi_ok) {
            LOG_INFO("controller %d: MSI enabled, vector 0x%02x", ctrl_count, (unsigned)msi_vec);

            uint32_t iman = rd32(ctrls[ctrl_count].rt_base, 0x20);
            wr32(ctrls[ctrl_count].rt_base, 0x20, iman | 3u);
            uint32_t cmd = rd32(ctrls[ctrl_count].op_base, XHCI_OP_USBCMD);
            wr32(ctrls[ctrl_count].op_base, XHCI_OP_USBCMD, cmd | (1u << 2));
            FULL_BARRIER();
            ctrls[ctrl_count].use_polling = false;

            if (pdev->irq_line && pdev->irq_line != 0xFF) {
                uint8_t irq_vec2 = (uint8_t)(0x20 + pdev->irq_line);
                ioapic_map_pci_irq(pdev->irq_line, irq_vec2, lapic);
                irq_register_handler(irq_vec2, xhci_irq_legacy_handler);
                LOG_DEBUG("controller %d: legacy IRQ %u fallback on vector 0x%02x", ctrl_count, (unsigned)pdev->irq_line, (unsigned)irq_vec2);
            }
        } else {
            LOG_WARNING("controller %d: no MSI, using legacy IRQ %u", ctrl_count, (unsigned)pdev->irq_line);
            uint8_t irq_vec = (uint8_t)(0x20 + pdev->irq_line);
            uint8_t lapic2 = apic_get_lapic_id();
            ioapic_map_pci_irq(pdev->irq_line, irq_vec, lapic2);
            irq_register_handler(irq_vec, xhci_irq_legacy_handler);
            ctrls[ctrl_count].use_polling = false;
        }

        if (!ctrls[ctrl_count].use_polling) {

        }

        ctrl_count++;
    }

    LOG_INFO("%d xHCI controller(s) ready", ctrl_count);
    return &xhci_driver_loaded;
}

struct driver* return_meta_xhci_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };

    static struct driver meta = {
        .name = "XHCI USB Controller Driver",
        .type = USB_DRIVER,
        .sub_type = USB_TYPE_XHCI,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &xhci_driver_loaded,
        .init = (void*)return_xhci_driver,
    };
    return &meta;
}
