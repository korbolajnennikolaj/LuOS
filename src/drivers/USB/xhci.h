#ifndef XHCI_H
#define XHCI_H
#include "kernel/scheduler/spinlock.h"

#include <stdint.h>

struct usb_setup_packet;
struct tsc_driver;

typedef struct xhci_trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb;

typedef struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) xhci_erst_entry;

typedef struct xhci_topology {
    uint8_t root_port;

    uint32_t route_string;

    uint8_t parent_hub_slot;

    uint8_t parent_port;

    uint8_t speed_id;

} xhci_topology;

typedef struct xhci_controller {
    uint8_t type;
    uint64_t base_addr;
    uint64_t cap_len;
    uint64_t op_base;
    uint64_t rt_base;
    uint64_t db_base;
    uint64_t csz;

    uint32_t max_slots;
    uint32_t max_ports;

    struct xhci_trb* cmd_ring;
    uint32_t cmd_ring_idx;
    uint8_t cmd_cycle;

    struct xhci_trb* event_ring;
    uint32_t event_ring_idx;
    uint8_t event_cycle;

    struct xhci_erst_entry* erst;
    uint64_t* dcbaa;

    volatile uint8_t last_slot_id;
    volatile uint8_t last_completion_code;
    volatile uint8_t pending_xfer_valid;
    volatile uint8_t pending_xfer_slot;
    volatile uint8_t pending_xfer_code;
    volatile uint8_t transfer_in_progress;
    uint8_t initialized;
    uint8_t use_polling;
    spinlock_t lock;
} xhci_controller;

typedef struct xhci_driver {
    int (*get_controller_count)(void);
    struct xhci_controller* (*get_controller)(int index);

    int (*send_command)(struct xhci_controller* x, struct xhci_trb* trb);
    int (*enable_slot)(struct xhci_controller* x);

    int (*disable_slot)(struct xhci_controller* x, uint8_t slot_id);
    int (*address_device)(struct xhci_controller* x, uint8_t slot_id, const struct xhci_topology* topo);

    int (*evaluate_hub_slot)(struct xhci_controller* x, uint8_t slot_id, uint8_t port_count);

    int (*control_transfer)(struct xhci_controller* x, uint8_t slot_id, uint8_t endpoint,
                            void* setup, uint16_t setup_len,
                            void* data, uint16_t data_len, uint8_t direction);

    int (*interrupt_transfer)(struct xhci_controller* x,
                             uint8_t slot_id,
                             uint8_t endpoint,
                             void* data,
                             uint16_t data_len,
                             uint8_t direction);

    int (*bulk_transfer)(struct xhci_controller* x,
                         uint8_t slot_id,
                         uint8_t endpoint,
                         void* data,
                         uint16_t data_len,
                         uint8_t direction);

    int (*iso_transfer)(struct xhci_controller* x,
                        uint8_t slot_id,
                        uint8_t endpoint,
                        void* data,
                        uint16_t total_len,
                        uint8_t n_frames,
                        const uint16_t* frame_lens,
                        uint8_t direction);

    int (*reset_port)(struct xhci_controller* x, uint8_t port);
    void (*poll_event_ring)(struct xhci_controller* x);
    void (*reset_bulk_toggle)(struct xhci_controller* x,
                               uint8_t slot_id, uint8_t endpoint);

    void (*yield_to_host)(struct xhci_controller* x);
} xhci_driver;

#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP_STAGE 2
#define TRB_TYPE_DATA_STAGE 3
#define TRB_TYPE_STATUS_STAGE 4
#define TRB_TYPE_ISOCH 5
#define TRB_TYPE_LINK 6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_DISABLE_SLOT 10
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIG_EP 12
#define TRB_TYPE_EVALUATE_CONTEXT 13
#define TRB_TYPE_STOP_ENDPOINT 15
#define TRB_TYPE_RESET_ENDPOINT 14
#define TRB_TYPE_SET_TR_DEQUEUE 16
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_COMMAND_COMPL 33

#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_CRCR 0x18
#define XHCI_OP_DCBAAP 0x30
#define XHCI_OP_CONFIG 0x38
#define XHCI_OP_PORTSC(n) (0x400 + ((n)-1) * 0x10)

#define XHCI_PORTSC_CCS (1u << 0)
#define XHCI_PORTSC_PED (1u << 1)
#define XHCI_PORTSC_OCA (1u << 3)
#define XHCI_PORTSC_PR (1u << 4)
#define XHCI_PORTSC_PLS_SHIFT 5
#define XHCI_PORTSC_PLS_MASK (0xFu << 5)
#define XHCI_PORTSC_PP (1u << 9)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK (0xFu << 10)
#define XHCI_PORTSC_PIC_MASK (0x3u << 14)
#define XHCI_PORTSC_LWS (1u << 16)
#define XHCI_PORTSC_CSC (1u << 17)
#define XHCI_PORTSC_PEC (1u << 18)
#define XHCI_PORTSC_WRC (1u << 19)
#define XHCI_PORTSC_OCC (1u << 20)
#define XHCI_PORTSC_PRC (1u << 21)
#define XHCI_PORTSC_PLC (1u << 22)
#define XHCI_PORTSC_CEC (1u << 23)
#define XHCI_PORTSC_DR (1u << 30)
#define XHCI_PORTSC_WPR (1u << 31)

#define XHCI_PORTSC_CHANGE_MASK (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                 XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
                                 XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                                 XHCI_PORTSC_CEC)

#define XHCI_PORTSC_RO (XHCI_PORTSC_CCS | XHCI_PORTSC_OCA | \
                          XHCI_PORTSC_SPEED_MASK | XHCI_PORTSC_DR)

#define XHCI_PORTSC_RWS (XHCI_PORTSC_PLS_MASK | XHCI_PORTSC_PP | \
                          XHCI_PORTSC_PIC_MASK | (0x7u << 25))

static inline uint32_t xhci_portsc_neutral(uint32_t ps) {
    return (ps & XHCI_PORTSC_RO) | (ps & XHCI_PORTSC_RWS);
}

struct xhci_driver* return_xhci_driver(void);
struct driver* return_meta_xhci_driver(void);
void xhci_irq(void);

void xhci_debug_port(const char *msg, uint32_t port, uint32_t val);

#endif
