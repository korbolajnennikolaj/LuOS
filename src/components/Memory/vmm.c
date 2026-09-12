#include "vmm.h"

#include "mm.h"
#include "pmm.h"
#include "kernel/scheduler/spinlock.h"

static uint64_t kernel_pml4_phys = 0;
static spinlock_t vmm_lock = SPINLOCK_INIT;

static inline void vmm_memset(void* dst, uint8_t val, uint64_t size) {
    uint8_t* p = (uint8_t*)dst;
    while (size--) *p++ = val;
}

static inline uint16_t get_pml4_index(uint64_t virt) { return (virt >> 39) & 0x1FF; }
static inline uint16_t get_pdp_index(uint64_t virt) { return (virt >> 30) & 0x1FF; }
static inline uint16_t get_pd_index(uint64_t virt) { return (virt >> 21) & 0x1FF; }
static inline uint16_t get_pt_index(uint64_t virt) { return (virt >> 12) & 0x1FF; }

static uint64_t* vmm_split_huge(uint64_t* entry_ptr, int level) {
    uint64_t old = *entry_ptr;

    uint64_t base = (level == 3) ? (old & 0x000FFFFFC0000000ULL)
                                 : (old & 0x000FFFFFFFE00000ULL);

    uint64_t child_step = (level == 3) ? (2ULL * 1024 * 1024) : PAGE_SIZE;

    uint64_t child_flags = old & (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER |
                                  VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_GLOBAL);
    child_flags |= (old & VMM_FLAG_NX);
    if (level == 3) child_flags |= VMM_FLAG_HUGE_2M;

    uint64_t table_phys = pmm_alloc_page();
    if (!table_phys) return NULL;

    uint64_t* table = (uint64_t*)mm_phys_to_virt(table_phys);
    for (int i = 0; i < 512; i++) {
        table[i] = (base + (uint64_t)i * child_step) | child_flags;
    }

    uint64_t parent_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    if (old & VMM_FLAG_USER) parent_flags |= VMM_FLAG_USER;

    *entry_ptr = table_phys | parent_flags;

    vmm_flush_tlb();

    return table;
}

static uint64_t* get_next_level(uint64_t* current_level, uint16_t index, int allocate, uint64_t flags) {
    uint64_t entry = current_level[index];
    if (entry & VMM_FLAG_PRESENT) {

        if (entry & VMM_FLAG_HUGE_2M) return NULL;
        return (uint64_t*)mm_phys_to_virt(entry & ~0xFFFULL);
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

void vmm_init(void) {
    kernel_pml4_phys = pmm_alloc_page();
    if (!kernel_pml4_phys) return;

    uint64_t* pml4_virt = (uint64_t*)mm_phys_to_virt(kernel_pml4_phys);
    vmm_memset(pml4_virt, 0, PAGE_SIZE);

    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    uint64_t* current_pml4_virt = (uint64_t*)mm_phys_to_virt(current_cr3 & ~0xFFFULL);

    for (int i = 256; i < 512; i++) {
        pml4_virt[i] = current_pml4_virt[i];
    }

    vmm_switch_address_space(kernel_pml4_phys);
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
            uint64_t* pdp = (uint64_t*)mm_phys_to_virt(pml4_virt[i] & ~0xFFFULL);
            for (int j = 0; j < 512; j++) {
                if (pdp[j] & VMM_FLAG_PRESENT) {
                    if (pdp[j] & VMM_FLAG_HUGE_1G) continue;

                    uint64_t* pd = (uint64_t*)mm_phys_to_virt(pdp[j] & ~0xFFFULL);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & VMM_FLAG_PRESENT) {
                            if (pd[k] & VMM_FLAG_HUGE_2M) continue;

                            uint64_t* pt = (uint64_t*)mm_phys_to_virt(pd[k] & ~0xFFFULL);
                            pmm_free_page((uint64_t)mm_virt_to_phys((uint64_t)pt));
                        }
                    }
                    pmm_free_page((uint64_t)mm_virt_to_phys((uint64_t)pd));
                }
            }
            pmm_free_page((uint64_t)mm_virt_to_phys((uint64_t)pdp));
        }
    }
    pmm_free_page(pml4_phys);
}

static int vmm_map_page_impl(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);

    uint64_t* pdp = get_next_level(pml4, get_pml4_index(virt), 1, flags);
    if (!pdp) return -1;

    uint16_t pdp_i = get_pdp_index(virt);
    uint64_t* pd;
    if ((pdp[pdp_i] & VMM_FLAG_PRESENT) && (pdp[pdp_i] & VMM_FLAG_HUGE_1G)) {
        pd = vmm_split_huge(&pdp[pdp_i], 3);
    } else {
        pd = get_next_level(pdp, pdp_i, 1, flags);
    }
    if (!pd) return -1;

    uint16_t pd_i = get_pd_index(virt);
    uint64_t* pt;
    if ((pd[pd_i] & VMM_FLAG_PRESENT) && (pd[pd_i] & VMM_FLAG_HUGE_2M)) {
        pt = vmm_split_huge(&pd[pd_i], 2);
    } else {
        pt = get_next_level(pd, pd_i, 1, flags);
    }
    if (!pt) return -1;

    pt[get_pt_index(virt)] = (phys & ~0xFFFULL) | flags;
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
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);

    uint64_t* pdp = get_next_level(pml4, get_pml4_index(virt), 0, 0);
    if (!pdp) return;

    uint64_t* pd = get_next_level(pdp, get_pdp_index(virt), 0, 0);
    if (!pd) return;

    uint64_t* pt = get_next_level(pd, get_pd_index(virt), 0, 0);
    if (!pt) return;

    pt[get_pt_index(virt)] = 0;
    vmm_invlpg(virt);
}

void vmm_unmap_page(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    vmm_unmap_page_impl(pml4_phys, virt);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
}

static uint64_t vmm_get_phys_impl(uint64_t pml4_phys, uint64_t virt) {
    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);

    uint64_t* pdp = get_next_level(pml4, get_pml4_index(virt), 0, 0);
    if (!pdp) return 0;

    uint64_t* pd = get_next_level(pdp, get_pdp_index(virt), 0, 0);
    if (!pd) return 0;

    uint64_t* pt = get_next_level(pd, get_pd_index(virt), 0, 0);
    if (!pt) return 0;

    uint64_t entry = pt[get_pt_index(virt)];
    if (!(entry & VMM_FLAG_PRESENT)) return 0;

    return (entry & ~0xFFFULL) + (virt & 0xFFF);
}

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t p = vmm_get_phys_impl(pml4_phys, virt);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return p;
}

int vmm_is_mapped(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);

    uint64_t* pml4 = (uint64_t*)mm_phys_to_virt(pml4_phys);
    uint64_t e;

    e = pml4[get_pml4_index(virt)];
    if (!(e & VMM_FLAG_PRESENT)) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 0; }
    if (e & VMM_FLAG_HUGE_2M) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 1; }

    uint64_t* pdp = (uint64_t*)mm_phys_to_virt(e & ~0xFFFULL);
    e = pdp[get_pdp_index(virt)];
    if (!(e & VMM_FLAG_PRESENT)) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 0; }
    if (e & VMM_FLAG_HUGE_2M) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 1; }

    uint64_t* pd = (uint64_t*)mm_phys_to_virt(e & ~0xFFFULL);
    e = pd[get_pd_index(virt)];
    if (!(e & VMM_FLAG_PRESENT)) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 0; }
    if (e & VMM_FLAG_HUGE_2M) { spin_unlock_irqrestore(&vmm_lock, irq_flags); return 1; }

    uint64_t* pt = (uint64_t*)mm_phys_to_virt(e & ~0xFFFULL);
    e = pt[get_pt_index(virt)];
    int r = (e & VMM_FLAG_PRESENT) ? 1 : 0;
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return r;
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

#define VMM_MMIO_WINDOW_BASE 0xFFFFA00000000000ULL
#define VMM_MMIO_WINDOW_SIZE (256ULL * 1024 * 1024)
#define VMM_MMIO_MAX_ENTRIES 32

static uint64_t mmio_next_virt = VMM_MMIO_WINDOW_BASE;

static struct {
    uint64_t phys_base;
    uint64_t virt_base;
    uint64_t size;
} mmio_map_cache[VMM_MMIO_MAX_ENTRIES];
static int mmio_map_count = 0;

uint64_t vmm_map_mmio(uint64_t phys, uint64_t size, uint64_t extra_flags) {
    if (!phys || !size) return 0;

    uint64_t page_off = phys & (PAGE_SIZE - 1);
    uint64_t phys_base = phys - page_off;
    uint64_t map_size = (size + page_off + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);

    for (int i = 0; i < mmio_map_count; i++) {
        if (mmio_map_cache[i].phys_base == phys_base &&
            mmio_map_cache[i].size >= map_size) {
            uint64_t v = mmio_map_cache[i].virt_base + page_off;
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return v;
        }
    }

    if (mmio_next_virt + map_size > VMM_MMIO_WINDOW_BASE + VMM_MMIO_WINDOW_SIZE) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }

    uint64_t virt_base = mmio_next_virt;
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_GLOBAL |
                     VMM_FLAG_PCD | VMM_FLAG_PWT | VMM_FLAG_NX | extra_flags;

    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (vmm_map_page_impl(kernel_pml4_phys, virt_base + off, phys_base + off, flags) != 0) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }
    }

    mmio_next_virt = virt_base + map_size + PAGE_SIZE;

    if (mmio_map_count < VMM_MMIO_MAX_ENTRIES) {
        mmio_map_cache[mmio_map_count].phys_base = phys_base;
        mmio_map_cache[mmio_map_count].virt_base = virt_base;
        mmio_map_cache[mmio_map_count].size = map_size;
        mmio_map_count++;
    }

    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return virt_base + page_off;
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
            pmm_free_page(phys);
            vmm_unmap_page_impl(current_pml4, curr_virt);
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
}
