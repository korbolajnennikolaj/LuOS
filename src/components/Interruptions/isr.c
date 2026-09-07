#include "isr.h"

#include "components/drivers.h"
#include "components/Interruptions/msi.h"
#include "components/panic.h"

static irq_handler_t s_irq_handlers[256];

void irq_register_handler(uint8_t vector, irq_handler_t handler) {
    s_irq_handlers[vector] = handler;
}

void irq_unregister_handler(uint8_t vector) {
    s_irq_handlers[vector] = (irq_handler_t)0;
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

void isr_handler(struct registers *regs)
{
    uint64_t vec = regs->int_no;

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
    }

    apic_send_eoi();
}
