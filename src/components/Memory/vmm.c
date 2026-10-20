// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "vmm.h"

#include "mm.h"
#include "pmm.h"
#include "components/logger.h"
#include "kernel/scheduler/spinlock.h"
#include "kernel/smp/tlb.h"

#include <cpuid.h>
#include <msr.h>
#include <stdbool.h>

static uint64_t kernel_pml4_phys = 0;
static spinlock_t vmm_lock = SPINLOCK_INIT;
static uint64_t vmm_huge_splits = 0;
static bool vmm_has_clflush = false;
static uint32_t vmm_clflush_line = 64;

#define VMM_LEAF_FLAGS (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_GLOBAL | \
                        VMM_FLAG_ACCESSED | VMM_FLAG_DIRTY | VMM_FLAG_NX)
#define VMM_CACHE_BITS_ANY (VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT_4K | VMM_FLAG_PAT_LARGE)

static inline void vmm_memset(void* dst, uint8_t val, uint64_t size) {
    uint8_t* p = (uint8_t*)dst;
    while (size--) *p++ = val;
}

static inline uint16_t get_pml4_index(uint64_t virt) { return (virt >> 39) & 0x1FF; }
static inline uint16_t get_pdp_index(uint64_t virt) { return (virt >> 30) & 0x1FF; }
static inline uint16_t get_pd_index(uint64_t virt) { return (virt >> 21) & 0x1FF; }
static inline uint16_t get_pt_index(uint64_t virt) { return (virt >> 12) & 0x1FF; }

static inline uint64_t* vmm_table(uint64_t entry) {
    return (uint64_t*)mm_phys_to_virt(entry & VMM_ADDR_MASK_4K);
}

static inline bool vmm_is_large(uint64_t entry) {
    return (entry & VMM_FLAG_PRESENT) && (entry & VMM_FLAG_HUGE_2M);
}

static uint64_t* vmm_split_huge(uint64_t* entry_ptr, int level, uint64_t virt) {
    uint64_t old = *entry_ptr;

    uint64_t base = (level == 3) ? (old & VMM_ADDR_MASK_1G) : (old & VMM_ADDR_MASK_2M);
    uint64_t child_step = (level == 3) ? VMM_PAGE_2M : PAGE_SIZE;

    uint64_t child_flags = old & VMM_LEAF_FLAGS;
    if (level == 3)
        child_flags |= VMM_FLAG_HUGE_2M | (old & (VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT_LARGE));
    else
        child_flags |= pat_large_to_4k(old);

    uint64_t table_phys = pmm_alloc_page();
    if (!table_phys) return NULL;

    uint64_t* table = (uint64_t*)mm_phys_to_virt(table_phys);
    for (int i = 0; i < 512; i++)
        table[i] = (base + (uint64_t)i * child_step) | child_flags;

    uint64_t parent_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    if (old & VMM_FLAG_USER) parent_flags |= VMM_FLAG_USER;

    *entry_ptr = table_phys | parent_flags;

    vmm_invlpg(virt & ((level == 3) ? ~(VMM_PAGE_1G - 1) : ~(VMM_PAGE_2M - 1)));
    vmm_huge_splits++;

    return table;
}

static uint64_t* get_next_level(uint64_t* current_level, uint16_t index, int allocate, uint64_t flags) {
    uint64_t entry = current_level[index];
    if (entry & VMM_FLAG_PRESENT) {
        if (entry & VMM_FLAG_HUGE_2M) return NULL;
        return vmm_table(entry);
    }

    if (!allocate) return NULL;

    uint64_t next_level_phys = pmm_alloc_page();
    if (!next_level_phys) return NULL;

    uint64_t* next_level_virt = (uint64_t*)mm_phys_to_virt(next_level_phys);
    vmm_memset(next_level_virt, 0, PAGE_SIZE);

    current_level[index] = next_level_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    if (flags & VMM_FLAG_USER) {
        current_level[index] |= VMM_FLAG_USER;
    }

    return next_level_virt;
}

static uint64_t* vmm_slot(uint64_t pml4_phys, uint64_t virt, int level, int allocate, uint64_t flags) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    uint64_t* pdp = get_next_level(pml4, get_pml4_index(virt), allocate, flags);
    if (!pdp) return NULL;
    uint64_t* pdpe = &pdp[get_pdp_index(virt)];
    if (level == 3) return pdpe;

    uint64_t* pd;
    if (vmm_is_large(*pdpe)) {
        if (!allocate) return NULL;
        pd = vmm_split_huge(pdpe, 3, virt);
    } else {
        pd = get_next_level(pdp, get_pdp_index(virt), allocate, flags);
    }
    if (!pd) return NULL;
    return &pd[get_pd_index(virt)];
}

static uint64_t* vmm_pte(uint64_t pml4_phys, uint64_t virt, int allocate, int split, uint64_t flags) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);

    uint64_t* pdp = get_next_level(pml4, get_pml4_index(virt), allocate, flags);
    if (!pdp) return NULL;

    uint16_t pdp_i = get_pdp_index(virt);
    uint64_t* pd;
    if (vmm_is_large(pdp[pdp_i])) {
        if (!split) return NULL;
        pd = vmm_split_huge(&pdp[pdp_i], 3, virt);
    } else {
        pd = get_next_level(pdp, pdp_i, allocate, flags);
    }
    if (!pd) return NULL;

    uint16_t pd_i = get_pd_index(virt);
    uint64_t* pt;
    if (vmm_is_large(pd[pd_i])) {
        if (!split) return NULL;
        pt = vmm_split_huge(&pd[pd_i], 2, virt);
    } else {
        pt = get_next_level(pd, pd_i, allocate, flags);
    }
    if (!pt) return NULL;

    return &pt[get_pt_index(virt)];
}

static int vmm_lookup(uint64_t pml4_phys, uint64_t virt, uint64_t** out_entry, int* out_level) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    uint64_t e = pml4[get_pml4_index(virt)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;

    uint64_t* pdp = vmm_table(e);
    uint64_t* pdpe = &pdp[get_pdp_index(virt)];
    if (!(*pdpe & VMM_FLAG_PRESENT)) return 0;
    if (*pdpe & VMM_FLAG_HUGE_1G) { *out_entry = pdpe; *out_level = 3; return 1; }

    uint64_t* pd = vmm_table(*pdpe);
    uint64_t* pde = &pd[get_pd_index(virt)];
    if (!(*pde & VMM_FLAG_PRESENT)) return 0;
    if (*pde & VMM_FLAG_HUGE_2M) { *out_entry = pde; *out_level = 2; return 1; }

    uint64_t* pt = vmm_table(*pde);
    uint64_t* pte = &pt[get_pt_index(virt)];
    if (!(*pte & VMM_FLAG_PRESENT)) return 0;
    *out_entry = pte;
    *out_level = 1;
    return 1;
}

void vmm_init(void) {
    kernel_pml4_phys = pmm_alloc_page();
    if (!kernel_pml4_phys) {
        LOG_ERROR("could not allocate kernel PML4");
        return;
    }

    uint64_t* pml4_virt = (uint64_t*)mm_phys_to_virt(kernel_pml4_phys);
    vmm_memset(pml4_virt, 0, PAGE_SIZE);

    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    uint64_t* current_pml4_virt = (uint64_t*)mm_phys_to_virt(current_cr3 & ~0xFFFULL);

    for (int i = 256; i < 512; i++) {
        pml4_virt[i] = current_pml4_virt[i];
    }

    int prealloc = 0;
    for (int i = 256; i < 512; i++) {
        if (pml4_virt[i] & VMM_FLAG_PRESENT) continue;
        uint64_t t = pmm_alloc_page();
        if (!t) break;
        vmm_memset((void*)mm_phys_to_virt(t), 0, PAGE_SIZE);
        pml4_virt[i] = t | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
        prealloc++;
    }

    vmm_switch_address_space(kernel_pml4_phys);

    struct cpuid_result r = cpuid(1, 0);
    vmm_has_clflush = (r.edx & (1u << 19)) != 0;
    if (vmm_has_clflush) {
        uint32_t line = ((r.ebx >> 8) & 0xFF) * 8;
        if (line) vmm_clflush_line = line;
    }

    LOG_DEBUG("kernel PML4 at 0x%llx, higher half copied from bootloader CR3 0x%llx, %d kernel PDPTs preallocated",
              (unsigned long long)kernel_pml4_phys, (unsigned long long)(current_cr3 & ~0xFFFULL), prealloc);
}

uint64_t vmm_kernel_pml4(void) {
    return kernel_pml4_phys;
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

uint64_t vmm_create_address_space(void) {
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return 0;

    uint64_t* pml4_virt = (uint64_t*)mm_phys_to_virt(pml4_phys);
    vmm_memset(pml4_virt, 0, PAGE_SIZE);

    uint64_t* k_pml4_virt = (uint64_t*)mm_phys_to_virt(kernel_pml4_phys);

    for (int i = 256; i < 512; i++) {
        pml4_virt[i] = k_pml4_virt[i];
    }

    return pml4_phys;
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys || pml4_phys == kernel_pml4_phys) return;

    uint64_t* pml4_virt = (uint64_t*)mm_phys_to_virt(pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (pml4_virt[i] & VMM_FLAG_PRESENT) {
            uint64_t* pdp = vmm_table(pml4_virt[i]);
            for (int j = 0; j < 512; j++) {
                if (pdp[j] & VMM_FLAG_PRESENT) {
                    if (pdp[j] & VMM_FLAG_HUGE_1G) continue;

                    uint64_t* pd = vmm_table(pdp[j]);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & VMM_FLAG_PRESENT) {
                            if (pd[k] & VMM_FLAG_HUGE_2M) continue;
                            pmm_free_page(pd[k] & VMM_ADDR_MASK_4K);
                        }
                    }
                    pmm_free_page(pdp[j] & VMM_ADDR_MASK_4K);
                }
            }
            pmm_free_page(pml4_virt[i] & VMM_ADDR_MASK_4K);
        }
    }
    pmm_free_page(pml4_phys);
}

static int vmm_map_page_impl(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pte = vmm_pte(pml4_phys, virt, 1, 1, flags);
    if (!pte) return -1;

    *pte = (phys & VMM_ADDR_MASK_4K) | flags;
    vmm_invlpg(virt);

    return 0;
}

int vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    int r = vmm_map_page_impl(pml4_phys, virt, phys, flags);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return r;
}

static void vmm_unmap_page_impl(uint64_t pml4_phys, uint64_t virt) {
    uint64_t* pte = vmm_pte(pml4_phys, virt, 0, 1, 0);
    if (!pte) return;

    *pte = 0;
    vmm_invlpg(virt);
}

void vmm_unmap_page(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    vmm_unmap_page_impl(pml4_phys, virt);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    tlb_shootdown(virt & ~0xFFFULL, PAGE_SIZE);
}

static uint64_t vmm_get_phys_impl(uint64_t pml4_phys, uint64_t virt) {
    uint64_t* entry;
    int level;
    if (!vmm_lookup(pml4_phys, virt, &entry, &level)) return 0;

    if (level == 3) return (*entry & VMM_ADDR_MASK_1G) + (virt & (VMM_PAGE_1G - 1));
    if (level == 2) return (*entry & VMM_ADDR_MASK_2M) + (virt & (VMM_PAGE_2M - 1));
    return (*entry & VMM_ADDR_MASK_4K) + (virt & 0xFFF);
}

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t p = vmm_get_phys_impl(pml4_phys, virt);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return p;
}

int vmm_is_mapped(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t* entry;
    int level;
    int r = vmm_lookup(pml4_phys, virt, &entry, &level);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return r;
}

int vmm_get_cache_type(uint64_t pml4_phys, uint64_t virt, enum vmm_cache_type *out) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t* entry;
    int level;
    int r = vmm_lookup(pml4_phys, virt, &entry, &level);
    if (r && out) *out = pat_type_from_pte(*entry, level > 1);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return r ? 0 : -1;
}

int vmm_map_range(uint64_t pml4_phys, uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    virt_base &= ~0xFFFULL;
    phys_base &= ~0xFFFULL;

    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_map_page_impl(pml4_phys, virt_base + i * PAGE_SIZE, phys_base + i * PAGE_SIZE, flags) != 0) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return -1;
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    return 0;
}

static int vmm_map_range_type_impl(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t size,
                                   uint64_t flags, enum vmm_cache_type type) {
    flags &= ~(VMM_CACHE_BITS_ANY | VMM_FLAG_HUGE_2M);
    uint64_t off = 0;

    while (off < size) {
        uint64_t va = virt + off;
        uint64_t pa = phys + off;
        uint64_t rem = size - off;

        if (((va | pa) & (VMM_PAGE_2M - 1)) == 0 && rem >= VMM_PAGE_2M) {
            uint64_t* pde = vmm_slot(pml4_phys, va, 2, 1, flags);
            if (!pde) return -1;
            if (!(*pde & VMM_FLAG_PRESENT) || (*pde & VMM_FLAG_HUGE_2M)) {
                *pde = pa | flags | VMM_FLAG_HUGE_2M | pat_pte_bits(type, true);
                vmm_invlpg(va);
                off += VMM_PAGE_2M;
                continue;
            }
        }

        uint64_t* pte = vmm_pte(pml4_phys, va, 1, 1, flags);
        if (!pte) return -1;
        *pte = pa | flags | pat_pte_bits(type, false);
        vmm_invlpg(va);
        off += PAGE_SIZE;
    }
    return 0;
}

int vmm_map_range_type(uint64_t pml4_phys, uint64_t virt_base, uint64_t phys_base, uint64_t size,
                       uint64_t flags, enum vmm_cache_type type) {
    virt_base &= ~0xFFFULL;
    phys_base &= ~0xFFFULL;
    size = PAGE_ALIGN(size);

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    int r = vmm_map_range_type_impl(pml4_phys, virt_base, phys_base, size, flags, type);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return r;
}

static void vmm_unmap_range_impl(uint64_t pml4_phys, uint64_t virt, uint64_t size) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    uint64_t va = virt;
    uint64_t end = virt + size;

    while (va < end) {
        uint64_t e4 = pml4[get_pml4_index(va)];
        if (!(e4 & VMM_FLAG_PRESENT)) {
            va = (va + (1ULL << 39)) & ~((1ULL << 39) - 1);
            continue;
        }
        uint64_t* pdp = vmm_table(e4);
        uint64_t* pdpe = &pdp[get_pdp_index(va)];
        if (!(*pdpe & VMM_FLAG_PRESENT)) {
            va = (va + VMM_PAGE_1G) & ~(VMM_PAGE_1G - 1);
            continue;
        }
        if (*pdpe & VMM_FLAG_HUGE_1G) {
            if ((va & (VMM_PAGE_1G - 1)) == 0 && end - va >= VMM_PAGE_1G) {
                *pdpe = 0;
                vmm_invlpg(va);
                va += VMM_PAGE_1G;
                continue;
            }
            if (!vmm_split_huge(pdpe, 3, va)) return;
        }
        uint64_t* pd = vmm_table(*pdpe);
        uint64_t* pde = &pd[get_pd_index(va)];
        if (!(*pde & VMM_FLAG_PRESENT)) {
            va = (va + VMM_PAGE_2M) & ~(VMM_PAGE_2M - 1);
            continue;
        }
        if (*pde & VMM_FLAG_HUGE_2M) {
            if ((va & (VMM_PAGE_2M - 1)) == 0 && end - va >= VMM_PAGE_2M) {
                *pde = 0;
                vmm_invlpg(va);
                va += VMM_PAGE_2M;
                continue;
            }
            if (!vmm_split_huge(pde, 2, va)) return;
        }
        uint64_t* pt = vmm_table(*pde);
        pt[get_pt_index(va)] = 0;
        vmm_invlpg(va);
        va += PAGE_SIZE;
    }
}

void vmm_unmap_range(uint64_t pml4_phys, uint64_t virt_base, uint64_t size) {
    virt_base &= ~0xFFFULL;
    size = PAGE_ALIGN(size);
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    vmm_unmap_range_impl(pml4_phys, virt_base, size);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    tlb_shootdown(virt_base, size);
}

int vmm_unmap_collect(uint64_t pml4_phys, uint64_t start, uint64_t end, uint64_t *phys_out, int max,
                      uint64_t *next) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    uint64_t va = start & ~0xFFFULL;
    int n = 0;

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    while (va < end && n < max) {
        uint64_t e4 = pml4[get_pml4_index(va)];
        if (!(e4 & VMM_FLAG_PRESENT)) { va = (va + (1ULL << 39)) & ~((1ULL << 39) - 1); continue; }
        uint64_t* pdp = vmm_table(e4);
        uint64_t* pdpe = &pdp[get_pdp_index(va)];
        if (!(*pdpe & VMM_FLAG_PRESENT)) { va = (va + VMM_PAGE_1G) & ~(VMM_PAGE_1G - 1); continue; }
        if (*pdpe & VMM_FLAG_HUGE_1G) { *pdpe = 0; va = (va + VMM_PAGE_1G) & ~(VMM_PAGE_1G - 1); continue; }
        uint64_t* pd = vmm_table(*pdpe);
        uint64_t* pde = &pd[get_pd_index(va)];
        if (!(*pde & VMM_FLAG_PRESENT)) { va = (va + VMM_PAGE_2M) & ~(VMM_PAGE_2M - 1); continue; }
        if (*pde & VMM_FLAG_HUGE_2M) { *pde = 0; va = (va + VMM_PAGE_2M) & ~(VMM_PAGE_2M - 1); continue; }
        uint64_t* pt = vmm_table(*pde);
        uint64_t* pte = &pt[get_pt_index(va)];
        if (*pte & VMM_FLAG_PRESENT) {
            phys_out[n++] = *pte & VMM_ADDR_MASK_4K;
            *pte = 0;
        }
        va += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    if (next) *next = va;
    return n;
}

void vmm_update_flags(uint64_t pml4_phys, uint64_t start, uint64_t end, uint64_t set, uint64_t clear) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    uint64_t va = start & ~0xFFFULL;

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    while (va < end) {
        uint64_t e4 = pml4[get_pml4_index(va)];
        if (!(e4 & VMM_FLAG_PRESENT)) { va = (va + (1ULL << 39)) & ~((1ULL << 39) - 1); continue; }
        uint64_t* pdp = vmm_table(e4);
        uint64_t pdpe = pdp[get_pdp_index(va)];
        if (!(pdpe & VMM_FLAG_PRESENT) || (pdpe & VMM_FLAG_HUGE_1G)) { va = (va + VMM_PAGE_1G) & ~(VMM_PAGE_1G - 1); continue; }
        uint64_t* pd = vmm_table(pdpe);
        uint64_t pde = pd[get_pd_index(va)];
        if (!(pde & VMM_FLAG_PRESENT) || (pde & VMM_FLAG_HUGE_2M)) { va = (va + VMM_PAGE_2M) & ~(VMM_PAGE_2M - 1); continue; }
        uint64_t* pt = vmm_table(pde);
        uint64_t* pte = &pt[get_pt_index(va)];
        if (*pte & VMM_FLAG_PRESENT) *pte = (*pte & ~clear) | set;
        va += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    tlb_shootdown(start, end - start);
}

void vmm_free_user_tables(uint64_t pml4_phys) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;
        uint64_t* pdp = vmm_table(pml4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdp[j] & VMM_FLAG_PRESENT) || (pdp[j] & VMM_FLAG_HUGE_1G)) continue;
            uint64_t* pd = vmm_table(pdp[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT) || (pd[k] & VMM_FLAG_HUGE_2M)) continue;
                pmm_free_page(pd[k] & VMM_ADDR_MASK_4K);
            }
            pmm_free_page(pdp[j] & VMM_ADDR_MASK_4K);
        }
        pmm_free_page(pml4[i] & VMM_ADDR_MASK_4K);
        pml4[i] = 0;
    }
}

int vmm_vmemmap_fault(uint64_t addr) {
    if (addr < VMM_VMEMMAP_BASE || addr >= VMM_VMEMMAP_BASE + VMM_VMEMMAP_SIZE) return 0;
    uint64_t va = addr & ~0xFFFULL;
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t* entry;
    int level;
    if (vmm_lookup(kernel_pml4_phys, va, &entry, &level)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 1;
    }
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    vmm_memset((void*)mm_phys_to_virt(phys), 0, PAGE_SIZE);
    int r = vmm_map_page_impl(kernel_pml4_phys, va, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_GLOBAL | VMM_FLAG_NX);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    if (r != 0) {
        pmm_free_page(phys);
        return 0;
    }
    return 1;
}

void vmm_flush_cache_range(uint64_t virt, uint64_t size) {
    if (!size) return;
    if (!vmm_has_clflush) {
        wbinvd();
        return;
    }
    uint64_t line = vmm_clflush_line;
    uint64_t start = virt & ~(line - 1);
    uint64_t end = virt + size;
    mfence();
    for (uint64_t p = start; p < end; p += line)
        clflush((volatile void*)p);
    mfence();
}

int vmm_set_cache_type(uint64_t virt, uint64_t size, enum vmm_cache_type type) {
    virt &= ~0xFFFULL;
    size = PAGE_ALIGN(size);
    uint64_t end = virt + size;
    int r = 0;

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t va = virt;
    while (va < end) {
        uint64_t* entry;
        int level;
        if (!vmm_lookup(kernel_pml4_phys, va, &entry, &level)) { r = -1; break; }

        if (level == 3) {
            if ((va & (VMM_PAGE_1G - 1)) == 0 && end - va >= VMM_PAGE_1G) {
                *entry = (*entry & ~(VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT_LARGE)) | pat_pte_bits(type, true);
                va += VMM_PAGE_1G;
                continue;
            }
            if (!vmm_split_huge(entry, 3, va)) { r = -1; break; }
            continue;
        }
        if (level == 2) {
            if ((va & (VMM_PAGE_2M - 1)) == 0 && end - va >= VMM_PAGE_2M) {
                *entry = (*entry & ~(VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT_LARGE)) | pat_pte_bits(type, true);
                va += VMM_PAGE_2M;
                continue;
            }
            if (!vmm_split_huge(entry, 2, va)) { r = -1; break; }
            continue;
        }
        *entry = (*entry & ~(VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT_4K)) | pat_pte_bits(type, false);
        va += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    tlb_shootdown(virt, size);
    if (type != VMM_CACHE_WB) vmm_flush_cache_range(virt, size);
    return r;
}


#define VARENA_FRAGS 512

struct varena {
    const char* name;
    uint64_t base;
    uint64_t size;
    uint64_t used;
    uint64_t maps;
    int nfrag;
    struct { uint64_t start, len; } frag[VARENA_FRAGS];
};

static struct varena mmio_arena = { "mmio", VMM_MMIO_BASE, VMM_MMIO_SIZE, 0, 0, 1, { { VMM_MMIO_BASE, VMM_MMIO_SIZE } } };
static struct varena vmap_arena = { "vmap", VMM_VMAP_BASE, VMM_VMAP_SIZE, 0, 0, 1, { { VMM_VMAP_BASE, VMM_VMAP_SIZE } } };
static spinlock_t varena_lock = SPINLOCK_INIT;

static uint64_t varena_alloc(struct varena* a, uint64_t len, uint64_t align) {
    for (int i = 0; i < a->nfrag; i++) {
        uint64_t fs = a->frag[i].start;
        uint64_t fe = fs + a->frag[i].len;
        uint64_t s = (fs + align - 1) & ~(align - 1);
        if (s + len > fe || s + len < s) continue;

        bool left = s > fs;
        bool right = s + len < fe;
        if (left && right) {
            if (a->nfrag >= VARENA_FRAGS) return 0;
            for (int k = a->nfrag; k > i + 1; k--) a->frag[k] = a->frag[k - 1];
            a->nfrag++;
            a->frag[i].len = s - fs;
            a->frag[i + 1].start = s + len;
            a->frag[i + 1].len = fe - (s + len);
        } else if (left) {
            a->frag[i].len = s - fs;
        } else if (right) {
            a->frag[i].start = s + len;
            a->frag[i].len = fe - (s + len);
        } else {
            for (int k = i; k < a->nfrag - 1; k++) a->frag[k] = a->frag[k + 1];
            a->nfrag--;
        }
        a->used += len;
        a->maps++;
        return s;
    }
    return 0;
}

static void varena_free(struct varena* a, uint64_t start, uint64_t len) {
    int i = 0;
    while (i < a->nfrag && a->frag[i].start < start) i++;

    bool merge_prev = i > 0 && a->frag[i - 1].start + a->frag[i - 1].len == start;
    bool merge_next = i < a->nfrag && start + len == a->frag[i].start;

    if (merge_prev && merge_next) {
        a->frag[i - 1].len += len + a->frag[i].len;
        for (int k = i; k < a->nfrag - 1; k++) a->frag[k] = a->frag[k + 1];
        a->nfrag--;
    } else if (merge_prev) {
        a->frag[i - 1].len += len;
    } else if (merge_next) {
        a->frag[i].start = start;
        a->frag[i].len += len;
    } else {
        if (a->nfrag >= VARENA_FRAGS) {
            LOG_WARNING("%s arena fragment table full, leaking 0x%llx (+%llu)", a->name,
                        (unsigned long long)start, (unsigned long long)len);
            a->used -= len;
            a->maps--;
            return;
        }
        for (int k = a->nfrag; k > i; k--) a->frag[k] = a->frag[k - 1];
        a->frag[i].start = start;
        a->frag[i].len = len;
        a->nfrag++;
    }
    a->used -= len;
    a->maps--;
}

#define VMM_MAX_MAPPINGS 1024

enum vmm_map_kind { VMM_MAP_FREE = 0, VMM_MAP_MMIO_LEGACY, VMM_MAP_MMIO, VMM_MAP_VMAP };

struct vmm_mapping {
    uint64_t virt;
    uint64_t map_size;
    uint64_t alloc_base;
    uint64_t alloc_len;
    uint64_t phys;
    uint8_t type;
    uint8_t kind;
};

static struct vmm_mapping vmm_maps[VMM_MAX_MAPPINGS];

static int vmm_record_add(const struct vmm_mapping* m) {
    for (int i = 0; i < VMM_MAX_MAPPINGS; i++) {
        if (vmm_maps[i].kind == VMM_MAP_FREE) {
            vmm_maps[i] = *m;
            return i;
        }
    }
    return -1;
}

static int vmm_record_find(uint64_t virt, uint8_t kind_a, uint8_t kind_b) {
    for (int i = 0; i < VMM_MAX_MAPPINGS; i++) {
        struct vmm_mapping* m = &vmm_maps[i];
        if (m->kind != kind_a && m->kind != kind_b) continue;
        if (virt >= m->virt && virt < m->virt + m->map_size) return i;
    }
    return -1;
}

static uint64_t vmm_ioremap_kind(uint64_t phys, uint64_t size, enum vmm_cache_type type, uint8_t kind) {
    if (!size) return 0;

    uint64_t page_off = phys & (PAGE_SIZE - 1);
    uint64_t phys_base = phys - page_off;
    uint64_t map_size = PAGE_ALIGN(size + page_off);

    bool big = map_size >= VMM_PAGE_2M;
    uint64_t alloc_len = map_size + PAGE_SIZE + (big ? VMM_PAGE_2M : 0);

    uint64_t irq_flags = spin_lock_irqsave(&varena_lock);
    uint64_t base = varena_alloc(&mmio_arena, alloc_len, big ? VMM_PAGE_2M : PAGE_SIZE);
    spin_unlock_irqrestore(&varena_lock, irq_flags);
    if (!base) {
        LOG_ERROR("MMIO window exhausted, cannot map 0x%llx (+%llu bytes)",
                  (unsigned long long)phys, (unsigned long long)size);
        return 0;
    }

    uint64_t virt = base + (big ? (phys_base & (VMM_PAGE_2M - 1)) : 0);
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_GLOBAL | VMM_FLAG_NX;

    if (vmm_map_range_type(kernel_pml4_phys, virt, phys_base, map_size, flags, type) != 0) {
        vmm_unmap_range(kernel_pml4_phys, virt, map_size);
        irq_flags = spin_lock_irqsave(&varena_lock);
        varena_free(&mmio_arena, base, alloc_len);
        spin_unlock_irqrestore(&varena_lock, irq_flags);
        LOG_ERROR("failed to map MMIO 0x%llx (+%llu bytes)", (unsigned long long)phys_base,
                  (unsigned long long)map_size);
        return 0;
    }

    struct vmm_mapping m = { virt, map_size, base, alloc_len, phys_base, (uint8_t)type, kind };
    irq_flags = spin_lock_irqsave(&varena_lock);
    int slot = vmm_record_add(&m);
    spin_unlock_irqrestore(&varena_lock, irq_flags);
    if (slot < 0) LOG_WARNING("mapping table full, 0x%llx can never be unmapped", (unsigned long long)virt);

    LOG_DEBUG("MMIO 0x%llx (+%llu bytes) mapped at 0x%llx as %s", (unsigned long long)phys_base,
              (unsigned long long)map_size, (unsigned long long)virt, pat_type_name(type));
    return virt + page_off;
}

uint64_t vmm_ioremap(uint64_t phys, uint64_t size, enum vmm_cache_type type) {
    return vmm_ioremap_kind(phys, size, type, VMM_MAP_MMIO);
}

uint64_t vmm_map_mmio(uint64_t phys, uint64_t size, uint64_t extra_flags) {
    (void)extra_flags;
    if (!phys || !size) return 0;

    uint64_t page_off = phys & (PAGE_SIZE - 1);
    uint64_t phys_base = phys - page_off;
    uint64_t map_size = PAGE_ALIGN(size + page_off);

    uint64_t irq_flags = spin_lock_irqsave(&varena_lock);
    for (int i = 0; i < VMM_MAX_MAPPINGS; i++) {
        struct vmm_mapping* m = &vmm_maps[i];
        if (m->kind == VMM_MAP_MMIO_LEGACY && m->phys == phys_base && m->map_size >= map_size) {
            uint64_t v = m->virt + page_off;
            spin_unlock_irqrestore(&varena_lock, irq_flags);
            return v;
        }
    }
    spin_unlock_irqrestore(&varena_lock, irq_flags);

    return vmm_ioremap_kind(phys, size, VMM_CACHE_UC, VMM_MAP_MMIO_LEGACY);
}

void vmm_iounmap(uint64_t virt) {
    if (!virt) return;
    uint64_t irq_flags = spin_lock_irqsave(&varena_lock);
    int i = vmm_record_find(virt, VMM_MAP_MMIO, VMM_MAP_MMIO);
    if (i < 0) {
        bool legacy = vmm_record_find(virt, VMM_MAP_MMIO_LEGACY, VMM_MAP_MMIO_LEGACY) >= 0;
        spin_unlock_irqrestore(&varena_lock, irq_flags);
        if (!legacy) LOG_WARNING("iounmap of unknown address 0x%llx", (unsigned long long)virt);
        return;
    }
    struct vmm_mapping m = vmm_maps[i];
    vmm_maps[i].kind = VMM_MAP_FREE;
    spin_unlock_irqrestore(&varena_lock, irq_flags);

    vmm_unmap_range(kernel_pml4_phys, m.virt, m.map_size);

    irq_flags = spin_lock_irqsave(&varena_lock);
    varena_free(&mmio_arena, m.alloc_base, m.alloc_len);
    spin_unlock_irqrestore(&varena_lock, irq_flags);
}

uint64_t vmm_vmap(const uint64_t* phys_pages, uint64_t count, uint64_t flags, enum vmm_cache_type type) {
    if (!phys_pages || !count) return 0;
    uint64_t map_size = count * PAGE_SIZE;
    uint64_t alloc_len = map_size + PAGE_SIZE;

    uint64_t irq_flags = spin_lock_irqsave(&varena_lock);
    uint64_t base = varena_alloc(&vmap_arena, alloc_len, PAGE_SIZE);
    spin_unlock_irqrestore(&varena_lock, irq_flags);
    if (!base) {
        LOG_ERROR("vmap window exhausted (%llu pages)", (unsigned long long)count);
        return 0;
    }

    flags &= ~(VMM_CACHE_BITS_ANY | VMM_FLAG_HUGE_2M);
    flags |= VMM_FLAG_PRESENT;
    uint64_t cache = pat_pte_bits(type, false);

    irq_flags = spin_lock_irqsave(&vmm_lock);
    int r = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (vmm_map_page_impl(kernel_pml4_phys, base + i * PAGE_SIZE, phys_pages[i], flags | cache) != 0) {
            r = -1;
            break;
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    if (r != 0) {
        vmm_unmap_range(kernel_pml4_phys, base, map_size);
        irq_flags = spin_lock_irqsave(&varena_lock);
        varena_free(&vmap_arena, base, alloc_len);
        spin_unlock_irqrestore(&varena_lock, irq_flags);
        return 0;
    }

    struct vmm_mapping m = { base, map_size, base, alloc_len, 0, (uint8_t)type, VMM_MAP_VMAP };
    irq_flags = spin_lock_irqsave(&varena_lock);
    int slot = vmm_record_add(&m);
    spin_unlock_irqrestore(&varena_lock, irq_flags);
    if (slot < 0) LOG_WARNING("mapping table full, vmap 0x%llx can never be unmapped", (unsigned long long)base);
    return base;
}

void vmm_vunmap(uint64_t virt) {
    if (!virt) return;
    uint64_t irq_flags = spin_lock_irqsave(&varena_lock);
    int i = vmm_record_find(virt, VMM_MAP_VMAP, VMM_MAP_VMAP);
    if (i < 0) {
        spin_unlock_irqrestore(&varena_lock, irq_flags);
        LOG_WARNING("vunmap of unknown address 0x%llx", (unsigned long long)virt);
        return;
    }
    struct vmm_mapping m = vmm_maps[i];
    vmm_maps[i].kind = VMM_MAP_FREE;
    spin_unlock_irqrestore(&varena_lock, irq_flags);

    vmm_unmap_range(kernel_pml4_phys, m.virt, m.map_size);

    irq_flags = spin_lock_irqsave(&varena_lock);
    varena_free(&vmap_arena, m.alloc_base, m.alloc_len);
    spin_unlock_irqrestore(&varena_lock, irq_flags);
}

void vmm_get_stats(struct vmm_stats* out) {
    if (!out) return;
    uint64_t irq_flags = spin_lock_irqsave(&varena_lock);
    out->mmio_used = mmio_arena.used;
    out->mmio_maps = mmio_arena.maps;
    out->vmap_used = vmap_arena.used;
    out->vmap_maps = vmap_arena.maps;
    out->huge_splits = vmm_huge_splits;
    spin_unlock_irqrestore(&varena_lock, irq_flags);
}

uint64_t vmm_alloc(uint64_t virt, uint64_t pages, uint64_t flags) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t current_pml4 = cr3 & ~0xFFFULL;

    virt &= ~0xFFFULL;

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 0; }

        if (vmm_map_page_impl(current_pml4, virt + i * PAGE_SIZE, phys, flags) != 0) {
            pmm_free_page(phys);
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }

        vmm_memset((void*)(virt + i * PAGE_SIZE), 0, PAGE_SIZE);
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    return virt;
}

void vmm_free(uint64_t virt, uint64_t pages) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t current_pml4 = cr3 & ~0xFFFULL;

    virt &= ~0xFFFULL;

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t curr_virt = virt + i * PAGE_SIZE;
        uint64_t phys = vmm_get_phys_impl(current_pml4, curr_virt);

        if (phys) {
            vmm_unmap_page_impl(current_pml4, curr_virt);
            pmm_free_page(phys);
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    tlb_shootdown(virt, pages * PAGE_SIZE);
}
