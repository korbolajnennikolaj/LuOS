// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef PAT_H
#define PAT_H

#include <stdbool.h>
#include <stdint.h>

enum vmm_cache_type {
    VMM_CACHE_WB = 0,
    VMM_CACHE_WT = 1,
    VMM_CACHE_UC_MINUS = 2,
    VMM_CACHE_UC = 3,
    VMM_CACHE_WP = 4,
    VMM_CACHE_WC = 5,
    VMM_CACHE_COUNT
};

#define PAT_PTE_PWT (1ULL << 3)
#define PAT_PTE_PCD (1ULL << 4)
#define PAT_PTE_PAT_4K (1ULL << 7)
#define PAT_PTE_PAT_LARGE (1ULL << 12)
#define PAT_PTE_CACHE_MASK_4K (PAT_PTE_PWT | PAT_PTE_PCD | PAT_PTE_PAT_4K)
#define PAT_PTE_CACHE_MASK_LARGE (PAT_PTE_PWT | PAT_PTE_PCD | PAT_PTE_PAT_LARGE)

void pat_init(void);
void pat_init_ap(void);

bool pat_available(void);
uint64_t pat_boot_value(void);
uint64_t pat_current_value(void);

uint64_t pat_pte_bits(enum vmm_cache_type type, bool large);
enum vmm_cache_type pat_type_from_pte(uint64_t entry, bool large);
uint64_t pat_large_to_4k(uint64_t entry);
uint64_t pat_4k_to_large(uint64_t entry);

const char *pat_type_name(enum vmm_cache_type type);

#endif
