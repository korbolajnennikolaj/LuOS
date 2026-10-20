// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "idt.h"

#include "components/Interruptions/msi.h"
#include "components/logger.h"
#include "isr.h"

static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct idt_ptr idtp;

static uint16_t get_kernel_cs(void) {
    uint16_t cs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

void idt_set_gate(uint8_t vector, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low = addr & 0xFFFF;
    idt[vector].selector = get_kernel_cs();
    idt[vector].ist = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero = 0;
}

void init_idt(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], 0x8E);
    }
    idt[8].ist = 1;

    asm volatile("lidt %0" : : "m"(idtp));
    LOG_DEBUG("IDT loaded at 0x%llx, limit %u", (unsigned long long)idtp.base, (unsigned)idtp.limit);
}

void load_idt(void) {
    asm volatile("lidt %0" : : "m"(idtp));
}
