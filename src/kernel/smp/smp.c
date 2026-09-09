#include "kernel/smp/smp.h"

#include "kernel/limine.h"
#include "components/Memory/heap.h"
#include "components/Interruptions/idt.h"
#include "drivers/Timer/apic_driver.h"
#include "kernel/sched/sched.h"

#include <stdint.h>
#include <stddef.h>

#define AP_STACK_SIZE (32 * 1024)

extern struct limine_smp_request* get_smp_request(void);

static volatile uint32_t cpus_online = 1;
static volatile uint32_t next_logical_id = 1;
static uint32_t total_cpus = 1;

void ap_main(struct limine_smp_info *info) {
    (void)info;

    load_idt();
    apic_enable_this_core();

    uint32_t logical = __atomic_fetch_add(&next_logical_id, 1, __ATOMIC_SEQ_CST);
    sched_register_core(apic_get_lapic_id(), (int)logical);

    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_SEQ_CST);

    sched_start();

    asm volatile("sti");
    for (;;)
        asm volatile("hlt");
}

__attribute__((naked)) static void ap_trampoline(struct limine_smp_info *info __attribute__((unused))) {
    asm volatile(
        "mov 24(%rdi), %rsp\n\t"
        "jmp ap_main\n\t"
    );
}

void smp_init(void) {
    struct limine_smp_request *req = get_smp_request();
    if (!req || !req->response) return;

    struct limine_smp_response *resp = req->response;
    total_cpus = (uint32_t)resp->cpu_count;
    if (total_cpus <= 1) return;

    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (cpu->lapic_id == resp->bsp_lapic_id) continue;

        void *stack = kmalloc(AP_STACK_SIZE);
        if (!stack) continue;

        cpu->extra_argument = (uint64_t)stack + AP_STACK_SIZE;
        __atomic_store_n(&cpu->goto_address, ap_trampoline, __ATOMIC_SEQ_CST);
    }

    uint64_t timeout = 200000000ull;
    while (__atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST) < total_cpus && timeout--)
        asm volatile("pause");
}

int smp_cpu_count(void) {
    return (int)__atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST);
}
