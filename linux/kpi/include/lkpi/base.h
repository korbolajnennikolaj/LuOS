// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_BASE_H
#define _LKPI_BASE_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/compiler.h>
#include <linux/const.h>
#include <linux/build_bug.h>
#include <linux/bits.h>
#include <linux/sizes.h>
#include <linux/stringify.h>
#include <linux/container_of.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/minmax.h>
#include <linux/array_size.h>
#include <linux/align.h>
#include <linux/rbtree_types.h>
#include <linux/stdarg.h>
#include <linux/args.h>
#include <linux/typecheck.h>
#include <linux/instruction_pointer.h>
#include <linux/swab.h>
#include <asm/byteorder.h>
#include <asm/barrier.h>
#include <asm/errno.h>
#include <linux/errno.h>

typedef __s64 time64_t;
typedef unsigned long kernel_ulong_t;


#define PAGE_SHIFT 12
#define PAGE_SIZE (_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)
#define PAGE_ALIGNED(addr) IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
#define L1_CACHE_SHIFT 6
#define L1_CACHE_BYTES (1 << L1_CACHE_SHIFT)
#define SMP_CACHE_BYTES L1_CACHE_BYTES
#define ____cacheline_aligned __attribute__((__aligned__(SMP_CACHE_BYTES)))
#define ____cacheline_aligned_in_smp ____cacheline_aligned
#define __cacheline_aligned ____cacheline_aligned
#define __cacheline_aligned_in_smp ____cacheline_aligned
#define cache_line_size() L1_CACHE_BYTES
#define NR_CPUS CONFIG_NR_CPUS
#define HZ CONFIG_HZ
#define USER_HZ 100
#define BITS_PER_BYTE 8
#define BITS_PER_TYPE(type) (sizeof(type) * BITS_PER_BYTE)
#define BITS_TO_LONGS(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(long))
#define BITS_TO_U64(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(u64))
#define BITS_TO_U32(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(u32))
#define BITS_TO_BYTES(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(char))
#define TASK_COMM_LEN 16
#define NUMA_NO_NODE (-1)
#define MAX_ERRNO 4095

#define __init
#define __exit
#define __initdata
#define __initconst
#define __exitdata
#define __ro_after_init
#define __read_mostly
#define __refdata
#define __ref
#define __devinit
#define __init_or_module
#define __meminit
#define __always_unused __attribute__((__unused__))
#define __visible_for_testing static
#define __kprobes
#define nokprobe_inline __always_inline
#define asmlinkage
#define __weak __attribute__((__weak__))
#define __sched
#define __lockfunc
#define __acquires(x)
#define __releases(x)
#define __acquire(x) (void)0
#define __release(x) (void)0
#define __cond_lock(x, c) (c)
#define __must_hold(x)
#define __no_sanitize_or_inline __always_inline
#define __no_kcsan
#define randomized_struct_fields_start
#define randomized_struct_fields_end
#define __latent_entropy

#include <lkpi/export.h>


#define U8_MAX ((u8)~0U)
#define S8_MAX ((s8)(U8_MAX >> 1))
#define S8_MIN ((s8)(-S8_MAX - 1))
#define U16_MAX ((u16)~0U)
#define S16_MAX ((s16)(U16_MAX >> 1))
#define S16_MIN ((s16)(-S16_MAX - 1))
#define U32_MAX ((u32)~0U)
#define U32_MIN ((u32)0)
#define S32_MAX ((s32)(U32_MAX >> 1))
#define S32_MIN ((s32)(-S32_MAX - 1))
#define U64_MAX ((u64)~0ULL)
#define S64_MAX ((s64)(U64_MAX >> 1))
#define S64_MIN ((s64)(-S64_MAX - 1))

#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#define upper_16_bits(n) ((u16)((n) >> 16))
#define lower_16_bits(n) ((u16)((n) & 0xffff))


#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BIT_ULL_WORD(nr) ((nr) / BITS_PER_LONG_LONG)

static __always_inline void set_bit(long nr, volatile unsigned long *addr)
{
	asm volatile("lock btsq %1,%0" : "+m"(*(volatile long *)addr) : "Ir"(nr) : "memory");
}
static __always_inline void clear_bit(long nr, volatile unsigned long *addr)
{
	asm volatile("lock btrq %1,%0" : "+m"(*(volatile long *)addr) : "Ir"(nr) : "memory");
}
static __always_inline void change_bit(long nr, volatile unsigned long *addr)
{
	asm volatile("lock btcq %1,%0" : "+m"(*(volatile long *)addr) : "Ir"(nr) : "memory");
}
static __always_inline bool test_and_set_bit(long nr, volatile unsigned long *addr)
{
	bool r;
	asm volatile("lock btsq %2,%1" : "=@ccc"(r), "+m"(*(volatile long *)addr) : "Ir"(nr) : "memory");
	return r;
}
static __always_inline bool test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	bool r;
	asm volatile("lock btrq %2,%1" : "=@ccc"(r), "+m"(*(volatile long *)addr) : "Ir"(nr) : "memory");
	return r;
}
static __always_inline bool test_and_change_bit(long nr, volatile unsigned long *addr)
{
	bool r;
	asm volatile("lock btcq %2,%1" : "=@ccc"(r), "+m"(*(volatile long *)addr) : "Ir"(nr) : "memory");
	return r;
}
static __always_inline void __set_bit(unsigned long nr, volatile unsigned long *addr)
{
	addr[BIT_WORD(nr)] |= 1UL << (nr % BITS_PER_LONG);
}
static __always_inline void __clear_bit(unsigned long nr, volatile unsigned long *addr)
{
	addr[BIT_WORD(nr)] &= ~(1UL << (nr % BITS_PER_LONG));
}
static __always_inline void __change_bit(unsigned long nr, volatile unsigned long *addr)
{
	addr[BIT_WORD(nr)] ^= 1UL << (nr % BITS_PER_LONG);
}
static __always_inline bool __test_and_set_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long m = 1UL << (nr % BITS_PER_LONG);
	unsigned long old = addr[BIT_WORD(nr)];
	addr[BIT_WORD(nr)] = old | m;
	return (old & m) != 0;
}
static __always_inline bool __test_and_clear_bit(unsigned long nr, volatile unsigned long *addr)
{
	unsigned long m = 1UL << (nr % BITS_PER_LONG);
	unsigned long old = addr[BIT_WORD(nr)];
	addr[BIT_WORD(nr)] = old & ~m;
	return (old & m) != 0;
}
static __always_inline bool test_bit(unsigned long nr, const volatile unsigned long *addr)
{
	return 1UL & (addr[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG - 1)));
}
#define test_bit_acquire(nr, addr) ({ bool __r = test_bit(nr, addr); barrier(); __r; })
static __always_inline void clear_bit_unlock(long nr, volatile unsigned long *addr)
{
	barrier();
	clear_bit(nr, addr);
}
static __always_inline void __clear_bit_unlock(long nr, volatile unsigned long *addr)
{
	barrier();
	__clear_bit(nr, addr);
}
static __always_inline bool test_and_set_bit_lock(long nr, volatile unsigned long *addr)
{
	return test_and_set_bit(nr, addr);
}
#define set_bit_lock set_bit

static __always_inline unsigned long __ffs(unsigned long word) { return __builtin_ctzl(word); }
static __always_inline unsigned long ffz(unsigned long word) { return __builtin_ctzl(~word); }
static __always_inline unsigned long __fls(unsigned long word) { return BITS_PER_LONG - 1 - __builtin_clzl(word); }
static __always_inline int ffs(int x) { return __builtin_ffs(x); }
static __always_inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static __always_inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static __always_inline int fls_long(unsigned long l) { return fls64(l); }
static __always_inline unsigned int __ffs64(u64 word) { return __builtin_ctzll(word); }
static __always_inline int ffs64(u64 word) { return word ? __builtin_ctzll(word) + 1 : 0; }
static __always_inline unsigned int hweight32(unsigned int w) { return __builtin_popcount(w); }
static __always_inline unsigned int hweight16(unsigned int w) { return __builtin_popcount(w & 0xffff); }
static __always_inline unsigned int hweight8(unsigned int w) { return __builtin_popcount(w & 0xff); }
static __always_inline unsigned long hweight64(u64 w) { return __builtin_popcountll(w); }
static __always_inline unsigned long hweight_long(unsigned long w) { return __builtin_popcountl(w); }
#define __const_hweight8(w) ((unsigned int)__builtin_popcount((w) & 0xff))
#define __const_hweight16(w) ((unsigned int)__builtin_popcount((w) & 0xffff))
#define __const_hweight32(w) ((unsigned int)__builtin_popcount((u32)(w)))
#define __const_hweight64(w) ((unsigned int)__builtin_popcountll((u64)(w)))
static inline u32 rol32(u32 word, unsigned int shift) { return (word << (shift & 31)) | (word >> ((-shift) & 31)); }
static inline u32 ror32(u32 word, unsigned int shift) { return (word >> (shift & 31)) | (word << ((-shift) & 31)); }
static inline u64 rol64(u64 word, unsigned int shift) { return (word << (shift & 63)) | (word >> ((-shift) & 63)); }
static inline u64 ror64(u64 word, unsigned int shift) { return (word >> (shift & 63)) | (word << ((-shift) & 63)); }
static inline u16 rol16(u16 word, unsigned int shift) { return (word << (shift & 15)) | (word >> ((-shift) & 15)); }
static inline u8 rol8(u8 word, unsigned int shift) { return (word << (shift & 7)) | (word >> ((-shift) & 7)); }
static inline s32 sign_extend32(u32 value, int index) { u8 shift = 31 - index; return (s32)(value << shift) >> shift; }
static inline s64 sign_extend64(u64 value, int index) { u8 shift = 63 - index; return (s64)(value << shift) >> shift; }
static inline unsigned int fls_long_u(unsigned long l) { return fls64(l); }
static inline int get_count_order(unsigned int count) { if (count == 0) return -1; return fls(--count); }
static inline int get_count_order_long(unsigned long l) { if (l == 0UL) return -1; return (int)fls_long(--l); }
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
#define for_each_clear_bit(bit, addr, size) \
	for ((bit) = find_first_zero_bit((addr), (size)); (bit) < (size); (bit) = find_next_zero_bit((addr), (size), (bit) + 1))
#define for_each_set_bit_from(bit, addr, size) \
	for ((bit) = find_next_bit((addr), (size), (bit)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_first_bit(const unsigned long *addr, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);
unsigned long find_last_bit(const unsigned long *addr, unsigned long size);
unsigned long find_next_and_bit(const unsigned long *a, const unsigned long *b, unsigned long size,
				unsigned long offset);


#define ATOMIC_INIT(i) { (i) }
#define ATOMIC64_INIT(i) { (i) }
#define ATOMIC_LONG_INIT(i) ATOMIC64_INIT(i)

#define __LKPI_ATOMIC_OPS(pfx, atype, itype) \
static __always_inline itype pfx##_read(const atype *v) { return READ_ONCE(v->counter); } \
static __always_inline itype pfx##_read_acquire(const atype *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); } \
static __always_inline void pfx##_set(atype *v, itype i) { WRITE_ONCE(v->counter, i); } \
static __always_inline void pfx##_set_release(atype *v, itype i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); } \
static __always_inline void pfx##_add(itype i, atype *v) { __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline void pfx##_sub(itype i, atype *v) { __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline void pfx##_inc(atype *v) { pfx##_add(1, v); } \
static __always_inline void pfx##_dec(atype *v) { pfx##_sub(1, v); } \
static __always_inline itype pfx##_add_return(itype i, atype *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_sub_return(itype i, atype *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_inc_return(atype *v) { return pfx##_add_return(1, v); } \
static __always_inline itype pfx##_dec_return(atype *v) { return pfx##_sub_return(1, v); } \
static __always_inline itype pfx##_add_return_relaxed(itype i, atype *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline itype pfx##_inc_return_relaxed(atype *v) { return pfx##_add_return_relaxed(1, v); } \
static __always_inline itype pfx##_fetch_add(itype i, atype *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_fetch_add_relaxed(itype i, atype *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED); } \
static __always_inline itype pfx##_fetch_sub(itype i, atype *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_fetch_sub_release(itype i, atype *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_RELEASE); } \
static __always_inline itype pfx##_fetch_inc(atype *v) { return pfx##_fetch_add(1, v); } \
static __always_inline itype pfx##_fetch_dec(atype *v) { return pfx##_fetch_sub(1, v); } \
static __always_inline itype pfx##_fetch_or(itype i, atype *v) { return __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_fetch_and(itype i, atype *v) { return __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_fetch_andnot(itype i, atype *v) { return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_fetch_xor(itype i, atype *v) { return __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline void pfx##_or(itype i, atype *v) { __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline void pfx##_and(itype i, atype *v) { __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline void pfx##_andnot(itype i, atype *v) { __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); } \
static __always_inline void pfx##_xor(itype i, atype *v) { __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); } \
static __always_inline bool pfx##_inc_and_test(atype *v) { return pfx##_inc_return(v) == 0; } \
static __always_inline bool pfx##_dec_and_test(atype *v) { return pfx##_dec_return(v) == 0; } \
static __always_inline bool pfx##_sub_and_test(itype i, atype *v) { return pfx##_sub_return(i, v) == 0; } \
static __always_inline bool pfx##_add_negative(itype i, atype *v) { return pfx##_add_return(i, v) < 0; } \
static __always_inline itype pfx##_xchg(atype *v, itype n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); } \
static __always_inline itype pfx##_cmpxchg(atype *v, itype o, itype n) { \
	__atomic_compare_exchange_n(&v->counter, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return o; } \
static __always_inline itype pfx##_cmpxchg_relaxed(atype *v, itype o, itype n) { return pfx##_cmpxchg(v, o, n); } \
static __always_inline bool pfx##_try_cmpxchg(atype *v, itype *o, itype n) { \
	return __atomic_compare_exchange_n(&v->counter, o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); } \
static __always_inline bool pfx##_try_cmpxchg_relaxed(atype *v, itype *o, itype n) { return pfx##_try_cmpxchg(v, o, n); } \
static __always_inline bool pfx##_try_cmpxchg_acquire(atype *v, itype *o, itype n) { return pfx##_try_cmpxchg(v, o, n); } \
static __always_inline bool pfx##_try_cmpxchg_release(atype *v, itype *o, itype n) { return pfx##_try_cmpxchg(v, o, n); } \
static __always_inline itype pfx##_fetch_add_unless(atype *v, itype a, itype u) { \
	itype c = pfx##_read(v); \
	do { if (unlikely(c == u)) break; } while (!pfx##_try_cmpxchg(v, &c, c + a)); \
	return c; } \
static __always_inline bool pfx##_add_unless(atype *v, itype a, itype u) { return pfx##_fetch_add_unless(v, a, u) != u; } \
static __always_inline bool pfx##_inc_not_zero(atype *v) { return pfx##_add_unless(v, 1, 0); } \
static __always_inline bool pfx##_inc_unless_negative(atype *v) { \
	itype c = pfx##_read(v); \
	do { if (unlikely(c < 0)) return false; } while (!pfx##_try_cmpxchg(v, &c, c + 1)); \
	return true; } \
static __always_inline itype pfx##_dec_if_positive(atype *v) { \
	itype dec, c = pfx##_read(v); \
	do { dec = c - 1; if (unlikely(dec < 0)) break; } while (!pfx##_try_cmpxchg(v, &c, dec)); \
	return dec; }

__LKPI_ATOMIC_OPS(atomic, atomic_t, int)
__LKPI_ATOMIC_OPS(atomic64, atomic64_t, s64)

typedef atomic64_t atomic_long_t;
#define atomic_long_read atomic64_read
#define atomic_long_set atomic64_set
#define atomic_long_inc atomic64_inc
#define atomic_long_dec atomic64_dec
#define atomic_long_add atomic64_add
#define atomic_long_sub atomic64_sub
#define atomic_long_add_return atomic64_add_return
#define atomic_long_sub_return atomic64_sub_return
#define atomic_long_inc_return atomic64_inc_return
#define atomic_long_dec_return atomic64_dec_return
#define atomic_long_cmpxchg atomic64_cmpxchg
#define atomic_long_try_cmpxchg atomic64_try_cmpxchg
#define atomic_long_xchg atomic64_xchg
#define atomic_long_fetch_add atomic64_fetch_add
#define atomic_long_dec_and_test atomic64_dec_and_test
#define atomic_long_inc_not_zero atomic64_inc_not_zero
#define atomic_long_or atomic64_or
#define atomic_long_andnot atomic64_andnot
#define atomic_long_try_cmpxchg_acquire atomic64_try_cmpxchg_acquire
#define atomic_long_try_cmpxchg_release atomic64_try_cmpxchg_release
#define atomic_long_try_cmpxchg_relaxed atomic64_try_cmpxchg_relaxed
#define atomic_long_cmpxchg_relaxed atomic64_cmpxchg_relaxed
#define atomic_long_set_release atomic64_set_release
#define atomic_long_read_acquire atomic64_read_acquire
#define atomic_long_fetch_or atomic64_fetch_or
#define atomic_long_fetch_andnot atomic64_fetch_andnot
#define atomic_long_fetch_and atomic64_fetch_and
#define atomic_long_fetch_sub atomic64_fetch_sub
#define atomic_long_and atomic64_and
#define atomic_long_add_unless atomic64_add_unless
#define atomic_long_fetch_add_unless atomic64_fetch_add_unless
#define atomic_long_dec_if_positive atomic64_dec_if_positive
#define atomic_long_sub_and_test atomic64_sub_and_test
#define atomic_long_inc_unless_negative atomic64_inc_unless_negative
#define atomic_long_add_return_relaxed atomic64_add_return_relaxed
#define atomic_long_fetch_add_relaxed atomic64_fetch_add_relaxed
#define atomic_long_xchg_relaxed atomic64_xchg_relaxed

#define xchg(ptr, v) __atomic_exchange_n((ptr), (v), __ATOMIC_SEQ_CST)
#define xchg_relaxed(ptr, v) __atomic_exchange_n((ptr), (v), __ATOMIC_RELAXED)
#define xchg_acquire(ptr, v) __atomic_exchange_n((ptr), (v), __ATOMIC_ACQUIRE)
#define xchg_release(ptr, v) __atomic_exchange_n((ptr), (v), __ATOMIC_RELEASE)
#define cmpxchg(ptr, o, n) ({ \
	__typeof__(*(ptr)) __o = (o); \
	__atomic_compare_exchange_n((ptr), &__o, (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	__o; })
#define cmpxchg_relaxed cmpxchg
#define cmpxchg_acquire cmpxchg
#define cmpxchg_release cmpxchg
#define cmpxchg64 cmpxchg
#define cmpxchg64_relaxed cmpxchg
#define cmpxchg_local cmpxchg
#define try_cmpxchg(ptr, po, n) \
	__atomic_compare_exchange_n((ptr), (po), (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define try_cmpxchg_relaxed try_cmpxchg
#define try_cmpxchg_acquire try_cmpxchg
#define try_cmpxchg_release try_cmpxchg
#define try_cmpxchg64 try_cmpxchg


#define KERN_SOH "\001"
#define KERN_SOH_ASCII '\001'
#define KERN_EMERG KERN_SOH "0"
#define KERN_ALERT KERN_SOH "1"
#define KERN_CRIT KERN_SOH "2"
#define KERN_ERR KERN_SOH "3"
#define FW_BUG		"[Firmware Bug]: "
#define FW_WARN		"[Firmware Warn]: "
#define FW_INFO		"[Firmware Info]: "
#define HW_ERR		"[Hardware Error]: "
#define DEPRECATED	"[Deprecated]: "
#define KERN_WARNING KERN_SOH "4"
#define KERN_NOTICE KERN_SOH "5"
#define KERN_INFO KERN_SOH "6"
#define KERN_DEBUG KERN_SOH "7"
#define KERN_DEFAULT ""
#define KERN_CONT KERN_SOH "c"
#define LOGLEVEL_EMERG 0
#define LOGLEVEL_ALERT 1
#define LOGLEVEL_CRIT 2
#define LOGLEVEL_ERR 3
#define LOGLEVEL_WARNING 4
#define LOGLEVEL_NOTICE 5
#define LOGLEVEL_INFO 6
#define LOGLEVEL_DEBUG 7

struct va_format {
	const char *fmt;
	va_list *va;
};

__printf(1, 2) int _printk(const char *fmt, ...);
__printf(1, 0) int vprintk(const char *fmt, va_list args);
#define printk(fmt, ...) _printk(fmt, ##__VA_ARGS__)
#define printk_deferred printk
#define no_printk(fmt, ...) ({ if (0) _printk(fmt, ##__VA_ARGS__); 0; })
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif
#define pr_emerg(fmt, ...) printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning pr_warn
#define pr_notice(fmt, ...) printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...) printk(KERN_CONT fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define pr_debug(fmt, ...) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif
#define pr_devel(fmt, ...) no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info_ratelimited pr_info
#define pr_err_ratelimited pr_err
#define pr_warn_ratelimited pr_warn
#define pr_debug_ratelimited pr_debug
#define pr_notice_ratelimited pr_notice
#define pr_err_once(fmt, ...) printk_once(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn_once(fmt, ...) printk_once(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info_once(fmt, ...) printk_once(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice_once(fmt, ...) printk_once(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug_once(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)
#define printk_once(fmt, ...) ({ \
	static bool __section(".data") __print_once; \
	bool __ret_print_once = !__print_once; \
	if (!__print_once) { __print_once = true; printk(fmt, ##__VA_ARGS__); } \
	unlikely(__ret_print_once); })
#define printk_ratelimited printk
#define printk_ratelimit() 1
#define print_hex_dump(level, prefix, type, rowsize, groupsize, buf, len, ascii) do { } while (0)
#define print_hex_dump_debug(...) do { } while (0)
#define print_hex_dump_bytes(...) do { } while (0)
enum { DUMP_PREFIX_NONE, DUMP_PREFIX_ADDRESS, DUMP_PREFIX_OFFSET };
#define DEFAULT_RATELIMIT_INTERVAL (5 * HZ)
#define DEFAULT_RATELIMIT_BURST 10
struct ratelimit_state { int interval; int burst; int printed; int missed; unsigned long begin; };
#define RATELIMIT_STATE_INIT(name, interval_init, burst_init) { .interval = (interval_init), .burst = (burst_init) }
#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init) \
	struct ratelimit_state name = RATELIMIT_STATE_INIT(name, interval_init, burst_init)
#define __ratelimit(state) (((void)(state)), 1)
static inline void ratelimit_state_init(struct ratelimit_state *rs, int interval, int burst) { rs->interval = interval; rs->burst = burst; }
static inline void ratelimit_state_reset_interval(struct ratelimit_state *rs, int interval_init)
{
	rs->interval = interval_init;
	rs->printed = 0;
	rs->missed = 0;
}
static inline void ratelimit_set_flags(struct ratelimit_state *rs, unsigned long flags) { }
#define RATELIMIT_MSG_ON_RELEASE 1
void dump_stack(void);
#define console_loglevel 7


void lkpi_bug(const char *file, int line, const char *func) __noreturn __cold;
void lkpi_warn(const char *file, int line, const char *func, const char *fmt, ...) __cold;

#define BUG() lkpi_bug(__FILE__, __LINE__, __func__)
#define BUG_ON(cond) do { if (unlikely(cond)) BUG(); } while (0)
#define __WARN() lkpi_warn(__FILE__, __LINE__, __func__, NULL)
#define __WARN_printf(taint, fmt, ...) lkpi_warn(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define WARN_ON(condition) ({ \
	int __ret_warn_on = !!(condition); \
	if (unlikely(__ret_warn_on)) lkpi_warn(__FILE__, __LINE__, __func__, "%s", #condition); \
	unlikely(__ret_warn_on); })
#define WARN(condition, format...) ({ \
	int __ret_warn_on = !!(condition); \
	if (unlikely(__ret_warn_on)) lkpi_warn(__FILE__, __LINE__, __func__, format); \
	unlikely(__ret_warn_on); })
#define WARN_TAINT(condition, taint, format...) WARN(condition, format)
#define WARN_ON_ONCE(condition) ({ \
	static bool __section(".data") __warned; \
	int __ret_warn_once = !!(condition); \
	if (unlikely(__ret_warn_once && !__warned)) { __warned = true; lkpi_warn(__FILE__, __LINE__, __func__, "%s", #condition); } \
	unlikely(__ret_warn_once); })
#define WARN_ONCE(condition, format...) ({ \
	static bool __section(".data") __warned; \
	int __ret_warn_once = !!(condition); \
	if (unlikely(__ret_warn_once && !__warned)) { __warned = true; lkpi_warn(__FILE__, __LINE__, __func__, format); } \
	unlikely(__ret_warn_once); })
#define WARN_TAINT_ONCE(condition, taint, format...) WARN_ONCE(condition, format)
#define WARN_ON_SMP(x) WARN_ON(x)
#define BUILD_BUG_ON_INVALID(e) ((void)(sizeof((__force long)(e))))
#define VM_BUG_ON(cond) BUILD_BUG_ON_INVALID(cond)
#define VM_WARN_ON(cond) BUILD_BUG_ON_INVALID(cond)
#define VM_WARN_ON_ONCE(cond) BUILD_BUG_ON_INVALID(cond)
#define VM_BUG_ON_PAGE(cond, page) BUILD_BUG_ON_INVALID(cond)
#define VM_BUG_ON_FOLIO(cond, folio) BUILD_BUG_ON_INVALID(cond)
#define VM_WARN_ON_FOLIO(cond, folio) BUILD_BUG_ON_INVALID(cond)
#define VM_WARN_ON_ONCE_FOLIO(cond, folio) BUILD_BUG_ON_INVALID(cond)
#define TAINT_PROPRIETARY_MODULE 0
#define TAINT_FORCED_MODULE 1
#define TAINT_CPU_OUT_OF_SPEC 2
#define TAINT_FORCED_RMMOD 3
#define TAINT_MACHINE_CHECK 4
#define TAINT_BAD_PAGE 5
#define TAINT_USER 6
#define TAINT_DIE 7
#define TAINT_OVERRIDDEN_ACPI_TABLE 8
#define TAINT_WARN 9
#define TAINT_CRAP 10
#define TAINT_FIRMWARE_WORKAROUND 11
#define TAINT_OOT_MODULE 12
#define TAINT_UNSIGNED_MODULE 13
#define TAINT_SOFTLOCKUP 14
#define TAINT_LIVEPATCH 15
#define TAINT_AUX 16
#define TAINT_RANDSTRUCT 17
#define TAINT_TEST 18
#define TAINT_FWCTL 19
#define LOCKDEP_STILL_OK 0
static inline void add_taint(unsigned int flag, int lockdep_ok) { }

void lkpi_panic(const char *fmt, ...) __noreturn __cold;
#define panic(fmt, ...) lkpi_panic(fmt, ##__VA_ARGS__)


void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);

char *strncat(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
ssize_t sized_strscpy(char *d, const char *s, size_t n);
#define __strscpy0(dst, src, ...) sized_strscpy(dst, src, sizeof(dst) + __must_be_array(dst))
#define __strscpy1(dst, src, size) sized_strscpy(dst, src, size)
#define strscpy(dst, src, ...) CONCATENATE(__strscpy, COUNT_ARGS(__VA_ARGS__))(dst, src, __VA_ARGS__)
ssize_t strscpy_pad(char *d, const char *s, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strnchr(const char *s, size_t count, int c);
char *strchrnul(const char *s, int c);
char *strnstr(const char *h, const char *n, size_t len);
char *strsep(char **s, const char *ct);
char *skip_spaces(const char *s);
char *strim(char *s);
static inline char *strstrip(char *s) { return strim(s); }
void *memchr_inv(const void *s, int c, size_t n);
void *memscan(void *addr, int c, size_t size);
char *strreplace(char *s, char old, char new);
bool sysfs_streq(const char *a, const char *b);
int match_string(const char *const *array, size_t n, const char *string);
#define sysfs_match_string(_a, _s) match_string(_a, ARRAY_SIZE(_a), _s)
bool str_has_prefix_impl(const char *str, const char *prefix);
static inline size_t str_has_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) == 0 ? len : 0;
}
static inline void memset32(u32 *s, u32 v, size_t count) { while (count--) *s++ = v; }
static inline void memset64(u64 *s, u64 v, size_t count) { while (count--) *s++ = v; }
static inline void memset16(u16 *s, u16 v, size_t count) { while (count--) *s++ = v; }
#define memset_startat(obj, v, member) ({ \
	u8 *__ptr = (u8 *)(obj); typeof(v) __val = (v); \
	memset(__ptr + offsetof(typeof(*(obj)), member), __val, sizeof(*(obj)) - offsetof(typeof(*(obj)), member)); })
#define memset_after(obj, v, member) ({ \
	u8 *__ptr = (u8 *)(obj); typeof(v) __val = (v); \
	memset(__ptr + offsetofend(typeof(*(obj)), member), __val, sizeof(*(obj)) - offsetofend(typeof(*(obj)), member)); })
#define memcpy_and_pad(dest, dest_len, src, count, pad) do { \
	size_t __dl = (dest_len), __c = (count); \
	if (__dl > __c) { memcpy(dest, src, __c); memset((char *)(dest) + __c, pad, __dl - __c); } \
	else memcpy(dest, src, __dl); } while (0)
#define unsafe_memcpy(dst, src, bytes, justification) memcpy(dst, src, bytes)
#define strtomem_pad(dest, src, pad) do { \
	const size_t _dest_len = sizeof(dest); const size_t _src_len = strnlen(src, _dest_len); \
	memcpy_and_pad(dest, _dest_len, src, _src_len, pad); } while (0)
#define strtomem(dest, src) do { \
	const size_t _dest_len = sizeof(dest); const size_t _src_len = strnlen(src, _dest_len); \
	memcpy(dest, src, min(_dest_len, _src_len)); } while (0)
#define mem_is_zero(s, n) (!memchr_inv((s), 0, (n)))
#include <linux/string_choices.h>
#define memzero_explicit(s, n) do { memset((s), 0, (n)); barrier(); } while (0)

#define snprintf lkpi_snprintf
#define vsnprintf lkpi_vsnprintf
#define sprintf lkpi_sprintf
#define vsprintf lkpi_vsprintf
__printf(3, 4) int lkpi_snprintf(char *buf, size_t size, const char *fmt, ...);
__printf(3, 0) int lkpi_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(2, 3) int lkpi_sprintf(char *buf, const char *fmt, ...);
__printf(2, 0) int lkpi_vsprintf(char *buf, const char *fmt, va_list args);
__printf(3, 4) int scnprintf(char *buf, size_t size, const char *fmt, ...);
__printf(3, 0) int vscnprintf(char *buf, size_t size, const char *fmt, va_list args);
__printf(2, 3) int sscanf(const char *buf, const char *fmt, ...);
int vsscanf(const char *buf, const char *fmt, va_list args);
__printf(2, 3) int sysfs_emit(char *buf, const char *fmt, ...);
__printf(3, 4) int sysfs_emit_at(char *buf, int at, const char *fmt, ...);

#include <linux/kstrtox.h>
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);
long simple_strtol(const char *cp, char **endp, unsigned int base);
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base);
long long simple_strtoll(const char *cp, char **endp, unsigned int base);
#include <linux/ctype.h>
#include <linux/hex.h>
int get_option(char **str, int *pint);
char *get_options(const char *str, int nints, int *ints);
unsigned long long memparse(const char *ptr, char **retptr);


#include <linux/math.h>
#include <linux/log2.h>
#include <linux/wordpart.h>

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
static inline s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
static inline u64 div64_u64(u64 dividend, u64 divisor) { return dividend / divisor; }
static inline s64 div64_s64(s64 dividend, s64 divisor) { return dividend / divisor; }
static inline u64 div_u64(u64 dividend, u32 divisor) { return dividend / divisor; }
static inline s64 div_s64(s64 dividend, s32 divisor) { return dividend / divisor; }
#define div64_ul(x, y) div64_u64((x), (y))
#define div64_long(x, y) div64_s64((x), (y))
static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a * b; }
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift) { return (u64)(((unsigned __int128)a * mul) >> shift); }
static inline u64 mul_u64_u64_shr(u64 a, u64 mul, unsigned int shift) { return (u64)(((unsigned __int128)a * mul) >> shift); }
static inline u64 mul_u64_u32_div(u64 a, u32 mul, u32 divisor) { return (u64)(((unsigned __int128)a * mul) / divisor); }
static inline u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c) { return (u64)(((unsigned __int128)a * b) / c); }
#define DIV64_U64_ROUND_UP(ll, d) ({ u64 _tmp = (d); div64_u64((ll) + _tmp - 1, _tmp); })
#define DIV64_U64_ROUND_CLOSEST(dividend, divisor) ({ u64 _tmp = (divisor); div64_u64((dividend) + _tmp / 2, _tmp); })
#define DIV_U64_ROUND_UP(ll, d) ({ u32 _tmp = (d); div_u64((ll) + _tmp - 1, _tmp); })
#define DIV_U64_ROUND_CLOSEST(dividend, divisor) ({ u32 _tmp = (divisor); div_u64((u64)(dividend) + _tmp / 2, _tmp); })
#define DIV_S64_ROUND_CLOSEST(dividend, divisor) ({ \
	s64 __x = (dividend); s32 __d = (divisor); \
	((__x > 0) == (__d > 0)) ? div_s64((__x + (__d / 2)), __d) : div_s64((__x - (__d / 2)), __d); })
unsigned long gcd(unsigned long a, unsigned long b);
unsigned long lcm(unsigned long a, unsigned long b);
void rational_best_approximation(unsigned long given_numerator, unsigned long given_denominator,
				 unsigned long max_numerator, unsigned long max_denominator,
				 unsigned long *best_numerator, unsigned long *best_denominator);
u32 lkpi_prandom_u32(void);
#define get_random_u32() lkpi_prandom_u32()
static inline u64 get_random_u64(void) { return ((u64)lkpi_prandom_u32() << 32) | lkpi_prandom_u32(); }
static inline u32 get_random_u32_below(u32 ceil) { return ceil ? lkpi_prandom_u32() % ceil : 0; }
static inline unsigned long get_random_long(void) { return get_random_u64(); }
void get_random_bytes(void *buf, size_t len);
#define prandom_u32_max get_random_u32_below


#include <linux/list.h>
#include <linux/hash.h>

#define HLIST_HEAD_INIT { .first = NULL }


#define STACK_MAGIC 0xdeadbeef
#define REPEAT_BYTE(x) ((~0ul / 0xff) * (x))
#define _RET_IP_ (unsigned long)__builtin_return_address(0)
#define _THIS_IP_ ({ __label__ __here; __here: (unsigned long)&&__here; })
#define might_fault() do { } while (0)
#define might_resched() do { } while (0)
#define might_alloc(gfp) do { (void)(gfp); } while (0)
#define might_lock(lock) do { } while (0)
#define might_lock_read(lock) do { } while (0)
#define might_lock_nested(lock, subclass) do { } while (0)
#define lockdep_assert(cond) do { } while (0)
#define lockdep_assert_once(cond) do { } while (0)
#define lockdep_assert_held(l) do { (void)(l); } while (0)
#define lockdep_assert_held_once(l) do { (void)(l); } while (0)
#define lockdep_assert_held_write(l) do { (void)(l); } while (0)
#define lockdep_assert_held_read(l) do { (void)(l); } while (0)
#define lockdep_assert_not_held(l) do { (void)(l); } while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#define lockdep_assert_irqs_disabled() do { } while (0)
#define lockdep_assert_irqs_enabled() do { } while (0)
#define lockdep_assert_in_irq() do { } while (0)
#define lockdep_is_held(l) 1
#define lockdep_set_class(lock, key) do { (void)(key); } while (0)
#define lockdep_set_class_and_name(lock, key, name) do { (void)(key); } while (0)
#define lockdep_set_subclass(lock, sub) do { } while (0)
#define lockdep_init_map(lock, name, key, sub) do { (void)(key); } while (0)
#define lockdep_register_key(key) do { } while (0)
#define lockdep_unregister_key(key) do { } while (0)
#define lockdep_off() do { } while (0)
#define lockdep_on() do { } while (0)
#define lock_map_acquire(l) do { } while (0)
#define lock_map_acquire_read(l) do { } while (0)
#define lock_map_acquire_try(l) do { } while (0)
#define lock_map_release(l) do { } while (0)
#define lock_acquire(l, s, t, r, c, n, i) do { } while (0)
#define lock_release(l, i) do { } while (0)
#define lock_acquire_exclusive(l, s, t, n, i) do { } while (0)
#define lock_acquire_shared(l, s, t, n, i) do { } while (0)
#define lock_acquire_shared_recursive(l, s, t, n, i) do { } while (0)
#define lock_contended(l, ip) do { } while (0)
#define lock_acquired(l, ip) do { } while (0)
#define rwsem_acquire(l, s, t, i) do { } while (0)
#define rwsem_acquire_read(l, s, t, i) do { } while (0)
#define rwsem_release(l, i) do { } while (0)
#define mutex_acquire(l, s, t, i) do { } while (0)
#define mutex_release(l, i) do { } while (0)
#define spin_acquire(l, s, t, i) do { } while (0)
#define spin_release(l, i) do { } while (0)
#define lockdep_pin_lock(l) ((struct pin_cookie){ })
#define lockdep_unpin_lock(l, c) do { } while (0)
#define rcu_lockdep_assert(c, s) do { } while (0)
#define RCU_LOCKDEP_WARN(c, s) do { } while (0)
#define lock_is_held(l) 1
#define fs_reclaim_acquire(gfp) do { } while (0)
#define fs_reclaim_release(gfp) do { } while (0)
struct lock_class_key { };
struct lockdep_map { };
struct pin_cookie { };
#define STATIC_LOCKDEP_MAP_INIT(_name, _key) { }
#define DEFINE_WAIT_OVERRIDE_MAP(_name, _wait_type) struct lockdep_map _name = { }
#define LD_WAIT_SLEEP 0
#define SINGLE_DEPTH_NESTING 1

#define in_task() (!lkpi_in_irq())
#define in_interrupt() lkpi_in_irq()
#define in_irq() lkpi_in_irq()
#define in_hardirq() lkpi_in_irq()
#define in_softirq() 0
#define in_serving_softirq() 0
#define in_nmi() 0
#define in_atomic() (lkpi_in_irq() || lkpi_preempt_count() > 0)
#define irqs_disabled() lkpi_irqs_disabled()
bool lkpi_in_irq(void);
int lkpi_preempt_count(void);
bool lkpi_irqs_disabled(void);

#define system_state SYSTEM_RUNNING
enum system_states { SYSTEM_BOOTING, SYSTEM_SCHEDULING, SYSTEM_FREEING_INITMEM, SYSTEM_RUNNING, SYSTEM_HALT,
		     SYSTEM_POWER_OFF, SYSTEM_RESTART, SYSTEM_SUSPEND };
#define oops_in_progress 0
#define panic_timeout 0

#define num_online_cpus() lkpi_num_online_cpus()
#define num_possible_cpus() lkpi_num_online_cpus()
#define nr_cpu_ids ((unsigned int)lkpi_num_online_cpus())
unsigned int lkpi_num_online_cpus(void);
#define smp_processor_id() lkpi_cpu_id()
#define raw_smp_processor_id() lkpi_cpu_id()
#define get_cpu() ({ preempt_disable(); lkpi_cpu_id(); })
#define put_cpu() preempt_enable()
int lkpi_cpu_id(void);
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < (int)nr_cpu_ids; (cpu)++)
#define for_each_online_cpu(cpu) for_each_possible_cpu(cpu)
#define for_each_present_cpu(cpu) for_each_possible_cpu(cpu)
#define cpu_to_node(cpu) 0
#define numa_node_id() 0
#define dev_to_node(dev) NUMA_NO_NODE
#define cpu_online(cpu) ((cpu) < (int)nr_cpu_ids)

#define trace_printk(fmt, ...) do { } while (0)

enum cpuhp_state { CPUHP_INVALID = -1, CPUHP_OFFLINE = 0, CPUHP_RADIX_DEAD = 20, CPUHP_AP_ONLINE_DYN = 200 };
#define cpuhp_setup_state_nocalls(state, name, startup, teardown) 0
#define cpuhp_setup_state(state, name, startup, teardown) 0
#define cpuhp_remove_state(state) do { } while (0)
#define cpuhp_remove_state_nocalls(state) do { } while (0)

struct static_key { atomic_t enabled; };
struct static_key_true { struct static_key key; };
struct static_key_false { struct static_key key; };
#define STATIC_KEY_INIT_TRUE { .enabled = ATOMIC_INIT(1) }
#define STATIC_KEY_INIT_FALSE { .enabled = ATOMIC_INIT(0) }
#define STATIC_KEY_TRUE_INIT (struct static_key_true) { .key = STATIC_KEY_INIT_TRUE }
#define STATIC_KEY_FALSE_INIT (struct static_key_false) { .key = STATIC_KEY_INIT_FALSE }
#define DEFINE_STATIC_KEY_TRUE(name) struct static_key_true name = STATIC_KEY_TRUE_INIT
#define DEFINE_STATIC_KEY_FALSE(name) struct static_key_false name = STATIC_KEY_FALSE_INIT
#define DEFINE_STATIC_KEY_TRUE_RO DEFINE_STATIC_KEY_TRUE
#define DEFINE_STATIC_KEY_FALSE_RO DEFINE_STATIC_KEY_FALSE
#define DECLARE_STATIC_KEY_TRUE(name) extern struct static_key_true name
#define DECLARE_STATIC_KEY_FALSE(name) extern struct static_key_false name
#define static_key_enabled(x) (atomic_read(&(x)->key.enabled) > 0)
#define static_branch_likely(x) likely(static_key_enabled(x))
#define static_branch_unlikely(x) unlikely(static_key_enabled(x))
#define static_branch_enable(x) atomic_set(&(x)->key.enabled, 1)
#define static_branch_disable(x) atomic_set(&(x)->key.enabled, 0)
#define static_branch_inc(x) atomic_inc(&(x)->key.enabled)
#define static_branch_dec(x) atomic_dec(&(x)->key.enabled)
#define static_key_slow_inc(k) atomic_inc(&(k)->enabled)
#define static_key_slow_dec(k) atomic_dec(&(k)->enabled)
#define static_branch_maybe(config, x) static_branch_unlikely(x)

enum class_map_type { DD_CLASS_TYPE_DISJOINT_BITS, DD_CLASS_TYPE_LEVEL_NUM, DD_CLASS_TYPE_DISJOINT_NAMES, DD_CLASS_TYPE_LEVEL_NAMES };
struct ddebug_class_map { const char *mod_name; const char **class_names; int length; int base; enum class_map_type map_type; };
#define DECLARE_DYNDBG_CLASSMAP(_var, _maptype, _base, ...) \
	static const char *_var##_classnames[] = { __VA_ARGS__ }; \
	static struct ddebug_class_map __maybe_unused _var = { .mod_name = KBUILD_MODNAME, .base = _base, \
		.map_type = _maptype, .length = ARRAY_SIZE(_var##_classnames), .class_names = _var##_classnames }
#define DYNDBG_CLASSMAP_DEFINE(...)
#define DYNDBG_CLASSMAP_USE(...)
#define DYNAMIC_DEBUG_BRANCH(descriptor) false
#define tracing_off() do { } while (0)
#define kmemleak_update_trace(p) do { } while (0)
#define kmemleak_alloc(p, s, c, g) do { } while (0)
#define kmemleak_free(p) do { } while (0)
#define kmemleak_not_leak(p) do { } while (0)
#define kmemleak_ignore(p) do { } while (0)

#include <linux/pfn.h>
#define PAGE_SECTORS_SHIFT (PAGE_SHIFT - 9)

#endif
