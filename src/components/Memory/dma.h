// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef DMA_H
#define DMA_H

#include "components/Memory/pat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DMA_LIMIT_ANY 0ULL
#define DMA_LIMIT_32BIT 0x100000000ULL

struct dma_buffer {
    void *virt;
    uint64_t phys;
    uint64_t size;
    uint64_t pages;
    enum vmm_cache_type type;
};

int dma_alloc(uint64_t size, uint64_t align, uint64_t limit, enum vmm_cache_type type, struct dma_buffer *out);
void dma_free(struct dma_buffer *buf);

uint64_t dma_alloc_page(uint64_t limit, enum vmm_cache_type type);
void dma_free_page(uint64_t phys, enum vmm_cache_type type);

void dma_sync_for_device(void *virt, uint64_t size);
void dma_sync_for_cpu(void *virt, uint64_t size);

uint64_t dma_bytes_in_use(void);

#endif
