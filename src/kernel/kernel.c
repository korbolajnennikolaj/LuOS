#include "kernel.h"

#include "components/drivers.h"
#include "components/panic.h"
#include "components/GDT/gdt.h"

#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"
#include "kernel/smp/smp.h"

#include "kernel/programs/programs.h"

#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/Memory/heap.h"

#include <cpuid.h>
#include <math.h>
#include <ports.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void _start(void){
    init_gdt();

    fpu_init();
    pmm_init();
    vmm_init();

    uint64_t free_pages = pmm_free_page_count();
    uint64_t heap_pages_count = (free_pages * 75) / 100;
    uint64_t heap_size = heap_pages_count * 4096;
    uint64_t heap_virt = 0xFFFF900000000000;

    for (uint64_t i = 0; i < heap_pages_count; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) break;
        vmm_map_page(vmm_kernel_pml4(), heap_virt + (i * 4096), phys, 0x03);
    }

    heap_init(heap_virt, heap_size);

    init_drivers();

    scheduler_init();
    smp_init();
    scheduler_start();

    stdio_register_video_stream();

    start_service_manager();

    while (1) {
        asm volatile("hlt");
    }
}