// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/panic.h"
#include "components/GDT/gdt.h"

#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"
#include "kernel/smp/smp.h"

#include "kernel/programs/programs.h"
#include "kernel/kernel_info.h"
#include "kernel/lkpi/lkpi.h"

#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/Memory/heap.h"
#include "components/Memory/pat.h"
#include "kernel/proc/initfs.h"
#include "kernel/smp/tlb.h"

#include <cpuid.h>
#include <math.h>
#include <ports.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HEAP_INITIAL_BYTES (64ULL * 1024 * 1024)

void _start(void){
    init_gdt();

    fpu_init();

    logger_init();
    LOG_INFO("LuOS kernel %s (built %s, %s)", KERNEL_VERSION, KERNEL_COMPILATION_DATE, KERNEL_COMPILER);
    LOG_DEBUG("GDT loaded, FPU/SSE enabled");

    pat_init();
    pmm_init();
    vmm_init();
    pmm_dma32_pool_init(PMM_DMA32_POOL_PAGES);

    uint64_t free_pages = pmm_free_page_count();
    uint64_t heap_pages_count = HEAP_INITIAL_BYTES / 4096;
    if (heap_pages_count > free_pages / 2) heap_pages_count = free_pages / 2;
    uint64_t heap_size = heap_pages_count * 4096;
    uint64_t heap_virt = VMM_HEAP_BASE;

    for (uint64_t i = 0; i < heap_pages_count; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            heap_size = i * 4096;
            break;
        }
        vmm_map_page(vmm_kernel_pml4(), heap_virt + (i * 4096), phys, 0x03);
    }

    heap_init(heap_virt, heap_size);
    heap_enable_growth(VMM_HEAP_LIMIT);
    initfs_init();
    LOG_INFO("Heap at 0x%llx, %llu KiB mapped now, grows on demand (%llu free pages left)",
             (unsigned long long)heap_virt, (unsigned long long)(heap_size / 1024),
             (unsigned long long)pmm_free_page_count());

    init_drivers();
    tlb_init();

    scheduler_init();
    smp_init();
    scheduler_start();
    LOG_INFO("Scheduler started on %d core(s)", scheduler_core_count());

    stdio_register_video_stream();

    lkpi_boot();

    LOG_INFO("Starting service manager");
    start_service_manager();

    scheduler_idle_loop();
}