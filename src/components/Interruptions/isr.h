#ifndef ISR_H
#define ISR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers;

void isr_stub(void);
void isr_spurious(void);

void isr0(void);
void isr8(void);
void isr13(void);
void isr14(void);

void isr32(void);
void isr33(void);
void isr64(void);
void isr65(void);

void isr48(void);
void isr49(void);
void isr50(void);
void isr51(void);
void isr52(void);
void isr53(void);
void isr54(void);
void isr55(void);

void isr80(void);
void isr81(void);
void isr82(void);
void isr83(void);

void isr88(void);
void isr89(void);
void isr90(void);
void isr91(void);
void isr92(void);
void isr93(void);
void isr94(void);
void isr95(void);
void isr96(void);
void isr97(void);
void isr98(void);
void isr99(void);

typedef void (*irq_handler_t)(struct registers *regs);

void irq_register_handler(uint8_t vector, irq_handler_t handler);

void irq_unregister_handler(uint8_t vector);

void isr_handler(struct registers *regs);

#endif
