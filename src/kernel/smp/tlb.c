// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/smp/tlb.h"

#include "components/Interruptions/isr.h"
#include "components/logger.h"
#include "drivers/Timer/apic_driver.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>

#define TLB_FULL_FLUSH_PAGES 64
#define TLB_WAIT_SPINS 400000000ULL

static volatile uint8_t tlb_cpu[256];
static volatile uint8_t tlb_need_flush[256];
static volatile uint64_t tlb_req_start;
static volatile uint64_t tlb_req_size;
static volatile int32_t tlb_req_pending;
static volatile int32_t tlb_cpus;
static volatile uint64_t tlb_rounds;
static spinlock_t tlb_lock = SPINLOCK_INIT;
static bool tlb_ready = false;

static void tlb_flush_everything_local(void) {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 7)) {
        asm volatile("mov %0, %%cr4" :: "r"(cr4 & ~(1ULL << 7)) : "memory");
        asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    } else {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
}

static void tlb_flush_local(uint64_t start, uint64_t size) {
    if (size == 0 || size / 4096 > TLB_FULL_FLUSH_PAGES) {
        tlb_flush_everything_local();
        return;
    }
    uint64_t end = start + size;
    for (uint64_t va = start & ~0xFFFULL; va < end; va += 4096)
        asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static void tlb_service_local(void) {
    uint8_t me = apic_get_lapic_id();
    if (!__atomic_load_n(&tlb_need_flush[me], __ATOMIC_ACQUIRE)) return;
    tlb_flush_local(tlb_req_start, tlb_req_size);
    __atomic_store_n(&tlb_need_flush[me], 0, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&tlb_req_pending, 1, __ATOMIC_ACQ_REL);
}

static void tlb_ipi_handler(struct registers *regs) {
    (void)regs;
    tlb_service_local();
}

void tlb_init(void) {
    if (tlb_ready) return;
    irq_register_handler(IRQ_VECTOR_TLB_SHOOTDOWN, tlb_ipi_handler);
    tlb_ready = true;
    tlb_cpu_online();
}

void tlb_cpu_online(void) {
    uint8_t me = apic_get_lapic_id();
    if (__atomic_exchange_n(&tlb_cpu[me], 1, __ATOMIC_ACQ_REL) == 0)
        __atomic_fetch_add(&tlb_cpus, 1, __ATOMIC_ACQ_REL);
}

int tlb_online_cpus(void) { return __atomic_load_n(&tlb_cpus, __ATOMIC_RELAXED); }
uint64_t tlb_shootdown_count(void) { return __atomic_load_n(&tlb_rounds, __ATOMIC_RELAXED); }

void tlb_shootdown(uint64_t start, uint64_t size) {
    tlb_flush_local(start, size);
    if (!tlb_ready || __atomic_load_n(&tlb_cpus, __ATOMIC_ACQUIRE) <= 1) return;

    uint64_t rflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    while (!spin_trylock(&tlb_lock)) {
        tlb_service_local();
        asm volatile("pause");
    }

    uint8_t me = apic_get_lapic_id();
    tlb_req_start = start;
    tlb_req_size = size;

    int32_t targets = 0;
    for (int id = 0; id < 256; id++)
        if (tlb_cpu[id] && id != me) targets++;
    __atomic_store_n(&tlb_req_pending, targets, __ATOMIC_RELEASE);

    for (int id = 0; id < 256; id++) {
        if (!tlb_cpu[id] || id == me) continue;
        __atomic_store_n(&tlb_need_flush[id], 1, __ATOMIC_RELEASE);
        apic_send_ipi((uint8_t)id, IRQ_VECTOR_TLB_SHOOTDOWN);
    }

    uint64_t spins = 0;
    while (__atomic_load_n(&tlb_req_pending, __ATOMIC_ACQUIRE) > 0) {
        asm volatile("pause");
        if (++spins == TLB_WAIT_SPINS) {
            for (int id = 0; id < 256; id++)
                if (tlb_need_flush[id]) LOG_WARNING("TLB shootdown: LAPIC %d did not answer", id);
        }
    }

    __atomic_fetch_add(&tlb_rounds, 1, __ATOMIC_RELAXED);
    spin_unlock(&tlb_lock);
    if (rflags & (1 << 9)) asm volatile("sti");
}

void tlb_shootdown_all(void) {
    tlb_shootdown(0, 0);
}

int tlb_cpu_lapic_ids(uint8_t *out, int max) {
    int n = 0;
    for (int id = 0; id < 256 && n < max; id++)
        if (tlb_cpu[id]) out[n++] = (uint8_t)id;
    return n;
}
