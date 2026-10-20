// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE32 0x18
#define GDT_USER_DATA 0x20
#define GDT_USER_CODE 0x28
#define GDT_TSS 0x30

#define USER_CS (GDT_USER_CODE | 3)
#define USER_SS (GDT_USER_DATA | 3)

#define GDT_MAX_CPUS 64

struct cpu_local {
    uint64_t self;
    uint64_t user_rsp;
    uint64_t kernel_rsp;
    uint64_t cpu;
};

#define CPU_LOCAL_USER_RSP 8
#define CPU_LOCAL_KERNEL_RSP 16

void init_gdt(void);
void load_gdt(void);

void gdt_init_cpu(int cpu);

void cpu_set_kernel_stack(int cpu, uint64_t top);

#endif
