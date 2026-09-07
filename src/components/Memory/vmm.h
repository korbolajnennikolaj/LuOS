#ifndef VMM_H
#define VMM_H

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
#define VMM_FLAG_GLOBAL (1ULL << 8)
#define VMM_FLAG_NX (1ULL << 63)

#define VMM_KERNEL_RW (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_GLOBAL)
#define VMM_KERNEL_RO (VMM_FLAG_PRESENT | VMM_FLAG_GLOBAL | VMM_FLAG_NX)
#define VMM_USER_RW (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER)
#define VMM_USER_RO (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX)

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

uint64_t vmm_map_mmio(uint64_t phys, uint64_t size, uint64_t extra_flags);

static inline void vmm_invlpg(uint64_t virt) {
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

static inline void vmm_flush_tlb(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

#endif
