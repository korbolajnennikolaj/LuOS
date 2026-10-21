// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_MM_H
#define _LKPI_MM_H

#include <lkpi/core.h>
#include <linux/kref.h>
#include <linux/rbtree.h>
#include <linux/maple_tree.h>
#include <linux/xarray.h>

struct device;
struct address_space;
struct file;

struct address_space;

struct page {
        unsigned long flags;
        union {
                struct list_head lru;
                struct {
                        void *__filler;
                        unsigned int mlock_count;
                };
        };
        struct address_space *mapping;
        pgoff_t index;
        unsigned long private;
        atomic_t _mapcount;
        atomic_t _refcount;
};

struct folio {
        union {
                struct {
                        unsigned long flags;
                        struct list_head lru;
                        struct address_space *mapping;
                        pgoff_t index;
                        void *private;
                        atomic_t _mapcount;
                        atomic_t _refcount;
                };
                struct page page;
        };
};

#define LKPI_VMEMMAP_BASE 0xFFFFC00000000000UL
#define vmemmap ((struct page *)LKPI_VMEMMAP_BASE)
#define LKPI_HHDM_BASE lkpi_hhdm_base
extern unsigned long lkpi_hhdm_base;

#define page_to_pfn(pg) ((unsigned long)((const struct page *)(pg) - vmemmap))
#define pfn_to_page(pfn) (vmemmap + (pfn))
#define page_to_phys(pg) ((phys_addr_t)page_to_pfn(pg) << PAGE_SHIFT)
#define phys_to_page(phys) pfn_to_page((phys) >> PAGE_SHIFT)
#define __pa(x) lkpi_virt_to_phys((const void *)(x))
#define __va(x) ((void *)((unsigned long)(x) + lkpi_hhdm_base))
#define virt_to_phys(x) lkpi_virt_to_phys((const void *)(x))
#define phys_to_virt(x) __va(x)
#define virt_to_page(addr) pfn_to_page(lkpi_virt_to_phys((const void *)(addr)) >> PAGE_SHIFT)
#define virt_to_pfn(addr) (lkpi_virt_to_phys((const void *)(addr)) >> PAGE_SHIFT)
#define virt_to_folio(addr) page_folio(virt_to_page(addr))
#define page_address(pg) __va(page_to_phys(pg))
#define lowmem_page_address(pg) page_address(pg)
#define pfn_valid(pfn) lkpi_pfn_valid(pfn)
#define virt_addr_valid(addr) true
#define page_to_virt(pg) page_address(pg)
#define pfn_to_kaddr(pfn) __va((pfn) << PAGE_SHIFT)
#define nth_page(pg, n) ((pg) + (n))
#define folio_page(folio, n) (&(folio)->page + (n))
#define folio_file_page(folio, index) folio_page(folio, 0)
#define page_folio(p) ((struct folio *)(p))
#define folio_pfn(folio) page_to_pfn(&(folio)->page)
#define folio_address(folio) page_address(&(folio)->page)
#define pfn_folio(pfn) page_folio(pfn_to_page(pfn))
phys_addr_t lkpi_virt_to_phys(const void *x);
#define MAX_PAGE_ORDER 10
#define NR_PAGE_ORDERS (MAX_PAGE_ORDER + 1)
bool lkpi_pfn_valid(unsigned long pfn);

#define PG_locked 0
#define PG_referenced 2
#define PG_uptodate 3
#define PG_dirty 4
#define PG_lru 5
#define PG_active 6
#define PG_reserved 10
#define PG_private 11
#define PG_writeback 13
#define PG_head 15
#define PG_mappedtodisk 16
#define PG_reclaim 17
#define PG_swapbacked 18
#define PG_unevictable 19
#define PG_lkpi_wc 28
#define PG_lkpi_uc 29

#define __LKPI_PAGEFLAG(Name, bit) \
        static inline bool Page##Name(const struct page *page) { return test_bit(bit, &page->flags); } \
        static inline void SetPage##Name(struct page *page) { set_bit(bit, &page->flags); } \
        static inline void ClearPage##Name(struct page *page) { clear_bit(bit, &page->flags); } \
        static inline void __SetPage##Name(struct page *page) { __set_bit(bit, &page->flags); } \
        static inline void __ClearPage##Name(struct page *page) { __clear_bit(bit, &page->flags); } \
        static inline bool folio_test_##Name(const struct folio *folio) { return test_bit(bit, &folio->flags); } \
        static inline void folio_set_##Name(struct folio *folio) { set_bit(bit, &folio->flags); } \
        static inline void folio_clear_##Name(struct folio *folio) { clear_bit(bit, &folio->flags); }
__LKPI_PAGEFLAG(Reserved, PG_reserved)
__LKPI_PAGEFLAG(Dirty, PG_dirty)
__LKPI_PAGEFLAG(Uptodate, PG_uptodate)
__LKPI_PAGEFLAG(Locked, PG_locked)
__LKPI_PAGEFLAG(Private, PG_private)
__LKPI_PAGEFLAG(Writeback, PG_writeback)
__LKPI_PAGEFLAG(Referenced, PG_referenced)
__LKPI_PAGEFLAG(Unevictable, PG_unevictable)
__LKPI_PAGEFLAG(SwapBacked, PG_swapbacked)
#define folio_test_dirty_lkpi folio_test_Dirty
#define folio_mark_dirty(folio) ({ folio_set_Dirty(folio); true; })
#define set_page_dirty(page) ({ SetPageDirty(page); 1; })
#define set_page_dirty_lock(page) set_page_dirty(page)
#define folio_mark_accessed(folio) do { } while (0)
#define mark_page_accessed(page) do { } while (0)
#define folio_test_large(folio) false
#define folio_test_highmem(folio) false
#define PageHighMem(page) false
#define PageCompound(page) false
#define PageTail(page) false
#define PageSlab(page) false
#define folio_order(folio) 0
#define folio_nr_pages(folio) 1L
#define folio_size(folio) PAGE_SIZE
#define folio_shift(folio) PAGE_SHIFT
#define folio_mapping(folio) ((folio)->mapping)
#define compound_order(page) 0
#define compound_head(page) (page)
#define page_size(page) PAGE_SIZE
#define page_private(page) ((page)->private)
#define set_page_private(page, v) do { (page)->private = (v); } while (0)
#define folio_lock(f) do { } while (0)
#define folio_unlock(f) do { } while (0)
#define folio_trylock(f) true
#define lock_page(p) do { } while (0)
#define unlock_page(p) do { } while (0)
#define trylock_page(p) true
#define folio_wait_writeback(f) do { } while (0)
#define folio_clear_dirty_for_io(f) false
#define folio_index(f) ((f)->index)
#define page_mapped(p) false
#define folio_mapped(f) false
#define PageAnon(p) false
#define folio_test_anon(f) false

static inline int page_ref_count(const struct page *page) { return atomic_read(&page->_refcount); }
static inline int folio_ref_count(const struct folio *folio) { return atomic_read(&folio->_refcount); }
static inline void set_page_count(struct page *page, int v) { atomic_set(&page->_refcount, v); }
static inline void init_page_count(struct page *page) { set_page_count(page, 1); }
static inline void get_page(struct page *page) { atomic_inc(&page->_refcount); }
static inline void folio_get(struct folio *folio) { atomic_inc(&folio->_refcount); }
static inline bool folio_try_get(struct folio *folio) { return atomic_inc_not_zero(&folio->_refcount); }
static inline bool get_page_unless_zero(struct page *page) { return atomic_inc_not_zero(&page->_refcount); }
void __free_pages(struct page *page, unsigned int order);
void put_page(struct page *page);
static inline void folio_put(struct folio *folio) { put_page(&folio->page); }
static inline void folio_put_refs(struct folio *folio, int refs) { while (refs--) folio_put(folio); }
static inline int page_ref_inc_return(struct page *page) { return atomic_inc_return(&page->_refcount); }
static inline int page_ref_dec_return(struct page *page) { return atomic_dec_return(&page->_refcount); }
static inline void page_ref_inc(struct page *page) { atomic_inc(&page->_refcount); }
static inline void page_ref_dec(struct page *page) { atomic_dec(&page->_refcount); }
static inline int put_page_testzero(struct page *page) { return atomic_dec_and_test(&page->_refcount); }
#define page_count(p) page_ref_count(p)
#define folio_ref_add(f, n) atomic_add(n, &(f)->_refcount)

struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order);
#define alloc_pages(gfp, order) alloc_pages_noprof(gfp, order)
#define alloc_pages_node(nid, gfp, order) alloc_pages(gfp, order)
#define alloc_page(gfp) alloc_pages(gfp, 0)
#define alloc_page_vma(gfp, vma, addr) alloc_page(gfp)
#define __alloc_pages(gfp, order, nid, nodemask) alloc_pages(gfp, order)
#define folio_alloc(gfp, order) page_folio(alloc_pages(gfp, order))
#define __free_page(page) __free_pages((page), 0)
#define free_page(addr) free_pages((addr), 0)
unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
#define __get_free_page(gfp) __get_free_pages((gfp), 0)
unsigned long get_zeroed_page(gfp_t gfp_mask);
void free_pages(unsigned long addr, unsigned int order);
void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);
#define get_order(n) (__builtin_constant_p(n) ? \
        ((n) == 0UL ? BITS_PER_LONG - PAGE_SHIFT : (((n) < (1UL << PAGE_SHIFT)) ? 0 : ilog2((n) - 1) - PAGE_SHIFT + 1)) : \
        lkpi_get_order(n))
static inline int lkpi_get_order(unsigned long size) { size--; size >>= PAGE_SHIFT; return fls64(size); }
unsigned long totalram_pages(void);
#define si_mem_available() ((long)totalram_pages() / 2)
void si_meminfo(struct sysinfo *val);
#define totalhigh_pages() 0UL
#define nr_free_pages() (totalram_pages() / 2)

static inline void *kmap(struct page *page) { return page_address(page); }
static inline void kunmap(struct page *page) { }
static inline void *kmap_atomic(struct page *page) { preempt_disable(); return page_address(page); }
static inline void kunmap_atomic_impl(void *addr) { preempt_enable(); }
#define kunmap_atomic(addr) kunmap_atomic_impl(addr)
static inline void *kmap_local_page(struct page *page) { return page_address(page); }
static inline void *kmap_local_page_try_from_panic(struct page *page) { return page_address(page); }
static inline void *kmap_local_folio(struct folio *folio, size_t offset) { return (char *)folio_address(folio) + offset; }
static inline void *kmap_local_pfn(unsigned long pfn) { return pfn_to_kaddr(pfn); }
#define kunmap_local(addr) do { (void)(addr); } while (0)
static inline void *kmap_atomic_pfn(unsigned long pfn) { preempt_disable(); return pfn_to_kaddr(pfn); }
static inline void clear_page(void *page) { memset(page, 0, PAGE_SIZE); }
static inline void copy_page(void *to, void *from) { memcpy(to, from, PAGE_SIZE); }
static inline void clear_highpage(struct page *page) { clear_page(page_address(page)); }
static inline void memcpy_from_page(char *to, struct page *page, size_t offset, size_t len) { memcpy(to, (char *)page_address(page) + offset, len); }
static inline void memcpy_to_page(struct page *page, size_t offset, const char *from, size_t len) { memcpy((char *)page_address(page) + offset, from, len); }
static inline void memzero_page(struct page *page, size_t offset, size_t len) { memset((char *)page_address(page) + offset, 0, len); }
#define flush_dcache_page(page) do { } while (0)
#define flush_dcache_folio(folio) do { } while (0)
#define flush_kernel_vmap_range(a, s) do { } while (0)
#define invalidate_kernel_vmap_range(a, s) do { } while (0)

#define VM_MAP 0x00000004
#define VM_IOREMAP 0x00000001
#define VM_ALLOC 0x00000002
#define VM_USERMAP 0x00000008
#define VM_DMA_COHERENT 0x00000010
typedef struct { unsigned long pgprot; } pgprot_t;
void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot);
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot);
void vunmap(const void *addr);
struct page *vmalloc_to_page(const void *addr);
unsigned long vmalloc_to_pfn(const void *addr);
#define VMALLOC_START 0xFFFFB00000000000UL
#define VMALLOC_END 0xFFFFC00000000000UL

#define _PAGE_PRESENT 0x001UL
#define _PAGE_RW 0x002UL
#define _PAGE_USER 0x004UL
#define _PAGE_PWT 0x008UL
#define _PAGE_PCD 0x010UL
#define _PAGE_ACCESSED 0x020UL
#define _PAGE_DIRTY 0x040UL
#define _PAGE_PAT 0x080UL
#define _PAGE_GLOBAL 0x100UL
#define _PAGE_NX (1UL << 63)
#define _PAGE_CACHE_MASK (_PAGE_PWT | _PAGE_PCD | _PAGE_PAT)
#define _PAGE_CACHE_WB 0UL
#define _PAGE_CACHE_WC (_PAGE_PAT | _PAGE_PWT)
#define _PAGE_CACHE_UC_MINUS (_PAGE_PCD)
#define _PAGE_CACHE_UC (_PAGE_PCD | _PAGE_PWT)
#define pgprot_val(x) ((x).pgprot)
#define __pgprot(x) ((pgprot_t){ (x) })
#define PAGE_NONE __pgprot(0)
#define PAGE_SHARED __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_NX)
#define PAGE_READONLY __pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_NX)
#define PAGE_KERNEL __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_NX | _PAGE_GLOBAL)
#define PAGE_KERNEL_RO __pgprot(_PAGE_PRESENT | _PAGE_NX | _PAGE_GLOBAL)
#define PAGE_KERNEL_IO PAGE_KERNEL
#define PAGE_KERNEL_NOCACHE __pgprot(pgprot_val(PAGE_KERNEL) | _PAGE_CACHE_UC)
#define PAGE_KERNEL_EXEC __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_GLOBAL)
static inline pgprot_t pgprot_writecombine(pgprot_t p) { return __pgprot((pgprot_val(p) & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_WC); }
static inline pgprot_t pgprot_noncached(pgprot_t p) { return __pgprot((pgprot_val(p) & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_UC_MINUS); }
#define pgprot_device pgprot_noncached
#define pgprot_decrypted(p) (p)
#define pgprot_encrypted(p) (p)
#define pgprot_dmacoherent(p) (p)
#define pgprot_tagged(p) (p)
#define vm_get_page_prot(flags) lkpi_vm_get_page_prot(flags)

int set_memory_wc(unsigned long addr, int numpages);
int set_memory_uc(unsigned long addr, int numpages);
int set_memory_wb(unsigned long addr, int numpages);
int set_pages_array_wc(struct page **pages, int addrinarray);
int set_pages_array_uc(struct page **pages, int addrinarray);
int set_pages_array_wb(struct page **pages, int addrinarray);
int set_pages_wb(struct page *page, int numpages);
int set_pages_uc(struct page *page, int numpages);
#define set_memory_decrypted(a, n) 0
#define set_memory_encrypted(a, n) 0
#define cc_platform_has(attr) false
#define mem_encrypt_active() false
#define sme_me_mask 0ULL
void clflush_cache_range(void *addr, unsigned int size);
#define drm_clflush_virt_range_lkpi clflush_cache_range
static inline bool static_cpu_has_clflush(void) { return true; }
void wbinvd_on_all_cpus(void);
static inline void clflush(volatile void *p) { asm volatile("clflush %0" : "+m"(*(volatile char *)p)); }
extern bool lkpi_has_clflushopt;
static inline void clflushopt(volatile void *p)
{
	if (lkpi_has_clflushopt) asm volatile(".byte 0x66; clflush %0" : "+m"(*(volatile char *)p));
	else asm volatile("clflush %0" : "+m"(*(volatile char *)p));
}
void kernel_fpu_begin(void);
void kernel_fpu_end(void);
#define kernel_fpu_begin_mask(m) kernel_fpu_begin()
#define may_use_simd() (!lkpi_in_irq())


typedef unsigned long vm_flags_t;
#define VM_NONE 0x00000000
#define VM_READ 0x00000001
#define VM_WRITE 0x00000002
#define VM_EXEC 0x00000004
#define VM_SHARED 0x00000008
#define VM_MAYREAD 0x00000010
#define VM_MAYWRITE 0x00000020
#define VM_MAYEXEC 0x00000040
#define VM_MAYSHARE 0x00000080
#define VM_GROWSDOWN 0x00000100
#define VM_PFNMAP 0x00000400
#define VM_LOCKED 0x00002000
#define VM_IO 0x00004000
#define VM_SEQ_READ 0x00008000
#define VM_RAND_READ 0x00010000
#define VM_DONTCOPY 0x00020000
#define VM_DONTEXPAND 0x00040000
#define VM_LOCKONFAULT 0x00080000
#define VM_ACCOUNT 0x00100000
#define VM_NORESERVE 0x00200000
#define VM_HUGETLB 0x00400000
#define VM_SYNC 0x00800000
#define VM_WIPEONFORK 0x02000000
#define VM_DONTDUMP 0x04000000
#define VM_MIXEDMAP 0x10000000
#define VM_HUGEPAGE 0x20000000
#define VM_NOHUGEPAGE 0x40000000
#define VM_ACCESS_FLAGS (VM_READ | VM_WRITE | VM_EXEC)

struct vm_area_struct;
struct mm_struct {
        struct rw_semaphore mmap_lock;
        atomic_t mm_users;
        atomic_t mm_count;
        unsigned long total_vm;
        void *luos_mm;
};
#define mmap_read_lock(mm) down_read(&(mm)->mmap_lock)
#define mmap_read_unlock(mm) up_read(&(mm)->mmap_lock)
#define mmap_write_lock(mm) down_write(&(mm)->mmap_lock)
#define mmap_write_unlock(mm) up_write(&(mm)->mmap_lock)
#define mmap_read_trylock(mm) down_read_trylock(&(mm)->mmap_lock)
#define mmap_read_lock_killable(mm) down_read_killable(&(mm)->mmap_lock)
#define mmap_assert_locked(mm) do { } while (0)
#define mmap_assert_write_locked(mm) do { } while (0)
#define mmgrab(mm) atomic_inc(&(mm)->mm_count)
#define mmdrop(mm) atomic_dec(&(mm)->mm_count)
#define mmget(mm) atomic_inc(&(mm)->mm_users)
#define mmget_not_zero(mm) atomic_inc_not_zero(&(mm)->mm_users)
#define mmput(mm) atomic_dec(&(mm)->mm_users)
#define get_task_mm(t) ((t)->mm)

typedef unsigned int vm_fault_t;
enum vm_fault_reason {
        VM_FAULT_OOM = 0x000001,
        VM_FAULT_SIGBUS = 0x000002,
        VM_FAULT_MAJOR = 0x000004,
        VM_FAULT_HWPOISON = 0x000010,
        VM_FAULT_HWPOISON_LARGE = 0x000020,
        VM_FAULT_SIGSEGV = 0x000040,
        VM_FAULT_NOPAGE = 0x000100,
        VM_FAULT_LOCKED = 0x000200,
        VM_FAULT_RETRY = 0x000400,
        VM_FAULT_FALLBACK = 0x000800,
        VM_FAULT_DONE_COW = 0x001000,
        VM_FAULT_NEEDDSYNC = 0x002000,
        VM_FAULT_COMPLETED = 0x004000,
};
#define VM_FAULT_ERROR (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE | VM_FAULT_FALLBACK)
enum fault_flag {
        FAULT_FLAG_WRITE = 1 << 0,
        FAULT_FLAG_MKWRITE = 1 << 1,
        FAULT_FLAG_ALLOW_RETRY = 1 << 2,
        FAULT_FLAG_RETRY_NOWAIT = 1 << 3,
        FAULT_FLAG_KILLABLE = 1 << 4,
        FAULT_FLAG_TRIED = 1 << 5,
        FAULT_FLAG_USER = 1 << 6,
        FAULT_FLAG_REMOTE = 1 << 7,
        FAULT_FLAG_INSTRUCTION = 1 << 8,
        FAULT_FLAG_INTERRUPTIBLE = 1 << 9,
        FAULT_FLAG_VMA_LOCK = 1 << 12,
};
struct vm_fault {
        struct vm_area_struct *vma;
        gfp_t gfp_mask;
        pgoff_t pgoff;
        unsigned long address;
        unsigned long real_address;
        enum fault_flag flags;
        struct page *page;
        struct page *cow_page;
};
struct vm_operations_struct {
        void (*open)(struct vm_area_struct *area);
        void (*close)(struct vm_area_struct *area);
        int (*may_split)(struct vm_area_struct *area, unsigned long addr);
        int (*mremap)(struct vm_area_struct *area);
        int (*mprotect)(struct vm_area_struct *vma, unsigned long start, unsigned long end, unsigned long newflags);
        vm_fault_t (*fault)(struct vm_fault *vmf);
        vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
        vm_fault_t (*map_pages)(struct vm_fault *vmf, pgoff_t start_pgoff, pgoff_t end_pgoff);
        unsigned long (*pagesize)(struct vm_area_struct *area);
        vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
        vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);
        int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf, int len, int write);
        const char *(*name)(struct vm_area_struct *vma);
};
struct file;
struct vm_area_struct {
        unsigned long vm_start;
        unsigned long vm_end;
        struct mm_struct *vm_mm;
        pgprot_t vm_page_prot;
        union {
                const vm_flags_t vm_flags;
                vm_flags_t __vm_flags;
        };
        unsigned long vm_pgoff;
        struct file *vm_file;
        void *vm_private_data;
        const struct vm_operations_struct *vm_ops;
        struct list_head lkpi_node;
        void *luos_proc;
};
static inline void vm_flags_init(struct vm_area_struct *vma, vm_flags_t flags) { vma->__vm_flags = flags; }
static inline void vm_flags_reset(struct vm_area_struct *vma, vm_flags_t flags) { vma->__vm_flags = flags; }
static inline void vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags) { vma->__vm_flags |= flags; }
static inline void vm_flags_clear(struct vm_area_struct *vma, vm_flags_t flags) { vma->__vm_flags &= ~flags; }
static inline void vm_flags_mod(struct vm_area_struct *vma, vm_flags_t set, vm_flags_t clear)
{
        vma->__vm_flags = (vma->__vm_flags | set) & ~clear;
}
#define vm_flags_reset_once vm_flags_reset
static inline unsigned long vma_pages(const struct vm_area_struct *vma) { return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT; }
static inline bool is_cow_mapping(vm_flags_t flags) { return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE; }
pgprot_t lkpi_vm_get_page_prot(unsigned long vm_flags);
#define vma_lookup(mm, addr) ((struct vm_area_struct *)NULL)
#define find_vma(mm, addr) ((struct vm_area_struct *)NULL)

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn);
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t pgprot);
vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn);
vm_fault_t vmf_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page);
int vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page);
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot);
#define io_remap_pfn_range remap_pfn_range
int vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len);
void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address, unsigned long size);
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin, loff_t const holelen, int even_cows);
int vm_munmap(unsigned long start, size_t len);
unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len, unsigned long prot, unsigned long flag,
                      unsigned long offset);
#define vmf_error(err) ((err) == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS)
#define pfn_t_lkpi unsigned long

#define TASK_SIZE 0x00007FFFFFFFF000UL
#define TASK_SIZE_MAX TASK_SIZE
#define lkpi_access_ok(addr, size) \
        (current->uaccess_kds > 0 || (size <= TASK_SIZE && addr <= TASK_SIZE - size))
#define access_ok(addr, size) lkpi_access_ok((unsigned long)(addr), (size))


unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n);
unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long lkpi_clear_user(void __user *to, unsigned long n);
#define copy_to_user(to, from, n) lkpi_copy_to_user((to), (from), (n))
#define copy_from_user(to, from, n) lkpi_copy_from_user((to), (from), (n))
#define __copy_to_user copy_to_user
#define __copy_from_user copy_from_user
#define clear_user lkpi_clear_user
#define __clear_user lkpi_clear_user
#define copy_to_user_nofault(to, from, n) (copy_to_user(to, from, n) ? -EFAULT : 0)
#define copy_from_user_nofault(to, from, n) (copy_from_user(to, from, n) ? -EFAULT : 0)
#define __get_user_size_lkpi(x, ptr) ({ typeof(*(ptr)) __gu_tmp; \
        int __gu_err = lkpi_copy_from_user(&__gu_tmp, (ptr), sizeof(__gu_tmp)) ? -EFAULT : 0; \
        (x) = __gu_err ? (typeof(*(ptr)))0 : __gu_tmp; __gu_err; })
#define get_user(x, ptr) __get_user_size_lkpi(x, ptr)
#define __get_user(x, ptr) get_user(x, ptr)
#define put_user(x, ptr) ({ typeof(*(ptr)) __pu_tmp = (x); lkpi_copy_to_user((ptr), &__pu_tmp, sizeof(__pu_tmp)) ? -EFAULT : 0; })
#define __put_user(x, ptr) put_user(x, ptr)
long strncpy_from_user(char *dst, const char __user *src, long count);
long strnlen_user(const char __user *str, long n);
#include <linux/util_macros.h>
static inline int check_zeroed_user(const void __user *from, size_t size)
{
        u8 b;
        for (size_t i = 0; i < size; i++) {
                if (copy_from_user(&b, (const u8 __user *)from + i, 1)) return -EFAULT;
                if (b) return 0;
        }
        return 1;
}
static inline int copy_struct_from_user(void *dst, size_t ksize, const void __user *src, size_t usize)
{
        size_t size = min(ksize, usize);
        size_t rest = max(ksize, usize) - size;
        if (usize < ksize) memset((char *)dst + size, 0, rest);
        else if (usize > ksize) {
                int ret = check_zeroed_user((const char __user *)src + size, rest);
                if (ret <= 0) return ret ?: -E2BIG;
        }
        if (copy_from_user(dst, src, size)) return -EFAULT;
        return 0;
}
#define user_access_begin(ptr, len) access_ok(ptr, len)
#define user_access_end() do { } while (0)
#define user_read_access_begin user_access_begin
#define user_read_access_end user_access_end
#define user_write_access_begin user_access_begin
#define user_write_access_end user_access_end
#define unsafe_put_user(x, ptr, label) do { if (put_user(x, ptr)) goto label; } while (0)
#define unsafe_get_user(x, ptr, label) do { if (get_user(x, ptr)) goto label; } while (0)
#define unsafe_copy_to_user(d, s, l, label) do { if (copy_to_user(d, s, l)) goto label; } while (0)
#define pagefault_disable() preempt_disable()
#define pagefault_enable() preempt_enable()
#define pagefault_disabled() 0
#define faulthandler_disabled() 0
long get_user_pages_fast(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages);
long pin_user_pages_fast(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages);
void unpin_user_pages(struct page **pages, unsigned long npages);
#define FOLL_WRITE 0x01
#define FOLL_LONGTERM 0x100


struct resource {
        resource_size_t start;
        resource_size_t end;
        const char *name;
        unsigned long flags;
        unsigned long desc;
        struct resource *parent, *sibling, *child;
};
#define IORESOURCE_BITS 0x000000ff
#define IORESOURCE_TYPE_BITS 0x00001f00
#define IORESOURCE_IO 0x00000100
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_REG 0x00000300
#define IORESOURCE_IRQ 0x00000400
#define IORESOURCE_DMA 0x00000800
#define IORESOURCE_BUS 0x00001000
#define IORESOURCE_PREFETCH 0x00002000
#define IORESOURCE_READONLY 0x00004000
#define IORESOURCE_CACHEABLE 0x00008000
#define IORESOURCE_SIZEALIGN 0x00040000
#define IORESOURCE_MEM_64 0x00100000
#define IORESOURCE_WINDOW 0x00200000
#define IORESOURCE_ROM_SHADOW (1 << 1)
#define IORESOURCE_ROM_ENABLE (1 << 0)
#define IORESOURCE_UNSET 0x20000000
#define IORESOURCE_DISABLED 0x10000000
#define IORESOURCE_BUSY 0x80000000
#define IORESOURCE_SYSTEM_RAM 0x01000000
#define DEFINE_RES_NAMED(_start, _size, _name, _flags) \
        (struct resource){ .start = (_start), .end = (_start) + (_size) - 1, .name = (_name), .flags = (_flags) }
#define DEFINE_RES(_start, _size, _flags) DEFINE_RES_NAMED(_start, _size, NULL, _flags)
#define DEFINE_RES_MEM_NAMED(_start, _size, _name) DEFINE_RES_NAMED(_start, _size, _name, IORESOURCE_MEM)
#define DEFINE_RES_MEM(_start, _size) DEFINE_RES_MEM_NAMED(_start, _size, NULL)
#define DEFINE_RES_IO(_start, _size) DEFINE_RES_NAMED(_start, _size, NULL, IORESOURCE_IO)
static inline resource_size_t resource_size(const struct resource *res) { return res->end - res->start + 1; }
static inline unsigned long resource_type(const struct resource *res) { return res->flags & IORESOURCE_TYPE_BITS; }
static inline bool resource_contains(const struct resource *r1, const struct resource *r2)
{
        return r1->start <= r2->start && r1->end >= r2->end;
}
static inline bool resource_overlaps(const struct resource *r1, const struct resource *r2)
{
        return r1->start <= r2->end && r1->end >= r2->start;
}
extern struct resource iomem_resource;
extern struct resource ioport_resource;
struct resource *request_mem_region(resource_size_t start, resource_size_t n, const char *name);
struct resource *request_region(resource_size_t start, resource_size_t n, const char *name);
#define request_mem_region_exclusive request_mem_region
#define request_mem_region_muxed request_mem_region
void release_mem_region(resource_size_t start, resource_size_t n);
void release_region(resource_size_t start, resource_size_t n);
int request_resource(struct resource *root, struct resource *new);
int release_resource(struct resource *new);
int insert_resource(struct resource *parent, struct resource *new);
int remove_resource(struct resource *old);
int region_intersects(resource_size_t offset, size_t size, unsigned long flags, unsigned long desc);
#define REGION_INTERSECTS 0
#define REGION_DISJOINT 1
#define REGION_MIXED 2
#define IORES_DESC_NONE 0
int walk_iomem_res_desc(unsigned long desc, unsigned long flags, u64 start, u64 end, void *arg,
                        int (*func)(struct resource *, void *));
int devm_request_resource(struct device *dev, struct resource *root, struct resource *new);

void __iomem *ioremap(resource_size_t offset, unsigned long size);
void __iomem *ioremap_wc(resource_size_t offset, unsigned long size);
void __iomem *ioremap_uc(resource_size_t offset, unsigned long size);
void __iomem *ioremap_cache(resource_size_t offset, unsigned long size);
#define ioremap_wt ioremap
#define ioremap_np ioremap
#define ioremap_encrypted ioremap_cache
void *memremap(resource_size_t offset, size_t size, unsigned long flags);
void memunmap(void *addr);
#define MEMREMAP_WB 1
#define MEMREMAP_WT 2
#define MEMREMAP_WC 4
#define MEMREMAP_ENC 8
#define MEMREMAP_DEC 16
void iounmap(volatile void __iomem *addr);
int arch_phys_wc_add(unsigned long base, unsigned long size);
void arch_phys_wc_del(int handle);
int arch_phys_wc_index(int handle);
int arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size);
void arch_io_free_memtype_wc(resource_size_t start, resource_size_t size);
int io_reserve_memtype(resource_size_t start, resource_size_t end, int *pcm);

#define __LKPI_MMIO(bwl, type) \
        static __always_inline type __raw_read##bwl(const volatile void __iomem *addr) { return *(const volatile type __force *)addr; } \
        static __always_inline void __raw_write##bwl(type val, volatile void __iomem *addr) { *(volatile type __force *)addr = val; } \
        static __always_inline type read##bwl(const volatile void __iomem *addr) { type v = __raw_read##bwl(addr); barrier(); return v; } \
        static __always_inline void write##bwl(type val, volatile void __iomem *addr) { barrier(); __raw_write##bwl(val, addr); } \
        static __always_inline type read##bwl##_relaxed(const volatile void __iomem *addr) { return __raw_read##bwl(addr); } \
        static __always_inline void write##bwl##_relaxed(type val, volatile void __iomem *addr) { __raw_write##bwl(val, addr); }
__LKPI_MMIO(b, u8)
__LKPI_MMIO(w, u16)
__LKPI_MMIO(l, u32)
__LKPI_MMIO(q, u64)
#define ioread8(a) readb(a)
#define ioread16(a) readw(a)
#define ioread32(a) readl(a)
#define ioread64(a) readq(a)
#define iowrite8(v, a) writeb(v, a)
#define iowrite16(v, a) writew(v, a)
#define iowrite32(v, a) writel(v, a)
#define iowrite64(v, a) writeq(v, a)
#define ioread16be(a) be16_to_cpu(readw(a))
#define ioread32be(a) be32_to_cpu(readl(a))
#define iowrite16be(v, a) writew(cpu_to_be16(v), a)
#define iowrite32be(v, a) writel(cpu_to_be32(v), a)
#define readq_relaxed_lkpi readq_relaxed
#define mmiowb() do { } while (0)
static inline void memcpy_fromio(void *dst, const volatile void __iomem *src, size_t count) { memcpy(dst, (const void __force *)src, count); }
static inline void memcpy_toio(volatile void __iomem *dst, const void *src, size_t count) { memcpy((void __force *)dst, src, count); }
static inline void memset_io(volatile void __iomem *dst, int c, size_t count) { memset((void __force *)dst, c, count); }
static inline void __iowrite32_copy(void __iomem *to, const void *from, size_t count)
{
        u32 __iomem *dst = to; const u32 *src = from;
        while (count--) __raw_writel(*src++, dst++);
}
static inline void __ioread32_copy(void *to, const void __iomem *from, size_t count)
{
        u32 *dst = to; const u32 __iomem *src = from;
        while (count--) *dst++ = __raw_readl(src++);
}
#define __iowrite64_copy(to, from, count) __iowrite32_copy(to, from, (count) * 2)

static __always_inline u8 inb(u16 port) { u8 v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static __always_inline u16 inw(u16 port) { u16 v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static __always_inline u32 inl(u16 port) { u32 v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static __always_inline void outb(u8 v, u16 port) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static __always_inline void outw(u16 v, u16 port) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static __always_inline void outl(u32 v, u16 port) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
#define inb_p inb
#define outb_p outb

struct io_mapping {
        resource_size_t base;
        unsigned long size;
        pgprot_t prot;
        void __iomem *iomem;
};
struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size);
struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base, unsigned long size);
void io_mapping_fini(struct io_mapping *mapping);
void io_mapping_free(struct io_mapping *mapping);
static inline void __iomem *io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset, unsigned long size)
{
        return (char __iomem *)mapping->iomem + offset;
}
#define io_mapping_map_atomic_wc(m, off) io_mapping_map_wc(m, off, PAGE_SIZE)
#define io_mapping_map_local_wc(m, off) io_mapping_map_wc(m, off, PAGE_SIZE)
#define io_mapping_unmap(v) do { (void)(v); } while (0)
#define io_mapping_unmap_atomic(v) do { (void)(v); } while (0)
#define io_mapping_unmap_local(v) do { (void)(v); } while (0)


struct scatterlist {
        unsigned long page_link;
        unsigned int offset;
        unsigned int length;
        dma_addr_t dma_address;
        unsigned int dma_length;
        unsigned int dma_flags;
};
struct sg_table {
        struct scatterlist *sgl;
        unsigned int nents;
        unsigned int orig_nents;
};
struct sg_append_table {
        struct sg_table sgt;
        struct scatterlist *prv;
        unsigned int total_nents;
};
#define SG_CHAIN 0x01UL
#define SG_END 0x02UL
#define SG_PAGE_LINK_MASK (SG_CHAIN | SG_END)
#define SG_MAX_SINGLE_ALLOC (PAGE_SIZE / sizeof(struct scatterlist))
#define SG_CHUNK_SIZE 128
#define sg_is_chain(sg) ((sg)->page_link & SG_CHAIN)
#define sg_is_last(sg) ((sg)->page_link & SG_END)
#define sg_chain_ptr(sg) ((struct scatterlist *)((sg)->page_link & ~SG_PAGE_LINK_MASK))
static inline struct page *sg_page(struct scatterlist *sg) { return (struct page *)((sg)->page_link & ~SG_PAGE_LINK_MASK); }
static inline void sg_assign_page(struct scatterlist *sg, struct page *page)
{
        unsigned long page_link = sg->page_link & SG_PAGE_LINK_MASK;
        sg->page_link = page_link | (unsigned long)page;
}
static inline void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int offset)
{
        sg_assign_page(sg, page);
        sg->offset = offset;
        sg->length = len;
}
static inline void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
        sg_set_page(sg, virt_to_page(buf), buflen, offset_in_page(buf));
}
static inline void sg_mark_end(struct scatterlist *sg) { sg->page_link |= SG_END; sg->page_link &= ~SG_CHAIN; }
static inline void sg_unmark_end(struct scatterlist *sg) { sg->page_link &= ~SG_END; }
static inline void sg_chain(struct scatterlist *prv, unsigned int prv_nents, struct scatterlist *sgl)
{
        prv[prv_nents - 1].offset = 0;
        prv[prv_nents - 1].length = 0;
        prv[prv_nents - 1].page_link = ((unsigned long)sgl | SG_CHAIN) & ~SG_END;
}
static inline dma_addr_t sg_phys(struct scatterlist *sg) { return page_to_phys(sg_page(sg)) + sg->offset; }
static inline void *sg_virt(struct scatterlist *sg) { return (char *)page_address(sg_page(sg)) + sg->offset; }
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
#define sg_dma_is_bus_address(sg) false
#define sg_dma_mark_bus_address(sg) do { } while (0)
#define sg_dma_unmark_bus_address(sg) do { } while (0)
#define sg_dma_is_swiotlb(sg) false
struct scatterlist *sg_next(struct scatterlist *sg);
int sg_nents(struct scatterlist *sg);
int sg_nents_for_len(struct scatterlist *sg, u64 len);
struct scatterlist *sg_last(struct scatterlist *s, unsigned int nents);
void sg_init_table(struct scatterlist *sgl, unsigned int nents);
void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen);
int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp_mask);
void sg_free_table(struct sg_table *table);
void sg_free_append_table(struct sg_append_table *sgt);
int sg_alloc_append_table_from_pages(struct sg_append_table *sgt, struct page **pages, unsigned int n_pages,
                                     unsigned int offset, unsigned long size, unsigned int max_segment,
                                     unsigned int left_pages, gfp_t gfp_mask);
int sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages, unsigned int n_pages,
                                      unsigned int offset, unsigned long size, unsigned int max_segment, gfp_t gfp_mask);
static inline int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages, unsigned int n_pages,
                                            unsigned int offset, unsigned long size, gfp_t gfp_mask)
{
        return sg_alloc_table_from_pages_segment(sgt, pages, n_pages, offset, size, UINT_MAX, gfp_mask);
}
size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf, size_t buflen);
size_t sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen);
#define for_each_sg(sglist, sg, nr, __i) for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))
#define for_each_sgtable_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

struct sg_page_iter {
        struct scatterlist *sg;
        unsigned int sg_pgoffset;
        unsigned int __nents;
        int __pg_advance;
};
struct sg_dma_page_iter { struct sg_page_iter base; };
bool __sg_page_iter_next(struct sg_page_iter *piter);
bool __sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter);
void __sg_page_iter_start(struct sg_page_iter *piter, struct scatterlist *sglist, unsigned int nents,
                          unsigned long pgoffset);
static inline struct page *sg_page_iter_page(struct sg_page_iter *piter) { return nth_page(sg_page(piter->sg), piter->sg_pgoffset); }
static inline dma_addr_t sg_page_iter_dma_address(struct sg_dma_page_iter *dma_iter)
{
        return sg_dma_address(dma_iter->base.sg) + (dma_iter->base.sg_pgoffset << PAGE_SHIFT);
}
#define for_each_sg_page(sglist, piter, nents, pgoffset) \
        for (__sg_page_iter_start((piter), (sglist), (nents), (pgoffset)); __sg_page_iter_next(piter);)
#define for_each_sg_dma_page(sglist, dma_iter, dma_nents, pgoffset) \
        for (__sg_page_iter_start(&(dma_iter)->base, sglist, dma_nents, pgoffset); __sg_page_iter_dma_next(dma_iter);)
#define for_each_sgtable_page(sgt, piter, pgoffset) for_each_sg_page((sgt)->sgl, piter, (sgt)->orig_nents, pgoffset)
#define for_each_sgtable_dma_page(sgt, dma_iter, pgoffset) for_each_sg_dma_page((sgt)->sgl, dma_iter, (sgt)->nents, pgoffset)


enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)
#define DMA_ATTR_WEAK_ORDERING (1UL << 1)
#define DMA_ATTR_WRITE_COMBINE (1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)
#define DMA_ATTR_FORCE_CONTIGUOUS (1UL << 6)
#define DMA_ATTR_NO_WARN (1UL << 8)
struct device;
int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
        int rc = dma_set_mask(dev, mask);
        if (rc == 0) dma_set_coherent_mask(dev, mask);
        return rc;
}
u64 dma_get_mask(struct device *dev);
#define dma_coerce_mask_and_coherent(dev, mask) dma_set_mask_and_coherent(dev, mask)
#define dma_get_required_mask(dev) DMA_BIT_MASK(64)
#define dma_addressing_limited(dev) false
#define dma_max_mapping_size(dev) SIZE_MAX
#define dma_opt_mapping_size(dev) SIZE_MAX
#define dma_set_max_seg_size(dev, size) 0
#define dma_get_max_seg_size(dev) UINT_MAX
#define dma_set_seg_boundary(dev, mask) 0
#define dma_get_seg_boundary(dev) ULONG_MAX
#define dma_is_dma_lkpi true
#define dma_dev_need_sync(dev) false
#define dma_need_sync(dev, addr) false
#define dma_skip_sync(dev) true
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag, unsigned long attrs);
void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle, unsigned long attrs);
#define dma_alloc_coherent(dev, size, h, gfp) dma_alloc_attrs(dev, size, h, gfp, 0)
#define dma_free_coherent(dev, size, cpu, h) dma_free_attrs(dev, size, cpu, h, 0)
#define dma_alloc_wc(dev, size, h, gfp) dma_alloc_attrs(dev, size, h, gfp, DMA_ATTR_WRITE_COMBINE)
#define dma_free_wc(dev, size, cpu, h) dma_free_attrs(dev, size, cpu, h, DMA_ATTR_WRITE_COMBINE)
#define dma_alloc_noncoherent(dev, size, h, dir, gfp) dma_alloc_attrs(dev, size, h, gfp, 0)
#define dma_free_noncoherent(dev, size, cpu, h, dir) dma_free_attrs(dev, size, cpu, h, 0)
#define dmam_alloc_coherent(dev, size, h, gfp) dma_alloc_coherent(dev, size, h, gfp)
static inline dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset, size_t size,
                                            enum dma_data_direction dir, unsigned long attrs)
{
        return page_to_phys(page) + offset;
}
#define dma_map_page(d, p, o, s, r) dma_map_page_attrs(d, p, o, s, r, 0)
static inline void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
                                        enum dma_data_direction dir, unsigned long attrs) { }
#define dma_unmap_page(d, a, s, r) dma_unmap_page_attrs(d, a, s, r, 0)
static inline dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                              enum dma_data_direction dir, unsigned long attrs)
{
        return virt_to_phys(ptr);
}
#define dma_map_single(d, a, s, r) dma_map_single_attrs(d, a, s, r, 0)
#define dma_unmap_single(d, a, s, r) do { } while (0)
#define dma_unmap_single_attrs(d, a, s, r, at) do { } while (0)
static inline dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys_addr, size_t size,
                                          enum dma_data_direction dir, unsigned long attrs)
{
        return phys_addr;
}
#define dma_unmap_resource(d, a, s, r, at) do { } while (0)
#define dma_mapping_error(dev, addr) ((addr) == DMA_MAPPING_ERROR)
int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir,
                     unsigned long attrs);
#define dma_map_sg(d, s, n, r) dma_map_sg_attrs(d, s, n, r, 0)
#define dma_unmap_sg(d, s, n, r) do { } while (0)
#define dma_unmap_sg_attrs(d, s, n, r, a) do { } while (0)
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                                  unsigned long attrs)
{
        int nents = dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
        if (nents < 0) return nents;
        sgt->nents = nents;
        return 0;
}
static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                                     unsigned long attrs) { }
#define dma_sync_single_for_cpu(d, a, s, r) do { } while (0)
#define dma_sync_single_for_device(d, a, s, r) do { } while (0)
#define dma_sync_single_range_for_cpu(d, a, o, s, r) do { } while (0)
#define dma_sync_single_range_for_device(d, a, o, s, r) do { } while (0)
#define kmsan_handle_dma(p, s, d) do { } while (0)
#define dma_sync_sg_for_cpu(d, s, n, r) do { } while (0)
#define dma_sync_sg_for_device(d, s, n, r) do { } while (0)
#define dma_sync_sgtable_for_cpu(d, s, r) do { } while (0)
#define dma_sync_sgtable_for_device(d, s, r) do { } while (0)
int dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt, void *cpu_addr, dma_addr_t dma_addr, size_t size,
                          unsigned long attrs);
#define dma_get_sgtable(d, t, v, h, s) dma_get_sgtable_attrs(d, t, v, h, s, 0)
int dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu_addr, dma_addr_t dma_addr, size_t size,
                   unsigned long attrs);
#define dma_mmap_coherent(d, v, c, h, s) dma_mmap_attrs(d, v, c, h, s, 0)
#define dma_mmap_wc(d, v, c, h, s) dma_mmap_attrs(d, v, c, h, s, DMA_ATTR_WRITE_COMBINE)
#define dma_mmap_noncoherent(d, v, s, h, p) dma_mmap_attrs(d, v, page_address(p), h, s, 0)
#define valid_dma_direction(dir) ((dir) == DMA_BIDIRECTIONAL || (dir) == DMA_TO_DEVICE || (dir) == DMA_FROM_DEVICE)
#define dev_is_dma_coherent(dev) true

#endif
