#include "pmm.h"

#include "components/logger.h"
#include "kernel/limine.h"
#include "kernel/scheduler/spinlock.h"

extern volatile struct limine_memmap_request memmap_req;
extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_kernel_address_request kernel_address_request;

static spinlock_t pmm_lock = SPINLOCK_INIT;

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
    if (!mm) {
        LOG_ERROR("bootloader provided no memory map");
        return;
    }

    uint64_t highest_addr = 0;
    pmm_usable_total = 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint64_t end = e->base + e->length;

        LOG_DEBUG("memmap 0x%016llx-0x%016llx type %llu (%llu KiB)",
                  (unsigned long long)e->base, (unsigned long long)(end - 1),
                  (unsigned long long)e->type, (unsigned long long)(e->length / 1024));

        if (e->type == LIMINE_MEMMAP_USABLE ||
            e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            pmm_usable_total += e->length / PAGE_SIZE;
            if (end > highest_addr) highest_addr = end;
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

    if (!pmm_bitmap) {
        LOG_ERROR("no usable region large enough for the %llu byte frame bitmap",
                  (unsigned long long)pmm_bitmap_size);
        return;
    }

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

    LOG_INFO("PMM: %llu memmap entries, highest address 0x%llx, %llu MiB usable, %llu MiB free, bitmap %llu bytes",
             (unsigned long long)mm->entry_count, (unsigned long long)highest_addr,
             (unsigned long long)(pmm_usable_total * PAGE_SIZE / (1024 * 1024)),
             (unsigned long long)(pmm_free * PAGE_SIZE / (1024 * 1024)),
             (unsigned long long)pmm_bitmap_size);
}

static uint64_t pmm_alloc_page_impl(void) {
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

uint64_t pmm_alloc_page(void) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t p = pmm_alloc_page_impl();
    spin_unlock_irqrestore(&pmm_lock, flags);
    if (p == PMM_ALLOC_FAIL) LOG_WARNING("out of physical pages");
    return p;
}

static uint64_t pmm_alloc_run_locked(uint64_t count, uint64_t limit_frame) {
    uint64_t consecutive = 0;
    uint64_t start_frame = 0;
    uint64_t end = pmm_total < limit_frame ? pmm_total : limit_frame;

    for (uint64_t frame = 1; frame < end; frame++) {
        if ((frame % BITS_PER_ENTRY) == 0 && pmm_bitmap[frame / BITS_PER_ENTRY] == 0xFFFFFFFFFFFFFFFFULL &&
            frame + BITS_PER_ENTRY <= end) {
            consecutive = 0;
            frame += BITS_PER_ENTRY - 1;
            continue;
        }
        if (!_bitmap_test(frame)) {
            if (consecutive == 0) start_frame = frame;
            consecutive++;
            if (consecutive == count) {
                for (uint64_t i = start_frame; i < start_frame + count; i++) _bitmap_set(i);
                pmm_free -= count;
                return PAGE_TO_PHYS(start_frame);
            }
        } else {
            consecutive = 0;
        }
    }
    return PMM_ALLOC_FAIL;
}

uint64_t pmm_alloc_pages_below(uint64_t count, uint64_t limit) {
    if (!pmm_bitmap || count == 0 || pmm_free < count) return PMM_ALLOC_FAIL;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t p = pmm_alloc_run_locked(count, PHYS_TO_PAGE(limit));
    spin_unlock_irqrestore(&pmm_lock, flags);
    return p;
}

#define PMM_DMA32_POOL_MAX PMM_DMA32_POOL_PAGES

static uint64_t dma32_base = 0;
static uint64_t dma32_pages = 0;
static uint64_t dma32_free = 0;
static uint64_t dma32_bits[PMM_DMA32_POOL_MAX / 64];
static spinlock_t dma32_lock = SPINLOCK_INIT;

void pmm_dma32_pool_init(uint64_t pages) {
    if (dma32_pages) return;
    if (pages > PMM_DMA32_POOL_MAX) pages = PMM_DMA32_POOL_MAX;
    for (uint64_t try_pages = pages; try_pages >= 64; try_pages /= 2) {
        uint64_t phys = pmm_alloc_pages_below(try_pages, PMM_DMA32_LIMIT);
        if (!phys) continue;
        dma32_base = phys;
        dma32_pages = try_pages;
        dma32_free = try_pages;
        for (uint64_t i = 0; i < PMM_DMA32_POOL_MAX / 64; i++) dma32_bits[i] = 0;
        LOG_INFO("32-bit DMA pool: %llu KiB at 0x%llx", (unsigned long long)(try_pages * PAGE_SIZE / 1024),
                 (unsigned long long)phys);
        return;
    }
    LOG_WARNING("no memory below 4 GiB for the 32-bit DMA pool");
}

uint64_t pmm_alloc_dma32_pages(uint64_t count) {
    if (count == 0) return PMM_ALLOC_FAIL;
    if (dma32_pages && count <= dma32_free) {
        uint64_t flags = spin_lock_irqsave(&dma32_lock);
        uint64_t run = 0, start = 0;
        for (uint64_t i = 0; i < dma32_pages; i++) {
            if (dma32_bits[i / 64] & (1ULL << (i % 64))) {
                run = 0;
                continue;
            }
            if (run == 0) start = i;
            if (++run == count) {
                for (uint64_t k = start; k < start + count; k++) dma32_bits[k / 64] |= 1ULL << (k % 64);
                dma32_free -= count;
                spin_unlock_irqrestore(&dma32_lock, flags);
                return dma32_base + start * PAGE_SIZE;
            }
        }
        spin_unlock_irqrestore(&dma32_lock, flags);
    }
    uint64_t p = pmm_alloc_pages_below(count, PMM_DMA32_LIMIT);
    if (!p) LOG_WARNING("cannot allocate %llu page(s) below 4 GiB (pool %llu/%llu free)", (unsigned long long)count,
                        (unsigned long long)dma32_free, (unsigned long long)dma32_pages);
    return p;
}

void pmm_free_dma32_pages(uint64_t phys, uint64_t count) {
    if (!phys || !count) return;
    if (dma32_pages && phys >= dma32_base && phys < dma32_base + dma32_pages * PAGE_SIZE) {
        uint64_t flags = spin_lock_irqsave(&dma32_lock);
        uint64_t first = (phys - dma32_base) / PAGE_SIZE;
        for (uint64_t k = first; k < first + count && k < dma32_pages; k++) {
            if (dma32_bits[k / 64] & (1ULL << (k % 64))) {
                dma32_bits[k / 64] &= ~(1ULL << (k % 64));
                dma32_free++;
            }
        }
        spin_unlock_irqrestore(&dma32_lock, flags);
        return;
    }
    pmm_free_pages(phys, count);
}

uint64_t pmm_dma32_pool_total(void) { return dma32_pages; }
uint64_t pmm_dma32_pool_free(void) { return dma32_free; }

uint64_t pmm_alloc_pages(uint64_t count) {
    if (!pmm_bitmap || count == 0 || pmm_free < count) {
        LOG_WARNING("cannot allocate %llu page(s), %llu free", (unsigned long long)count, (unsigned long long)pmm_free);
        return PMM_ALLOC_FAIL;
    }

    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    if (count == 1) {
        uint64_t p = pmm_alloc_page_impl();
        spin_unlock_irqrestore(&pmm_lock, flags);
        return p;
    }

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
                spin_unlock_irqrestore(&pmm_lock, flags);
                return PAGE_TO_PHYS(start_frame);
            }
        } else {
            consecutive = 0;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    LOG_WARNING("no %llu contiguous free pages (%llu free in total)",
                (unsigned long long)count, (unsigned long long)pmm_free);
    return PMM_ALLOC_FAIL;
}

static void pmm_free_page_impl(uint64_t phys) {
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

void pmm_free_page(uint64_t phys) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    pmm_free_page_impl(phys);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_pages(uint64_t phys, uint64_t count) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    for (uint64_t i = 0; i < count; i++)
        pmm_free_page_impl(phys + i * PAGE_SIZE);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_total_pages(void) { return pmm_usable_total; }
uint64_t pmm_free_page_count(void) { return pmm_free; }
uint64_t pmm_used_pages(void) { return pmm_usable_total - pmm_free; }

uint64_t pmm_phys_to_virt(uint64_t phys) { return _phys_to_virt(phys); }
uint64_t pmm_virt_to_phys(uint64_t virt) { return _virt_to_phys(virt); }

uint64_t pmm_get_free_memory(void) {
    return pmm_free * PAGE_SIZE;
}
