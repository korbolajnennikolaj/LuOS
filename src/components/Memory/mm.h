#ifndef MM_H
#define MM_H

#include "components/Memory/vmm.h"
#include "kernel/limine.h"

#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_rsdp_request rsdp_request;

static inline uint64_t mm_virt_to_phys(uint64_t addr) {
    if (kernel_address_request.response) {
        uint64_t kv = kernel_address_request.response->virtual_base;
        uint64_t kp = kernel_address_request.response->physical_base;

        if (addr >= kv && addr < kv + 0x10000000ULL)
            return addr - kv + kp;
    }
    if (hhdm_req.response) {
        uint64_t off = hhdm_req.response->offset;

        uint64_t walked = vmm_get_phys(vmm_kernel_pml4(), addr);
        if (walked != 0) return walked;

        if (addr >= off)
            return addr - off;
    }
    return addr;
}

static inline uint64_t mm_ptr_to_phys(void *ptr) {
    return mm_virt_to_phys((uint64_t)ptr);
}

static inline uint64_t mm_phys_to_virt(uint64_t phys) {
    uint64_t off = hhdm_req.response ? hhdm_req.response->offset
                                     : 0xffff800000000000ULL;
    return phys + off;
}

#endif
