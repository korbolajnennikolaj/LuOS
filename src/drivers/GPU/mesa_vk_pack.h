// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef MESA_VK_PACK_H
#define MESA_VK_PACK_H

#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa_vk_uapi.h"

#include <stdint.h>
#include <string.h>

#define VKP_MAP_MAX 64
#define VKP_STRV 0x7FFF
#define VKP_MAP_AREA (8 + 8 * VKP_MAP_MAX)

struct vkp {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    uint32_t nmap;
    bool overflow;
};

static inline void vkp_init(struct vkp *p, uint8_t *buf, uint32_t cap) {
    p->buf = buf;
    p->cap = cap;
    p->len = VKP_MAP_AREA + 8;
    p->nmap = 0;
    p->overflow = false;
    memset(buf, 0, VKP_MAP_AREA + 8);
}

static inline uint32_t vkp_align8(struct vkp *p) {
    uint32_t pad = (8 - (p->len % 8)) % 8;
    if (p->len + pad > p->cap) {
        p->overflow = true;
        return 0;
    }
    memset(p->buf + p->len, 0, pad);
    p->len += pad;
    return p->len;
}

static inline void *vkp_alloc(struct vkp *p, uint16_t struct_id, uint32_t size, uint32_t *off_out) {
    vkp_align8(p);
    if (p->len + size > p->cap || p->nmap >= VKP_MAP_MAX) {
        p->overflow = true;
        return NULL;
    }
    uint32_t off = p->len;
    memset(p->buf + off, 0, size);
    p->len += size;
    uint8_t *m = p->buf + 8 + p->nmap * 8;
    uint32_t mo = off - VKP_MAP_AREA;
    memcpy(m, &mo, 4);
    m[4] = (uint8_t)(struct_id & 0xFF);
    m[5] = (uint8_t)(struct_id >> 8);
    p->nmap++;
    if (off_out) *off_out = off - VKP_MAP_AREA;
    return p->buf + off;
}

static inline uint32_t vkp_put(struct vkp *p, const void *data, uint32_t size) {
    vkp_align8(p);
    if (p->len + size > p->cap) {
        p->overflow = true;
        return 0;
    }
    uint32_t off = p->len;
    memcpy(p->buf + off, data, size);
    p->len += size;
    return off - VKP_MAP_AREA;
}

static inline uint32_t vkp_str(struct vkp *p, const char *s) {
    return vkp_put(p, s, (uint32_t)strlen(s) + 1);
}

static inline uint32_t vkp_strv(struct vkp *p, const char *const *names, uint32_t n) {
    uint64_t offs[16];
    if (n > 15) n = 15;
    for (uint32_t i = 0; i < n; i++) offs[i] = vkp_str(p, names[i]);
    offs[n] = 0;
    uint32_t off = 0;
    void *arr = vkp_alloc(p, VKP_STRV, (n + 1) * 8, &off);
    if (arr) memcpy(arr, offs, (n + 1) * 8);
    return off;
}

static inline uint32_t vkp_finish(struct vkp *p) {
    uint32_t map_bytes = 8 + 8 * p->nmap;
    if (map_bytes > VKP_MAP_AREA) return 0;
    memmove(p->buf + map_bytes, p->buf + VKP_MAP_AREA, p->len - VKP_MAP_AREA);
    uint16_t n = (uint16_t)p->nmap;
    memset(p->buf, 0, 8);
    memcpy(p->buf, &n, 2);
    return map_bytes + (p->len - VKP_MAP_AREA);
}

static inline void vkp_setptr(uint64_t *field, uint32_t off) { *field = off; }

#define VKP(T, p, id) ((T *)vkp_alloc((p), (id), sizeof(T), NULL))

#endif
