#include "components/Interruptions/msi.h"

#include "components/Interruptions/idt.h"
#include "kernel/limine.h"

#include <ports.h>
#include <stddef.h>

extern volatile struct limine_hhdm_request hhdm_req;

static msi_vector_t s_vectors[MSI_MAX_VECTORS];
static int s_vector_count = 0;

extern void isr80(void);
extern void isr81(void);
extern void isr82(void);
extern void isr83(void);
extern void isr88(void);
extern void isr89(void);
extern void isr90(void);
extern void isr91(void);
extern void isr92(void);
extern void isr93(void);
extern void isr94(void);
extern void isr95(void);
extern void isr96(void);
extern void isr97(void);
extern void isr98(void);
extern void isr_stub(void);

static void* get_stub_for_vector(uint8_t vector) {
    switch (vector) {
        case 80: return (void*)isr80;
        case 81: return (void*)isr81;
        case 82: return (void*)isr82;
        case 83: return (void*)isr83;
        case 88: return (void*)isr88;
        case 89: return (void*)isr89;
        case 90: return (void*)isr90;
        case 91: return (void*)isr91;
        case 92: return (void*)isr92;
        case 93: return (void*)isr93;
        case 94: return (void*)isr94;
        case 95: return (void*)isr95;
        case 96: return (void*)isr96;
        case 97: return (void*)isr97;
        case 98: return (void*)isr98;
        default: return (void*)isr_stub;
    }
}

void msi_init(void) {
    for (int i = 0; i < MSI_MAX_VECTORS; i++) {
        s_vectors[i].vector = 0;
        s_vectors[i].enabled = false;
        s_vectors[i].handler = (void*)0;
        s_vectors[i].cap_offset = 0;
        s_vectors[i].is_msix = false;
    }
    s_vector_count = 0;
}

uint8_t msi_find_cap(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id) {
    uint16_t status = pci_read16(bus, dev, func, 0x06);
    if (!(status & (1 << 4))) return 0;

    uint8_t cap_ptr = pci_read8(bus, dev, func, 0x34) & 0xFC;

    int safety = 0;
    while (cap_ptr && safety < 64) {
        uint8_t id = pci_read8(bus, dev, func, cap_ptr);
        uint8_t next = pci_read8(bus, dev, func, cap_ptr + 1);

        if (id == cap_id) return cap_ptr;

        cap_ptr = next & 0xFC;
        safety++;
    }
    return 0;
}

bool msi_is_available(uint8_t bus, uint8_t dev, uint8_t func) {
    if (msi_find_cap(bus, dev, func, PCI_CAP_ID_MSI)) return true;
    if (msi_find_cap(bus, dev, func, PCI_CAP_ID_MSIX)) return true;
    return false;
}

bool msi_enable(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector, uint8_t lapic_id, void (*handler)(void))
{
    if (s_vector_count >= MSI_MAX_VECTORS) return false;

    uint8_t cap = msi_find_cap(bus, dev, func, PCI_CAP_ID_MSI);
    bool msix = false;
    if (!cap) {
        cap = msi_find_cap(bus, dev, func, PCI_CAP_ID_MSIX);
        msix = true;
    }
                if (!cap) { return false; }

    if (!msix) {

        uint16_t ctrl = pci_read16(bus, dev, func, cap + 2);
        bool b64 = (ctrl & MSI_CTRL_64BIT) != 0;

        uint32_t msg_addr_lo = 0xFEE00000U | ((uint32_t)lapic_id << 12);
        uint32_t msg_addr_hi = 0;
        uint16_t msg_data = (uint16_t)vector;

        pci_write_config(bus, dev, func, cap + 4, msg_addr_lo);
        if (b64) {
            pci_write_config(bus, dev, func, cap + 8, msg_addr_hi);
            pci_write16(bus, dev, func, cap + 12, msg_data);
        } else {
            pci_write16(bus, dev, func, cap + 8, msg_data);
        }

        ctrl &= ~(0x7 << 4);
        ctrl |= MSI_CTRL_ENABLE;
        pci_write16(bus, dev, func, cap + 2, ctrl);

    } else {

        uint16_t ctrl = pci_read16(bus, dev, func, cap + 2);

        uint32_t tbl_info = pci_read_config(bus, dev, func, cap + 4);
        uint8_t bir = (uint8_t)(tbl_info & 0x7);
        uint32_t tbl_off = tbl_info & ~0x7U;

        uint32_t bar_lo = pci_read_config(bus, dev, func, 0x10 + bir * 4);
        if (bar_lo & 1) return false;

        uint64_t tbl_base;
        if (((bar_lo >> 1) & 0x3) == 0x2) {

            uint32_t bar_hi = pci_read_config(bus, dev, func, 0x10 + bir * 4 + 4);
            tbl_base = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFU);
        } else {

            tbl_base = bar_lo & ~0xFU;
        }
        tbl_base += tbl_off;

        uint64_t hhdm_off = hhdm_req.response
                          ? hhdm_req.response->offset
                          : 0xFFFF800000000000ULL;

        volatile uint32_t *entry = (volatile uint32_t *)(tbl_base + hhdm_off);

        uint32_t msg_addr_lo = 0xFEE00000U | ((uint32_t)lapic_id << 12);
        entry[0] = msg_addr_lo;
        entry[1] = 0;
        entry[2] = (uint32_t)vector;
        entry[3] = 0;

        ctrl |= (1 << 15);
        ctrl &= ~(1 << 14);
        pci_write16(bus, dev, func, cap + 2, ctrl);
    }

    void *stub = get_stub_for_vector(vector);
    idt_set_gate(vector, stub, 0x8E);

    int idx = s_vector_count++;
    s_vectors[idx].vector = vector;
    s_vectors[idx].pci_bus = bus;
    s_vectors[idx].pci_dev = dev;
    s_vectors[idx].pci_func = func;
    s_vectors[idx].cap_offset = cap;
    s_vectors[idx].is_msix = msix;
    s_vectors[idx].enabled = true;
    s_vectors[idx].handler = handler;
          return true;
}

void msi_disable(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < s_vector_count; i++) {
        msi_vector_t *v = &s_vectors[i];
        if (v->pci_bus != bus || v->pci_dev != dev || v->pci_func != func)
            continue;
        if (!v->enabled) continue;

        if (!v->is_msix) {
            uint16_t ctrl = pci_read16(bus, dev, func, v->cap_offset + 2);
            ctrl &= ~MSI_CTRL_ENABLE;
            pci_write16(bus, dev, func, v->cap_offset + 2, ctrl);
        } else {
            uint16_t ctrl = pci_read16(bus, dev, func, v->cap_offset + 2);
            ctrl &= ~(1 << 15);
            pci_write16(bus, dev, func, v->cap_offset + 2, ctrl);
        }
        v->enabled = false;
    }
}

void msi_dispatch(uint8_t vector) {
    for (int i = 0; i < s_vector_count; i++) {
        if (s_vectors[i].vector == vector && s_vectors[i].enabled) {
            static uint8_t dbg_once[256] = {0};
            if (!dbg_once[vector]) {
                dbg_once[vector] = 1;
            }
            if (s_vectors[i].handler)
                s_vectors[i].handler();
            return;
        }
    }
    static uint8_t no_handler_warned[256] = {0};
    if (!no_handler_warned[vector]) {
        no_handler_warned[vector] = 1;
    }
}
