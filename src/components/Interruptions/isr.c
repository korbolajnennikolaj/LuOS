// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "isr.h"

#include "components/drivers.h"
#include "components/Interruptions/msi.h"
#include "components/logger.h"
#include "components/panic.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"
#include "kernel/proc/process.h"
#include "components/Memory/vmm.h"

static irq_handler_t s_irq_handlers[256];

static struct {
    irq_ctx_handler_t handler;
    void *ctx;
} s_ctx_handlers[256];

static uint8_t s_dyn_used[256];
static uint64_t s_vector_hits[256];
static spinlock_t s_vector_lock = SPINLOCK_INIT;

int irq_alloc_vector(void) {
    int v = -1;
    uint64_t flags = spin_lock_irqsave(&s_vector_lock);
    for (int i = IRQ_VECTOR_DYN_BASE; i <= IRQ_VECTOR_DYN_LAST; i++) {
        if (!s_dyn_used[i] && !s_irq_handlers[i] && !s_ctx_handlers[i].handler) {
            s_dyn_used[i] = 1;
            v = i;
            break;
        }
    }
    spin_unlock_irqrestore(&s_vector_lock, flags);
    if (v < 0) LOG_WARNING("no free interrupt vectors in 0x%02x-0x%02x", IRQ_VECTOR_DYN_BASE, IRQ_VECTOR_DYN_LAST);
    return v;
}

int irq_alloc_vectors(int count, int *out_vectors) {
    int got = 0;
    for (; got < count; got++) {
        int v = irq_alloc_vector();
        if (v < 0) break;
        out_vectors[got] = v;
    }
    if (got < count) {
        for (int i = 0; i < got; i++) irq_free_vector((uint8_t)out_vectors[i]);
        return -1;
    }
    return 0;
}

void irq_free_vector(uint8_t vector) {
    uint64_t flags = spin_lock_irqsave(&s_vector_lock);
    s_ctx_handlers[vector].handler = (irq_ctx_handler_t)0;
    s_ctx_handlers[vector].ctx = (void *)0;
    s_dyn_used[vector] = 0;
    spin_unlock_irqrestore(&s_vector_lock, flags);
}

int irq_register_ctx_handler(uint8_t vector, irq_ctx_handler_t handler, void *ctx) {
    uint64_t flags = spin_lock_irqsave(&s_vector_lock);
    if (s_ctx_handlers[vector].handler && s_ctx_handlers[vector].handler != handler) {
        spin_unlock_irqrestore(&s_vector_lock, flags);
        LOG_WARNING("vector 0x%02x already has a handler", (unsigned)vector);
        return -1;
    }
    s_ctx_handlers[vector].ctx = ctx;
    __atomic_store_n(&s_ctx_handlers[vector].handler, handler, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_vector_lock, flags);
    return 0;
}

void irq_unregister_ctx_handler(uint8_t vector) {
    uint64_t flags = spin_lock_irqsave(&s_vector_lock);
    __atomic_store_n(&s_ctx_handlers[vector].handler, (irq_ctx_handler_t)0, __ATOMIC_RELEASE);
    s_ctx_handlers[vector].ctx = (void *)0;
    spin_unlock_irqrestore(&s_vector_lock, flags);
}

uint64_t irq_vector_hits(uint8_t vector) {
    return __atomic_load_n(&s_vector_hits[vector], __ATOMIC_RELAXED);
}

int irq_dyn_vectors_used(void) {
    int n = 0;
    for (int i = IRQ_VECTOR_DYN_BASE; i <= IRQ_VECTOR_DYN_LAST; i++) n += s_dyn_used[i] ? 1 : 0;
    return n;
}

void irq_register_handler(uint8_t vector, irq_handler_t handler) {
    if (s_irq_handlers[vector] && s_irq_handlers[vector] != handler)
        LOG_WARNING("vector 0x%02x handler replaced (%p -> %p)", (unsigned)vector,
                    (void *)s_irq_handlers[vector], (void *)handler);
    s_irq_handlers[vector] = handler;
    LOG_DEBUG("vector 0x%02x handler %p registered", (unsigned)vector, (void *)handler);
}

void irq_unregister_handler(uint8_t vector) {
    s_irq_handlers[vector] = (irq_handler_t)0;
    LOG_DEBUG("vector 0x%02x handler unregistered", (unsigned)vector);
}

extern void apic_send_eoi(void);

static inline uint64_t read_cr2(void) {
    uint64_t val;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

extern void *scheduler_set_isr_fx(void *fx);

static void isr_dispatch(struct registers *regs);

static void isr_user_return_check(struct registers *regs, struct task *entry_task)
{
    if ((regs->cs & 3) != 3 || !entry_task || !entry_task->proc) return;
    if (current_task() != entry_task) return;
    proc_check_pending();
}

static volatile int s_isr_depth[MAX_CORES];

int isr_in_interrupt(void)
{
    return s_isr_depth[current_core()] > 0;
}

void isr_handler(struct registers *regs, void *fx)
{
    void *prev_fx = scheduler_set_isr_fx(fx);
    struct task *entry_task = (regs->cs & 3) == 3 ? current_task() : NULL;
    s_isr_depth[current_core()]++;
    isr_dispatch(regs);
    s_isr_depth[current_core()]--;
    scheduler_set_isr_fx(prev_fx);
    isr_user_return_check(regs, entry_task);
}

static void isr_dispatch(struct registers *regs)
{
    uint64_t vec = regs->int_no;

    if (vec == 14 && vmm_vmemmap_fault(read_cr2())) return;
    if (vec == 14 && user_fault(regs, read_cr2())) return;
    if (vec < 32 && (regs->cs & 3) == 3) {
        user_exception(regs);
        return;
    }

    if (vec < 32) {

        cpu_regs_t cpu_regs;
        cpu_regs.rax = regs->rax;
        cpu_regs.rbx = regs->rbx;
        cpu_regs.rcx = regs->rcx;
        cpu_regs.rdx = regs->rdx;
        cpu_regs.rsi = regs->rsi;
        cpu_regs.rdi = regs->rdi;
        cpu_regs.rbp = regs->rbp;
        cpu_regs.rsp = regs->rsp;
        cpu_regs.r8 = regs->r8;
        cpu_regs.r9 = regs->r9;
        cpu_regs.r10 = regs->r10;
        cpu_regs.r11 = regs->r11;
        cpu_regs.r12 = regs->r12;
        cpu_regs.r13 = regs->r13;
        cpu_regs.r14 = regs->r14;
        cpu_regs.r15 = regs->r15;
        cpu_regs.rip = regs->rip;
        cpu_regs.rflags = regs->rflags;
        cpu_regs.cr2 = read_cr2();
        cpu_regs.cr3 = read_cr3();
        cpu_regs.err_code = regs->err_code;

        enum panic_code code;
        const char *msg;

        switch (vec) {
            case 8:
                code = PANIC_CODE_DOUBLE_FAULT;
                msg = "Double Fault (#DF)";
                break;
            case 13:
                code = PANIC_CODE_GENERAL;
                msg = "General Protection Fault (#GP)";
                break;
            case 14:
                code = PANIC_CODE_PAGE_FAULT;
                msg = "Page Fault (#PF)";
                break;
            case 6:
                code = PANIC_CODE_INVALID_OPCODE;
                msg = "Invalid Opcode (#UD)";
                break;
            case 12:
                code = PANIC_CODE_STACK_OVERFLOW;
                msg = "Stack-Segment Fault (#SS)";
                break;
            default:
                code = PANIC_CODE_GENERAL;
                msg = "Unhandled CPU Exception";
                break;
        }

        struct panic_info info = { code, msg, &cpu_regs };
        panic(&info);

        for (;;) asm volatile("hlt");
    }

    if (vec == 255) {

        return;
    }

    __atomic_fetch_add(&s_vector_hits[vec], 1, __ATOMIC_RELAXED);

    irq_ctx_handler_t ctx_handler = __atomic_load_n(&s_ctx_handlers[vec].handler, __ATOMIC_ACQUIRE);
    if (ctx_handler) {
        ctx_handler(s_ctx_handlers[vec].ctx, regs);
        apic_send_eoi();
        return;
    }

    if (vec >= MSI_VECTOR_XHCI_BASE && vec < MSI_VECTOR_MAX) {

        if (s_irq_handlers[vec]) {
            s_irq_handlers[vec](regs);
        } else {
            msi_dispatch((uint8_t)vec);
        }
        apic_send_eoi();
        return;
    }

    if (s_irq_handlers[vec]) {
        s_irq_handlers[vec](regs);
    } else {
        static uint8_t unhandled_warned[256];
        if (!unhandled_warned[vec]) {
            unhandled_warned[vec] = 1;
            LOG_WARNING("unhandled interrupt vector 0x%02x on core %d", (unsigned)vec, current_core());
        }
    }

    if (vec != 0x41) apic_send_eoi();
}
