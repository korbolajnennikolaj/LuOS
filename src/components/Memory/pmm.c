#include "pmm.h"

#include "kernel/limine.h"

extern volatile struct limine_memmap_request memmap_req;
extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_kernel_address_request kernel_address_request;

#define BITS_PER_ENTRY 64ULL
#define BITMAP_ENTRY_T uint64_t

#define PMM_ALLOC_FAIL 0ULL

static BITMAP_ENTRY_T *pmm_bitmap = NULL;
static uint64_t pmm_bitmap_size = 0;
static uint64_t pmm_total = 0;
static uint64_t pmm_free = 0;
static uint64_t pmm_usable_total = 0;
static uint64_t pmm_last_idx = 0;
static uint64_t pmm_hhdm_offset = 0;

static inline uint64_t _phys_to_virt(uint64_t phys) { return phys + pmm_hhdm_offset; }
static inline uint64_t _virt_to_phys(uint64_t virt) { return virt - pmm_hhdm_offset; }

static inline void _bitmap_set(uint64_t frame) {
    pmm_bitmap[frame / BITS_PER_ENTRY] |= (1ULL << (frame % BITS_PER_ENTRY));
}

static inline void _bitmap_clear(uint64_t frame) {
    pmm_bitmap[frame / BITS_PER_ENTRY] &= ~(1ULL << (frame % BITS_PER_ENTRY));
}

static inline int _bitmap_test(uint64_t frame) {
    return (pmm_bitmap[frame / BITS_PER_ENTRY] >> (frame % BITS_PER_ENTRY)) & 1;
}

static void _bitmap_fill(void) {
    uint64_t entries = (pmm_total + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY;
    for (uint64_t i = 0; i < entries; i++)
        pmm_bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
}

static void _pmm_free_region(uint64_t base, uint64_t length) {
    uint64_t start = PAGE_ALIGN(base);
    uint64_t end = PAGE_ALIGN_DOWN(base + length);
    if (start >= end) return;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t frame = PHYS_TO_PAGE(addr);
        if (frame >= pmm_total) break;
        if (_bitmap_test(frame)) {
            _bitmap_clear(frame);
            pmm_free++;
        }
    }
}

static void _pmm_reserve_region(uint64_t base, uint64_t length) {
    uint64_t start = PAGE_ALIGN_DOWN(base);
    uint64_t end = PAGE_ALIGN(base + length);
    if (start >= end) return;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t frame = PHYS_TO_PAGE(addr);
        if (frame >= pmm_total) break;
        if (!_bitmap_test(frame)) {
            _bitmap_set(frame);
            if (pmm_free > 0) pmm_free--;
        }
    }
}

void pmm_init(void) {
    pmm_hhdm_offset = hhdm_req.response ? hhdm_req.response->offset : 0xffff800000000000ULL;
    struct limine_memmap_response *mm = memmap_req.response;
    if (!mm) return;

    uint64_t highest_addr = 0;
    pmm_usable_total = 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t end = e->base + e->length;
        if (end > highest_addr) highest_addr = end;

        if (e->type == LIMINE_MEMMAP_USABLE ||
            e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            pmm_usable_total += e->length / PAGE_SIZE;
        }
    }

    pmm_total = PHYS_TO_PAGE(PAGE_ALIGN(highest_addr));
    pmm_bitmap_size = ((pmm_total + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY) * sizeof(BITMAP_ENTRY_T);

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->length >= pmm_bitmap_size) {
            pmm_bitmap = (BITMAP_ENTRY_T *)_phys_to_virt(e->base);
            break;
        }
    }

    if (!pmm_bitmap) return;

    pmm_free = 0;
    _bitmap_fill();

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE)
            _pmm_free_region(e->base, e->length);
    }

    _pmm_reserve_region(_virt_to_phys((uint64_t)pmm_bitmap), pmm_bitmap_size);

    if (kernel_address_request.response) {
        _pmm_reserve_region(kernel_address_request.response->physical_base, 8 * 1024 * 1024);
    }

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_KERNEL_AND_MODULES ||
            e->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            _pmm_reserve_region(e->base, e->length);
        }
    }

    pmm_last_idx = 0;
}

uint64_t pmm_alloc_page(void) {
    if (!pmm_bitmap || pmm_free == 0) return PMM_ALLOC_FAIL;

    uint64_t entries = (pmm_total + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY;

    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = (pass == 0) ? pmm_last_idx : 0;
        uint64_t end = (pass == 0) ? entries : pmm_last_idx;

        for (uint64_t ei = start; ei < end; ei++) {
            BITMAP_ENTRY_T entry = pmm_bitmap[ei];
            if (entry == 0xFFFFFFFFFFFFFFFFULL) continue;

            uint64_t bit = 0;
            BITMAP_ENTRY_T inv = ~entry;

            bit = (uint64_t)__builtin_ctzll(inv);

            uint64_t frame = ei * BITS_PER_ENTRY + bit;
            if (frame >= pmm_total) continue;

            _bitmap_set(frame);
            pmm_free--;
            pmm_last_idx = ei;
            return PAGE_TO_PHYS(frame);
        }
    }

    return PMM_ALLOC_FAIL;
}

uint64_t pmm_alloc_pages(uint64_t count) {
    if (!pmm_bitmap || count == 0 || pmm_free < count) return PMM_ALLOC_FAIL;
    if (count == 1) return pmm_alloc_page();

    uint64_t consecutive = 0;
    uint64_t start_frame = 0;

    for (uint64_t frame = 0; frame < pmm_total; frame++) {
        if (!_bitmap_test(frame)) {
            if (consecutive == 0) start_frame = frame;
            consecutive++;
            if (consecutive == count) {

                for (uint64_t i = start_frame; i < start_frame + count; i++)
                    _bitmap_set(i);
                pmm_free -= count;
                pmm_last_idx = (start_frame + count) / BITS_PER_ENTRY;
                return PAGE_TO_PHYS(start_frame);
            }
        } else {
            consecutive = 0;
        }
    }

    return PMM_ALLOC_FAIL;
}

void pmm_free_page(uint64_t phys) {
    if (!pmm_bitmap || phys == 0) return;
    uint64_t frame = PHYS_TO_PAGE(PAGE_ALIGN_DOWN(phys));
    if (frame >= pmm_total) return;
    if (_bitmap_test(frame)) {
        _bitmap_clear(frame);
        pmm_free++;

        uint64_t ei = frame / BITS_PER_ENTRY;
        if (ei < pmm_last_idx) pmm_last_idx = ei;
    }
}

void pmm_free_pages(uint64_t phys, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

uint64_t pmm_total_pages(void) { return pmm_usable_total; }
uint64_t pmm_free_page_count(void) { return pmm_free; }
uint64_t pmm_used_pages(void) { return pmm_usable_total - pmm_free; }

uint64_t pmm_phys_to_virt(uint64_t phys) { return _phys_to_virt(phys); }
uint64_t pmm_virt_to_phys(uint64_t virt) { return _virt_to_phys(virt); }

uint64_t pmm_get_free_memory(void) {
    return pmm_free * PAGE_SIZE;
}
