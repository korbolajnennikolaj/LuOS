#include "kernel/limine.h"

#include <stddef.h>

__attribute__((used, section(".requests")))
volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};

__attribute__((used, section(".requests")))
static volatile void *limine_requests[] = {
    (void *)&framebuffer_request,
    (void *)&hhdm_req,
    (void *)&kernel_address_request,
    (void *)&rsdp_request,
    (void *)&smp_request,
    NULL
};

__attribute__((used, section(".requests")))
volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

struct limine_framebuffer_request* get_framebuffer_request() {
    return (struct limine_framebuffer_request*)&framebuffer_request;
}

struct limine_smp_request* get_smp_request() {
    return (struct limine_smp_request*)&smp_request;
}
