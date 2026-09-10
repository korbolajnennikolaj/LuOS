#ifndef USB_EVENT_H
#define USB_EVENT_H

#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_XFER_CONTROL = 0,
    USB_XFER_INTERRUPT = 1,
    USB_XFER_BULK = 2,
    USB_XFER_ISO = 3,
} usb_transfer_type_t;

typedef enum {
    USB_EVENT_NONE = 0,
    USB_EVENT_TRANSFER_DONE = 1,
    USB_EVENT_TRANSFER_ERR = 2,
    USB_EVENT_DEVICE_CONN = 3,
    USB_EVENT_DEVICE_DISC = 4,
    USB_EVENT_PORT_CHANGE = 5,
    USB_EVENT_BULK_DONE = 6,
    USB_EVENT_BULK_ERR = 7,
    USB_EVENT_ISO_DONE = 8,
    USB_EVENT_ISO_ERR = 9,
} usb_event_type_t;

typedef enum {
    USB_SRC_XHCI = 0,
    USB_SRC_EHCI = 1,
    USB_SRC_OHCI = 2,
    USB_SRC_UHCI = 3,
} usb_event_src_t;

typedef struct {
    usb_event_type_t type;
    usb_event_src_t src;
    usb_transfer_type_t transfer_type;

    uint8_t slot_id;
    uint8_t endpoint;
    uint8_t port;
    uint8_t completion_code;

    void *device;

    void *data;
    uint16_t data_len;
    uint8_t inline_data[8];

    uint16_t iso_frame_index;
    uint16_t iso_expected_len;

    void *cookie;

    uint32_t seq;
} usb_event_t;

#define USB_EVENT_RING_SIZE 256
#define USB_EVENT_RING_MASK (USB_EVENT_RING_SIZE - 1)

#define USB_BULK_RING_SIZE 128
#define USB_BULK_RING_MASK (USB_BULK_RING_SIZE - 1)

#define USB_ISO_RING_SIZE 512
#define USB_ISO_RING_MASK (USB_ISO_RING_SIZE - 1)

typedef struct {
    usb_event_t *entries;
    uint32_t capacity;
    uint32_t mask;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t seq_counter;
    uint32_t drop_count;
    spinlock_t lock;
} usb_event_ring_t;

extern usb_event_ring_t g_usb_event_ring;
extern usb_event_ring_t g_usb_bulk_ring;
extern usb_event_ring_t g_usb_iso_ring;

void usb_event_ring_init(usb_event_ring_t *ring);
bool usb_event_enqueue (usb_event_ring_t *ring, const usb_event_t *evt);
bool usb_event_dequeue (usb_event_ring_t *ring, usb_event_t *out);
uint32_t usb_event_pending (const usb_event_ring_t *ring);

static inline bool usb_push_event (const usb_event_t *e) { return usb_event_enqueue(&g_usb_event_ring, e); }
static inline bool usb_pop_event (usb_event_t *o) { return usb_event_dequeue(&g_usb_event_ring, o); }
static inline uint32_t usb_events_pending (void) { return usb_event_pending(&g_usb_event_ring); }

static inline bool usb_push_bulk_event (const usb_event_t *e) { return usb_event_enqueue(&g_usb_bulk_ring, e); }
static inline bool usb_pop_bulk_event (usb_event_t *o) { return usb_event_dequeue(&g_usb_bulk_ring, o); }
static inline uint32_t usb_bulk_events_pending(void) { return usb_event_pending(&g_usb_bulk_ring); }

static inline bool usb_push_iso_event (const usb_event_t *e) { return usb_event_enqueue(&g_usb_iso_ring, e); }
static inline bool usb_pop_iso_event (usb_event_t *o) { return usb_event_dequeue(&g_usb_iso_ring, o); }
static inline uint32_t usb_iso_events_pending(void) { return usb_event_pending(&g_usb_iso_ring); }

typedef void (*usb_event_handler_t)(const usb_event_t *evt, void *ctx);

#define USB_EVENT_MAX_HANDLERS 8

void usb_event_register_handler(usb_event_handler_t fn, void *ctx);

void usb_event_register_handler_for_device(usb_event_handler_t fn, void *ctx, void *device, uint8_t endpoint, usb_transfer_type_t xfer_type);

void usb_bulk_register_handler(usb_event_handler_t fn, void *ctx);

void usb_bulk_register_handler_for_device(usb_event_handler_t fn, void *ctx, void *device, uint8_t endpoint);

void usb_iso_register_handler(usb_event_handler_t fn, void *ctx);

void usb_iso_register_handler_for_device(usb_event_handler_t fn, void *ctx, void *device, uint8_t endpoint);

void usb_event_unregister_for_device(void *device);

void usb_event_dispatch_all(void);
void usb_bulk_dispatch_all (void);
void usb_iso_dispatch_all (void);
void usb_all_rings_init (void);

static inline bool usb_event_verify(const usb_event_t *evt, const void *expected_device, uint8_t expected_endpoint, usb_transfer_type_t expected_xfer)
{
    if (!evt) return false;
    if (evt->type == USB_EVENT_NONE) return false;
    if (evt->transfer_type != expected_xfer) return false;
    if (expected_device && evt->device != expected_device) return false;
    if (expected_endpoint && evt->endpoint != expected_endpoint) return false;

    if (evt->cookie && expected_device &&
        evt->cookie != expected_device) return false;
    return true;
}

#endif
