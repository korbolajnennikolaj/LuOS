// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/scheduler/fpu.h"

#include "kernel/scheduler/task.h"

#include <cpuid.h>
#include <msr.h>

#define XCR0_X87 0x1ULL
#define XCR0_SSE 0x2ULL
#define XCR0_AVX 0x4ULL
#define XCR0_AVX512 0xE0ULL

uint64_t fpu_save_size = 512;
uint8_t fpu_use_xsave;

static uint64_t g_xcr0;

static inline void xsetbv(uint32_t index, uint64_t value) {
    asm volatile("xsetbv" :: "c"(index), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}

static uint64_t pick_xcr0(void) {
    struct cpuid_result l0 = cpuid(0, 0);
    struct cpuid_result l1 = cpuid(1, 0);
    if (l0.eax < 0xD || !(l1.ecx & (1u << 26))) return 0;
    struct cpuid_result d = cpuid(0xD, 0);
    uint64_t supported = d.eax | ((uint64_t)d.edx << 32);
    uint64_t xcr0 = XCR0_X87 | XCR0_SSE;
    if ((l1.ecx & (1u << 28)) && (supported & XCR0_AVX)) xcr0 |= XCR0_AVX;
    if ((xcr0 & XCR0_AVX) && l0.eax >= 7) {
        struct cpuid_result l7 = cpuid(7, 0);
        if ((l7.ebx & (1u << 16)) && (supported & XCR0_AVX512) == XCR0_AVX512) xcr0 |= XCR0_AVX512;
    }
    return xcr0;
}

void fpu_init_cpu(int cpu) {
    if (cpu == 0) g_xcr0 = pick_xcr0();
    if (!g_xcr0) return;
    write_cr4(read_cr4() | (1ULL << 18));
    xsetbv(0, g_xcr0);
    if (cpu != 0) return;

    uint32_t size = cpuid(0xD, 0).ebx;
    if (size > TASK_FX_MAX && (g_xcr0 & XCR0_AVX512)) {
        g_xcr0 &= ~XCR0_AVX512;
        xsetbv(0, g_xcr0);
        size = cpuid(0xD, 0).ebx;
    }
    if (size > TASK_FX_MAX) {
        g_xcr0 = XCR0_X87 | XCR0_SSE;
        xsetbv(0, g_xcr0);
        size = cpuid(0xD, 0).ebx;
    }
    fpu_save_size = ((uint64_t)size + 63) & ~63ULL;
    fpu_use_xsave = 1;
}

uint64_t fpu_xcr0(void) {
    return g_xcr0;
}
