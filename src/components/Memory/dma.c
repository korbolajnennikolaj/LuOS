// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "dma.h"

#include "components/logger.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"

#include <string.h>

static volatile uint64_t dma_in_use = 0;

static uint64_t dma_order_align_pages(uint64_t align) {
    if (align <= PAGE_SIZE) return 1;
    return (align + PAGE_SIZE - 1) / PAGE_SIZE;
}

int dma_alloc(uint64_t size, uint64_t align, uint64_t limit, enum vmm_cache_type type, struct dma_buffer *out) {
    if (!out || !size) return -1;
    if (align & (align - 1)) return -1;

    uint64_t pages = PAGE_ALIGN(size) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages_aligned(pages, dma_order_align_pages(align), limit);
    if (!phys) {
        LOG_WARNING("cannot allocate %llu contiguous page(s) (align %llu, limit 0x%llx)", (unsigned long long)pages,
                    (unsigned long long)align, (unsigned long long)limit);
        return -1;
    }

    void *virt = (void *)mm_phys_to_virt(phys);
    memset(virt, 0, pages * PAGE_SIZE);

    if (type != VMM_CACHE_WB) {
        if (vmm_set_cache_type((uint64_t)virt, pages * PAGE_SIZE, type) != 0) {
            vmm_set_cache_type((uint64_t)virt, pages * PAGE_SIZE, VMM_CACHE_WB);
            pmm_free_pages(phys, pages);
            LOG_WARNING("cannot switch DMA buffer 0x%llx to %s", (unsigned long long)phys, pat_type_name(type));
            return -1;
        }
    }

    out->virt = virt;
    out->phys = phys;
    out->size = size;
    out->pages = pages;
    out->type = type;
    __atomic_fetch_add(&dma_in_use, pages * PAGE_SIZE, __ATOMIC_RELAXED);
    return 0;
}

void dma_free(struct dma_buffer *buf) {
    if (!buf || !buf->phys) return;
    if (buf->type != VMM_CACHE_WB)
        vmm_set_cache_type((uint64_t)buf->virt, buf->pages * PAGE_SIZE, VMM_CACHE_WB);
    pmm_free_pages(buf->phys, buf->pages);
    __atomic_fetch_sub(&dma_in_use, buf->pages * PAGE_SIZE, __ATOMIC_RELAXED);
    buf->virt = NULL;
    buf->phys = 0;
    buf->pages = 0;
}

uint64_t dma_alloc_page(uint64_t limit, enum vmm_cache_type type) {
    struct dma_buffer b;
    if (dma_alloc(PAGE_SIZE, PAGE_SIZE, limit, type, &b) != 0) return 0;
    return b.phys;
}

void dma_free_page(uint64_t phys, enum vmm_cache_type type) {
    struct dma_buffer b = { (void *)mm_phys_to_virt(phys), phys, PAGE_SIZE, 1, type };
    dma_free(&b);
}

void dma_sync_for_device(void *virt, uint64_t size) {
    (void)virt;
    (void)size;
    asm volatile("sfence" ::: "memory");
}

void dma_sync_for_cpu(void *virt, uint64_t size) {
    (void)virt;
    (void)size;
    asm volatile("lfence" ::: "memory");
}

uint64_t dma_bytes_in_use(void) {
    return __atomic_load_n(&dma_in_use, __ATOMIC_RELAXED);
}
