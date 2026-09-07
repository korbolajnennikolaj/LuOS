#ifndef PMM_H
#define PMM_H

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) (((uint64_t)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PHYS_TO_PAGE(x) ((uint64_t)(x) >> PAGE_SHIFT)
#define PAGE_TO_PHYS(x) ((uint64_t)(x) << PAGE_SHIFT)

void pmm_init(void);

uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(uint64_t count);

void pmm_free_page(uint64_t phys);
void pmm_free_pages(uint64_t phys, uint64_t count);

uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);
uint64_t pmm_used_pages(void);

uint64_t pmm_phys_to_virt(uint64_t phys);
uint64_t pmm_virt_to_phys(uint64_t virt);

uint64_t pmm_get_free_memory(void);

#endif
