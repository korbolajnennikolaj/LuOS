// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_MEMCORE_H
#define _LKPI_MEMCORE_H

#include <lkpi/sched.h>


#define ___GFP_DMA 0x01u
#define ___GFP_HIGHMEM 0x02u
#define ___GFP_DMA32 0x04u
#define ___GFP_MOVABLE 0x08u
#define ___GFP_RECLAIMABLE 0x10u
#define ___GFP_HIGH 0x20u
#define ___GFP_IO 0x40u
#define ___GFP_FS 0x80u
#define ___GFP_ZERO 0x100u
#define ___GFP_DIRECT_RECLAIM 0x400u
#define ___GFP_KSWAPD_RECLAIM 0x800u
#define ___GFP_WRITE 0x1000u
#define ___GFP_NOWARN 0x2000u
#define ___GFP_RETRY_MAYFAIL 0x4000u
#define ___GFP_NOFAIL 0x8000u
#define ___GFP_NORETRY 0x10000u
#define ___GFP_MEMALLOC 0x20000u
#define ___GFP_COMP 0x40000u
#define ___GFP_NOMEMALLOC 0x80000u
#define ___GFP_HARDWALL 0x100000u
#define ___GFP_THISNODE 0x200000u
#define ___GFP_ACCOUNT 0x400000u
#define ___GFP_ZEROTAGS 0x800000u
#define ___GFP_NOLOCKDEP 0x1000000u
#define __GFP_DMA ((__force gfp_t)___GFP_DMA)
#define __GFP_HIGHMEM ((__force gfp_t)___GFP_HIGHMEM)
#define __GFP_DMA32 ((__force gfp_t)___GFP_DMA32)
#define __GFP_MOVABLE ((__force gfp_t)___GFP_MOVABLE)
#define __GFP_RECLAIMABLE ((__force gfp_t)___GFP_RECLAIMABLE)
#define GFP_ZONEMASK (__GFP_DMA | __GFP_HIGHMEM | __GFP_DMA32 | __GFP_MOVABLE)
#define __GFP_HIGH ((__force gfp_t)___GFP_HIGH)
#define __GFP_IO ((__force gfp_t)___GFP_IO)
#define __GFP_FS ((__force gfp_t)___GFP_FS)
#define __GFP_ZERO ((__force gfp_t)___GFP_ZERO)
#define __GFP_DIRECT_RECLAIM ((__force gfp_t)___GFP_DIRECT_RECLAIM)
#define __GFP_KSWAPD_RECLAIM ((__force gfp_t)___GFP_KSWAPD_RECLAIM)
#define __GFP_RECLAIM ((__force gfp_t)(___GFP_DIRECT_RECLAIM | ___GFP_KSWAPD_RECLAIM))
#define __GFP_WRITE ((__force gfp_t)___GFP_WRITE)
#define __GFP_NOWARN ((__force gfp_t)___GFP_NOWARN)
#define __GFP_RETRY_MAYFAIL ((__force gfp_t)___GFP_RETRY_MAYFAIL)
#define __GFP_NOFAIL ((__force gfp_t)___GFP_NOFAIL)
#define __GFP_NORETRY ((__force gfp_t)___GFP_NORETRY)
#define __GFP_MEMALLOC ((__force gfp_t)___GFP_MEMALLOC)
#define __GFP_COMP ((__force gfp_t)___GFP_COMP)
#define __GFP_NOMEMALLOC ((__force gfp_t)___GFP_NOMEMALLOC)
#define __GFP_HARDWALL ((__force gfp_t)___GFP_HARDWALL)
#define __GFP_THISNODE ((__force gfp_t)___GFP_THISNODE)
#define __GFP_ACCOUNT ((__force gfp_t)___GFP_ACCOUNT)
#define __GFP_ZEROTAGS ((__force gfp_t)___GFP_ZEROTAGS)
#define __GFP_NOLOCKDEP ((__force gfp_t)___GFP_NOLOCKDEP)
#define __GFP_SKIP_ZERO ((__force gfp_t)0)
#define __GFP_SKIP_KASAN ((__force gfp_t)0)
#define __GFP_UNMAPPED ((__force gfp_t)0)
#define GFP_ATOMIC (__GFP_HIGH | __GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT (__GFP_KSWAPD_RECLAIM | __GFP_NOWARN)
#define GFP_NOIO (__GFP_RECLAIM)
#define GFP_NOFS (__GFP_RECLAIM | __GFP_IO)
#define GFP_USER (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA __GFP_DMA
#define GFP_DMA32 __GFP_DMA32
#define GFP_HIGHUSER (GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE (GFP_HIGHUSER | __GFP_MOVABLE)
#define GFP_TRANSHUGE_LIGHT ((GFP_HIGHUSER_MOVABLE | __GFP_COMP | __GFP_NOMEMALLOC | __GFP_NOWARN) & ~__GFP_RECLAIM)
#define GFP_TRANSHUGE (GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)
#define GFP_RECLAIM_MASK (__GFP_RECLAIM | __GFP_HIGH | __GFP_IO | __GFP_FS | __GFP_NOWARN | __GFP_RETRY_MAYFAIL | __GFP_NOFAIL | __GFP_NORETRY | __GFP_MEMALLOC | __GFP_NOMEMALLOC)
#define __GFP_BITS_SHIFT 26
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))
static inline bool gfpflags_allow_blocking(const gfp_t gfp_flags) { return !!(gfp_flags & __GFP_DIRECT_RECLAIM); }
static inline bool gfpflags_normal_context(const gfp_t gfp_flags) { return true; }
#define gfp_has_io_fs(gfp) (((gfp) & (__GFP_IO | __GFP_FS)) == (__GFP_IO | __GFP_FS))


#define ARCH_KMALLOC_MINALIGN 8
#define ARCH_DMA_MINALIGN 64
#define ARCH_SLAB_MINALIGN 8
#define KMALLOC_MAX_SIZE (1UL << 30)
#define KMALLOC_MIN_SIZE 8
#define KMALLOC_SHIFT_HIGH 13
#define KMALLOC_MAX_CACHE_SIZE (1UL << KMALLOC_SHIFT_HIGH)
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

void *lkpi_kmalloc(size_t size, gfp_t flags);
void *lkpi_krealloc(const void *p, size_t new_size, gfp_t flags);
void kfree(const void *objp);
void kfree_sensitive(const void *objp);
size_t ksize(const void *objp);
static __always_inline void *kmalloc(size_t size, gfp_t flags) { return lkpi_kmalloc(size, flags); }
static __always_inline void *kmalloc_noprof(size_t size, gfp_t flags) { return lkpi_kmalloc(size, flags); }
static __always_inline void *kzalloc(size_t size, gfp_t flags) { return lkpi_kmalloc(size, flags | __GFP_ZERO); }
static __always_inline void *kmalloc_node(size_t size, gfp_t flags, int node) { return kmalloc(size, flags); }
static __always_inline void *kzalloc_node(size_t size, gfp_t flags, int node) { return kzalloc(size, flags); }
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes))) return NULL;
	return kmalloc(bytes, flags);
}
static inline void *kcalloc(size_t n, size_t size, gfp_t flags) { return kmalloc_array(n, size, flags | __GFP_ZERO); }
#define kmalloc_array_node(n, size, flags, node) kmalloc_array(n, size, flags)
#define kcalloc_node(n, size, flags, node) kcalloc(n, size, flags)
static inline void *krealloc(const void *p, size_t new_size, gfp_t flags) { return lkpi_krealloc(p, new_size, flags); }
static inline void *krealloc_array(void *p, size_t new_n, size_t new_size, gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(new_n, new_size, &bytes))) return NULL;
	return krealloc(p, bytes, flags);
}
#ifndef default_gfp
#define __default_gfp(a, b, ...) b
#define default_gfp(...) __default_gfp(, ##__VA_ARGS__, GFP_KERNEL)
#endif
#define __alloc_objs(KMALLOC, GFP, TYPE, COUNT) \
({ \
	const size_t __obj_size = size_mul(sizeof(TYPE), COUNT); \
	(TYPE *)KMALLOC(__obj_size, GFP); \
})
#define __alloc_flex(KMALLOC, GFP, TYPE, FAM, COUNT) \
({ \
	const size_t __count = (COUNT); \
	const size_t __obj_size = struct_size_t(TYPE, FAM, __count); \
	TYPE *__obj_ptr = KMALLOC(__obj_size, GFP); \
	__obj_ptr; \
})
#define kmalloc_obj(VAR_OR_TYPE, ...) __alloc_objs(kmalloc, default_gfp(__VA_ARGS__), typeof(VAR_OR_TYPE), 1)
#define kmalloc_objs(VAR_OR_TYPE, COUNT, ...) __alloc_objs(kmalloc, default_gfp(__VA_ARGS__), typeof(VAR_OR_TYPE), COUNT)
#define kmalloc_flex(VAR_OR_TYPE, FAM, COUNT, ...) \
	__alloc_flex(kmalloc, default_gfp(__VA_ARGS__), typeof(VAR_OR_TYPE), FAM, COUNT)
#define kzalloc_obj(P, ...) __alloc_objs(kzalloc, default_gfp(__VA_ARGS__), typeof(P), 1)
#define kzalloc_objs(P, COUNT, ...) __alloc_objs(kzalloc, default_gfp(__VA_ARGS__), typeof(P), COUNT)
#define kzalloc_flex(P, FAM, COUNT, ...) __alloc_flex(kzalloc, default_gfp(__VA_ARGS__), typeof(P), FAM, COUNT)
#define kvmalloc_obj(P, ...) __alloc_objs(kvmalloc, default_gfp(__VA_ARGS__), typeof(P), 1)
#define kvmalloc_objs(P, COUNT, ...) __alloc_objs(kvmalloc, default_gfp(__VA_ARGS__), typeof(P), COUNT)
#define kvmalloc_flex(P, FAM, COUNT, ...) __alloc_flex(kvmalloc, default_gfp(__VA_ARGS__), typeof(P), FAM, COUNT)
#define kvzalloc_obj(P, ...) __alloc_objs(kvzalloc, default_gfp(__VA_ARGS__), typeof(P), 1)
#define kvzalloc_objs(P, COUNT, ...) __alloc_objs(kvzalloc, default_gfp(__VA_ARGS__), typeof(P), COUNT)
#define kvzalloc_flex(P, FAM, COUNT, ...) __alloc_flex(kvzalloc, default_gfp(__VA_ARGS__), typeof(P), FAM, COUNT)
#define kmalloc_track_caller(size, flags) kmalloc(size, flags)
#define kmalloc_node_track_caller(size, flags, node) kmalloc(size, flags)
#define kmalloc_size_roundup(size) (size)
void *kmemdup(const void *src, size_t len, gfp_t gfp);
void *kmemdup_nul(const char *s, size_t len, gfp_t gfp);
#define kmemdup_array(src, count, element_size, gfp) kmemdup(src, size_mul(count, element_size), gfp)
char *kstrdup(const char *s, gfp_t gfp);
const char *kstrdup_const(const char *s, gfp_t gfp);
char *kstrndup(const char *s, size_t max, gfp_t gfp);
void kfree_const(const void *x);
__printf(2, 3) char *kasprintf(gfp_t gfp, const char *fmt, ...);
__printf(2, 0) char *kvasprintf(gfp_t gfp, const char *fmt, va_list args);
__printf(2, 0) const char *kvasprintf_const(gfp_t gfp, const char *fmt, va_list args);
char **argv_split(gfp_t gfp, const char *str, int *argcp);
void argv_free(char **argv);
void *memdup_user(const void __user *src, size_t len);
void *vmemdup_user(const void __user *src, size_t len);
void *memdup_user_nul(const void __user *src, size_t len);
#define memdup_array_user(src, n, size) memdup_user(src, size_mul(n, size))
#define vmemdup_array_user(src, n, size) vmemdup_user(src, size_mul(n, size))
char *strndup_user(const char __user *s, long n);

void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);
void *vmalloc_user(unsigned long size);
void *vmalloc_32(unsigned long size);
void *vmalloc_noprof(unsigned long size);
#define vmalloc_node(size, node) vmalloc(size)
#define vzalloc_node(size, node) vzalloc(size)
#define vmalloc_array(n, size) vmalloc(size_mul(n, size))
#define vcalloc(n, size) vzalloc(size_mul(n, size))
void *__vmalloc(unsigned long size, gfp_t gfp_mask);
void vfree(const void *addr);
bool is_vmalloc_addr(const void *x);
void *kvmalloc_node(size_t size, gfp_t flags, int node);
#define kvmalloc(size, flags) kvmalloc_node(size, flags, NUMA_NO_NODE)
#define kvzalloc(size, flags) kvmalloc(size, (flags) | __GFP_ZERO)
#define kvzalloc_node(size, flags, node) kvmalloc_node(size, (flags) | __GFP_ZERO, node)
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes))) return NULL;
	return kvmalloc(bytes, flags);
}
#define kvcalloc(n, size, flags) kvmalloc_array(n, size, (flags) | __GFP_ZERO)
#define kvmalloc_array_node(n, size, flags, node) kvmalloc_array(n, size, flags)
void *kvrealloc(const void *p, size_t size, gfp_t flags);
void kvfree(const void *addr);
void kvfree_sensitive(const void *addr, size_t len);
#define kvmemdup(src, len, gfp) kmemdup(src, len, gfp)

#define SLAB_HWCACHE_ALIGN 0x00002000U
#define SLAB_CACHE_DMA 0x00004000U
#define SLAB_CACHE_DMA32 0x00008000U
#define SLAB_PANIC 0x00040000U
#define SLAB_TYPESAFE_BY_RCU 0x00080000U
#define SLAB_RECLAIM_ACCOUNT 0x00020000U
#define SLAB_ACCOUNT 0x04000000U
#define SLAB_MEM_SPREAD 0
#define SLAB_NO_MERGE 0x01000000U
typedef unsigned int slab_flags_t;
struct kmem_cache;
struct kmem_cache_args {
	unsigned int align;
	unsigned int useroffset;
	unsigned int usersize;
	unsigned int freeptr_offset;
	bool use_freeptr_offset;
	void (*ctor)(void *);
	unsigned int sheaf_capacity;
};
struct slab_sheaf;
struct slab_sheaf *kmem_cache_prefill_sheaf(struct kmem_cache *s, gfp_t gfp, unsigned int size);
int kmem_cache_refill_sheaf(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf **sheafp, unsigned int size);
void kmem_cache_return_sheaf(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf *sheaf);
void *kmem_cache_alloc_from_sheaf(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf *sheaf);
unsigned int kmem_cache_sheaf_size(struct slab_sheaf *sheaf);
void kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p);
int kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t gfp, size_t size, void **p);
struct kmem_cache *__kmem_cache_create_args(const char *name, unsigned int object_size,
					    struct kmem_cache_args *args, slab_flags_t flags);
static inline struct kmem_cache *__kmem_cache_create(const char *name, unsigned int size, unsigned int align,
						     slab_flags_t flags, void (*ctor)(void *))
{
	struct kmem_cache_args kmem_args = { .align = align, .ctor = ctor };
	return __kmem_cache_create_args(name, size, &kmem_args, flags);
}
static inline struct kmem_cache *__kmem_cache_default_args(const char *name, unsigned int size,
							   struct kmem_cache_args *args, slab_flags_t flags)
{
	struct kmem_cache_args kmem_default_args = {};
	return __kmem_cache_create_args(name, size, args ? args : &kmem_default_args, flags);
}
#define kmem_cache_create(__name, __object_size, __args, ...) \
	_Generic((__args), \
		struct kmem_cache_args *: __kmem_cache_create_args, \
		void *: __kmem_cache_default_args, \
		default: __kmem_cache_create)(__name, __object_size, __args, __VA_ARGS__)
#define kmem_cache_create_usercopy(name, size, align, flags, useroffset, usersize, ctor) \
	__kmem_cache_create(name, size, align, flags, ctor)
#define KMEM_CACHE(__struct, __flags) \
	__kmem_cache_create_args(#__struct, sizeof(struct __struct), \
		&(struct kmem_cache_args) { .align = __alignof__(struct __struct) }, (__flags))
#define KMEM_CACHE_USERCOPY(__struct, __flags, __field) KMEM_CACHE(__struct, __flags)
void kmem_cache_destroy(struct kmem_cache *s);
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags);
#define kmem_cache_alloc_noprof kmem_cache_alloc
#define kmem_cache_alloc_node(s, flags, node) kmem_cache_alloc(s, flags)
static inline void *kmem_cache_zalloc(struct kmem_cache *k, gfp_t flags) { return kmem_cache_alloc(k, flags | __GFP_ZERO); }
void kmem_cache_free(struct kmem_cache *s, void *objp);
int kmem_cache_shrink(struct kmem_cache *s);
unsigned int kmem_cache_size(struct kmem_cache *s);
#define kmem_cache_alloc_lru(s, lru, flags) kmem_cache_alloc(s, flags)

#include <linux/cleanup.h>
DEFINE_FREE(kfree, void *, if (!IS_ERR_OR_NULL(_T)) kfree(_T))
DEFINE_FREE(kvfree, void *, if (!IS_ERR_OR_NULL(_T)) kvfree(_T))
DEFINE_FREE(vfree, void *, if (_T) vfree(_T))
DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))
DEFINE_GUARD_COND(mutex, _try, mutex_trylock(_T))
DEFINE_GUARD_COND(mutex, _intr, mutex_lock_interruptible(_T), _RET == 0)
DEFINE_LOCK_GUARD_1(spinlock, spinlock_t, spin_lock(_T->lock), spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t, spin_lock_irq(_T->lock), spin_unlock_irq(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t, spin_lock_irqsave(_T->lock, _T->flags),
		    spin_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t, raw_spin_lock(_T->lock), raw_spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t, raw_spin_lock_irqsave(_T->lock, _T->flags),
		    raw_spin_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())
DEFINE_LOCK_GUARD_0(preempt, preempt_disable(), preempt_enable())
DEFINE_LOCK_GUARD_0(irq, local_irq_disable(), local_irq_enable())
DEFINE_LOCK_GUARD_0(irqsave, local_irq_save(_T->flags), local_irq_restore(_T->flags), unsigned long flags)
DEFINE_GUARD(rwsem_read, struct rw_semaphore *, down_read(_T), up_read(_T))
DEFINE_GUARD(rwsem_write, struct rw_semaphore *, down_write(_T), up_write(_T))
DEFINE_LOCK_GUARD_1(srcu, struct srcu_struct, _T->idx = srcu_read_lock(_T->lock), srcu_read_unlock(_T->lock, _T->idx), int idx)


#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))
#define small_const_nbits(nbits) (__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG && (nbits) > 0)
static inline void bitmap_zero(unsigned long *dst, unsigned int nbits) { memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long)); }
static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
	memset(dst, 0xff, len);
	if (nbits % BITS_PER_LONG) dst[nbits / BITS_PER_LONG] &= BITMAP_LAST_WORD_MASK(nbits);
}
static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	memcpy(dst, src, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}
void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits);
void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits);
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits);
bool bitmap_empty(const unsigned long *src, unsigned int nbits);
bool bitmap_full(const unsigned long *src, unsigned int nbits);
bool bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned int nbits);
bool bitmap_intersects(const unsigned long *a, const unsigned long *b, unsigned int nbits);
bool bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned int nbits);
bool bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_xor(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
bool bitmap_andnot(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned int nbits);
void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits);
void bitmap_shift_left(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits);
unsigned long bitmap_find_next_zero_area_off(unsigned long *map, unsigned long size, unsigned long start,
					     unsigned int nr, unsigned long align_mask, unsigned long align_offset);
static inline unsigned long bitmap_find_next_zero_area(unsigned long *map, unsigned long size, unsigned long start,
						       unsigned int nr, unsigned long align_mask)
{
	return bitmap_find_next_zero_area_off(map, size, start, nr, align_mask, 0);
}
int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order);
void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order);
int bitmap_allocate_region(unsigned long *bitmap, unsigned int pos, int order);
unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags);
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags);
void bitmap_free(const unsigned long *bitmap);
#define devm_bitmap_zalloc(dev, nbits, flags) bitmap_zalloc(nbits, flags)
static inline void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf, unsigned int nbits)
{
	for (unsigned int i = 0; i < BITS_TO_U32(nbits); i++) {
		if (i % 2) bitmap[i / 2] |= (unsigned long)buf[i] << 32;
		else bitmap[i / 2] = buf[i];
	}
}
static inline void bitmap_to_arr32(u32 *buf, const unsigned long *bitmap, unsigned int nbits)
{
	for (unsigned int i = 0; i < BITS_TO_U32(nbits); i++)
		buf[i] = (i % 2) ? (u32)(bitmap[i / 2] >> 32) : (u32)bitmap[i / 2];
}
static inline unsigned long bitmap_read(const unsigned long *map, unsigned long start, unsigned long nbits)
{
	unsigned long r = 0;
	for (unsigned long i = 0; i < nbits; i++) if (test_bit(start + i, map)) r |= 1UL << i;
	return r;
}
static inline void bitmap_write(unsigned long *map, unsigned long value, unsigned long start, unsigned long nbits)
{
	for (unsigned long i = 0; i < nbits; i++) {
		if (value & (1UL << i)) __set_bit(start + i, map);
		else __clear_bit(start + i, map);
	}
}
#define find_first_and_bit(a, b, size) find_next_and_bit(a, b, size, 0)

typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;
typedef struct cpumask cpumask_var_t[1];
#define cpumask_bits(maskp) ((maskp)->bits)
extern struct cpumask lkpi_cpu_online_mask;
#define cpu_online_mask (&lkpi_cpu_online_mask)
#define cpu_possible_mask (&lkpi_cpu_online_mask)
#define cpu_present_mask (&lkpi_cpu_online_mask)
#define cpumask_of(cpu) (&lkpi_cpu_online_mask)
#define cpumask_first(m) ((unsigned int)find_first_bit(cpumask_bits(m), nr_cpu_ids))
#define cpumask_next(n, m) ((unsigned int)find_next_bit(cpumask_bits(m), nr_cpu_ids, (n) + 1))
#define for_each_cpu(cpu, mask) for_each_set_bit(cpu, cpumask_bits(mask), nr_cpu_ids)
#define cpumask_weight(m) bitmap_weight(cpumask_bits(m), nr_cpu_ids)
#define cpumask_test_cpu(cpu, m) test_bit(cpu, cpumask_bits(m))
#define cpumask_set_cpu(cpu, m) set_bit(cpu, cpumask_bits(m))
#define cpumask_clear_cpu(cpu, m) clear_bit(cpu, cpumask_bits(m))
#define cpumask_clear(m) bitmap_zero(cpumask_bits(m), NR_CPUS)
#define cpumask_copy(d, s) bitmap_copy(cpumask_bits(d), cpumask_bits(s), NR_CPUS)
#define zalloc_cpumask_var(m, f) ({ cpumask_clear(*(m)); true; })
#define alloc_cpumask_var(m, f) true
#define free_cpumask_var(m) do { } while (0)

#endif
