#include "isr.h"

extern void isr_handler(struct registers* regs);

__attribute__((naked)) void isr_stub(void) {
    asm volatile(
        "push $0\n\t"
        "push $99\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr_spurious(void) {
    asm volatile(
        "push $0\n\t"
        "push $255\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr0(void) {
    asm volatile(
        "push $0\n\t"
        "push $0\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr8(void) {
    asm volatile(
        "push $8\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr13(void) {
    asm volatile(
        "push $13\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr14(void) {
    asm volatile(
        "push $14\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr32(void) {
    asm volatile(
        "push $0\n\t"
        "push $32\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr33(void) {
    asm volatile(
        "push $0\n\t"
        "push $33\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr64(void) {
    asm volatile(
        "push $0\n\t"
        "push $64\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr65(void) {
    asm volatile(
        "push $0\n\t"
        "push $65\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr34(void) {
    asm volatile(
        "push $0\n\t"
        "push $34\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr35(void) {
    asm volatile(
        "push $0\n\t"
        "push $35\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr36(void) {
    asm volatile(
        "push $0\n\t"
        "push $36\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr37(void) {
    asm volatile(
        "push $0\n\t"
        "push $37\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr38(void) {
    asm volatile(
        "push $0\n\t"
        "push $38\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr39(void) {
    asm volatile(
        "push $0\n\t"
        "push $39\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr40(void) {
    asm volatile(
        "push $0\n\t"
        "push $40\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr41(void) {
    asm volatile(
        "push $0\n\t"
        "push $41\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr42(void) {
    asm volatile(
        "push $0\n\t"
        "push $42\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr43(void) {
    asm volatile(
        "push $0\n\t"
        "push $43\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr44(void) {
    asm volatile(
        "push $0\n\t"
        "push $44\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr45(void) {
    asm volatile(
        "push $0\n\t"
        "push $45\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr46(void) {
    asm volatile(
        "push $0\n\t"
        "push $46\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr47(void) {
    asm volatile(
        "push $0\n\t"
        "push $47\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr48(void) {
    asm volatile(
        "push $0\n\t"
        "push $48\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr49(void) {
    asm volatile(
        "push $0\n\t"
        "push $49\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr50(void) {
    asm volatile(
        "push $0\n\t"
        "push $50\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr51(void) {
    asm volatile(
        "push $0\n\t"
        "push $51\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr52(void) {
    asm volatile(
        "push $0\n\t"
        "push $52\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr53(void) {
    asm volatile(
        "push $0\n\t"
        "push $53\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr54(void) {
    asm volatile(
        "push $0\n\t"
        "push $54\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr55(void) {
    asm volatile(
        "push $0\n\t"
        "push $55\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr80(void) {
    asm volatile(
        "push $0\n\t"
        "push $80\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr81(void) {
    asm volatile(
        "push $0\n\t"
        "push $81\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr82(void) {
    asm volatile(
        "push $0\n\t"
        "push $82\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr83(void) {
    asm volatile(
        "push $0\n\t"
        "push $83\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr88(void) {
    asm volatile(
        "push $0\n\t"
        "push $88\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr89(void) {
    asm volatile(
        "push $0\n\t"
        "push $89\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr90(void) {
    asm volatile(
        "push $0\n\t"
        "push $90\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr91(void) {
    asm volatile(
        "push $0\n\t"
        "push $91\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr92(void) {
    asm volatile(
        "push $0\n\t"
        "push $92\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr93(void) {
    asm volatile(
        "push $0\n\t"
        "push $93\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr94(void) {
    asm volatile(
        "push $0\n\t"
        "push $94\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr95(void) {
    asm volatile(
        "push $0\n\t"
        "push $95\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr96(void) {
    asm volatile(
        "push $0\n\t"
        "push $96\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr97(void) {
    asm volatile(
        "push $0\n\t"
        "push $97\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr98(void) {
    asm volatile(
        "push $0\n\t"
        "push $98\n\t"
        "jmp isr_common_stub\n\t"
    );
}
__attribute__((naked)) void isr99(void) {
    asm volatile(
        "push $0\n\t"
        "push $99\n\t"
        "jmp isr_common_stub\n\t"
    );
}

__attribute__((naked)) void isr_common_stub(void) {
    asm volatile(
        "push %rax\n\t"
        "push %rbx\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %rbp\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"

        "mov %rsp, %rdi\n\t"
        "sub $8, %rsp\n\t"
        "call isr_handler\n\t"
        "add $8, %rsp\n\t"

        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rbp\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        "add $16, %rsp\n\t"
        "iretq\n\t"
    );
}
