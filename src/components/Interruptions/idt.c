#include "idt.h"

#include "components/Interruptions/msi.h"
#include "isr.h"

static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct idt_ptr idtp;

extern void isr_stub(void);
extern void isr_spurious(void);

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
        idt_set_gate((uint8_t)i, isr_stub, 0x8E);
    }

    idt_set_gate(0, isr0, 0x8E);
    idt_set_gate(8, isr8, 0x8E);
    idt_set_gate(13, isr13, 0x8E);
    idt_set_gate(14, isr14, 0x8E);

    idt_set_gate(32, isr32, 0x8E);
    idt_set_gate(33, isr33, 0x8E);
    idt_set_gate(64, isr64, 0x8E);
    idt_set_gate(65, isr65, 0x8E);

    extern void isr34(void); extern void isr35(void); extern void isr36(void);
    extern void isr37(void); extern void isr38(void); extern void isr39(void);
    extern void isr40(void); extern void isr41(void); extern void isr42(void);
    extern void isr43(void); extern void isr44(void); extern void isr45(void);
    extern void isr46(void); extern void isr47(void);
    idt_set_gate(34, isr34, 0x8E);
    idt_set_gate(35, isr35, 0x8E);
    idt_set_gate(36, isr36, 0x8E);
    idt_set_gate(37, isr37, 0x8E);
    idt_set_gate(38, isr38, 0x8E);
    idt_set_gate(39, isr39, 0x8E);
    idt_set_gate(40, isr40, 0x8E);
    idt_set_gate(41, isr41, 0x8E);
    idt_set_gate(42, isr42, 0x8E);
    idt_set_gate(43, isr43, 0x8E);
    idt_set_gate(44, isr44, 0x8E);
    idt_set_gate(45, isr45, 0x8E);
    idt_set_gate(46, isr46, 0x8E);
    idt_set_gate(47, isr47, 0x8E);

    idt_set_gate(48, isr48, 0x8E);
    idt_set_gate(49, isr49, 0x8E);
    idt_set_gate(50, isr50, 0x8E);
    idt_set_gate(51, isr51, 0x8E);
    idt_set_gate(52, isr52, 0x8E);
    idt_set_gate(53, isr53, 0x8E);
    idt_set_gate(54, isr54, 0x8E);
    idt_set_gate(55, isr55, 0x8E);

    idt_set_gate(80, isr80, 0x8E);
    idt_set_gate(81, isr81, 0x8E);
    idt_set_gate(82, isr82, 0x8E);
    idt_set_gate(83, isr83, 0x8E);

    idt_set_gate(88, isr88, 0x8E);
    idt_set_gate(89, isr89, 0x8E);
    idt_set_gate(90, isr90, 0x8E);
    idt_set_gate(91, isr91, 0x8E);
    idt_set_gate(92, isr92, 0x8E);
    idt_set_gate(93, isr93, 0x8E);
    idt_set_gate(94, isr94, 0x8E);
    idt_set_gate(95, isr95, 0x8E);
    idt_set_gate(96, isr96, 0x8E);
    idt_set_gate(97, isr97, 0x8E);
    idt_set_gate(98, isr98, 0x8E);

    idt_set_gate(99, isr99, 0x8E);

    idt_set_gate(255, isr_spurious, 0x8E);

    asm volatile("lidt %0" : : "m"(idtp));
}

void load_idt(void) {
    asm volatile("lidt %0" : : "m"(idtp));
}
