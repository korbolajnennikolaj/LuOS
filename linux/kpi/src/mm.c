// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <uapi/linux/sysinfo.h>

unsigned long lkpi_hhdm_base;
static unsigned long lkpi_direct_end;

#define PG_lkpi_kmalloc 30
#define PG_lkpi_slab 31
#define PG_LKPI_ORDER_SHIFT 40
#define PG_LKPI_ORDER_MASK (0xffUL << PG_LKPI_ORDER_SHIFT)

bool lkpi_addr_is_direct(const void *p)
{
        unsigned long a = (unsigned long)p;
        return a >= lkpi_hhdm_base && a < lkpi_direct_end;
}

phys_addr_t lkpi_virt_to_phys(const void *x)
{
        if (lkpi_addr_is_direct(x)) return (unsigned long)x - lkpi_hhdm_base;
        return luos_virt_to_phys(x);
}

bool lkpi_pfn_valid(unsigned long pfn) { return pfn <= luos_highest_pfn(); }


static void page_init_block(struct page *page, unsigned long n, unsigned int order)
{
        for (unsigned long i = 0; i < n; i++) {
                struct page *p = page + i;
                memset(p, 0, sizeof(*p));
                INIT_LIST_HEAD(&p->lru);
        }
        page->flags = (unsigned long)order << PG_LKPI_ORDER_SHIFT;
        set_page_count(page, 1);
}

static inline unsigned int page_block_order(struct page *page)
{
        return (page->flags & PG_LKPI_ORDER_MASK) >> PG_LKPI_ORDER_SHIFT;
}

struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)
{
        unsigned long n = 1UL << order;
        luos_u64 limit = (gfp & __GFP_DMA32) ? (1ULL << 32) : (gfp & __GFP_DMA) ? (16ULL << 20) : 0;
        luos_u64 phys = luos_phys_alloc(n, n, limit);
        if (!phys) {
                if (!(gfp & __GFP_NOWARN)) pr_warn("LinuxKPI: page allocation of order %u failed\n", order);
                return NULL;
        }
        struct page *page = phys_to_page(phys);
        page_init_block(page, n, order);
        if (gfp & __GFP_ZERO) memset(__va(phys), 0, n << PAGE_SHIFT);
        return page;
}

static void free_page_block(struct page *page, unsigned int order)
{
        unsigned long n = 1UL << order;
        if (test_bit(PG_lkpi_wc, &page->flags) || test_bit(PG_lkpi_uc, &page->flags))
                luos_set_cache((luos_u64)page_address(page), n << PAGE_SHIFT, 0);
        page->flags = 0;
        page->mapping = NULL;
        luos_phys_free(page_to_phys(page), n);
}

void split_page(struct page *page, unsigned int order)
{
        unsigned long n = 1UL << order;
        page->flags &= ~PG_LKPI_ORDER_MASK;
        for (unsigned long i = 1; i < n; i++) {
                page[i].flags &= ~PG_LKPI_ORDER_MASK;
                set_page_count(page + i, 1);
        }
}

void __free_pages(struct page *page, unsigned int order)
{
        if (!page) return;
        if (put_page_testzero(page)) free_page_block(page, order);
}

void put_page(struct page *page)
{
        if (put_page_testzero(page)) free_page_block(page, page_block_order(page));
}

unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order)
{
        struct page *page = alloc_pages(gfp_mask, order);
        return page ? (unsigned long)page_address(page) : 0;
}

unsigned long get_zeroed_page(gfp_t gfp_mask) { return __get_free_pages(gfp_mask | __GFP_ZERO, 0); }

void free_pages(unsigned long addr, unsigned int order)
{
        if (addr) __free_pages(virt_to_page((void *)addr), order);
}

void *alloc_pages_exact(size_t size, gfp_t gfp_mask)
{
        return (void *)__get_free_pages(gfp_mask, get_order(size));
}

void free_pages_exact(void *virt, size_t size) { free_pages((unsigned long)virt, get_order(size)); }

unsigned long totalram_pages(void) { return luos_total_pages(); }

void si_meminfo(struct sysinfo *val)
{
        memset(val, 0, sizeof(*val));
        val->totalram = luos_total_pages();
        val->freeram = luos_free_pages();
        val->mem_unit = PAGE_SIZE;
}


static const unsigned short kmalloc_class_size[] = { 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048 };
#define KMALLOC_CLASSES ARRAY_SIZE(kmalloc_class_size)

struct kmalloc_class {
        raw_spinlock_t lock;
        void *free;
        unsigned long pages;
};
static struct kmalloc_class kmalloc_classes[KMALLOC_CLASSES];

static int kmalloc_class_for(size_t size)
{
        for (unsigned int i = 0; i < KMALLOC_CLASSES; i++)
                if (size <= kmalloc_class_size[i]) return i;
        return -1;
}

static void *kmalloc_small(int c, gfp_t flags)
{
        struct kmalloc_class *kc = &kmalloc_classes[c];
        unsigned long irqflags;
        void *p;
        raw_spin_lock_irqsave(&kc->lock, irqflags);
        p = kc->free;
        if (p) kc->free = *(void **)p;
        raw_spin_unlock_irqrestore(&kc->lock, irqflags);
        if (!p) {
                unsigned int size = kmalloc_class_size[c];
                luos_u64 phys = luos_phys_alloc(1, 1, 0);
                if (!phys) return NULL;
                struct page *page = phys_to_page(phys);
                page_init_block(page, 1, 0);
                __set_bit(PG_lkpi_slab, &page->flags);
                page->private = c;
                char *base = __va(phys);
                unsigned int n = PAGE_SIZE / size;
                p = base;
                raw_spin_lock_irqsave(&kc->lock, irqflags);
                for (unsigned int i = n - 1; i >= 1; i--) {
                        *(void **)(base + i * size) = kc->free;
                        kc->free = base + i * size;
                }
                kc->pages++;
                raw_spin_unlock_irqrestore(&kc->lock, irqflags);
        }
        if (flags & __GFP_ZERO) memset(p, 0, kmalloc_class_size[c]);
        return p;
}

static void kfree_small(struct page *page, const void *p)
{
        struct kmalloc_class *kc = &kmalloc_classes[page->private];
        unsigned long irqflags;
        raw_spin_lock_irqsave(&kc->lock, irqflags);
        *(void **)p = kc->free;
        kc->free = (void *)p;
        raw_spin_unlock_irqrestore(&kc->lock, irqflags);
}

static void *kmalloc_large(size_t size, gfp_t flags)
{
        unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
        luos_u64 limit = (flags & __GFP_DMA32) ? (1ULL << 32) : 0;
        luos_u64 phys = luos_phys_alloc(n, 1, limit);
        if (!phys) return NULL;
        struct page *page = phys_to_page(phys);
        page_init_block(page, n, 0);
        __set_bit(PG_lkpi_kmalloc, &page->flags);
        page->private = n;
        void *p = __va(phys);
        if (flags & __GFP_ZERO) memset(p, 0, n << PAGE_SHIFT);
        return p;
}

void *lkpi_kmalloc(size_t size, gfp_t flags)
{
        void *p;
        int c;
        if (!size) return ZERO_SIZE_PTR;
        c = kmalloc_class_for(size);
        p = c >= 0 ? kmalloc_small(c, flags) : kmalloc_large(size, flags);
        if (!p && !(flags & __GFP_NOWARN)) pr_warn("LinuxKPI: kmalloc(%zu) failed\n", size);
        return p;
}

static struct page *kmalloc_page(const void *p)
{
        if (!lkpi_addr_is_direct(p)) return NULL;
        struct page *page = virt_to_page(p);
        if (test_bit(PG_lkpi_slab, &page->flags)) return page;
        if (!offset_in_page(p) && test_bit(PG_lkpi_kmalloc, &page->flags)) return page;
        return NULL;
}

size_t ksize(const void *objp)
{
        if (ZERO_OR_NULL_PTR(objp)) return 0;
        struct page *page = kmalloc_page(objp);
        if (!page) return luos_heap_block_size(objp);
        if (test_bit(PG_lkpi_slab, &page->flags)) return kmalloc_class_size[page->private];
        return page->private << PAGE_SHIFT;
}

void kfree(const void *objp)
{
        if (ZERO_OR_NULL_PTR(objp)) return;
        struct page *page = kmalloc_page(objp);
        if (page && test_bit(PG_lkpi_slab, &page->flags)) {
                kfree_small(page, objp);
                return;
        }
        if (page) {
                unsigned long n = page->private;
                page->flags = 0;
                page->private = 0;
                luos_phys_free(page_to_phys(page), n);
                return;
        }
        if (lkpi_addr_is_direct(objp)) {
                WARN_ONCE(1, "kfree of a direct-map pointer %p that kmalloc did not return\n", objp);
                return;
        }
        luos_heap_free((void *)objp);
}

void kfree_sensitive(const void *objp)
{
        if (!ZERO_OR_NULL_PTR(objp)) memzero_explicit((void *)objp, ksize(objp));
        kfree(objp);
}

void *lkpi_krealloc(const void *p, size_t new_size, gfp_t flags)
{
        if (!new_size) {
                kfree(p);
                return ZERO_SIZE_PTR;
        }
        if (ZERO_OR_NULL_PTR(p)) return kmalloc(new_size, flags);
        size_t old = ksize(p);
        if (new_size <= old) return (void *)p;
        void *n = kmalloc(new_size, flags & ~__GFP_ZERO);
        if (!n) return NULL;
        memcpy(n, p, old);
        if (flags & __GFP_ZERO) memset((char *)n + old, 0, new_size - old);
        kfree(p);
        return n;
}


struct vm_area_lkpi {
        struct hlist_node node;
        unsigned long addr;
        unsigned long count;
        bool owns_pages;
        struct page **pages;
};

#define VM_HASH_BITS 8
static struct hlist_head vm_hash[1 << VM_HASH_BITS];
static DEFINE_SPINLOCK(vm_lock);

static void vm_area_add(struct vm_area_lkpi *a)
{
        spin_lock(&vm_lock);
        hlist_add_head(&a->node, &vm_hash[hash_long(a->addr, VM_HASH_BITS)]);
        spin_unlock(&vm_lock);
}

static struct vm_area_lkpi *vm_area_take(unsigned long addr)
{
        struct vm_area_lkpi *a, *found = NULL;
        spin_lock(&vm_lock);
        hlist_for_each_entry(a, &vm_hash[hash_long(addr, VM_HASH_BITS)], node) {
                if (a->addr == addr) {
                        hlist_del(&a->node);
                        found = a;
                        break;
                }
        }
        spin_unlock(&vm_lock);
        return found;
}

static int prot_to_cache(pgprot_t prot)
{
        unsigned long v = pgprot_val(prot);
        return ((v & _PAGE_PAT) ? 4 : 0) | ((v & _PAGE_PCD) ? 2 : 0) | ((v & _PAGE_PWT) ? 1 : 0);
}

static void *vmap_phys_array(const luos_u64 *phys, unsigned long count, int cache, struct page **pages, bool owns)
{
        struct vm_area_lkpi *a = kzalloc(sizeof(*a), GFP_KERNEL);
        if (!a) return NULL;
        luos_u64 v = luos_vmap(phys, count, cache);
        if (!v) {
                kfree(a);
                return NULL;
        }
        a->addr = v;
        a->count = count;
        a->owns_pages = owns;
        a->pages = pages;
        vm_area_add(a);
        return (void *)v;
}

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
        luos_u64 *phys = kvmalloc_array(count, sizeof(*phys), GFP_KERNEL);
        if (!phys) return NULL;
        for (unsigned int i = 0; i < count; i++) phys[i] = page_to_phys(pages[i]);
        void *v = vmap_phys_array(phys, count, prot_to_cache(prot), NULL, false);
        kvfree(phys);
        return v;
}

void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
        luos_u64 *phys = kvmalloc_array(count, sizeof(*phys), GFP_KERNEL);
        if (!phys) return NULL;
        for (unsigned int i = 0; i < count; i++) phys[i] = (luos_u64)pfns[i] << PAGE_SHIFT;
        void *v = vmap_phys_array(phys, count, prot_to_cache(prot), NULL, false);
        kvfree(phys);
        return v;
}

void vunmap(const void *addr)
{
        if (!addr) return;
        struct vm_area_lkpi *a = vm_area_take((unsigned long)addr);
        luos_vunmap((luos_u64)addr);
        if (a) {
                if (a->owns_pages) {
                        for (unsigned long i = 0; i < a->count; i++) __free_page(a->pages[i]);
                        kvfree(a->pages);
                }
                kfree(a);
        }
}

void *__vmalloc(unsigned long size, gfp_t gfp_mask)
{
        unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
        if (!n) return NULL;
        struct page **pages = kmalloc_array(n, sizeof(*pages), GFP_KERNEL | __GFP_ZERO);
        luos_u64 *phys = kmalloc_array(n, sizeof(*phys), GFP_KERNEL);
        if (!pages || !phys) goto fail;
        for (unsigned long i = 0; i < n; i++) {
                pages[i] = alloc_page((gfp_mask & (__GFP_DMA32 | __GFP_ZERO)) | __GFP_NOWARN);
                if (!pages[i]) goto fail;
                phys[i] = page_to_phys(pages[i]);
        }
        void *v = vmap_phys_array(phys, n, 0, pages, true);
        if (!v) goto fail;
        kfree(phys);
        return v;
fail:
        if (pages)
                for (unsigned long i = 0; i < n; i++)
                        if (pages[i]) __free_page(pages[i]);
        kfree(pages);
        kfree(phys);
        return NULL;
}

void *vmalloc(unsigned long size) { return __vmalloc(size, GFP_KERNEL); }
void *vzalloc(unsigned long size) { return __vmalloc(size, GFP_KERNEL | __GFP_ZERO); }
void *vmalloc_user(unsigned long size) { return vzalloc(size); }
void *vmalloc_32(unsigned long size) { return __vmalloc(size, GFP_KERNEL | __GFP_DMA32); }
void *vmalloc_noprof(unsigned long size) { return vmalloc(size); }
void vfree(const void *addr) { vunmap(addr); }

bool is_vmalloc_addr(const void *x)
{
        unsigned long a = (unsigned long)x;
        return a >= VMALLOC_START && a < VMALLOC_END;
}

struct page *vmalloc_to_page(const void *addr) { return pfn_to_page(vmalloc_to_pfn(addr)); }
unsigned long vmalloc_to_pfn(const void *addr) { return luos_virt_to_phys(addr) >> PAGE_SHIFT; }

void *kvmalloc_node(size_t size, gfp_t flags, int node)
{
        void *p = kmalloc(size, flags | __GFP_NOWARN);
        if (p || size < PAGE_SIZE) return p;
        return __vmalloc(size, flags);
}

void kvfree(const void *addr)
{
        if (is_vmalloc_addr(addr)) vfree(addr);
        else kfree(addr);
}

void kvfree_sensitive(const void *addr, size_t len)
{
        if (addr) memzero_explicit((void *)addr, len);
        kvfree(addr);
}

void *kvrealloc(const void *p, size_t size, gfp_t flags)
{
        if (!p || !is_vmalloc_addr(p)) {
                void *n = krealloc(p, size, flags | __GFP_NOWARN);
                if (n || size < PAGE_SIZE) return n;
        }
        void *n = kvmalloc(size, flags);
        if (!n) return NULL;
        if (p) {
                size_t old = is_vmalloc_addr(p) ? size : ksize(p);
                memcpy(n, p, min(old, size));
                kvfree(p);
        }
        return n;
}


struct kmem_cache {
        const char *name;
        unsigned int size;
        unsigned int alloc_size;
        slab_flags_t flags;
        void (*ctor)(void *);
};

struct kmem_cache *__kmem_cache_create_args(const char *name, unsigned int object_size, struct kmem_cache_args *args,
                                            slab_flags_t flags)
{
        struct kmem_cache *s = kzalloc(sizeof(*s), GFP_KERNEL);
        if (!s) return NULL;
        s->name = name;
        s->size = object_size;
        s->flags = flags;
        s->ctor = args ? args->ctor : NULL;
        unsigned int align = args ? args->align : 0;
        if ((flags & SLAB_HWCACHE_ALIGN) && align < 64) align = 64;
        s->alloc_size = align > 1 ? ALIGN(object_size, align) : object_size;
        if (align > 1) {
                for (unsigned int i = 0; i < KMALLOC_CLASSES; i++) {
                        unsigned int cs = kmalloc_class_size[i];
                        if (cs >= s->alloc_size && (cs & -cs) >= align) {
                                s->alloc_size = cs;
                                break;
                        }
                }
        }
        return s;
}

void kmem_cache_destroy(struct kmem_cache *s)
{
        if (!s) return;
        if (s->flags & SLAB_TYPESAFE_BY_RCU) rcu_barrier();
        kfree(s);
}

void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags)
{
        void *p = kmalloc(s->alloc_size, flags);
        if (p && s->ctor) s->ctor(p);
        return p;
}

static void kmem_cache_rcu_free(struct callback_head *head)
{
        kfree(*(void **)(head + 1));
        kfree(head);
}

void kmem_cache_free(struct kmem_cache *s, void *objp)
{
        if (!objp) return;
        if (s && (s->flags & SLAB_TYPESAFE_BY_RCU)) {
                struct {
                        struct callback_head h;
                        void *obj;
                } *box = kmalloc(sizeof(*box), GFP_ATOMIC);
                if (box) {
                        box->obj = objp;
                        call_rcu(&box->h, kmem_cache_rcu_free);
                        return;
                }
                synchronize_rcu();
        }
        kfree(objp);
}

int kmem_cache_shrink(struct kmem_cache *s) { return 0; }
unsigned int kmem_cache_size(struct kmem_cache *s) { return s->size; }


int set_memory_wc(unsigned long addr, int numpages) { return luos_set_cache(addr, (luos_u64)numpages << PAGE_SHIFT, 5); }
int set_memory_uc(unsigned long addr, int numpages) { return luos_set_cache(addr, (luos_u64)numpages << PAGE_SHIFT, 2); }
int set_memory_wb(unsigned long addr, int numpages) { return luos_set_cache(addr, (luos_u64)numpages << PAGE_SHIFT, 0); }

static int set_pages_cache(struct page **pages, int n, int cache, int bit)
{
        for (int i = 0; i < n; i++) {
                struct page *p = pages[i];
                luos_set_cache((luos_u64)page_address(p), PAGE_SIZE, cache);
                clear_bit(PG_lkpi_wc, &p->flags);
                clear_bit(PG_lkpi_uc, &p->flags);
                if (bit >= 0) set_bit(bit, &p->flags);
        }
        return 0;
}

int set_pages_array_wc(struct page **pages, int addrinarray) { return set_pages_cache(pages, addrinarray, 5, PG_lkpi_wc); }
int set_pages_array_uc(struct page **pages, int addrinarray) { return set_pages_cache(pages, addrinarray, 2, PG_lkpi_uc); }
int set_pages_array_wb(struct page **pages, int addrinarray) { return set_pages_cache(pages, addrinarray, 0, -1); }

int set_pages_wb(struct page *page, int numpages)
{
        for (int i = 0; i < numpages; i++) {
                struct page *p = page + i;
                set_pages_cache(&p, 1, 0, -1);
        }
        return 0;
}

int set_pages_uc(struct page *page, int numpages)
{
        for (int i = 0; i < numpages; i++) {
                struct page *p = page + i;
                set_pages_cache(&p, 1, 2, PG_lkpi_uc);
        }
        return 0;
}

void clflush_cache_range(void *addr, unsigned int size) { luos_flush_cache((luos_u64)addr, size); }
void wbinvd_on_all_cpus(void) { luos_wbinvd_all(); }

static u8 fpu_save[NR_CPUS][2][512] __aligned(64);
static int fpu_depth[NR_CPUS];

void kernel_fpu_begin(void)
{
        preempt_disable();
        int cpu = smp_processor_id();
        int slot = fpu_depth[cpu]++;
        if (slot < 2) asm volatile("fxsave64 %0" : "=m"(fpu_save[cpu][slot]));
}

void kernel_fpu_end(void)
{
        int cpu = smp_processor_id();
        int slot = --fpu_depth[cpu];
        if (slot < 2) asm volatile("fxrstor64 %0" : : "m"(fpu_save[cpu][slot]));
        preempt_enable();
}


pgprot_t lkpi_vm_get_page_prot(unsigned long vm_flags)
{
        unsigned long v = _PAGE_PRESENT | _PAGE_USER;
        if (vm_flags & VM_WRITE) v |= _PAGE_RW;
        if (!(vm_flags & VM_EXEC)) v |= _PAGE_NX;
        return __pgprot(v);
}

vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t pgprot)
{
        if (addr < vma->vm_start || addr >= vma->vm_end) return VM_FAULT_SIGBUS;
        int writable = (vma->vm_flags & VM_WRITE) != 0;
        if (luos_user_map_pfn(vma->luos_proc, addr, pfn, writable, prot_to_cache(pgprot)) < 0) return VM_FAULT_SIGBUS;
        return VM_FAULT_NOPAGE;
}

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
        return vmf_insert_pfn_prot(vma, addr, pfn, vma->vm_page_prot);
}

vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
        return vmf_insert_pfn(vma, addr, pfn);
}

vm_fault_t vmf_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{
        return vmf_insert_pfn(vma, addr, page_to_pfn(page));
}

int vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{
        return vmf_insert_page(vma, addr, page) == VM_FAULT_NOPAGE ? 0 : -EFAULT;
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot)
{
        int cache = prot_to_cache(prot);
        int writable = (vma->vm_flags & VM_WRITE) != 0;
        vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
        for (unsigned long off = 0; off < PAGE_ALIGN(size); off += PAGE_SIZE)
                if (luos_user_map_pfn(vma->luos_proc, addr + off, pfn + (off >> PAGE_SHIFT), writable, cache) < 0) return -ENOMEM;
        return 0;
}

int vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len)
{
        unsigned long vm_len = vma->vm_end - vma->vm_start;
        unsigned long pfn = (start >> PAGE_SHIFT) + vma->vm_pgoff;
        if ((vma->vm_pgoff << PAGE_SHIFT) + vm_len > PAGE_ALIGN(len + offset_in_page(start))) return -EINVAL;
        return remap_pfn_range(vma, vma->vm_start, pfn, vm_len, vma->vm_page_prot);
}

int apply_to_page_range(struct mm_struct *mm, unsigned long address, unsigned long size, pte_fn_t fn, void *data)
{
        if (!mm || !mm->luos_mm) return -EINVAL;
        for (unsigned long off = 0; off < size; off += PAGE_SIZE) {
                pte_t pte = __pte(0);
                int err = fn(&pte, address + off, data);
                if (err) return err;
                if (!pte_present(pte)) continue;
                pgprot_t prot = __pgprot(pte_val(pte) & ~(LKPI_PTE_PFN_MASK | _PAGE_SPECIAL_LKPI));
                if (luos_user_map_pfn(mm->luos_mm, address + off, pte_pfn(pte), pte_write(pte), prot_to_cache(prot)) < 0)
                        return -ENOMEM;
        }
        return 0;
}

void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address, unsigned long size)
{
        luos_user_unmap(vma->luos_proc, address, size);
}

void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin, loff_t const holelen, int even_cows)
{
        struct vm_area_struct *vma;
        if (!mapping || !mapping->lkpi_vmas.next) return;
        u64 hb = holebegin >> PAGE_SHIFT;
        u64 he = holelen ? hb + (PAGE_ALIGN(holelen) >> PAGE_SHIFT) : ~0ULL;
        spin_lock(&mapping->lkpi_vma_lock);
        list_for_each_entry(vma, &mapping->lkpi_vmas, lkpi_node) {
                u64 vb = vma->vm_pgoff;
                u64 ve = vb + vma_pages(vma);
                u64 s = max(hb, vb), e = min(he, ve);
                if (s >= e) continue;
                luos_user_unmap(vma->luos_proc, vma->vm_start + ((s - vb) << PAGE_SHIFT), (e - s) << PAGE_SHIFT);
        }
        spin_unlock(&mapping->lkpi_vma_lock);
}

int vm_munmap(unsigned long start, size_t len) { return -EINVAL; }

unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len, unsigned long prot, unsigned long flag,
                      unsigned long offset)
{
        return (unsigned long)-ENOSYS;
}

long get_user_pages_fast(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages) { return -EFAULT; }
long pin_user_pages_fast(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages) { return -EFAULT; }

void unpin_user_pages(struct page **pages, unsigned long npages)
{
        for (unsigned long i = 0; i < npages; i++) put_page(pages[i]);
}



unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n)
{
        if (!n) return 0;
        if (current->uaccess_kds > 0) {
                memcpy((void *)to, from, n);
                return 0;
        }
        if (!access_ok(to, n)) return n;
        return luos_copy_to_user((luos_u64)to, from, n) ? n : 0;
}

unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n)
{
        if (!n) return 0;
        if (current->uaccess_kds > 0) {
                memcpy(to, (const void *)from, n);
                return 0;
        }
        if (!access_ok(from, n) || luos_copy_from_user(to, (luos_u64)from, n)) {
                memset(to, 0, n);
                return n;
        }
        return 0;
}

unsigned long lkpi_clear_user(void __user *to, unsigned long n)
{
        if (!n) return 0;
        if (current->uaccess_kds > 0) {
                memset((void *)to, 0, n);
                return 0;
        }
        if (!access_ok(to, n)) return n;
        return luos_clear_user((luos_u64)to, n) ? n : 0;
}

long strncpy_from_user(char *dst, const char __user *src, long count)
{
        long i;
        for (i = 0; i < count; i++) {
                if (luos_copy_from_user(&dst[i], (luos_u64)src + i, 1)) return -EFAULT;
                if (!dst[i]) return i;
        }
        return count;
}

long strnlen_user(const char __user *str, long n)
{
        for (long i = 0; i < n; i++) {
                char c;
                if (luos_copy_from_user(&c, (luos_u64)str + i, 1)) return 0;
                if (!c) return i + 1;
        }
        return n + 1;
}

void *memdup_user(const void __user *src, size_t len)
{
        void *p = kmalloc(len, GFP_KERNEL);
        if (!p) return ERR_PTR(-ENOMEM);
        if (copy_from_user(p, src, len)) {
                kfree(p);
                return ERR_PTR(-EFAULT);
        }
        return p;
}

void *vmemdup_user(const void __user *src, size_t len)
{
        void *p = kvmalloc(len, GFP_KERNEL);
        if (!p) return ERR_PTR(-ENOMEM);
        if (copy_from_user(p, src, len)) {
                kvfree(p);
                return ERR_PTR(-EFAULT);
        }
        return p;
}

void *memdup_user_nul(const void __user *src, size_t len)
{
        char *p = kmalloc(len + 1, GFP_KERNEL);
        if (!p) return ERR_PTR(-ENOMEM);
        if (copy_from_user(p, src, len)) {
                kfree(p);
                return ERR_PTR(-EFAULT);
        }
        p[len] = 0;
        return p;
}

char *strndup_user(const char __user *s, long n)
{
        long len = strnlen_user(s, n);
        if (!len) return ERR_PTR(-EFAULT);
        if (len > n) return ERR_PTR(-EINVAL);
        char *p = memdup_user(s, len);
        if (IS_ERR(p)) return p;
        p[len - 1] = 0;
        return p;
}


struct resource iomem_resource = { .name = "PCI mem", .start = 0, .end = ~(resource_size_t)0, .flags = IORESOURCE_MEM };
struct resource ioport_resource = { .name = "PCI IO", .start = 0, .end = 0xffff, .flags = IORESOURCE_IO };
static DEFINE_MUTEX(resource_lock);

int request_resource(struct resource *root, struct resource *new)
{
        mutex_lock(&resource_lock);
        new->parent = root;
        new->sibling = root->child;
        root->child = new;
        mutex_unlock(&resource_lock);
        return 0;
}

int release_resource(struct resource *old)
{
        struct resource **p;
        if (!old->parent) return -EINVAL;
        mutex_lock(&resource_lock);
        for (p = &old->parent->child; *p; p = &(*p)->sibling) {
                if (*p == old) {
                        *p = old->sibling;
                        break;
                }
        }
        old->parent = NULL;
        mutex_unlock(&resource_lock);
        return 0;
}

int insert_resource(struct resource *parent, struct resource *new) { return request_resource(parent, new); }
int remove_resource(struct resource *old) { return release_resource(old); }

static struct resource *request_region_common(struct resource *root, resource_size_t start, resource_size_t n,
                                              const char *name)
{
        struct resource *r = kzalloc(sizeof(*r), GFP_KERNEL);
        if (!r) return NULL;
        r->start = start;
        r->end = start + n - 1;
        r->name = name;
        r->flags = root->flags | IORESOURCE_BUSY;
        request_resource(root, r);
        return r;
}

static void release_region_common(struct resource *root, resource_size_t start, resource_size_t n)
{
        struct resource *r, *found = NULL;
        mutex_lock(&resource_lock);
        for (r = root->child; r; r = r->sibling)
                if (r->start == start && r->end == start + n - 1) found = r;
        mutex_unlock(&resource_lock);
        if (found) {
                release_resource(found);
                kfree(found);
        }
}

struct resource *request_mem_region(resource_size_t start, resource_size_t n, const char *name)
{
        return request_region_common(&iomem_resource, start, n, name);
}

struct resource *request_region(resource_size_t start, resource_size_t n, const char *name)
{
        return request_region_common(&ioport_resource, start, n, name);
}

void release_mem_region(resource_size_t start, resource_size_t n) { release_region_common(&iomem_resource, start, n); }
void release_region(resource_size_t start, resource_size_t n) { release_region_common(&ioport_resource, start, n); }

int region_intersects(resource_size_t offset, size_t size, unsigned long flags, unsigned long desc)
{
        if (!(flags & IORESOURCE_SYSTEM_RAM)) return REGION_DISJOINT;
        unsigned long ram = 0, total = 0;
        for (resource_size_t a = offset & PAGE_MASK; a < offset + size; a += PAGE_SIZE) {
                total++;
                if (luos_pfn_is_ram(a >> PAGE_SHIFT)) ram++;
        }
        if (!ram) return REGION_DISJOINT;
        return ram == total ? REGION_INTERSECTS : REGION_MIXED;
}

int walk_iomem_res_desc(unsigned long desc, unsigned long flags, u64 start, u64 end, void *arg,
                        int (*func)(struct resource *, void *))
{
        return -EINVAL;
}


void __iomem *ioremap(resource_size_t offset, unsigned long size) { return (void __iomem *)luos_ioremap(offset, size, 2); }
void __iomem *ioremap_wc(resource_size_t offset, unsigned long size) { return (void __iomem *)luos_ioremap(offset, size, 5); }
void __iomem *ioremap_uc(resource_size_t offset, unsigned long size) { return (void __iomem *)luos_ioremap(offset, size, 3); }
void __iomem *ioremap_cache(resource_size_t offset, unsigned long size) { return (void __iomem *)luos_ioremap(offset, size, 0); }

void iounmap(volatile void __iomem *addr)
{
        if (addr && !lkpi_addr_is_direct((const void __force *)addr)) luos_iounmap((luos_u64)addr);
}

void *memremap(resource_size_t offset, size_t size, unsigned long flags)
{
        if (flags & MEMREMAP_WB) {
                if (region_intersects(offset, size, IORESOURCE_SYSTEM_RAM, 0) == REGION_INTERSECTS) return __va(offset);
                return (void __force *)ioremap_cache(offset, size);
        }
        if (flags & MEMREMAP_WC) return (void __force *)ioremap_wc(offset, size);
        return (void __force *)ioremap(offset, size);
}

void memunmap(void *addr) { iounmap((void __iomem *)addr); }

int arch_phys_wc_add(unsigned long base, unsigned long size) { return 0; }
void arch_phys_wc_del(int handle) { }
int arch_phys_wc_index(int handle) { return -1; }
int arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size) { return 0; }
void arch_io_free_memtype_wc(resource_size_t start, resource_size_t size) { }
int io_reserve_memtype(resource_size_t start, resource_size_t end, int *pcm) { return 0; }

struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base, unsigned long size)
{
        iomap->iomem = ioremap_wc(base, size);
        if (!iomap->iomem) return NULL;
        iomap->base = base;
        iomap->size = size;
        iomap->prot = pgprot_writecombine(PAGE_KERNEL_IO);
        return iomap;
}

struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size)
{
        struct io_mapping *m = kzalloc(sizeof(*m), GFP_KERNEL);
        if (!m) return NULL;
        if (!io_mapping_init_wc(m, base, size)) {
                kfree(m);
                return NULL;
        }
        return m;
}

void io_mapping_fini(struct io_mapping *mapping) { iounmap(mapping->iomem); }

void io_mapping_free(struct io_mapping *mapping)
{
        io_mapping_fini(mapping);
        kfree(mapping);
}


struct scatterlist *sg_next(struct scatterlist *sg)
{
        if (sg_is_last(sg)) return NULL;
        sg++;
        if (unlikely(sg_is_chain(sg))) sg = sg_chain_ptr(sg);
        return sg;
}

int sg_nents(struct scatterlist *sg)
{
        int nents;
        for (nents = 0; sg; sg = sg_next(sg)) nents++;
        return nents;
}

int sg_nents_for_len(struct scatterlist *sg, u64 len)
{
        int nents;
        u64 total;
        if (!len) return 0;
        for (nents = 0, total = 0; sg; sg = sg_next(sg)) {
                nents++;
                total += sg->length;
                if (total >= len) return nents;
        }
        return -EINVAL;
}

struct scatterlist *sg_last(struct scatterlist *s, unsigned int nents)
{
        struct scatterlist *ret = NULL;
        for (unsigned int i = 0; i < nents && s; i++, s = sg_next(s)) ret = s;
        return ret;
}

void sg_init_table(struct scatterlist *sgl, unsigned int nents)
{
        memset(sgl, 0, sizeof(*sgl) * nents);
        sg_mark_end(&sgl[nents - 1]);
}

void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
        sg_init_table(sg, 1);
        sg_set_buf(sg, buf, buflen);
}

int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp_mask)
{
        memset(table, 0, sizeof(*table));
        if (!nents) return -EINVAL;
        table->sgl = kvmalloc_array(nents, sizeof(struct scatterlist), gfp_mask);
        if (!table->sgl) return -ENOMEM;
        sg_init_table(table->sgl, nents);
        table->nents = table->orig_nents = nents;
        return 0;
}

void sg_free_table(struct sg_table *table)
{
        if (!table || !table->sgl) return;
        kvfree(table->sgl);
        table->sgl = NULL;
        table->nents = table->orig_nents = 0;
}

int sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages, unsigned int n_pages,
                                      unsigned int offset, unsigned long size, unsigned int max_segment, gfp_t gfp_mask)
{
        unsigned int chunks = 0, cur, i;
        unsigned long seg_len = 0;
        struct scatterlist *s;
        int ret;

        max_segment = ALIGN_DOWN(max_segment, PAGE_SIZE);
        if (!max_segment) max_segment = UINT_MAX & PAGE_MASK;
        if (!n_pages) return -EINVAL;

        for (i = 0; i < n_pages; i++) {
                if (i == 0 || page_to_pfn(pages[i]) != page_to_pfn(pages[i - 1]) + 1 || seg_len + PAGE_SIZE > max_segment) {
                        chunks++;
                        seg_len = 0;
                }
                seg_len += PAGE_SIZE;
        }
        ret = sg_alloc_table(sgt, chunks, gfp_mask);
        if (ret) return ret;

        s = sgt->sgl;
        cur = 0;
        while (cur < n_pages) {
                unsigned int j = cur + 1;
                seg_len = PAGE_SIZE;
                while (j < n_pages && page_to_pfn(pages[j]) == page_to_pfn(pages[j - 1]) + 1 && seg_len + PAGE_SIZE <= max_segment) {
                        j++;
                        seg_len += PAGE_SIZE;
                }
                unsigned long chunk_size = ((unsigned long)(j - cur) << PAGE_SHIFT) - offset;
                sg_set_page(s, pages[cur], min_t(unsigned long, size, chunk_size), offset);
                size -= min_t(unsigned long, size, chunk_size);
                offset = 0;
                cur = j;
                s = sg_next(s);
        }
        return 0;
}

int sg_alloc_append_table_from_pages(struct sg_append_table *sgt, struct page **pages, unsigned int n_pages,
                                     unsigned int offset, unsigned long size, unsigned int max_segment,
                                     unsigned int left_pages, gfp_t gfp_mask)
{
        if (sgt->sgt.sgl) return -EOPNOTSUPP;
        int ret = sg_alloc_table_from_pages_segment(&sgt->sgt, pages, n_pages, offset, size, max_segment, gfp_mask);
        if (ret) return ret;
        sgt->total_nents = sgt->sgt.orig_nents;
        sgt->prv = sg_last(sgt->sgt.sgl, sgt->sgt.orig_nents);
        return 0;
}

void sg_free_append_table(struct sg_append_table *sgt) { sg_free_table(&sgt->sgt); }

static size_t sg_copy(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, bool to_buffer)
{
        size_t done = 0;
        struct scatterlist *sg;
        unsigned int i;
        for_each_sg(sgl, sg, nents, i) {
                size_t n = min_t(size_t, sg->length, buflen - done);
                if (!n) break;
                if (to_buffer) memcpy((char *)buf + done, sg_virt(sg), n);
                else memcpy(sg_virt(sg), (char *)buf + done, n);
                done += n;
        }
        return done;
}

size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf, size_t buflen)
{
        return sg_copy(sgl, nents, (void *)buf, buflen, false);
}

size_t sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen)
{
        return sg_copy(sgl, nents, buf, buflen, true);
}

static int sg_page_count(struct scatterlist *sg) { return PAGE_ALIGN(sg->offset + sg->length) >> PAGE_SHIFT; }
static int sg_dma_page_count(struct scatterlist *sg) { return PAGE_ALIGN(sg->offset + sg_dma_len(sg)) >> PAGE_SHIFT; }

void __sg_page_iter_start(struct sg_page_iter *piter, struct scatterlist *sglist, unsigned int nents,
                          unsigned long pgoffset)
{
        piter->__pg_advance = 0;
        piter->__nents = nents;
        piter->sg = sglist;
        piter->sg_pgoffset = pgoffset;
}

bool __sg_page_iter_next(struct sg_page_iter *piter)
{
        if (!piter->__nents || !piter->sg) return false;
        piter->sg_pgoffset += piter->__pg_advance;
        piter->__pg_advance = 1;
        while (piter->sg_pgoffset >= sg_page_count(piter->sg)) {
                piter->sg_pgoffset -= sg_page_count(piter->sg);
                piter->sg = sg_next(piter->sg);
                if (!--piter->__nents || !piter->sg) return false;
        }
        return true;
}

bool __sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter)
{
        struct sg_page_iter *piter = &dma_iter->base;
        if (!piter->__nents || !piter->sg) return false;
        piter->sg_pgoffset += piter->__pg_advance;
        piter->__pg_advance = 1;
        while (piter->sg_pgoffset >= sg_dma_page_count(piter->sg)) {
                piter->sg_pgoffset -= sg_dma_page_count(piter->sg);
                piter->sg = sg_next(piter->sg);
                if (!--piter->__nents || !piter->sg) return false;
        }
        return true;
}


int dma_set_mask(struct device *dev, u64 mask)
{
        if (dev->dma_mask) *dev->dma_mask = mask;
        return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
        dev->coherent_dma_mask = mask;
        return 0;
}

u64 dma_get_mask(struct device *dev) { return dev && dev->dma_mask ? *dev->dma_mask : DMA_BIT_MASK(32); }

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag, unsigned long attrs)
{
        unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
        u64 mask = dev && dev->coherent_dma_mask ? dev->coherent_dma_mask : DMA_BIT_MASK(32);
        luos_u64 limit = mask < DMA_BIT_MASK(64) ? mask + 1 : 0;
        luos_u64 align = n & (n - 1) ? 1 : n;
        luos_u64 phys = luos_phys_alloc(n, align, limit);
        if (!phys) return NULL;
        page_init_block(phys_to_page(phys), n, 0);
        void *v = __va(phys);
        memset(v, 0, n << PAGE_SHIFT);
        if (attrs & DMA_ATTR_WRITE_COMBINE) luos_set_cache((luos_u64)v, n << PAGE_SHIFT, 5);
        *dma_handle = phys;
        return v;
}

void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle, unsigned long attrs)
{
        unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
        if (!cpu_addr) return;
        if (attrs & DMA_ATTR_WRITE_COMBINE) luos_set_cache((luos_u64)cpu_addr, n << PAGE_SHIFT, 0);
        luos_phys_free(dma_handle, n);
}

int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir,
                     unsigned long attrs)
{
        struct scatterlist *s;
        int i;
        for_each_sg(sg, s, nents, i) {
                s->dma_address = sg_phys(s);
                s->dma_length = s->length;
        }
        return nents;
}

int dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt, void *cpu_addr, dma_addr_t dma_addr, size_t size,
                          unsigned long attrs)
{
        int ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
        if (ret) return ret;
        sg_set_page(sgt->sgl, phys_to_page(dma_addr), PAGE_ALIGN(size), 0);
        return 0;
}

int dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma, void *cpu_addr, dma_addr_t dma_addr, size_t size,
                   unsigned long attrs)
{
        pgprot_t prot = vma->vm_page_prot;
        if (attrs & DMA_ATTR_WRITE_COMBINE) prot = pgprot_writecombine(prot);
        if (vma->vm_pgoff + vma_pages(vma) > PAGE_ALIGN(size) >> PAGE_SHIFT) return -ENXIO;
        return remap_pfn_range(vma, vma->vm_start, (dma_addr >> PAGE_SHIFT) + vma->vm_pgoff, vma->vm_end - vma->vm_start, prot);
}

void lkpi_mm_init(void)
{
        lkpi_hhdm_base = luos_hhdm_offset();
        lkpi_direct_end = lkpi_hhdm_base + ((luos_highest_pfn() + 1) << PAGE_SHIFT);
        for (unsigned int i = 0; i < KMALLOC_CLASSES; i++) raw_spin_lock_init(&kmalloc_classes[i].lock);
}


struct slab_sheaf {
        unsigned int size;
        unsigned int capacity;
        void *objects[];
};

static struct slab_sheaf *sheaf_alloc(struct kmem_cache *s, gfp_t gfp, unsigned int capacity)
{
        struct slab_sheaf *sheaf = kzalloc(struct_size(sheaf, objects, capacity), gfp & ~__GFP_ZERO);
        if (sheaf) sheaf->capacity = capacity;
        return sheaf;
}

static int sheaf_fill(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf *sheaf, unsigned int size)
{
        while (sheaf->size < size) {
                void *obj = kmem_cache_alloc(s, gfp);
                if (!obj) return -ENOMEM;
                sheaf->objects[sheaf->size++] = obj;
        }
        return 0;
}

struct slab_sheaf *kmem_cache_prefill_sheaf(struct kmem_cache *s, gfp_t gfp, unsigned int size)
{
        struct slab_sheaf *sheaf = sheaf_alloc(s, gfp, size);
        if (!sheaf) return NULL;
        if (sheaf_fill(s, gfp, sheaf, size)) {
                kmem_cache_return_sheaf(s, gfp, sheaf);
                return NULL;
        }
        return sheaf;
}

int kmem_cache_refill_sheaf(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf **sheafp, unsigned int size)
{
        struct slab_sheaf *sheaf = *sheafp;
        if (sheaf->size >= size) return 0;
        if (sheaf->capacity < size) {
                struct slab_sheaf *bigger = sheaf_alloc(s, gfp, size);
                if (!bigger) return -ENOMEM;
                memcpy(bigger->objects, sheaf->objects, sheaf->size * sizeof(void *));
                bigger->size = sheaf->size;
                kfree(sheaf);
                *sheafp = sheaf = bigger;
        }
        return sheaf_fill(s, gfp, sheaf, size);
}

void kmem_cache_return_sheaf(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf *sheaf)
{
        if (!sheaf) return;
        while (sheaf->size) kmem_cache_free(s, sheaf->objects[--sheaf->size]);
        kfree(sheaf);
}

void *kmem_cache_alloc_from_sheaf(struct kmem_cache *s, gfp_t gfp, struct slab_sheaf *sheaf)
{
        void *obj;
        if (!sheaf->size) return NULL;
        obj = sheaf->objects[--sheaf->size];
        if (gfp & __GFP_ZERO) memset(obj, 0, s->size);
        return obj;
}

unsigned int kmem_cache_sheaf_size(struct slab_sheaf *sheaf) { return sheaf->size; }

void kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p)
{
        for (size_t i = 0; i < size; i++) kmem_cache_free(s, p[i]);
}

int kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t gfp, size_t size, void **p)
{
        for (size_t i = 0; i < size; i++) {
                p[i] = kmem_cache_alloc(s, gfp);
                if (!p[i]) {
                        kmem_cache_free_bulk(s, i, p);
                        return 0;
                }
        }
        return size;
}
