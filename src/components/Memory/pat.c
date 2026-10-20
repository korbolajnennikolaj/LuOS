// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "pat.h"

#include "components/logger.h"

#include <cpuid.h>
#include <msr.h>

#define PAT_MT_UC 0x00ULL
#define PAT_MT_WC 0x01ULL
#define PAT_MT_WT 0x04ULL
#define PAT_MT_WP 0x05ULL
#define PAT_MT_WB 0x06ULL
#define PAT_MT_UCM 0x07ULL

#define PAT_ENTRY(i, t) ((t) << ((i) * 8))

#define LUOS_PAT_VALUE (PAT_ENTRY(0, PAT_MT_WB) | PAT_ENTRY(1, PAT_MT_WT) | PAT_ENTRY(2, PAT_MT_UCM) | \
                        PAT_ENTRY(3, PAT_MT_UC) | PAT_ENTRY(4, PAT_MT_WP) | PAT_ENTRY(5, PAT_MT_WC) | \
                        PAT_ENTRY(6, PAT_MT_UCM) | PAT_ENTRY(7, PAT_MT_UC))

static const uint8_t pat_index_for_type[VMM_CACHE_COUNT] = {
    [VMM_CACHE_WB] = 0,
    [VMM_CACHE_WT] = 1,
    [VMM_CACHE_UC_MINUS] = 2,
    [VMM_CACHE_UC] = 3,
    [VMM_CACHE_WP] = 4,
    [VMM_CACHE_WC] = 5,
};

static const uint8_t pat_type_for_index[8] = {
    VMM_CACHE_WB, VMM_CACHE_WT, VMM_CACHE_UC_MINUS, VMM_CACHE_UC,
    VMM_CACHE_WP, VMM_CACHE_WC, VMM_CACHE_UC_MINUS, VMM_CACHE_UC,
};

static const char *const pat_names[VMM_CACHE_COUNT] = {
    [VMM_CACHE_WB] = "WB",
    [VMM_CACHE_WT] = "WT",
    [VMM_CACHE_UC_MINUS] = "UC-",
    [VMM_CACHE_UC] = "UC",
    [VMM_CACHE_WP] = "WP",
    [VMM_CACHE_WC] = "WC",
};

static bool pat_supported = false;
static uint64_t pat_on_boot = 0;

static void pat_flush_tlb_all(void) {
    uint64_t cr4 = read_cr4();
    if (cr4 & (1ULL << 7)) {
        write_cr4(cr4 & ~(1ULL << 7));
        write_cr4(cr4);
    } else {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }
}

static void pat_program(void) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    uint64_t cr0 = read_cr0();
    write_cr0((cr0 | (1ULL << 30)) & ~(1ULL << 29));
    wbinvd();
    pat_flush_tlb_all();

    wrmsr(MSR_IA32_PAT, LUOS_PAT_VALUE);

    wbinvd();
    pat_flush_tlb_all();
    write_cr0(cr0);

    if (rflags & (1 << 9)) asm volatile("sti");
}

void pat_init(void) {
    struct cpuid_result r = cpuid(1, 0);
    pat_supported = (r.edx & (1u << 16)) != 0;
    if (!pat_supported) {
        LOG_WARNING("CPU has no PAT, write-combining mappings fall back to UC");
        return;
    }

    pat_on_boot = rdmsr(MSR_IA32_PAT);
    if (pat_on_boot != LUOS_PAT_VALUE) pat_program();

    LOG_INFO("PAT 0x%016llx (boot 0x%016llx): 0=WB 1=WT 2=UC- 3=UC 4=WP 5=WC 6=UC- 7=UC",
             (unsigned long long)rdmsr(MSR_IA32_PAT), (unsigned long long)pat_on_boot);
}

void pat_init_ap(void) {
    if (!pat_supported) return;
    if (rdmsr(MSR_IA32_PAT) != LUOS_PAT_VALUE) pat_program();
}

bool pat_available(void) { return pat_supported; }
uint64_t pat_boot_value(void) { return pat_on_boot; }
uint64_t pat_current_value(void) { return pat_supported ? rdmsr(MSR_IA32_PAT) : 0; }

uint64_t pat_pte_bits(enum vmm_cache_type type, bool large) {
    if ((unsigned)type >= VMM_CACHE_COUNT) type = VMM_CACHE_UC;
    if (!pat_supported && (type == VMM_CACHE_WC || type == VMM_CACHE_WP)) type = VMM_CACHE_UC;

    uint8_t idx = pat_index_for_type[type];
    uint64_t bits = 0;
    if (idx & 1) bits |= PAT_PTE_PWT;
    if (idx & 2) bits |= PAT_PTE_PCD;
    if (idx & 4) bits |= large ? PAT_PTE_PAT_LARGE : PAT_PTE_PAT_4K;
    return bits;
}

enum vmm_cache_type pat_type_from_pte(uint64_t entry, bool large) {
    uint8_t idx = 0;
    if (entry & PAT_PTE_PWT) idx |= 1;
    if (entry & PAT_PTE_PCD) idx |= 2;
    if (entry & (large ? PAT_PTE_PAT_LARGE : PAT_PTE_PAT_4K)) idx |= 4;
    return (enum vmm_cache_type)pat_type_for_index[idx];
}

uint64_t pat_large_to_4k(uint64_t entry) {
    uint64_t bits = entry & (PAT_PTE_PWT | PAT_PTE_PCD);
    if (entry & PAT_PTE_PAT_LARGE) bits |= PAT_PTE_PAT_4K;
    return bits;
}

uint64_t pat_4k_to_large(uint64_t entry) {
    uint64_t bits = entry & (PAT_PTE_PWT | PAT_PTE_PCD);
    if (entry & PAT_PTE_PAT_4K) bits |= PAT_PTE_PAT_LARGE;
    return bits;
}

const char *pat_type_name(enum vmm_cache_type type) {
    if ((unsigned)type >= VMM_CACHE_COUNT) return "?";
    return pat_names[type];
}
