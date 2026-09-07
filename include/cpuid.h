#ifndef CPUID_H
#define CPUID_H

#include <stdint.h>

typedef struct cpuid_result {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} cpuid_result;

static inline struct cpuid_result cpuid(uint32_t leaf, uint32_t subleaf) {
    struct cpuid_result result;

    asm volatile ("cpuid"
        : "=a"(result.eax), "=b"(result.ebx), "=c"(result.ecx), "=d"(result.edx)
        : "a"(leaf), "c"(subleaf)
    );

    return result;
}

static inline int has_cpuid(void) {
    uint64_t rflags, rflags_id;

    asm volatile ("pushfq\n\t"
                  "popq %0"
                  : "=r"(rflags)
                  :
    );

    rflags_id = rflags ^ (1ULL << 21);

    asm volatile ("pushq %0\n\t"
                  "popfq"
                  :
                  : "r"(rflags_id)
    );

    asm volatile ("pushfq\n\t"
                  "popq %0"
                  : "=r"(rflags_id)
                  :
    );

    asm volatile ("pushq %0\n\t"
                  "popfq"
                  :
                  : "r"(rflags)
    );

    return ((rflags ^ rflags_id) & (1ULL << 21)) != 0;
}

static inline void cpuid_get_vendor(char vendor[13]) {
    struct cpuid_result result = cpuid(0, 0);

    *((uint32_t*)&vendor[0]) = result.ebx;
    *((uint32_t*)&vendor[4]) = result.edx;
    *((uint32_t*)&vendor[8]) = result.ecx;
    vendor[12] = '\0';
}

static inline int cpuid_has_apic(void) {
    struct cpuid_result result = cpuid(1, 0);
    return (result.edx & (1 << 9)) != 0;
}

static inline int cpuid_has_tsc(void) {
    struct cpuid_result result = cpuid(1, 0);
    return (result.edx & (1 << 4)) != 0;
}

static inline int cpuid_has_msr(void) {
    struct cpuid_result result = cpuid(1, 0);
    return (result.edx & (1 << 5)) != 0;
}

static inline int cpuid_has_x2apic(void) {
    struct cpuid_result result = cpuid(1, 0);
    return (result.ecx & (1 << 21)) != 0;
}

static inline int cpuid_has_invariant_tsc(void) {
    if (cpuid(0x80000000, 0).eax >= 0x80000007) {
        struct cpuid_result result = cpuid(0x80000007, 0);
        return (result.edx & (1 << 8)) != 0;
    }
    return 0;
}

static inline uint32_t cpuid_get_max_std_leaf(void) {
    struct cpuid_result result = cpuid(0, 0);
    return result.eax;
}

static inline uint32_t cpuid_get_max_ext_leaf(void) {
    struct cpuid_result result = cpuid(0x80000000, 0);
    return result.eax;
}

static inline struct cpuid_result cpuid_get_processor_info(void) {
    return cpuid(1, 0);
}

static inline struct cpuid_result cpuid_get_cache_info(uint32_t subleaf) {
    return cpuid(2, subleaf);
}

static inline struct cpuid_result cpuid_get_extended_features(void) {
    return cpuid(0x80000001, 0);
}

static inline uint32_t cpuid_get_tsc_frequency_khz(void) {
    if (cpuid_get_max_ext_leaf() >= 0x8000000A) {
        struct cpuid_result result = cpuid(0x8000000A, 0);
        return result.eax;
    }
    return 0;
}

static inline struct cpuid_result cpuid_get_topology_info(uint32_t subleaf) {
    return cpuid(0x0B, subleaf);
}

#define CPUID_FEATURE_FPU (1 << 0)
#define CPUID_FEATURE_VME (1 << 1)
#define CPUID_FEATURE_DE (1 << 2)
#define CPUID_FEATURE_PSE (1 << 3)
#define CPUID_FEATURE_TSC (1 << 4)
#define CPUID_FEATURE_MSR (1 << 5)
#define CPUID_FEATURE_PAE (1 << 6)
#define CPUID_FEATURE_MCE (1 << 7)
#define CPUID_FEATURE_CX8 (1 << 8)
#define CPUID_FEATURE_APIC (1 << 9)
#define CPUID_FEATURE_SEP (1 << 11)
#define CPUID_FEATURE_MTRR (1 << 12)
#define CPUID_FEATURE_PGE (1 << 13)
#define CPUID_FEATURE_MCA (1 << 14)
#define CPUID_FEATURE_CMOV (1 << 15)
#define CPUID_FEATURE_PAT (1 << 16)
#define CPUID_FEATURE_PSE36 (1 << 17)
#define CPUID_FEATURE_PSN (1 << 18)
#define CPUID_FEATURE_CLFLUSH (1 << 19)
#define CPUID_FEATURE_DS (1 << 21)
#define CPUID_FEATURE_ACPI (1 << 22)
#define CPUID_FEATURE_MMX (1 << 23)
#define CPUID_FEATURE_FXSR (1 << 24)
#define CPUID_FEATURE_SSE (1 << 25)
#define CPUID_FEATURE_SSE2 (1 << 26)
#define CPUID_FEATURE_SS (1 << 27)
#define CPUID_FEATURE_HTT (1 << 28)
#define CPUID_FEATURE_TM (1 << 29)
#define CPUID_FEATURE_IA64 (1 << 30)
#define CPUID_FEATURE_PBE (1 << 31)

#define CPUID_FEATURE_SSE3 (1 << 0)
#define CPUID_FEATURE_PCLMUL (1 << 1)
#define CPUID_FEATURE_DTES64 (1 << 2)
#define CPUID_FEATURE_MONITOR (1 << 3)
#define CPUID_FEATURE_DS_CPL (1 << 4)
#define CPUID_FEATURE_VMX (1 << 5)
#define CPUID_FEATURE_SMX (1 << 6)
#define CPUID_FEATURE_EST (1 << 7)
#define CPUID_FEATURE_TM2 (1 << 8)
#define CPUID_FEATURE_SSSE3 (1 << 9)
#define CPUID_FEATURE_CNXT_ID (1 << 10)
#define CPUID_FEATURE_SDBG (1 << 11)
#define CPUID_FEATURE_FMA (1 << 12)
#define CPUID_FEATURE_CX16 (1 << 13)
#define CPUID_FEATURE_XTPR (1 << 14)
#define CPUID_FEATURE_PDCM (1 << 15)
#define CPUID_FEATURE_PCID (1 << 17)
#define CPUID_FEATURE_DCA (1 << 18)
#define CPUID_FEATURE_SSE4_1 (1 << 19)
#define CPUID_FEATURE_SSE4_2 (1 << 20)
#define CPUID_FEATURE_X2APIC (1 << 21)
#define CPUID_FEATURE_MOVBE (1 << 22)
#define CPUID_FEATURE_POPCNT (1 << 23)
#define CPUID_FEATURE_TSCDLT (1 << 24)
#define CPUID_FEATURE_AES (1 << 25)
#define CPUID_FEATURE_XSAVE (1 << 26)
#define CPUID_FEATURE_OSXSAVE (1 << 27)
#define CPUID_FEATURE_AVX (1 << 28)
#define CPUID_FEATURE_F16C (1 << 29)
#define CPUID_FEATURE_RDRAND (1 << 30)
#define CPUID_FEATURE_HYPERV (1 << 31)

static inline void cpuid_get_family_model(uint8_t* family, uint8_t* model, uint8_t* stepping) {
    struct cpuid_result result = cpuid(1, 0);

    uint8_t base_family = (result.eax >> 8) & 0xF;
    uint8_t base_model = (result.eax >> 4) & 0xF;
    uint8_t ext_family = (result.eax >> 20) & 0xFF;
    uint8_t ext_model = (result.eax >> 16) & 0xF;

    *stepping = result.eax & 0xF;

    *family = base_family;
    if (base_family == 0xF)
        *family += ext_family;

    *model = base_model;
    if (base_family == 0x6 || base_family == 0xF)
        *model |= (ext_model << 4);
}

static inline int cpuid_supports_leaf(uint32_t leaf) {
    if (leaf <= 0xFFFF) {
        return leaf <= cpuid_get_max_std_leaf();
    } else {
        return leaf <= cpuid_get_max_ext_leaf();
    }
}

#endif