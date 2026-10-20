// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef VMM_H
#define VMM_H

#include "components/Memory/pat.h"

#include <stddef.h>
#include <stdint.h>

#define VMM_FLAG_PRESENT (1ULL << 0)
#define VMM_FLAG_WRITE (1ULL << 1)
#define VMM_FLAG_USER (1ULL << 2)
#define VMM_FLAG_PWT (1ULL << 3)
#define VMM_FLAG_PCD (1ULL << 4)
#define VMM_FLAG_ACCESSED (1ULL << 5)
#define VMM_FLAG_DIRTY (1ULL << 6)
#define VMM_FLAG_HUGE_2M (1ULL << 7)
#define VMM_FLAG_HUGE_1G (1ULL << 7)
#define VMM_FLAG_PAT_4K (1ULL << 7)
#define VMM_FLAG_GLOBAL (1ULL << 8)
#define VMM_FLAG_PAT_LARGE (1ULL << 12)
#define VMM_FLAG_NX (1ULL << 63)

#define VMM_ADDR_MASK_4K 0x000FFFFFFFFFF000ULL
#define VMM_ADDR_MASK_2M 0x000FFFFFFFE00000ULL
#define VMM_ADDR_MASK_1G 0x000FFFFFC0000000ULL

#define VMM_KERNEL_RW (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_GLOBAL)
#define VMM_KERNEL_RO (VMM_FLAG_PRESENT | VMM_FLAG_GLOBAL | VMM_FLAG_NX)
#define VMM_USER_RW (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER)
#define VMM_USER_RO (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX)

#define VMM_PAGE_2M (2ULL * 1024 * 1024)
#define VMM_PAGE_1G (1024ULL * 1024 * 1024)

#define VMM_HEAP_BASE 0xFFFF900000000000ULL
#define VMM_HEAP_LIMIT 0xFFFF9F0000000000ULL
#define VMM_MMIO_BASE 0xFFFFA00000000000ULL
#define VMM_MMIO_SIZE (1ULL << 40)
#define VMM_VMAP_BASE 0xFFFFB00000000000ULL
#define VMM_VMAP_SIZE (1ULL << 40)
#define VMM_VMEMMAP_BASE 0xFFFFC00000000000ULL
#define VMM_VMEMMAP_SIZE (1ULL << 40)
#define VMM_INITFS_BASE 0xFFFFD00000000000ULL
#define VMM_INITFS_SIZE (1ULL << 40)

void vmm_init(void);

uint64_t vmm_create_address_space(void);
void vmm_destroy_address_space(uint64_t pml4_phys);

int vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

void vmm_unmap_page(uint64_t pml4_phys, uint64_t virt);

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt);

int vmm_is_mapped(uint64_t pml4_phys, uint64_t virt);

void vmm_switch_address_space(uint64_t pml4_phys);

uint64_t vmm_kernel_pml4(void);

uint64_t vmm_alloc(uint64_t virt, uint64_t pages, uint64_t flags);

void vmm_free(uint64_t virt, uint64_t pages);

int vmm_map_range(uint64_t pml4_phys, uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags);

int vmm_map_range_type(uint64_t pml4_phys, uint64_t virt_base, uint64_t phys_base, uint64_t size,
                       uint64_t flags, enum vmm_cache_type type);
void vmm_unmap_range(uint64_t pml4_phys, uint64_t virt_base, uint64_t size);

uint64_t vmm_map_mmio(uint64_t phys, uint64_t size, uint64_t extra_flags);

uint64_t vmm_ioremap(uint64_t phys, uint64_t size, enum vmm_cache_type type);
void vmm_iounmap(uint64_t virt);

uint64_t vmm_vmap(const uint64_t *phys_pages, uint64_t count, uint64_t flags, enum vmm_cache_type type);
void vmm_vunmap(uint64_t virt);

int vmm_set_cache_type(uint64_t virt, uint64_t size, enum vmm_cache_type type);
int vmm_get_cache_type(uint64_t pml4_phys, uint64_t virt, enum vmm_cache_type *out);

void vmm_flush_cache_range(uint64_t virt, uint64_t size);

int vmm_unmap_collect(uint64_t pml4_phys, uint64_t start, uint64_t end, uint64_t *phys_out, int max,
                      uint64_t *next);
void vmm_update_flags(uint64_t pml4_phys, uint64_t start, uint64_t end, uint64_t set, uint64_t clear);
void vmm_free_user_tables(uint64_t pml4_phys);
int vmm_vmemmap_fault(uint64_t addr);

struct vmm_stats {
    uint64_t mmio_used;
    uint64_t mmio_maps;
    uint64_t vmap_used;
    uint64_t vmap_maps;
    uint64_t huge_splits;
};
void vmm_get_stats(struct vmm_stats *out);

static inline void vmm_invlpg(uint64_t virt) {
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

static inline void vmm_flush_tlb(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

#endif
