// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef MSR_H
#define MSR_H

#include <stdint.h>

#define MSR_IA32_APIC_BASE 0x1B
#define MSR_IA32_MTRRCAP 0xFE
#define MSR_IA32_PAT 0x277
#define MSR_IA32_MTRR_DEF_TYPE 0x2FF
#define MSR_IA32_EFER 0xC0000080
#define MSR_IA32_STAR 0xC0000081
#define MSR_IA32_LSTAR 0xC0000082
#define MSR_IA32_FMASK 0xC0000084
#define MSR_IA32_FS_BASE 0xC0000100
#define MSR_IA32_GS_BASE 0xC0000101
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    asm volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)) : "memory");
}

static inline void wbinvd(void) {
    asm volatile("wbinvd" ::: "memory");
}

static inline void clflush(volatile void *p) {
    asm volatile("clflush (%0)" :: "r"(p) : "memory");
}

static inline void mfence(void) {
    asm volatile("mfence" ::: "memory");
}

static inline uint64_t read_cr0(void) {
    uint64_t v;
    asm volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    asm volatile("mov %0, %%cr0" :: "r"(v) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    asm volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    asm volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}

#endif
