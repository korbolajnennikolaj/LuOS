// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/elf.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/pmm.h"
#include "kernel/proc/vfs.h"

#include <cpuid.h>
#include <string.h>

#define EI_NIDENT 16
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct elf64_ehdr {
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

#define ELF_MAX_PHDRS 64

struct elf_image {
    struct elf64_ehdr eh;
    struct elf64_phdr ph[ELF_MAX_PHDRS];
    uint64_t base;
    uint64_t phdr_addr;
    uint64_t brk;
    char interp[PROC_PATH_MAX];
};

static uint64_t pg_down(uint64_t v) { return v & ~(uint64_t)(PAGE_SIZE - 1); }
static uint64_t pg_up(uint64_t v) { return (v + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); }

static int elf_read_headers(struct ufile *f, struct elf_image *img) {
    if (!f->ops->pread) return -LX_EACCES;
    if (f->ops->pread(f, &img->eh, sizeof(img->eh), 0) != (int64_t)sizeof(img->eh)) return -LX_ENOEXEC;
    struct elf64_ehdr *e = &img->eh;
    if (e->e_ident[0] != 0x7F || e->e_ident[1] != 'E' || e->e_ident[2] != 'L' || e->e_ident[3] != 'F')
        return -LX_ENOEXEC;
    if (e->e_ident[4] != 2 || e->e_ident[5] != 1) return -LX_ENOEXEC;
    if (e->e_machine != EM_X86_64 || (e->e_type != ET_EXEC && e->e_type != ET_DYN)) return -LX_ENOEXEC;
    if (e->e_phentsize != sizeof(struct elf64_phdr) || e->e_phnum == 0 || e->e_phnum > ELF_MAX_PHDRS)
        return -LX_ENOEXEC;
    uint64_t sz = (uint64_t)e->e_phnum * sizeof(struct elf64_phdr);
    if (f->ops->pread(f, img->ph, sz, e->e_phoff) != (int64_t)sz) return -LX_ENOEXEC;

    img->interp[0] = '\0';
    for (int i = 0; i < e->e_phnum; i++) {
        struct elf64_phdr *ph = &img->ph[i];
        if (ph->p_type != PT_INTERP) continue;
        if (ph->p_filesz == 0 || ph->p_filesz >= sizeof(img->interp)) return -LX_ENOEXEC;
        if (f->ops->pread(f, img->interp, ph->p_filesz, ph->p_offset) != (int64_t)ph->p_filesz)
            return -LX_ENOEXEC;
        img->interp[ph->p_filesz] = '\0';
    }
    return 0;
}

static int elf_span(struct elf_image *img, uint64_t *lo, uint64_t *hi) {
    *lo = ~0ULL;
    *hi = 0;
    for (int i = 0; i < img->eh.e_phnum; i++) {
        struct elf64_phdr *ph = &img->ph[i];
        if (ph->p_type != PT_LOAD) continue;
        if (pg_down(ph->p_vaddr) < *lo) *lo = pg_down(ph->p_vaddr);
        if (pg_up(ph->p_vaddr + ph->p_memsz) > *hi) *hi = pg_up(ph->p_vaddr + ph->p_memsz);
    }
    return *hi > *lo ? 0 : -LX_ENOEXEC;
}

static int elf_map(struct process *p, struct ufile *f, struct elf_image *img, uint64_t base) {
    img->base = base;
    img->brk = 0;
    img->phdr_addr = 0;

    for (int i = 0; i < img->eh.e_phnum; i++) {
        struct elf64_phdr *ph = &img->ph[i];
        if (ph->p_type == PT_PHDR) img->phdr_addr = base + ph->p_vaddr;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if ((ph->p_vaddr & (PAGE_SIZE - 1)) != (ph->p_offset & (PAGE_SIZE - 1))) return -LX_ENOEXEC;
        if (ph->p_filesz > ph->p_memsz) return -LX_ENOEXEC;

        uint32_t prot = 0;
        if (ph->p_flags & PF_R) prot |= LX_PROT_READ;
        if (ph->p_flags & PF_W) prot |= LX_PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= LX_PROT_EXEC;

        uint64_t vaddr = base + ph->p_vaddr;
        uint64_t start = pg_down(vaddr);
        uint64_t file_off = pg_down(ph->p_offset);
        uint64_t lead = vaddr - start;
        uint64_t file_end = pg_up(vaddr + ph->p_filesz);
        uint64_t mem_end = pg_up(vaddr + ph->p_memsz);

        if (ph->p_filesz) {
            int r = mm_map(&p->mm, start, file_end - start, prot, LX_MAP_PRIVATE | LX_MAP_FIXED, f, file_off,
                           lead + ph->p_filesz, NULL);
            if (r) return r;
        } else {
            file_end = start;
        }
        if (mem_end > file_end) {
            int r = mm_map(&p->mm, file_end, mem_end - file_end, prot, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED,
                           NULL, 0, 0, NULL);
            if (r) return r;
        }
        if (mem_end > img->brk) img->brk = mem_end;

        if (!img->phdr_addr && img->eh.e_phoff >= ph->p_offset &&
            img->eh.e_phoff < ph->p_offset + ph->p_filesz)
            img->phdr_addr = vaddr + (img->eh.e_phoff - ph->p_offset);
    }
    return 0;
}

static int elf_load_interp(struct process *p, const char *path, uint64_t *base_out, uint64_t *entry_out) {
    struct ufile *f = NULL;
    int r = vfs_open(path, LX_O_RDONLY, 0, &f);
    if (r) {
        LOG_WARNING("interpreter %s: open failed (%d)", path, r);
        return r == -LX_ENOENT ? -LX_ENOENT : r;
    }
    struct elf_image *img = kmalloc(sizeof(*img));
    if (!img) {
        ufile_put(f);
        return -LX_ENOMEM;
    }
    r = elf_read_headers(f, img);
    if (!r && img->eh.e_type != ET_DYN) r = -LX_ENOEXEC;
    uint64_t lo = 0, hi = 0;
    if (!r) r = elf_span(img, &lo, &hi);
    uint64_t base = 0;
    if (!r) {
        kmutex_lock(&p->mm.lock);
        base = mm_find_free(&p->mm, hi - lo, 0x200000);
        kmutex_unlock(&p->mm.lock);
        if (!base) r = -LX_ENOMEM;
        else base -= lo;
    }
    if (!r) r = elf_map(p, f, img, base);
    if (!r) {
        *base_out = base;
        *entry_out = base + img->eh.e_entry;
    }
    kfree(img);
    ufile_put(f);
    return r;
}

struct stack_builder {
    uint8_t *buf;
    uint64_t size;
    uint64_t top_user;
    uint64_t used;
};

static uint64_t sb_push_bytes(struct stack_builder *sb, const void *data, uint64_t n) {
    if (sb->used + n > sb->size) return 0;
    sb->used += n;
    memcpy(sb->buf + sb->size - sb->used, data, n);
    return sb->top_user - sb->used;
}

static uint64_t sb_push_str(struct stack_builder *sb, const char *s) {
    return sb_push_bytes(sb, s, strlen(s) + 1);
}

int elf_exec(struct process *p, const char *path, int argc, const char *const *argv, int envc,
             const char *const *envp, uint64_t *entry, uint64_t *sp) {
    char abs[PROC_PATH_MAX];
    int r = vfs_abs_path(p, LX_AT_FDCWD, path, abs, sizeof(abs));
    if (r) return r;

    struct ufile *f = NULL;
    r = vfs_open(abs, LX_O_RDONLY, 0, &f);
    if (r) return r;
    if ((f->mode & LX_S_IFMT) != LX_S_IFREG) {
        ufile_put(f);
        return -LX_EACCES;
    }

    struct elf_image *img = kmalloc(sizeof(*img));
    if (!img) {
        ufile_put(f);
        return -LX_ENOMEM;
    }
    r = elf_read_headers(f, img);
    if (r) goto out;

    uint64_t base = 0;
    if (img->eh.e_type == ET_DYN) {
        uint64_t lo, hi;
        r = elf_span(img, &lo, &hi);
        if (r) goto out;
        base = USER_PIE_BASE - lo;
    }
    r = elf_map(p, f, img, base);
    if (r) goto out;

    uint64_t prog_entry = base + img->eh.e_entry;
    uint64_t interp_base = 0;
    uint64_t start = prog_entry;
    if (img->interp[0]) {
        uint64_t ientry = 0;
        r = elf_load_interp(p, img->interp, &interp_base, &ientry);
        if (r) goto out;
        start = ientry;
    }

    p->mm.brk_start = p->mm.brk = pg_up(img->brk) + PAGE_SIZE;

    r = mm_map(&p->mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, LX_PROT_READ | LX_PROT_WRITE,
               LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED | LX_MAP_GROWSDOWN, NULL, 0, 0, NULL);
    if (r) goto out;

    struct stack_builder sb;
    sb.size = 64 * 1024;
    sb.buf = kmalloc(sb.size);
    sb.top_user = USER_STACK_TOP;
    sb.used = 0;
    if (!sb.buf) {
        r = -LX_ENOMEM;
        goto out;
    }

    uint64_t execfn = sb_push_str(&sb, abs);
    uint64_t platform = sb_push_str(&sb, "x86_64");
    uint8_t rnd[16];
    uint64_t tsc;
    asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) :: "rdx");
    for (int i = 0; i < 16; i++) {
        tsc ^= tsc << 13; tsc ^= tsc >> 7; tsc ^= tsc << 17;
        rnd[i] = (uint8_t)tsc;
    }
    uint64_t random = sb_push_bytes(&sb, rnd, 16);

    uint64_t argp[PROC_MAX_ARGS + 1];
    uint64_t envpp[PROC_MAX_ARGS + 1];
    for (int i = envc - 1; i >= 0; i--) envpp[i] = sb_push_str(&sb, envp[i]);
    for (int i = argc - 1; i >= 0; i--) argp[i] = sb_push_str(&sb, argv[i]);
    if (!execfn || !platform || !random) {
        kfree(sb.buf);
        r = -LX_E2BIG;
        goto out;
    }

    struct cpuid_result c1 = cpuid(1, 0);
    uint64_t auxv[][2] = {
        { LX_AT_PHDR, img->phdr_addr },
        { LX_AT_PHENT, sizeof(struct elf64_phdr) },
        { LX_AT_PHNUM, img->eh.e_phnum },
        { LX_AT_PAGESZ, PAGE_SIZE },
        { LX_AT_BASE, interp_base },
        { LX_AT_FLAGS, 0 },
        { LX_AT_ENTRY, prog_entry },
        { LX_AT_UID, 0 },
        { LX_AT_EUID, 0 },
        { LX_AT_GID, 0 },
        { LX_AT_EGID, 0 },
        { LX_AT_PLATFORM, platform },
        { LX_AT_HWCAP, c1.edx },
        { LX_AT_CLKTCK, 100 },
        { LX_AT_SECURE, 0 },
        { LX_AT_RANDOM, random },
        { LX_AT_HWCAP2, 0 },
        { LX_AT_EXECFN, execfn },
        { LX_AT_NULL, 0 },
    };
    uint64_t nauxv = sizeof(auxv) / sizeof(auxv[0]);

    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + nauxv * 2;
    uint64_t strings_end = sb.top_user - sb.used;
    uint64_t rsp = (strings_end - words * 8) & ~0xFULL;
    uint64_t pad = strings_end - rsp - words * 8;
    sb.used += pad;

    uint64_t *vec = kmalloc(words * 8);
    if (!vec) {
        kfree(sb.buf);
        r = -LX_ENOMEM;
        goto out;
    }
    uint64_t k = 0;
    vec[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) vec[k++] = argp[i];
    vec[k++] = 0;
    for (int i = 0; i < envc; i++) vec[k++] = envpp[i];
    vec[k++] = 0;
    for (uint64_t i = 0; i < nauxv; i++) {
        vec[k++] = auxv[i][0];
        vec[k++] = auxv[i][1];
    }
    sb_push_bytes(&sb, vec, words * 8);
    kfree(vec);

    r = copy_to_user(sb.top_user - sb.used, sb.buf + sb.size - sb.used, sb.used);
    kfree(sb.buf);
    if (r) goto out;

    *entry = start;
    *sp = rsp;
    LOG_DEBUG("exec %s: %s base 0x%llx entry 0x%llx%s%s", abs, img->eh.e_type == ET_DYN ? "PIE" : "static",
              (unsigned long long)base, (unsigned long long)prog_entry, img->interp[0] ? ", interpreter " : "",
              img->interp);
    r = 0;

out:
    kfree(img);
    ufile_put(f);
    return r;
}
