#include "drivers/USB/usb_event.h"

#include "drivers/USB/usb_core.h"

#include <stddef.h>

static usb_event_t s_intr_entries[USB_EVENT_RING_SIZE];
static usb_event_t s_bulk_entries[USB_BULK_RING_SIZE];
static usb_event_t s_iso_entries [USB_ISO_RING_SIZE];

usb_event_ring_t g_usb_event_ring = {
    .entries = s_intr_entries,
    .capacity = USB_EVENT_RING_SIZE,
    .mask = USB_EVENT_RING_MASK,
    .head = 0,
    .tail = 0,
    .seq_counter = 0,
    .drop_count = 0,
};

usb_event_ring_t g_usb_bulk_ring = {
    .entries = s_bulk_entries,
    .capacity = USB_BULK_RING_SIZE,
    .mask = USB_BULK_RING_MASK,
    .head = 0,
    .tail = 0,
    .seq_counter = 0,
    .drop_count = 0,
};

usb_event_ring_t g_usb_iso_ring = {
    .entries = s_iso_entries,
    .capacity = USB_ISO_RING_SIZE,
    .mask = USB_ISO_RING_MASK,
    .head = 0,
    .tail = 0,
    .seq_counter = 0,
    .drop_count = 0,
};

typedef struct handler_entry {
    usb_event_handler_t fn;
    void *ctx;
    void *device;
    uint8_t endpoint;
    usb_transfer_type_t xfer_type;
    bool has_device_filter;
} handler_entry;

static struct handler_entry s_intr_handlers[USB_EVENT_MAX_HANDLERS];
static int s_intr_handler_count = 0;

static struct handler_entry s_bulk_handlers[USB_EVENT_MAX_HANDLERS];
static int s_bulk_handler_count = 0;

static struct handler_entry s_iso_handlers[USB_EVENT_MAX_HANDLERS];
static int s_iso_handler_count = 0;

static spinlock_t handlers_lock = SPINLOCK_INIT;

void usb_event_ring_init(usb_event_ring_t *ring) {
    ring->head = 0;
    ring->tail = 0;
    ring->seq_counter = 0;
    ring->drop_count = 0;
    spin_lock_init(&ring->lock);
    for (uint32_t i = 0; i < ring->capacity; i++) {
        usb_event_t *e = &ring->entries[i];
        e->type = USB_EVENT_NONE;
        e->src = USB_SRC_XHCI;
        e->transfer_type = USB_XFER_INTERRUPT;
        e->slot_id = 0;
        e->endpoint = 0;
        e->port = 0;
        e->completion_code = 0;
        e->device = (void *)0;
        e->data = (void *)0;
        e->data_len = 0;
        e->iso_frame_index = 0;
        e->iso_expected_len = 0;
        e->cookie = (void *)0;
        e->seq = 0;
        for (int j = 0; j < 8; j++) e->inline_data[j] = 0;
    }
}

void usb_all_rings_init(void) {
    usb_event_ring_init(&g_usb_event_ring);
    usb_event_ring_init(&g_usb_bulk_ring);
    usb_event_ring_init(&g_usb_iso_ring);
    s_intr_handler_count = 0;
    s_bulk_handler_count = 0;
    s_iso_handler_count = 0;
}

bool usb_event_enqueue(usb_event_ring_t *ring, const usb_event_t *evt) {
    spin_lock(&ring->lock);
    uint32_t next = (ring->head + 1) & ring->mask;
    if (next == ring->tail) {
        ring->drop_count++;
        spin_unlock(&ring->lock);
        return false;
    }
    ring->entries[ring->head] = *evt;
    ring->entries[ring->head].seq = ring->seq_counter++;

    if (evt->data && evt->data_len > 0 && evt->data_len <= 8) {
        const uint8_t *src = (const uint8_t *)evt->data;
        for (int i = 0; i < (int)evt->data_len; i++)
            ring->entries[ring->head].inline_data[i] = src[i];
    }
    asm volatile("mfence" ::: "memory");
    ring->head = next;
    spin_unlock(&ring->lock);
    return true;
}

bool usb_event_dequeue(usb_event_ring_t *ring, usb_event_t *out) {
    spin_lock(&ring->lock);
    if (ring->tail == ring->head) {
        spin_unlock(&ring->lock);
        return false;
    }
    *out = ring->entries[ring->tail];
    if (out->data_len > 0 && out->data_len <= 8)
        out->data = out->inline_data;
    asm volatile("mfence" ::: "memory");
    ring->tail = (ring->tail + 1) & ring->mask;
    spin_unlock(&ring->lock);
    return true;
}

uint32_t usb_event_pending(const usb_event_ring_t *ring) {
    uint32_t h = ring->head, t = ring->tail;
    if (h >= t) return h - t;
    return ring->capacity - t + h;
}

void usb_event_register_handler(usb_event_handler_t fn, void *ctx) {
    spin_lock(&handlers_lock);
    if (s_intr_handler_count >= USB_EVENT_MAX_HANDLERS) { spin_unlock(&handlers_lock); return; }
    struct handler_entry *e = &s_intr_handlers[s_intr_handler_count++];
    e->fn = fn; e->ctx = ctx;
    e->device = NULL; e->endpoint = 0;
    e->xfer_type = USB_XFER_INTERRUPT;
    e->has_device_filter = false;
    spin_unlock(&handlers_lock);
}

void usb_event_register_handler_for_device(usb_event_handler_t fn, void *ctx, void *device, uint8_t endpoint, usb_transfer_type_t xfer_type)
{
    struct handler_entry *table;
    int *count;
    switch (xfer_type) {
    case USB_XFER_BULK:
        table = s_bulk_handlers; count = &s_bulk_handler_count; break;
    case USB_XFER_ISO:
        table = s_iso_handlers; count = &s_iso_handler_count; break;
    default:
        table = s_intr_handlers; count = &s_intr_handler_count; break;
    }
    spin_lock(&handlers_lock);
    if (*count >= USB_EVENT_MAX_HANDLERS) { spin_unlock(&handlers_lock); return; }
    struct handler_entry *e = &table[(*count)++];
    e->fn = fn; e->ctx = ctx;
    e->device = device; e->endpoint = endpoint;
    e->xfer_type = xfer_type;
    e->has_device_filter = true;
    spin_unlock(&handlers_lock);

    if (device) {
        struct usb_device *dev = (struct usb_device *)device;
        dev->driver_managed = 1;
    }
}

void usb_bulk_register_handler(usb_event_handler_t fn, void *ctx) {
    spin_lock(&handlers_lock);
    if (s_bulk_handler_count >= USB_EVENT_MAX_HANDLERS) { spin_unlock(&handlers_lock); return; }
    struct handler_entry *e = &s_bulk_handlers[s_bulk_handler_count++];
    e->fn = fn; e->ctx = ctx;
    e->device = NULL; e->endpoint = 0;
    e->xfer_type = USB_XFER_BULK;
    e->has_device_filter = false;
    spin_unlock(&handlers_lock);
}

void usb_bulk_register_handler_for_device(usb_event_handler_t fn, void *ctx, void *device, uint8_t endpoint)
{
    spin_lock(&handlers_lock);
    if (s_bulk_handler_count >= USB_EVENT_MAX_HANDLERS) { spin_unlock(&handlers_lock); return; }
    struct handler_entry *e = &s_bulk_handlers[s_bulk_handler_count++];
    e->fn = fn; e->ctx = ctx;
    e->device = device; e->endpoint = endpoint;
    e->xfer_type = USB_XFER_BULK;
    e->has_device_filter = true;
    spin_unlock(&handlers_lock);
}

void usb_iso_register_handler(usb_event_handler_t fn, void *ctx) {
    spin_lock(&handlers_lock);
    if (s_iso_handler_count >= USB_EVENT_MAX_HANDLERS) { spin_unlock(&handlers_lock); return; }
    struct handler_entry *e = &s_iso_handlers[s_iso_handler_count++];
    e->fn = fn; e->ctx = ctx;
    e->device = NULL; e->endpoint = 0;
    e->xfer_type = USB_XFER_ISO;
    e->has_device_filter = false;
    spin_unlock(&handlers_lock);
}

void usb_iso_register_handler_for_device(usb_event_handler_t fn, void *ctx, void *device, uint8_t endpoint)
{
    spin_lock(&handlers_lock);
    if (s_iso_handler_count >= USB_EVENT_MAX_HANDLERS) { spin_unlock(&handlers_lock); return; }
    struct handler_entry *e = &s_iso_handlers[s_iso_handler_count++];
    e->fn = fn; e->ctx = ctx;
    e->device = device; e->endpoint = endpoint;
    e->xfer_type = USB_XFER_ISO;
    e->has_device_filter = true;
    spin_unlock(&handlers_lock);
}

static void dispatch_to_table(struct handler_entry *table, int count, const usb_event_t *evt)
{
    for (int i = 0; i < count; i++) {
        struct handler_entry *e = &table[i];
        if (!e->fn) continue;

        if (e->has_device_filter) {

            if (evt->transfer_type != e->xfer_type) continue;
            if (e->device && evt->device != e->device) continue;
            if (e->endpoint && evt->endpoint != e->endpoint) continue;
        }

        if (e->has_device_filter && evt->cookie &&
            e->device && evt->cookie != e->device) continue;

        e->fn(evt, e->ctx);
    }
}

static int remove_matching(struct handler_entry *table, int count, void *device) {
    int i = 0;
    while (i < count) {
        if (table[i].has_device_filter && table[i].device == device) {
            table[i] = table[count - 1];
            count--;
        } else {
            i++;
        }
    }
    return count;
}

void usb_event_unregister_for_device(void *device) {
    if (!device) return;
    spin_lock(&handlers_lock);
    s_intr_handler_count = remove_matching(s_intr_handlers, s_intr_handler_count, device);
    s_bulk_handler_count = remove_matching(s_bulk_handlers, s_bulk_handler_count, device);
    s_iso_handler_count = remove_matching(s_iso_handlers, s_iso_handler_count, device);
    spin_unlock(&handlers_lock);
}

void usb_event_dispatch_all(void) {
    usb_event_t evt;
    while (usb_pop_event(&evt)) {
        if (evt.type == USB_EVENT_NONE) continue;
        spin_lock(&handlers_lock);
        dispatch_to_table(s_intr_handlers, s_intr_handler_count, &evt);
        spin_unlock(&handlers_lock);
    }
}

void usb_bulk_dispatch_all(void) {
    usb_event_t evt;
    while (usb_pop_bulk_event(&evt)) {
        if (evt.type == USB_EVENT_NONE) continue;
        spin_lock(&handlers_lock);
        dispatch_to_table(s_bulk_handlers, s_bulk_handler_count, &evt);
        spin_unlock(&handlers_lock);
    }
}

void usb_iso_dispatch_all(void) {
    usb_event_t evt;
    while (usb_pop_iso_event(&evt)) {
        if (evt.type == USB_EVENT_NONE) continue;
        spin_lock(&handlers_lock);
        dispatch_to_table(s_iso_handlers, s_iso_handler_count, &evt);
        spin_unlock(&handlers_lock);
    }
}
