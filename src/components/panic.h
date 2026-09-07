#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

enum panic_code{
    PANIC_CODE_GENERAL = 0,
    PANIC_CODE_PAGE_FAULT = 1,
    PANIC_CODE_DOUBLE_FAULT = 2,
    PANIC_CODE_INVALID_OPCODE = 3,
    PANIC_CODE_STACK_OVERFLOW = 4,
};

typedef struct cpu_regs{
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;

    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rsp;

    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint64_t rip;
    uint64_t rflags;

    uint64_t cr2;
    uint64_t cr3;
} cpu_regs_t;

typedef struct panic_info {
    enum panic_code code;
    const char* message;

    cpu_regs_t* regs;
} panic_info;

void panic(struct panic_info* info);

#endif
