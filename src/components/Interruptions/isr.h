// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef ISR_H
#define ISR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers;

extern void *const isr_stub_table[256];

#define IRQ_VECTOR_TLB_SHOOTDOWN 0x42
#define IRQ_VECTOR_RESCHEDULE 0x43
#define IRQ_VECTOR_DYN_BASE 0x70
#define IRQ_VECTOR_DYN_LAST 0xEF

typedef void (*irq_handler_t)(struct registers *regs);
typedef void (*irq_ctx_handler_t)(void *ctx, struct registers *regs);

void irq_register_handler(uint8_t vector, irq_handler_t handler);

void irq_unregister_handler(uint8_t vector);

int irq_alloc_vector(void);
int irq_alloc_vectors(int count, int *out_vectors);
void irq_free_vector(uint8_t vector);

int irq_register_ctx_handler(uint8_t vector, irq_ctx_handler_t handler, void *ctx);
void irq_unregister_ctx_handler(uint8_t vector);

uint64_t irq_vector_hits(uint8_t vector);
int irq_dyn_vectors_used(void);

void isr_handler(struct registers *regs, void *fx);
int isr_in_interrupt(void);

#endif
