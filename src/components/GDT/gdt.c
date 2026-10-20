// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "components/GDT/gdt.h"
#include "kernel/scheduler/fpu.h"

#include <cpuid.h>
#include <msr.h>
#include <stdint.h>

static uint64_t gdt[3] __attribute__((aligned(16))) = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00CF92000000FFFFULL,
};

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdtr gdtr;

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

#define GDT_ENTRIES 8
#define IST_STACK_SIZE 8192

static uint64_t cpu_gdt[GDT_MAX_CPUS][GDT_ENTRIES] __attribute__((aligned(16)));
static struct tss64 cpu_tss[GDT_MAX_CPUS] __attribute__((aligned(16)));
static struct gdtr cpu_gdtr[GDT_MAX_CPUS];
static uint8_t cpu_df_stack[GDT_MAX_CPUS][IST_STACK_SIZE] __attribute__((aligned(16)));
static struct cpu_local cpu_locals[GDT_MAX_CPUS] __attribute__((aligned(64)));

extern void syscall_entry(void);

void init_gdt(void) {
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)&gdt;
    load_gdt();
    gdt_init_cpu(0);
}

void load_gdt(void) {
    asm volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq $0x08\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "m"(gdtr)
        : "rax", "memory"
    );
}

static void gdt_load_cpu_table(struct gdtr *g) {
    asm volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq $0x08\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x30, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(*g)
        : "rax", "memory"
    );
}

void gdt_init_cpu(int cpu) {
    if (cpu < 0 || cpu >= GDT_MAX_CPUS) return;

    uint64_t *g = cpu_gdt[cpu];
    g[0] = 0;
    g[1] = 0x00AF9A000000FFFFULL;
    g[2] = 0x00CF92000000FFFFULL;
    g[3] = 0x00CFFA000000FFFFULL;
    g[4] = 0x00CFF2000000FFFFULL;
    g[5] = 0x00AFFA000000FFFFULL;

    struct tss64 *t = &cpu_tss[cpu];
    for (unsigned i = 0; i < sizeof(*t); i++) ((uint8_t *)t)[i] = 0;
    t->ist[0] = (uint64_t)&cpu_df_stack[cpu][IST_STACK_SIZE];
    t->iomap_base = sizeof(*t);

    uint64_t base = (uint64_t)t;
    uint64_t limit = sizeof(*t) - 1;
    g[6] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
           (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g[7] = base >> 32;

    cpu_gdtr[cpu].limit = sizeof(cpu_gdt[cpu]) - 1;
    cpu_gdtr[cpu].base = (uint64_t)g;
    gdt_load_cpu_table(&cpu_gdtr[cpu]);

    struct cpu_local *cl = &cpu_locals[cpu];
    cl->self = (uint64_t)cl;
    cl->cpu = (uint64_t)cpu;
    wrmsr(MSR_IA32_GS_BASE, 0);
    wrmsr(MSR_IA32_KERNEL_GS_BASE, (uint64_t)cl);

    wrmsr(MSR_IA32_EFER, rdmsr(MSR_IA32_EFER) | 1);
    wrmsr(MSR_IA32_STAR, ((uint64_t)(GDT_USER_CODE32 | 3) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_IA32_FMASK, 0x47700);

    struct cpuid_result r = cpuid(0, 0);
    if (r.eax >= 7) {
        struct cpuid_result f = cpuid(7, 0);
        if (f.ebx & (1u << 7)) write_cr4(read_cr4() | (1ULL << 20));
    }
    fpu_init_cpu(cpu);
}

void cpu_set_kernel_stack(int cpu, uint64_t top) {
    if (cpu < 0 || cpu >= GDT_MAX_CPUS) return;
    top &= ~0xFULL;
    cpu_tss[cpu].rsp[0] = top;
    cpu_locals[cpu].kernel_rsp = top;
}
