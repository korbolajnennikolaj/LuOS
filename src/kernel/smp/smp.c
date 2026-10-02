#include "kernel/smp/smp.h"

#include "kernel/limine.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/GDT/gdt.h"
#include "components/Interruptions/idt.h"
#include "components/drivers.h"
#include "drivers/Timer/apic_driver.h"
#include "drivers/Timer/timer.h"
#include "kernel/scheduler/scheduler.h"
#include <math.h>

#include <stdint.h>
#include <stddef.h>

#define AP_STACK_SIZE (32 * 1024)

extern struct limine_smp_request* get_smp_request(void);
extern uint64_t vmm_kernel_pml4(void);

static volatile uint32_t cpus_online = 1;
static volatile uint32_t next_logical_id = 1;
static uint32_t total_cpus = 1;
static volatile uint8_t ap_started[256];
static volatile uint8_t ap_ready[256];
static volatile uint32_t ap_gate_closed = 0;

#define SMP_AP_TIMEOUT_MS 2000

void ap_main(struct limine_smp_info *info) {
    __atomic_store_n(&ap_started[info->lapic_id & 0xFF], 1, __ATOMIC_SEQ_CST);

    load_gdt();
    load_idt();

    if (__atomic_load_n(&ap_gate_closed, __ATOMIC_SEQ_CST)) {
        for (;;) asm volatile("cli; hlt");
    }

    fpu_init();
    apic_enable_this_core();

    uint32_t logical = __atomic_fetch_add(&next_logical_id, 1, __ATOMIC_SEQ_CST);
    scheduler_register_core(apic_get_lapic_id(), (int)logical);
    LOG_DEBUG("AP with LAPIC %u online as core %u", (unsigned)apic_get_lapic_id(), logical);
    __atomic_store_n(&ap_ready[info->lapic_id & 0xFF], 1, __ATOMIC_SEQ_CST);

    __atomic_fetch_add(&cpus_online, 1, __ATOMIC_SEQ_CST);

    scheduler_start();

    asm volatile("sti");
    scheduler_idle_loop();
}

__attribute__((naked)) static void ap_trampoline(struct limine_smp_info *info __attribute__((unused))) {
    asm volatile(
        "push %rdi\n\t"
        "push %rax\n\t"
        "call vmm_kernel_pml4\n\t"
        "mov %rax, %rcx\n\t"
        "pop %rax\n\t"
        "pop %rdi\n\t"
        "mov %rcx, %cr3\n\t"

        "mov 24(%rdi), %rsp\n\t"
        "jmp ap_main\n\t"
    );
}

void smp_init(void) {
    struct limine_smp_request *req = get_smp_request();
    if (!req || !req->response) {
        LOG_WARNING("bootloader provided no SMP information, running on the BSP only");
        return;
    }

    struct limine_smp_response *resp = req->response;
    total_cpus = (uint32_t)resp->cpu_count;
    if (total_cpus <= 1) {
        LOG_INFO("single CPU system, BSP LAPIC %u", (unsigned)resp->bsp_lapic_id);
        return;
    }

    LOG_INFO("starting %u application processor(s), BSP LAPIC %u",
             total_cpus - 1, (unsigned)resp->bsp_lapic_id);

    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (cpu->lapic_id == resp->bsp_lapic_id) continue;

        void *stack = kmalloc(AP_STACK_SIZE);
        if (!stack) {
            LOG_ERROR("no memory for AP stack, LAPIC %u stays offline", (unsigned)cpu->lapic_id);
            continue;
        }

        cpu->extra_argument = (uint64_t)stack + AP_STACK_SIZE;
        __atomic_store_n(&cpu->goto_address, ap_trampoline, __ATOMIC_SEQ_CST);
    }

    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    int use_tsc = tsc && tsc->get_tsc_uptime_ms && tsc->get_tsc_ticks_per_ms && tsc->get_tsc_ticks_per_ms();
    uint64_t deadline = use_tsc ? tsc->get_tsc_uptime_ms() + SMP_AP_TIMEOUT_MS : 0;
    uint64_t timeout = 200000000ull;
    while (__atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST) < total_cpus) {
        if (use_tsc) {
            if (tsc->get_tsc_uptime_ms() >= deadline) break;
        } else if (!timeout--) {
            break;
        }
        asm volatile("pause");
    }

    uint32_t online = __atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST);
    if (online < total_cpus) {
        __atomic_store_n(&ap_gate_closed, 1, __ATOMIC_SEQ_CST);
        if (use_tsc) {
            uint64_t settle = tsc->get_tsc_uptime_ms() + 50;
            while (tsc->get_tsc_uptime_ms() < settle) asm volatile("pause");
        }
        online = __atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST);
    }
    if (online < total_cpus) {
        LOG_WARNING("only %u of %u CPUs came online within %u ms", online, total_cpus, (unsigned)SMP_AP_TIMEOUT_MS);
        for (uint64_t i = 0; i < resp->cpu_count; i++) {
            struct limine_smp_info *cpu = resp->cpus[i];
            if (cpu->lapic_id == resp->bsp_lapic_id) continue;
            if (!__atomic_load_n(&ap_started[cpu->lapic_id & 0xFF], __ATOMIC_SEQ_CST))
                LOG_WARNING("AP with LAPIC %u never reached ap_main", (unsigned)cpu->lapic_id);
            else if (!__atomic_load_n(&ap_ready[cpu->lapic_id & 0xFF], __ATOMIC_SEQ_CST))
                LOG_WARNING("AP with LAPIC %u entered ap_main but did not finish bring-up", (unsigned)cpu->lapic_id);
        }
    } else
        LOG_INFO("all %u CPUs online", online);
}

int smp_cpu_count(void) {
    return (int)__atomic_load_n(&cpus_online, __ATOMIC_SEQ_CST);
}
