// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_ASM_BARRIER_H
#define _LKPI_ASM_BARRIER_H

#include <linux/compiler.h>

#define mb() asm volatile("mfence" ::: "memory")
#define rmb() asm volatile("lfence" ::: "memory")
#define wmb() asm volatile("sfence" ::: "memory")
#define dma_rmb() barrier()
#define dma_wmb() barrier()
#define __smp_mb() asm volatile("lock; addl $0,-4(%%rsp)" ::: "memory", "cc")
#define __smp_rmb() dma_rmb()
#define __smp_wmb() barrier()
#define smp_mb() __smp_mb()
#define smp_rmb() __smp_rmb()
#define smp_wmb() __smp_wmb()
#define virt_mb() __smp_mb()
#define virt_rmb() __smp_rmb()
#define virt_wmb() __smp_wmb()
#define virt_store_mb(var, value) do { WRITE_ONCE(var, value); __smp_mb(); } while (0)
#define virt_store_release(p, v) smp_store_release(p, v)
#define virt_load_acquire(p) smp_load_acquire(p)
#define smp_store_mb(var, value) do { WRITE_ONCE(var, value); smp_mb(); } while (0)
#define smp_store_release(p, v) do { barrier(); WRITE_ONCE(*(p), v); } while (0)
#define smp_load_acquire(p) ({ typeof(*(p)) ___p1 = READ_ONCE(*(p)); barrier(); ___p1; })
#define smp_mb__before_atomic() barrier()
#define smp_mb__after_atomic() barrier()
#define smp_mb__after_spinlock() do { } while (0)
#define smp_acquire__after_ctrl_dep() smp_rmb()
#define smp_cond_load_relaxed(ptr, cond_expr) ({ \
    typeof(ptr) __PTR = (ptr); \
    __unqual_scalar_typeof(*ptr) VAL; \
    for (;;) { VAL = READ_ONCE(*__PTR); if (cond_expr) break; asm volatile("pause" ::: "memory"); } \
    (typeof(*ptr))VAL; })
#define smp_cond_load_acquire(ptr, cond_expr) ({ \
    __unqual_scalar_typeof(*ptr) _val = smp_cond_load_relaxed(ptr, cond_expr); barrier(); (typeof(*ptr))_val; })
#define barrier_nospec() asm volatile("lfence" ::: "memory")
static __always_inline unsigned long array_index_mask_nospec(unsigned long index, unsigned long size)
{
    unsigned long mask;
    asm volatile("cmp %1,%2; sbb %0,%0;" : "=r"(mask) : "g"(size), "r"(index) : "cc");
    return mask;
}
#define array_index_mask_nospec array_index_mask_nospec
#define cpu_relax() ({ asm volatile("pause" ::: "memory"); })

#endif
