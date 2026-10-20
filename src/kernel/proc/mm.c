// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/process.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "kernel/proc/vfs.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/smp/tlb.h"

#include <string.h>

#define PF_PRESENT (1u << 0)
#define PF_WRITE (1u << 1)
#define PF_USER (1u << 2)
#define PF_FETCH (1u << 4)

#define MM_COLLECT_BATCH 256

void kmutex_lock(struct kmutex *m) {
    while (!spin_trylock(&m->lock)) scheduler_yield();
}

static uint64_t page_up(uint64_t v) { return (v + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); }

const struct mm_device_hooks *mm_device_hooks;

int mm_init(struct mm *mm) {
    memset(mm, 0, sizeof(*mm));
    mm->pml4 = vmm_create_address_space();
    if (!mm->pml4) return -LX_ENOMEM;
    mm->cap = 32;
    mm->vmas = kmalloc(sizeof(struct vma) * (size_t)mm->cap);
    if (!mm->vmas) {
        vmm_destroy_address_space(mm->pml4);
        return -LX_ENOMEM;
    }
    kmutex_init(&mm->lock);
    mm->mmap_hint = USER_MMAP_TOP;
    return 0;
}

static int vma_lower_bound(struct mm *mm, uint64_t addr) {
    int lo = 0, hi = mm->nvma;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (mm->vmas[mid].end <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static struct vma *vma_find(struct mm *mm, uint64_t addr) {
    int i = vma_lower_bound(mm, addr);
    if (i < mm->nvma && mm->vmas[i].start <= addr) return &mm->vmas[i];
    return NULL;
}

static int vma_insert_at(struct mm *mm, int idx, const struct vma *v) {
    if (mm->nvma == mm->cap) {
        int ncap = mm->cap * 2;
        struct vma *n = krealloc(mm->vmas, sizeof(struct vma) * (size_t)ncap);
        if (!n) return -LX_ENOMEM;
        mm->vmas = n;
        mm->cap = ncap;
    }
    memmove(&mm->vmas[idx + 1], &mm->vmas[idx], sizeof(struct vma) * (size_t)(mm->nvma - idx));
    mm->vmas[idx] = *v;
    mm->nvma++;
    return 0;
}

static void vma_remove_at(struct mm *mm, int idx) {
    if (mm->vmas[idx].kind == VMA_DEVICE && mm->vmas[idx].dev_ctx && mm_device_hooks)
        mm_device_hooks->close(mm->vmas[idx].dev_ctx);
    if (mm->vmas[idx].file) ufile_put(mm->vmas[idx].file);
    memmove(&mm->vmas[idx], &mm->vmas[idx + 1], sizeof(struct vma) * (size_t)(mm->nvma - idx - 1));
    mm->nvma--;
}

static void mm_release_range(struct mm *mm, uint64_t start, uint64_t end, bool free_frames) {
    uint64_t phys[MM_COLLECT_BATCH];
    uint64_t va = start;
    while (va < end) {
        uint64_t next = end;
        int n = vmm_unmap_collect(mm->pml4, va, end, phys, MM_COLLECT_BATCH, &next);
        if (n > 0) {
            tlb_shootdown(va, next - va);
            if (free_frames) {
                for (int i = 0; i < n; i++) pmm_free_page(phys[i]);
                mm->resident_pages -= (uint64_t)n;
            }
        }
        va = next;
    }
}

static void mm_release_vma_pages(struct mm *mm, struct vma *v, uint64_t start, uint64_t end) {
    mm_release_range(mm, start, end, v->kind == VMA_ANON || v->kind == VMA_FILE);
}

static void vma_writeback(struct mm *mm, struct vma *v, uint64_t start, uint64_t end) {
    if (v->kind != VMA_FILE || !v->file || !v->file->ops->pwrite || !(v->prot & LX_PROT_WRITE) ||
        (v->flags & LX_MAP_TYPE) == LX_MAP_PRIVATE)
        return;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t rel = va - v->start;
        if (rel >= v->file_bytes) break;
        if (!vmm_is_mapped(mm->pml4, va)) continue;
        uint64_t phys = vmm_get_phys(mm->pml4, va) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t n = v->file_bytes - rel < PAGE_SIZE ? v->file_bytes - rel : PAGE_SIZE;
        v->file->ops->pwrite(v->file, (const void *)mm_phys_to_virt(phys), n, v->file_off + rel);
    }
}

int mm_sync(struct mm *mm, uint64_t addr, uint64_t len) {
    if (addr & (PAGE_SIZE - 1)) return -LX_EINVAL;
    len = page_up(len);
    uint64_t end = addr + len;
    kmutex_lock(&mm->lock);
    for (int i = vma_lower_bound(mm, addr); i < mm->nvma && mm->vmas[i].start < end; i++) {
        struct vma *v = &mm->vmas[i];
        vma_writeback(mm, v, v->start > addr ? v->start : addr, v->end < end ? v->end : end);
    }
    kmutex_unlock(&mm->lock);
    return 0;
}

static int vma_split(struct mm *mm, uint64_t addr) {
    int i = vma_lower_bound(mm, addr);
    if (i >= mm->nvma) return 0;
    struct vma *v = &mm->vmas[i];
    if (v->start >= addr) return 0;

    struct vma right = *v;
    uint64_t delta = addr - v->start;
    right.start = addr;
    if (right.kind == VMA_FILE || right.kind == VMA_SHARED) {
        right.file_off += delta;
        right.file_bytes = right.file_bytes > delta ? right.file_bytes - delta : 0;
        ufile_get(right.file);
    }
    if (right.kind == VMA_DEVICE && right.dev_ctx && mm_device_hooks)
        right.dev_ctx = mm_device_hooks->split(v->dev_ctx, v->start, addr, v->end);
    v->end = addr;
    if (v->kind == VMA_FILE && v->file_bytes > delta) v->file_bytes = delta;
    int r = vma_insert_at(mm, i + 1, &right);
    if (r && right.file) ufile_put(right.file);
    return r;
}

static int mm_unmap_locked(struct mm *mm, uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    if (vma_split(mm, addr) || vma_split(mm, end)) return -LX_ENOMEM;

    int i = vma_lower_bound(mm, addr);
    while (i < mm->nvma && mm->vmas[i].start < end) {
        vma_writeback(mm, &mm->vmas[i], mm->vmas[i].start, mm->vmas[i].end);
        mm_release_vma_pages(mm, &mm->vmas[i], mm->vmas[i].start, mm->vmas[i].end);
        vma_remove_at(mm, i);
    }
    return 0;
}

int mm_unmap(struct mm *mm, uint64_t addr, uint64_t len) {
    if (addr & (PAGE_SIZE - 1)) return -LX_EINVAL;
    len = page_up(len);
    if (!len || addr + len > USER_LIMIT || addr + len < addr) return -LX_EINVAL;
    kmutex_lock(&mm->lock);
    int r = mm_unmap_locked(mm, addr, len);
    kmutex_unlock(&mm->lock);
    return r;
}

static bool mm_range_free(struct mm *mm, uint64_t addr, uint64_t len) {
    int i = vma_lower_bound(mm, addr);
    return i >= mm->nvma || mm->vmas[i].start >= addr + len;
}

uint64_t mm_find_free(struct mm *mm, uint64_t len, uint64_t align) {
    if (align < PAGE_SIZE) align = PAGE_SIZE;
    uint64_t top = USER_MMAP_TOP;
    for (int i = mm->nvma - 1; i >= -1; i--) {
        uint64_t gap_start = i >= 0 ? mm->vmas[i].end : USER_MMAP_BOTTOM;
        if (i >= 0 && mm->vmas[i].start >= top) continue;
        if (gap_start < USER_MMAP_BOTTOM) gap_start = USER_MMAP_BOTTOM;
        if (top > gap_start && top - gap_start >= len) {
            uint64_t a = (top - len) & ~(align - 1);
            if (a >= gap_start) return a;
        }
        if (i >= 0 && mm->vmas[i].start < top) top = mm->vmas[i].start;
        if (top <= USER_MMAP_BOTTOM) break;
    }
    return 0;
}

static void vma_try_merge(struct mm *mm, int i) {
    if (i < 0 || i + 1 >= mm->nvma) return;
    struct vma *a = &mm->vmas[i];
    struct vma *b = &mm->vmas[i + 1];
    if (a->end != b->start || a->kind != VMA_ANON || b->kind != VMA_ANON) return;
    if (a->prot != b->prot || a->flags != b->flags) return;
    a->end = b->end;
    vma_remove_at(mm, i + 1);
}

static int mm_map_impl(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, struct ufile *file,
                       uint64_t off, uint64_t file_bytes, void *dev_ctx, uint64_t *out_addr);

int mm_map(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, struct ufile *file,
           uint64_t off, uint64_t file_bytes, uint64_t *out_addr) {
    return mm_map_impl(mm, addr, len, prot, flags, file, off, file_bytes, NULL, out_addr);
}

int mm_map_device(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, void *dev_ctx,
                  uint64_t *out_addr) {
    return mm_map_impl(mm, addr, len, prot, flags, NULL, 0, 0, dev_ctx, out_addr);
}

int mm_reserve_range(struct mm *mm, uint64_t addr, uint64_t len, uint32_t flags, uint64_t *out_addr) {
    len = page_up(len);
    if (!len) return -LX_EINVAL;
    kmutex_lock(&mm->lock);
    if (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) {
        if ((addr & (PAGE_SIZE - 1)) || addr + len > USER_LIMIT || addr + len < addr || addr < PAGE_SIZE) {
            kmutex_unlock(&mm->lock);
            return -LX_EINVAL;
        }
    } else if (!(addr && !(addr & (PAGE_SIZE - 1)) && addr + len <= USER_LIMIT && addr >= USER_MMAP_BOTTOM &&
                 mm_range_free(mm, addr, len))) {
        addr = mm_find_free(mm, len, PAGE_SIZE);
        if (!addr) {
            kmutex_unlock(&mm->lock);
            return -LX_ENOMEM;
        }
    }
    kmutex_unlock(&mm->lock);
    *out_addr = addr;
    return 0;
}

static int mm_map_impl(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, struct ufile *file,
                       uint64_t off, uint64_t file_bytes, void *dev_ctx, uint64_t *out_addr) {
    len = page_up(len);
    if (!len) return -LX_EINVAL;
    if (addr & (PAGE_SIZE - 1)) {
        if (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) return -LX_EINVAL;
        addr = 0;
    }

    kmutex_lock(&mm->lock);

    if (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) {
        if (addr + len > USER_LIMIT || addr + len < addr || addr < PAGE_SIZE) {
            kmutex_unlock(&mm->lock);
            return -LX_EINVAL;
        }
        if (!mm_range_free(mm, addr, len)) {
            if (flags & LX_MAP_FIXED_NOREPLACE) {
                kmutex_unlock(&mm->lock);
                return -LX_EEXIST;
            }
            int r = mm_unmap_locked(mm, addr, len);
            if (r) {
                kmutex_unlock(&mm->lock);
                return r;
            }
        }
    } else if (!(addr && addr + len <= USER_LIMIT && addr + len > addr && addr >= USER_MMAP_BOTTOM &&
                 mm_range_free(mm, addr, len))) {
        addr = mm_find_free(mm, len, PAGE_SIZE);
        if (!addr) {
            kmutex_unlock(&mm->lock);
            return -LX_ENOMEM;
        }
    }

    struct vma v = {
        .start = addr,
        .end = addr + len,
        .prot = prot,
        .flags = flags & (LX_MAP_TYPE | LX_MAP_GROWSDOWN | LX_MAP_STACK),
        .kind = dev_ctx ? VMA_DEVICE
              : !file ? VMA_ANON
              : (file->ops->page && (flags & LX_MAP_TYPE) != LX_MAP_PRIVATE) ? VMA_SHARED : VMA_FILE,
        .file = file ? ufile_get(file) : NULL,
        .file_off = off,
        .file_bytes = file ? file_bytes : 0,
        .dev_ctx = dev_ctx,
    };
    int idx = vma_lower_bound(mm, addr);
    int r = vma_insert_at(mm, idx, &v);
    if (r) {
        if (v.file) ufile_put(v.file);
        kmutex_unlock(&mm->lock);
        return r;
    }
    vma_try_merge(mm, idx);
    vma_try_merge(mm, idx - 1);

    kmutex_unlock(&mm->lock);
    if (out_addr) *out_addr = addr;
    return 0;
}

static uint64_t prot_to_pte(uint32_t prot) {
    uint64_t f = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (prot & LX_PROT_WRITE) f |= VMM_FLAG_WRITE;
    if (!(prot & LX_PROT_EXEC)) f |= VMM_FLAG_NX;
    return f;
}

int mm_protect(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot) {
    if (addr & (PAGE_SIZE - 1)) return -LX_EINVAL;
    len = page_up(len);
    uint64_t end = addr + len;
    if (end > USER_LIMIT || end < addr) return -LX_EINVAL;
    if (!len) return 0;

    kmutex_lock(&mm->lock);
    for (uint64_t a = addr; a < end;) {
        struct vma *v = vma_find(mm, a);
        if (!v) {
            kmutex_unlock(&mm->lock);
            return -LX_ENOMEM;
        }
        a = v->end;
    }
    if (vma_split(mm, addr) || vma_split(mm, end)) {
        kmutex_unlock(&mm->lock);
        return -LX_ENOMEM;
    }
    for (int i = vma_lower_bound(mm, addr); i < mm->nvma && mm->vmas[i].start < end; i++)
        mm->vmas[i].prot = prot;

    if (prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) {
        uint64_t set = prot_to_pte(prot) & (VMM_FLAG_WRITE | VMM_FLAG_NX);
        vmm_update_flags(mm->pml4, addr, end, set, (VMM_FLAG_WRITE | VMM_FLAG_NX) & ~set);
    } else {
        for (int i = vma_lower_bound(mm, addr); i < mm->nvma && mm->vmas[i].start < end; i++)
            mm_release_vma_pages(mm, &mm->vmas[i], mm->vmas[i].start, mm->vmas[i].end);
    }
    kmutex_unlock(&mm->lock);
    return 0;
}

int mm_dontneed(struct mm *mm, uint64_t addr, uint64_t len) {
    if (addr & (PAGE_SIZE - 1)) return -LX_EINVAL;
    len = page_up(len);
    kmutex_lock(&mm->lock);
    uint64_t end = addr + len;
    for (int i = vma_lower_bound(mm, addr); i < mm->nvma && mm->vmas[i].start < end; i++) {
        struct vma *v = &mm->vmas[i];
        uint64_t s0 = v->start > addr ? v->start : addr;
        uint64_t e0 = v->end < end ? v->end : end;
        vma_writeback(mm, v, s0, e0);
        mm_release_vma_pages(mm, v, s0, e0);
    }
    kmutex_unlock(&mm->lock);
    return 0;
}

void mm_destroy(struct mm *mm) {
    if (!mm->pml4) return;
    kmutex_lock(&mm->lock);
    while (mm->nvma > 0) {
        vma_writeback(mm, &mm->vmas[0], mm->vmas[0].start, mm->vmas[0].end);
        mm_release_vma_pages(mm, &mm->vmas[0], mm->vmas[0].start, mm->vmas[0].end);
        vma_remove_at(mm, 0);
    }
    vmm_free_user_tables(mm->pml4);
    pmm_free_page(mm->pml4);
    mm->pml4 = 0;
    kfree(mm->vmas);
    mm->vmas = NULL;
    kmutex_unlock(&mm->lock);
}

#define MM_STACK_GROW_WINDOW (64ULL * 1024 * 1024)
#define MM_STACK_GROW_MAX (512ULL * 1024 * 1024)

static int vma_grow_stack(struct mm *mm, uint64_t addr) {
    int i = vma_lower_bound(mm, addr);
    if (i >= mm->nvma) return -1;
    struct vma *v = &mm->vmas[i];
    if (v->start <= addr) return -1;
    if (!(v->flags & (LX_MAP_GROWSDOWN | LX_MAP_STACK))) return -1;
    if (v->end - v->start >= MM_STACK_GROW_MAX) return -1;
    if (v->start - addr > MM_STACK_GROW_WINDOW) return -1;
    if (v->end - addr > MM_STACK_GROW_MAX) return -1;
    if (i > 0 && mm->vmas[i - 1].end > addr) return -1;
    uint64_t new_start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    v->start = new_start;
    if (v->kind == VMA_ANON) vma_try_merge(mm, i - 1);
    return 0;
}

int mm_handle_fault(struct mm *mm, uint64_t addr, uint64_t err) {
    uint64_t va = addr & ~(uint64_t)(PAGE_SIZE - 1);

    kmutex_lock(&mm->lock);
    struct vma *v = vma_find(mm, addr);
    if (!v) {
        if (vma_grow_stack(mm, addr) == 0)
            v = vma_find(mm, addr);
        if (!v) {
            kmutex_unlock(&mm->lock);
            return -1;
        }
    }
    uint32_t prot = v->prot;
    if (!(prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) ||
        ((err & PF_WRITE) && !(prot & LX_PROT_WRITE)) || ((err & PF_FETCH) && !(prot & LX_PROT_EXEC))) {
        kmutex_unlock(&mm->lock);
        return -1;
    }
    if (vmm_is_mapped(mm->pml4, va)) {
        kmutex_unlock(&mm->lock);
        return (err & PF_PRESENT) ? -1 : 0;
    }
    if (v->kind == VMA_DEVICE) {
        void *ctx = v->dev_ctx;
        kmutex_unlock(&mm->lock);
        if (!ctx || !mm_device_hooks) return -1;
        return mm_device_hooks->fault(ctx, proc_current(), addr, (err & PF_WRITE) != 0) == 0 ? 0 : -1;
    }
    if (v->kind == VMA_SHARED) {
        uint64_t phys = v->file->ops->page(v->file, (v->file_off + (va - v->start)) / PAGE_SIZE);
        int r = phys && vmm_map_page(mm->pml4, va, phys, prot_to_pte(prot)) == 0 ? 0 : -1;
        kmutex_unlock(&mm->lock);
        return r;
    }

    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kmutex_unlock(&mm->lock);
        LOG_ERROR("out of memory on page fault at 0x%llx", (unsigned long long)addr);
        return -1;
    }
    uint8_t *page = (uint8_t *)mm_phys_to_virt(phys);
    memset(page, 0, PAGE_SIZE);

    if (v->kind == VMA_FILE && v->file) {
        uint64_t rel = va - v->start;
        if (rel < v->file_bytes) {
            uint64_t n = v->file_bytes - rel;
            if (n > PAGE_SIZE) n = PAGE_SIZE;
            struct ufile *f = v->file;
            int64_t got = f->ops->pread ? f->ops->pread(f, page, n, v->file_off + rel) : -LX_EIO;
            if (got < 0) {
                pmm_free_page(phys);
                kmutex_unlock(&mm->lock);
                LOG_WARNING("read error %lld paging in %s at offset 0x%llx", (long long)got, f->path,
                            (unsigned long long)(v->file_off + rel));
                return -1;
            }
        }
    }

    if (vmm_map_page(mm->pml4, va, phys, prot_to_pte(prot)) != 0) {
        pmm_free_page(phys);
        kmutex_unlock(&mm->lock);
        return -1;
    }
    mm->resident_pages++;
    kmutex_unlock(&mm->lock);
    return 0;
}


extern char uaccess_copy_insn[];
extern char uaccess_set_insn[];
extern char uaccess_fault[];
int uaccess_copy(void *dst, const void *src, size_t n);
int uaccess_set(void *dst, int c, size_t n);

__attribute__((naked)) int uaccess_copy(void *dst, const void *src, size_t n) {
    (void)dst; (void)src; (void)n;
    asm volatile(
        "mov %rdx, %rcx\n\t"
        ".global uaccess_copy_insn\n"
        "uaccess_copy_insn:\n\t"
        "rep movsb\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t"
        ".global uaccess_fault\n"
        "uaccess_fault:\n\t"
        "mov $-14, %eax\n\t"
        "ret\n\t");
}

__attribute__((naked)) int uaccess_set(void *dst, int c, size_t n) {
    (void)dst; (void)c; (void)n;
    asm volatile(
        "mov %esi, %eax\n\t"
        "mov %rdx, %rcx\n\t"
        ".global uaccess_set_insn\n"
        "uaccess_set_insn:\n\t"
        "rep stosb\n\t"
        "xor %eax, %eax\n\t"
        "ret\n\t");
}

bool user_range_ok(uint64_t addr, size_t n) {
    if (addr + n < addr) return false;
    return addr + n <= USER_LIMIT;
}

int copy_from_user(void *dst, uint64_t src, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(src, n) || !src) return -LX_EFAULT;
    return uaccess_copy(dst, (const void *)src, n);
}

int copy_to_user(uint64_t dst, const void *src, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(dst, n) || !dst) return -LX_EFAULT;
    return uaccess_copy((void *)dst, src, n);
}

int clear_user(uint64_t dst, size_t n) {
    if (!n) return 0;
    if (!user_range_ok(dst, n) || !dst) return -LX_EFAULT;
    return uaccess_set((void *)dst, 0, n);
}

long strncpy_from_user(char *dst, uint64_t src, size_t cap) {
    size_t done = 0;
    while (done < cap) {
        uint64_t a = src + done;
        size_t chunk = PAGE_SIZE - (a & (PAGE_SIZE - 1));
        if (chunk > cap - done) chunk = cap - done;
        if (copy_from_user(dst + done, a, chunk) != 0) return -LX_EFAULT;
        for (size_t i = 0; i < chunk; i++)
            if (dst[done + i] == '\0') return (long)(done + i);
        done += chunk;
    }
    return -LX_ENAMETOOLONG;
}


static inline uint64_t read_cr2(void) {
    uint64_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void report_location(struct process *p, uint64_t rip) {
    kmutex_lock(&p->mm.lock);
    struct vma *v = vma_find(&p->mm, rip);
    if (v && v->file)
        LOG_WARNING("  rip is in %s at file offset 0x%llx", v->file->path,
                    (unsigned long long)(v->file_off + (rip - v->start)));
    else if (v)
        LOG_WARNING("  rip is in an anonymous mapping 0x%llx-0x%llx (+0x%llx)", (unsigned long long)v->start,
                    (unsigned long long)v->end, (unsigned long long)(rip - v->start));
    kmutex_unlock(&p->mm.lock);
}

int user_fault(struct registers *regs, uint64_t addr) {
    struct task *t = current_task();
    struct process *p = t ? (struct process *)t->proc : NULL;
    bool from_user = (regs->cs & 3) == 3;

    if (!p || addr >= USER_LIMIT) return 0;

    if (regs->rflags & (1 << 9)) asm volatile("sti");
    int r = mm_handle_fault(&p->mm, addr, regs->err_code);
    asm volatile("cli");
    if (r == 0) return 1;

    if (!from_user) {
        if (regs->rip == (uint64_t)uaccess_copy_insn || regs->rip == (uint64_t)uaccess_set_insn) {
            regs->rip = (uint64_t)uaccess_fault;
            return 1;
        }
        return 0;
    }

    LOG_WARNING("pid %d (%s) tid %u: segmentation fault at 0x%llx, rip 0x%llx, error 0x%llx", p->pid, p->name,
                t->tid, (unsigned long long)addr, (unsigned long long)regs->rip,
                (unsigned long long)regs->err_code);
    kmutex_lock(&p->mm.lock);
    LOG_WARNING("mm dump: brk 0x%llx brk_start 0x%llx nvma %d",
                (unsigned long long)p->mm.brk, (unsigned long long)p->mm.brk_start, p->mm.nvma);
    int vma_at = vma_lower_bound(&p->mm, addr);
    if (vma_at >= p->mm.nvma) vma_at = p->mm.nvma - 1;
    if (vma_at > 0 && p->mm.vmas[vma_at].start > addr) vma_at--;
    int vma_lo = vma_at - 4;
    if (vma_lo < 0) vma_lo = 0;
    int vma_hi = vma_lo + 8;
    if (vma_hi > p->mm.nvma) vma_hi = p->mm.nvma;
    for (int i = vma_lo; i < vma_hi; i++) {
        struct vma *v = &p->mm.vmas[i];
        LOG_WARNING("  vma[%d] 0x%llx-0x%llx kind %d prot %x", i,
                     (unsigned long long)v->start, (unsigned long long)v->end, v->kind, v->prot);
    }
    int vma_rest = p->mm.nvma - vma_hi;
    kmutex_unlock(&p->mm.lock);
    if (vma_lo > 0)
        LOG_WARNING("  ... %d mapping(s) before the listed window", vma_lo);
    if (vma_rest > 0)
        LOG_WARNING("  ... %d more mappings not listed", vma_rest);
    asm volatile("sti");
    report_location(p, regs->rip);
    proc_exit_group(PROC_EXIT_SIGNAL | LX_SIGSEGV);
}

void user_exception(struct registers *regs) {
    struct task *t = current_task();
    struct process *p = t ? (struct process *)t->proc : NULL;
    int sig;
    switch (regs->int_no) {
        case 0: case 16: case 19: sig = LX_SIGFPE; break;
        case 1: case 3: sig = LX_SIGTRAP; break;
        case 6: sig = LX_SIGILL; break;
        case 17: sig = LX_SIGBUS; break;
        default: sig = LX_SIGSEGV; break;
    }
    LOG_WARNING("pid %d (%s) tid %u: exception %llu at rip 0x%llx (error 0x%llx, cr2 0x%llx), killed with signal %d",
                p ? p->pid : -1, p ? p->name : "?", t ? t->tid : 0, (unsigned long long)regs->int_no,
                (unsigned long long)regs->rip, (unsigned long long)regs->err_code, (unsigned long long)read_cr2(), sig);
    asm volatile("sti");
    if (p) report_location(p, regs->rip);
    proc_exit_group(PROC_EXIT_SIGNAL | sig);
}
