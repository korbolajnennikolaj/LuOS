// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/initfs.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "kernel/proc/linux_abi.h"
#include "kernel/proc/process.h"

#include <string.h>

#define INITFS_RAW 2
#define INITFS_ZSTD 3
#define INITFS_SYMLINK 4
#define INITFS_MAX_LINKS 40
#define INITFS_MAX_DEPTH 64
#define INITFS_PATH 1024

struct initfs_header {
    char magic[8];
    uint32_t count;
    uint32_t reserved;
    uint64_t names_off;
    uint64_t names_len;
    uint64_t size;
} __attribute__((packed));

struct initfs_entry {
    uint32_t name_off;
    uint16_t name_len;
    uint16_t type;
    uint32_t mode;
    uint32_t end;
    uint64_t size;
    uint64_t data_off;
    uint64_t data_len;
} __attribute__((packed));

extern const uint8_t __initfs_start[];
extern const uint8_t __initfs_end[];

unsigned long lkpi_zstd_workspace_size(void);
long lkpi_zstd_decompress(void *dst, unsigned long cap, const void *src, unsigned long len, void *wksp,
                          unsigned long wksp_size);

static const uint8_t *image;
static const struct initfs_header *hdr;
static const struct initfs_entry *ents;
static const char *names;
static uint8_t **cache;
static struct kmutex cache_lock;
static void *wksp;
static uint64_t wksp_size;
static uint64_t cached_bytes;

void initfs_init(void) {
    uint64_t len = (uint64_t)(__initfs_end - __initfs_start);
    if (len < sizeof(struct initfs_header)) return;
    const struct initfs_header *h = (const struct initfs_header *)__initfs_start;
    if (memcmp(h->magic, "LUOSIFS1", 8) != 0 || h->size > len || !h->count ||
        sizeof(*h) + (uint64_t)h->count * sizeof(struct initfs_entry) > h->names_off ||
        h->names_off + h->names_len > h->size) {
        LOG_WARNING("built-in filesystem: bad image (%llu bytes), ignored", (unsigned long long)len);
        return;
    }
    cache = kmalloc((size_t)h->count * sizeof(*cache));
    if (!cache) return;
    memset(cache, 0, (size_t)h->count * sizeof(*cache));
    kmutex_init(&cache_lock);
    image = __initfs_start;
    names = (const char *)image + h->names_off;
    ents = (const struct initfs_entry *)(image + sizeof(*h));
    hdr = h;
    uint32_t entries;
    uint64_t img, files, cached;
    initfs_stats(&entries, &img, &files, &cached);
    LOG_INFO("built-in filesystem: %u entries, %llu MiB of files in a %llu MiB image", entries,
             (unsigned long long)(files >> 20), (unsigned long long)(img >> 20));
}

bool initfs_present(void) {
    return hdr != NULL;
}

static int find_child(uint32_t dir, const char *name, size_t len) {
    for (uint32_t i = dir + 1; i < ents[dir].end; i = ents[i].end)
        if (ents[i].name_len == len && memcmp(names + ents[i].name_off, name, len) == 0) return (int)i;
    return -1;
}

int initfs_lookup(const char *abs, bool follow, uint32_t *idx) {
    if (!hdr || !abs || abs[0] != '/') return -LX_ENOENT;
    char work[INITFS_PATH];
    char tmp[INITFS_PATH];
    uint32_t stack[INITFS_MAX_DEPTH];
    int links = 0;
    if (strlen(abs) >= sizeof(work)) return -LX_ENAMETOOLONG;
    strcpy(work, abs);

restart:;
    int depth = 0;
    stack[0] = 0;
    const char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        const char *rest = e;
        while (*rest == '/') rest++;
        bool last = *rest == 0;
        if (len == 1 && p[0] == '.') {
            p = e;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) depth--;
            p = e;
            continue;
        }
        uint32_t cur = stack[depth];
        if (ents[cur].type != INITFS_DIR) return -LX_ENOTDIR;
        int c = find_child(cur, p, len);
        if (c < 0) return -LX_ENOENT;
        if (ents[c].type == INITFS_SYMLINK && (!last || follow)) {
            if (++links > INITFS_MAX_LINKS) return -LX_ELOOP;
            const char *target = (const char *)image + ents[c].data_off;
            size_t tlen = (size_t)ents[c].data_len;
            size_t n = 0;
            if (tlen == 0 || target[0] != '/') {
                for (int i = 1; i <= depth; i++) {
                    const struct initfs_entry *d = &ents[stack[i]];
                    if (n + d->name_len + 2 >= sizeof(tmp)) return -LX_ENAMETOOLONG;
                    tmp[n++] = '/';
                    memcpy(tmp + n, names + d->name_off, d->name_len);
                    n += d->name_len;
                }
                tmp[n++] = '/';
            }
            size_t rlen = strlen(e);
            if (n + tlen + rlen + 1 >= sizeof(tmp)) return -LX_ENAMETOOLONG;
            memcpy(tmp + n, target, tlen);
            n += tlen;
            memcpy(tmp + n, e, rlen + 1);
            memcpy(work, tmp, n + rlen + 1);
            goto restart;
        }
        if (depth + 1 >= INITFS_MAX_DEPTH) return -LX_ENAMETOOLONG;
        stack[++depth] = (uint32_t)c;
        p = e;
    }
    *idx = stack[depth];
    return 0;
}

int initfs_type(uint32_t idx) {
    switch (ents[idx].type) {
        case INITFS_DIR: return INITFS_DIR;
        case INITFS_SYMLINK: return INITFS_LINK;
        default: return INITFS_FILE;
    }
}

uint64_t initfs_size(uint32_t idx) {
    return ents[idx].type == INITFS_DIR ? 4096 : ents[idx].size;
}

uint32_t initfs_mode(uint32_t idx) {
    return ents[idx].mode;
}

uint64_t initfs_ino(uint32_t idx) {
    return 0x1F5000000000ULL + idx;
}

long initfs_readlink(uint32_t idx, char *buf, size_t cap) {
    if (ents[idx].type != INITFS_SYMLINK) return -LX_EINVAL;
    size_t n = (size_t)ents[idx].data_len;
    if (n > cap) n = cap;
    memcpy(buf, image + ents[idx].data_off, n);
    return (long)n;
}

static uint64_t cache_top = VMM_INITFS_BASE;

static void cache_unmap(uint64_t base, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        uint64_t phys = vmm_get_phys(vmm_kernel_pml4(), base + i * PAGE_SIZE) & ~(uint64_t)(PAGE_SIZE - 1);
        vmm_unmap_page(vmm_kernel_pml4(), base + i * PAGE_SIZE);
        if (phys) pmm_free_page(phys);
    }
    if (base + count * PAGE_SIZE == cache_top) cache_top = base;
}

static uint8_t *cache_map(uint64_t size, uint64_t *count_out) {
    uint64_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!count) count = 1;
    uint64_t base = cache_top;
    if (count * PAGE_SIZE > VMM_INITFS_BASE + VMM_INITFS_SIZE - base) return NULL;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys || vmm_map_page(vmm_kernel_pml4(), base + i * PAGE_SIZE, phys,
                                  VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_GLOBAL | VMM_FLAG_NX) != 0) {
            if (phys) pmm_free_page(phys);
            cache_top = base + i * PAGE_SIZE;
            cache_unmap(base, i);
            return NULL;
        }
    }
    cache_top = base + count * PAGE_SIZE + PAGE_SIZE;
    *count_out = count;
    return (uint8_t *)base;
}

const uint8_t *initfs_data(uint32_t idx) {
    const struct initfs_entry *e = &ents[idx];
    if (e->type == INITFS_RAW) return image + e->data_off;
    if (e->type != INITFS_ZSTD) return NULL;
    uint8_t *p = __atomic_load_n(&cache[idx], __ATOMIC_ACQUIRE);
    if (p) return p;
    kmutex_lock(&cache_lock);
    p = cache[idx];
    if (!p) {
        if (!wksp) {
            wksp_size = lkpi_zstd_workspace_size();
            wksp = kmalloc(wksp_size);
        }
        uint64_t count = 0;
        uint8_t *buf = wksp ? cache_map(e->size, &count) : NULL;
        if (buf) {
            long r = lkpi_zstd_decompress(buf, e->size, image + e->data_off, e->data_len, wksp, wksp_size);
            if (r == (long)e->size) {
                __atomic_store_n(&cache[idx], buf, __ATOMIC_RELEASE);
                cached_bytes += count * PAGE_SIZE;
                p = buf;
            } else {
                LOG_ERROR("built-in filesystem: entry %u does not decompress (%ld)", idx, r);
                cache_top = (uint64_t)buf + count * PAGE_SIZE;
                cache_unmap((uint64_t)buf, count);
            }
        } else {
            LOG_ERROR("built-in filesystem: no memory for entry %u (%llu bytes)", idx, (unsigned long long)e->size);
        }
    }
    kmutex_unlock(&cache_lock);
    return p;
}

bool initfs_next_child(uint32_t dir, uint32_t *cursor, char *name, size_t cap, int *type, uint32_t *child) {
    uint32_t i = *cursor ? *cursor : dir + 1;
    if (ents[dir].type != INITFS_DIR || i >= ents[dir].end) return false;
    size_t n = ents[i].name_len < cap - 1 ? ents[i].name_len : cap - 1;
    memcpy(name, names + ents[i].name_off, n);
    name[n] = 0;
    *type = initfs_type(i);
    if (child) *child = i;
    *cursor = ents[i].end;
    return true;
}

bool initfs_has_child(uint32_t dir, const char *name) {
    return ents[dir].type == INITFS_DIR && find_child(dir, name, strlen(name)) >= 0;
}

void initfs_stats(uint32_t *entries, uint64_t *image_bytes, uint64_t *file_bytes, uint64_t *cached) {
    uint64_t files = 0;
    if (hdr)
        for (uint32_t i = 0; i < hdr->count; i++)
            if (ents[i].type == INITFS_RAW || ents[i].type == INITFS_ZSTD) files += ents[i].size;
    *entries = hdr ? hdr->count : 0;
    *image_bytes = hdr ? hdr->size : 0;
    *file_bytes = files;
    *cached = cached_bytes;
}
