// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_sigs.h"
#include "drivers/GPU/mesa_vk_pack.h"
#include "drivers/GPU/mesa_gl_defs.h"
#include "drivers/GPU/gpu_core.h"

int gpu_slot_publish(int dev_idx, uint32_t seq, uint32_t status);
struct gpu_ring_slot *gpu_slot_by_seq(int dev_idx, uint32_t seq);

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "kernel/proc/linux_abi.h"
#include "kernel/proc/process.h"
#include "kernel/proc/syscall.h"
#include "kernel/proc/vfs.h"
#include "kernel/scheduler/scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MESA_VK_MAP_MAX 64
#define MESA_VK_DEPTH 6
#define MESA_BLOB_MAX (52 * 1024)
#define MESA_FIX_MAX 512
#define MESA_OUT_MAX 192
#define MESA_OUT_AREA_MAX 4096
#define MESA_FMASK_ARGS 16
#define MESA_BLOB_ALIGN 16

struct mesa_outslot {
    uint16_t cmd_index;
    uint16_t result_off;
    uint8_t has_ret;
    uint8_t nout;
    uint16_t out_len[2];
    uint16_t out_at[2];
};

struct mesa_frame {
    bool used;
    bool ready;
    bool sent;
    uint32_t seq;
    uint32_t slot_seq;
    int dev_idx;
    uint32_t api;
    uint32_t first_cmd;
    uint32_t cmd_count;
    uint16_t nout_total;
    uint16_t out_len[MESA_OUT_MAX];
    struct mesa_outslot slots[MESA_MAX_CALLS];
    uint8_t *data;
    uint32_t len;
};

static struct mesa_frame mesa_frames[MESA_MAX_FRAMES];
static spinlock_t mesa_lock = SPINLOCK_INIT;

static uint8_t *mesa_pix;
static volatile uint32_t mesa_pix_seq_v;
static volatile uint32_t mesa_pix_len_v;
static volatile uint32_t mesa_pix_done;

static volatile int mesa_host_pid;
static struct process *mesa_host_proc;
static spinlock_t mesa_host_proc_lock = SPINLOCK_INIT;
static volatile int mesa_host_starting;
static volatile bool mesa_attached;
static volatile bool mesa_boot_done;
static int mesa_walk_trace;

static volatile bool mesa_failed_v;
static volatile int mesa_prime_sel = MESA_PRIME_AUTO;
static int mesa_prime_resolved = MESA_PRIME_LLVM;
static volatile uint32_t mesa_prime_gen;
static volatile uint32_t mesa_host_gen;
static volatile int mesa_prime_boot_fails;
static volatile uint32_t mesa_epoch;
static char mesa_env[MESA_HOST_ENV_MAX];
static struct mesa_host_info mesa_info_v;
static uint32_t mesa_frame_seq;
static volatile int mesa_host_card;
static uint64_t mesa_boot_rets[MESA_MAX_CALLS];
static volatile uint32_t mesa_boot_nrets;

static struct {
    volatile int busy;
    volatile int done;
    volatile uint32_t seq;
    uint32_t status;
    uint32_t nrets;
    uint64_t rets[MESA_MAX_CALLS];
    uint32_t outs_len;
    uint8_t outs[MESA_DC_OUTS_MAX];
} mesa_dcr;

struct mesa_build {
    struct mesa_call calls[MESA_MAX_CALLS];
    struct mesa_call *cur;
    uint16_t calls_n;
    struct mesa_fixup fix[MESA_FIX_MAX];
    uint16_t fix_n;
    struct mesa_out outs[MESA_OUT_MAX];
    uint16_t out_n;
    uint8_t blob[MESA_BLOB_MAX];
    uint32_t blob_len;
    struct mesa_outslot *slots;
    bool overflow;
};

static uint32_t mb_align(struct mesa_build *b, uint32_t a) {
    uint32_t pad = (a - (b->blob_len % a)) % a;
    if (pad > MESA_BLOB_MAX - b->blob_len) {
        b->overflow = true;
        return 0;
    }
    memset(b->blob + b->blob_len, 0, pad);
    b->blob_len += pad;
    return b->blob_len;
}

static uint32_t mb_put(struct mesa_build *b, const void *data, uint32_t n) {
    if (n > MESA_BLOB_MAX - b->blob_len) {
        b->overflow = true;
        return 0;
    }
    uint32_t off = b->blob_len;
    if (data)
        memcpy(b->blob + off, data, n);
    else
        memset(b->blob + off, 0, n);
    b->blob_len += n;
    return off;
}

static uint32_t mb_blob8(struct mesa_build *b, const void *data, uint32_t n) {
    mb_align(b, 8);
    return mb_put(b, data, n);
}

static struct mesa_call *mb_call(struct mesa_build *b) {
    if (b->calls_n >= MESA_MAX_CALLS) {
        b->overflow = true;
        return NULL;
    }
    struct mesa_call *c = &b->calls[b->calls_n++];
    memset(c, 0, sizeof(*c));
    c->kind = MESA_CALL_FN;
    b->cur = c;
    return c;
}

static bool mb_fix(struct mesa_build *b, uint32_t where, uint32_t target, uint8_t kind) {
    if (b->fix_n >= MESA_FIX_MAX) {
        b->overflow = true;
        return false;
    }
    struct mesa_fixup *f = &b->fix[b->fix_n++];
    f->where = where;
    f->target = (target & 0x7FFFFFFFu) | ((uint32_t)(kind & 1) << 31);
    return true;
}

static uint16_t mb_out(struct mesa_build *b, uint32_t off, uint32_t len, uint16_t track) {
    if (b->out_n >= MESA_OUT_MAX) {
        b->overflow = true;
        return 0;
    }
    struct mesa_out *o = &b->outs[b->out_n];
    o->off = off;
    o->len = len;
    o->track = track;
    o->rsv = 0;
    if (b->cur) b->cur->nout++;
    return b->out_n++;
}

static bool mesa_frame_alloc(struct mesa_frame **out) {
    for (int i = 0; i < MESA_MAX_FRAMES; i++) {
        if (mesa_frames[i].used) continue;
        if (!mesa_frames[i].data) {
            mesa_frames[i].data = kmalloc(MESA_FRAME_MAX);
            if (!mesa_frames[i].data) return false;
        }
        uint8_t *data = mesa_frames[i].data;
        memset(&mesa_frames[i], 0, sizeof(mesa_frames[i]));
        mesa_frames[i].used = true;
        mesa_frames[i].data = data;
        *out = &mesa_frames[i];
        return true;
    }
    return false;
}

static void mesa_frame_free(struct mesa_frame *f) {
    f->ready = false;
    f->used = false;
}

static void mesa_frame_publish(struct mesa_frame *f) {
    uint64_t flags = spin_lock_irqsave(&mesa_lock);
    f->ready = true;
    spin_unlock_irqrestore(&mesa_lock, flags);
    vfs_poll_wake();
}

static bool mesa_compile_gl(struct mesa_build *b, uint16_t cmd_index, gpu_cmd_rec *rec,
                            const uint8_t *payload, uint32_t payload_len, uint16_t result_cap) {
    const struct mesa_gl_sig *sig = &mesa_gl_sigs[rec->opcode - GPU_CMD_GL_FIRST];
    if (rec->opcode == GPU_CMD_MESA_PIX) {
        struct mesa_call *c = mb_call(b);
        if (!c) return false;
        c->kind = MESA_CALL_PIX;
        c->argc = 1;
        c->args[0] = rec->args[0];
        b->slots[cmd_index].cmd_index = cmd_index;
        b->slots[cmd_index].result_off = 0;
        return true;
    }
    if (rec->opcode == GPU_CMD_MESA_TRACK_PIX) {
        struct mesa_call *c = mb_call(b);
        if (!c) return false;
        c->kind = MESA_CALL_TRACK_PIX;
        c->argc = 3;
        c->args[0] = rec->args[0];
        c->args[1] = rec->args[1];
        c->args[2] = rec->args[2];
        b->slots[cmd_index].cmd_index = cmd_index;
        b->slots[cmd_index].result_off = 0;
        return true;
    }
    if (rec->opcode == GPU_CMD_MESA_TRACK_POKE) {
        struct mesa_call *c = mb_call(b);
        if (!c) return false;
        uint32_t data_len = rec->payload_len;
        uint32_t blob_at = data_len ? mb_blob8(b, payload, data_len) : 0;
        c->kind = MESA_CALL_TRACK_POKE;
        c->argc = 4;
        c->args[0] = rec->args[0];
        c->args[1] = rec->args[1];
        c->args[2] = rec->args[2];
        c->args[3] = blob_at;
        c->fmask |= MESA_ACLS_PTR << 6;
        b->slots[cmd_index].cmd_index = cmd_index;
        b->slots[cmd_index].result_off = 0;
        return true;
    }
    if (!sig->name || sig->argc > MESA_FMASK_ARGS) return false;

    uint32_t n_extra = sig->argc > 6 ? (uint32_t)sig->argc - 6 : 0;
    uint32_t data_off = 8 * n_extra;
    if (data_off > payload_len) return false;

    uint32_t name_off = mb_put(b, sig->name, (uint32_t)strlen(sig->name) + 1);
    uint32_t blob_payload = 0;
    if (payload_len > data_off)
        blob_payload = mb_blob8(b, payload + data_off, payload_len - data_off);
    else
        blob_payload = mb_align(b, 8);

    struct mesa_call *c = mb_call(b);
    if (!c) return false;
    c->name_off = name_off;
    c->argc = sig->argc;
    uint16_t fix0 = b->fix_n;

    struct mesa_outslot *os = &b->slots[cmd_index];
    os->cmd_index = cmd_index;
    os->result_off = rec->result_off;
    os->has_ret = sig->has_ret;
    os->nout = 0;

    for (int i = 0; i < sig->argc; i++) {
        uint64_t val = i < 6 ? rec->args[i] : 0;
        if (i >= 6) {
            uint32_t head_off = 8 * (uint32_t)(i - 6);
            if (head_off + 8 > payload_len) return false;
            memcpy(&val, payload + head_off, 8);
        }
        if (sig->types[i] == M_T_P) {
            if (val == 0xFFFF || val == 0xFFFE) {
                c->args[i] = val;
            } else {
                if (i != sig->out_arg && i != sig->out2_arg && val >= payload_len - data_off)
                    return false;
                if (val > 0xFFFD) return false;
                c->args[i] = blob_payload + (uint32_t)val;
            }
            c->fmask |= MESA_ACLS_PTR << (2 * i);
        } else {
            c->args[i] = val;
            c->fmask |= (uint32_t)sig->types[i] << (2 * i);
        }
    }

    if (sig->out_kind == M_OUT_SRCS) {
        uint32_t count = (uint32_t)(sig->out_len_arg < 6 ? rec->args[sig->out_len_arg] : 0);
        if (count > 512) return false;
        uint32_t array_off = (uint32_t)c->args[sig->out_arg];
        mb_fix(b, array_off, count, MESA_FIX_STROFF);
    } else if (sig->out_kind != M_OUT_NONE && sig->out_kind != M_OUT_CSTR && sig->out_arg != MESA_SIG_NONE) {
        uint64_t oval = c->args[sig->out_arg];
        if (oval == 0xFFFF || oval == 0xFFFE) {
        } else {
            uint32_t out_len = 0;
            if (sig->out_kind == M_OUT_EXACT) {
                uint64_t cnt = sig->out_len_arg != MESA_SIG_NONE ? rec->args[sig->out_len_arg] : 1;
                out_len = (uint32_t)(cnt * sig->out_mul);
                if (!out_len) out_len = sig->out_mul;
            } else {
                out_len = result_cap > GPU_RESULT_HDR ? result_cap - GPU_RESULT_HDR : 4;
            }
            if (out_len > MESA_OUT_AREA_MAX) {
                LOG_WARNING("mesa: %s wants a %u-byte result, the frame carries at most %u", sig->name, out_len,
                            MESA_OUT_AREA_MAX);
                return false;
            }
            mb_align(b, 8);
            uint32_t area = mb_put(b, NULL, out_len);
            c->args[sig->out_arg] = area;
            uint16_t at = mb_out(b, area, out_len, 0);
            if (os->nout < 2) {
                os->out_len[os->nout] = (uint16_t)out_len;
                os->out_at[os->nout] = at;
                os->nout++;
            }
            if (sig->out2_arg != MESA_SIG_NONE && sig->out2_mul) {
                mb_align(b, 8);
                uint32_t area2 = mb_put(b, NULL, sig->out2_mul);
                c->args[sig->out2_arg] = area2;
                uint16_t at2 = mb_out(b, area2, sig->out2_mul, 0);
                if (os->nout < 2) {
                    os->out_len[os->nout] = sig->out2_mul;
                    os->out_at[os->nout] = at2;
                    os->nout++;
                }
            }
        }
    } else if (sig->out_kind == M_OUT_CSTR) {
        mb_align(b, 8);
        uint32_t area = mb_put(b, NULL, 256);
        c->rsv = area | 0x80000000u;
        uint16_t at = mb_out(b, area, 256, 0);
        os->out_len[os->nout] = 255;
        os->out_at[os->nout] = at;
        os->nout++;
    }
    c->nfix = (uint16_t)(b->fix_n - fix0);
    if (0) {
        os->has_ret = 0;
    }
    return true;
}

static int mesa_vk_map_find(const uint8_t *payload, uint32_t payload_len, uint32_t nmap,
                            uint32_t data_off, uint32_t val) {
    for (uint32_t i = 0; i < nmap && i < MESA_VK_MAP_MAX; i++) {
        uint32_t p = 8 + i * 8;
        if (p + 8 > payload_len) break;
        uint32_t off;
        memcpy(&off, payload + p, 4);
        if (off == val) return (int)payload[p + 4] | ((int)payload[p + 5] << 8);
    }
    (void)data_off;
    return -1;
}

static bool mesa_vk_walk(struct mesa_build *b, const uint8_t *payload, uint32_t payload_len,
                         uint32_t nmap, uint32_t base, uint32_t struct_off, uint16_t id,
                         int depth) {
    if (depth > MESA_VK_DEPTH || id == 0 || id >= VK_S_COUNT) return true;
    uint32_t data_len = payload_len > 8 + nmap * 8 ? payload_len - (8 + nmap * 8) : 0;
    const struct mesa_vk_struct *st = &mesa_vk_structs[id];
    for (uint16_t i = 0; i < st->nfld; i++) {
        const struct mesa_vk_fld *f = &st->flds[i];
        if (f->kind == M_VF_SCALAR) continue;
        if (f->kind == M_VF_INLINE) {
            if (!mesa_vk_walk(b, payload, payload_len, nmap, base, struct_off + f->off,
                              f->strct, depth + 1))
                return false;
            continue;
        }
        uint32_t src = base + struct_off + f->off;
        if (src + 8 > b->blob_len) continue;
        uint64_t val = 0;
        memcpy(&val, b->blob + src, 8);
        if (!val || val > 0xFFFF) continue;
        if (val >= data_len) return false;
        mb_fix(b, base + struct_off + f->off, base + (uint32_t)val, MESA_FIX_BLOB);
        int sub = mesa_vk_map_find(payload, payload_len, nmap, 0, (uint32_t)val);
        if (sub == VKP_STRV) {
            for (uint32_t at = base + (uint32_t)val; at + 8 <= b->blob_len; at += 8) {
                uint64_t s = 0;
                memcpy(&s, b->blob + at, 8);
                if (!s || s > 0xFFFF) break;
                mb_fix(b, at, base + (uint32_t)s, MESA_FIX_BLOB);
            }
            continue;
        }
        uint32_t elem = (uint32_t)val;
        while (sub > 0) {
            if (!mesa_vk_walk(b, payload, payload_len, nmap, base, elem,
                              (uint16_t)sub, depth + 1))
                return false;
            uint32_t step = mesa_vk_structs[sub].size;
            if (step == 0 || step > 4096) break;
            uint32_t next = elem + step;
            if (next > 0xFFFF) break;
            int nsub = mesa_vk_map_find(payload, payload_len, nmap, 0, next);
            if (nsub != sub) break;
            elem = next;
        }
    }
    return true;
}

static bool mesa_compile_vk(struct mesa_build *b, uint16_t cmd_index, gpu_cmd_rec *rec,
                            const uint8_t *payload, uint32_t payload_len, uint16_t result_cap) {
    const struct mesa_vk_sig *sig = &mesa_vk_sigs[rec->opcode - GPU_CMD_VK_FIRST];
    if (!sig->argc || sig->argc > MESA_FMASK_ARGS) return false;


    uint32_t nmap = 0;
    if (payload_len >= 4) memcpy(&nmap, payload, 4);
    if (nmap > MESA_VK_MAP_MAX) return false;
    uint32_t data_off = 8 + nmap * 8;
    if (payload_len == 0) {
        data_off = 0;
    } else if (data_off > payload_len) {
        return false;
    }

    const char *fn = gpu_cmd_name(GPU_API_VULKAN, rec->opcode);
    if (!fn) return false;
    uint32_t name_off = mb_put(b, fn, (uint32_t)strlen(fn) + 1);
    uint32_t vk_data_base = 0;
    if (payload_len > data_off)
        vk_data_base = mb_blob8(b, payload + data_off, payload_len - data_off);
    else
        vk_data_base = mb_align(b, 8);

    struct mesa_call *c = mb_call(b);
    if (!c) return false;
    c->name_off = name_off;
    c->argc = sig->argc;
    uint16_t fix0 = b->fix_n;

    struct mesa_outslot *os = &b->slots[cmd_index];
    os->cmd_index = cmd_index;
    os->result_off = rec->result_off;
    os->has_ret = sig->has_ret;
    os->nout = 0;
    int count_area_arg = -1;
    uint32_t count_area = 0;

    for (int i = 0; i < sig->argc && i < (int)(sizeof(sig->a) / sizeof(sig->a[0])); i++) {
        const struct mesa_vk_arg *a = &sig->a[i];
        uint64_t val = 0;
        if (i >= (int)rec->argc) {
            uint8_t bk = a->kind & 0xF;
            val = (bk == M_VA_SCALAR || bk == M_VA_HANDLE || bk == M_VA_POUT_STRUCT ||
                   bk == M_VA_POUT_HANDLE || bk == M_VA_PSCALAR_OUT)
                      ? 0
                      : 0xFFFE;
        } else {
            val = rec->args[i];
        }
        uint8_t base_kind = a->kind & 0xF;
        if (base_kind == M_VA_SCALAR) {
            c->args[i] = val;
            c->fmask |= (a->kind & 0x30) >> 4 << (2 * i);
        } else if (base_kind == M_VA_HANDLE) {
            c->args[i] = val;
        } else if (base_kind == M_VA_PSTRUCT || base_kind == M_VA_PSTRUCT_ARR ||
                   base_kind == M_VA_PSCALAR_IN || base_kind == M_VA_PSTR ||
                   base_kind == M_VA_POPEN) {
            if (val == 0xFFFE || val == 0xFFFF) {
                c->args[i] = val;
                c->fmask |= MESA_ACLS_PTR << (2 * i);
            } else {
            if (val > 0xFFFF) return false;
            if (val >= payload_len - data_off || payload_len <= data_off) return false;
            c->args[i] = vk_data_base + (uint32_t)val;
            c->fmask |= MESA_ACLS_PTR << (2 * i);
            }
        } else if (base_kind == M_VA_POUT_HANDLE || base_kind == M_VA_POUT_STRUCT ||
                   base_kind == M_VA_PSCALAR_OUT) {
            if (val == 0xFFFE || val == 0xFFFF) {
                c->args[i] = val;
                c->fmask |= MESA_ACLS_PTR << (2 * i);
                continue;
            }
            uint32_t out_len = 8;
            uint32_t elem = 0;
            if (base_kind == M_VA_POUT_STRUCT && a->strct)
                out_len = elem = mesa_vk_structs[a->strct].size;
            else if (base_kind == M_VA_POUT_HANDLE && a->count_arg != MESA_SIG_NONE) {
                uint64_t cnt = a->count_arg < rec->argc ? rec->args[a->count_arg] : 1;
                out_len = (uint32_t)(cnt ? cnt : 1) * 8;
            } else if (base_kind == M_VA_POUT_HANDLE) {
                elem = 8;
            } else if (base_kind == M_VA_PSCALAR_OUT) {
                out_len = 4;
            }
            if (elem && a->count_arg == MESA_SIG_NONE && count_area_arg == i - 1) {
                uint32_t n = 0;
                memcpy(&n, b->blob + count_area, 4);
                uint32_t fit = MESA_OUT_AREA_MAX / elem;
                if (n > fit) {
                    n = fit;
                    memcpy(b->blob + count_area, &n, 4);
                }
                out_len = elem * (n ? n : 1);
            }
            if (out_len > MESA_OUT_AREA_MAX) {
                LOG_WARNING("mesa: %s wants a %u-byte result, the frame carries at most %u", fn, out_len,
                            MESA_OUT_AREA_MAX);
                return false;
            }
            mb_align(b, 8);
            uint32_t area = mb_put(b, NULL, out_len);
            if (val != 0xFFFF && val != 0xFFFE && payload_len > data_off + (uint32_t)val) {
                uint32_t seed = payload_len - data_off - (uint32_t)val;
                if (seed > out_len) seed = out_len;
                if (seed) memcpy(b->blob + area, payload + data_off + (uint32_t)val, seed);
            }
            c->args[i] = area;
            c->fmask |= MESA_ACLS_PTR << (2 * i);
            if (base_kind == M_VA_PSCALAR_OUT) {
                count_area_arg = i;
                count_area = area;
            }
            uint16_t track = 0xFFFF;
            if (rec->opcode == GPU_CMD_VK_MAP_MEMORY && i == 5) track = 0;
            uint16_t at = mb_out(b, area, out_len, track);
            if (os->nout < 2) {
                os->out_len[os->nout] = (uint16_t)out_len;
                os->out_at[os->nout] = at;
                os->nout++;
            }
            continue;
        }
        if (val == 0xFFFE || val == 0xFFFF) {
        } else if (base_kind == M_VA_PSTRUCT && a->strct) {
            if (!mesa_vk_walk(b, payload, payload_len, nmap, vk_data_base, (uint32_t)val,
                              a->strct, 0))
                return false;
        } else if (base_kind == M_VA_PSTRUCT_ARR && a->strct) {
            uint32_t count = 1;
            if (a->count_arg != MESA_SIG_NONE && a->count_arg < rec->argc)
                count = (uint32_t)rec->args[a->count_arg];
            if (count > 64) count = 64;
            uint32_t step = mesa_vk_structs[a->strct].size;
            for (uint32_t e = 0; e < count; e++) {
                if (!mesa_vk_walk(b, payload, payload_len, nmap, vk_data_base,
                                  (uint32_t)val + e * step, a->strct, 0))
                    return false;
            }
        }
    }
    if (rec->result_cap && os->nout == 0) {
    }
    c->nfix = (uint16_t)(b->fix_n - fix0);
    return true;
}

static uint32_t mesa_finalize(struct mesa_build *b, uint32_t seq, struct mesa_frame *f,
                              uint32_t slot_seq, int dev_idx, uint32_t api,
                              uint32_t first_cmd, uint32_t cmd_count) {
    uint32_t blob_at = sizeof(struct mesa_frame_hdr) + (uint32_t)b->calls_n * sizeof(struct mesa_call) +
                       (uint32_t)b->fix_n * sizeof(struct mesa_fixup) + (uint32_t)b->out_n * sizeof(struct mesa_out);
    blob_at = (blob_at + MESA_BLOB_ALIGN - 1) & ~(uint32_t)(MESA_BLOB_ALIGN - 1);
    uint32_t len = blob_at + b->blob_len;
    if (len > MESA_FRAME_MAX || b->overflow) return 0;
    uint8_t *buf = f->data;
    struct mesa_frame_hdr *hdr = (struct mesa_frame_hdr *)buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = MESA_FRAME_MAGIC;
    hdr->seq = seq;
    hdr->calls = b->calls_n;
    uint32_t off = sizeof(struct mesa_frame_hdr);
    memcpy(buf + off, b->calls, (size_t)b->calls_n * sizeof(struct mesa_call));
    off += (uint32_t)b->calls_n * sizeof(struct mesa_call);
    hdr->fix_off = (uint16_t)off;
    memcpy(buf + off, b->fix, (size_t)b->fix_n * sizeof(struct mesa_fixup));
    off += (uint32_t)b->fix_n * sizeof(struct mesa_fixup);
    hdr->out_off = (uint16_t)off;
    memcpy(buf + off, b->outs, (size_t)b->out_n * sizeof(struct mesa_out));
    off += (uint32_t)b->out_n * sizeof(struct mesa_out);
    memset(buf + off, 0, blob_at - off);
    off = blob_at;
    hdr->blob_off = off;
    if (b->blob_len) memcpy(buf + off, b->blob, b->blob_len);
    hdr->frame_len = len;
    hdr->pix_seq = mesa_pix_seq_v;
    hdr->pix_len = mesa_pix_len_v;
    hdr->slot_seq = slot_seq;
    hdr->device = (uint32_t)dev_idx;
    hdr->api = api;
    hdr->first_cmd = first_cmd;
    hdr->cmd_count = cmd_count;
    f->len = len;
    f->slot_seq = slot_seq;
    f->dev_idx = dev_idx;
    f->api = api;
    f->first_cmd = first_cmd;
    f->cmd_count = cmd_count;
    f->seq = seq;
    f->nout_total = b->out_n;
    for (int i = 0; i < b->out_n && i < MESA_OUT_MAX; i++) f->out_len[i] = (uint16_t)b->outs[i].len;
    memcpy(f->slots, b->slots, sizeof(f->slots));
    return len;
}

static bool mesa_compile_slot(struct gpu_ring_slot *slot, int dev_idx) {
    struct mesa_build *b = kmalloc(sizeof(*b));
    if (!b) return false;
    memset(b, 0, sizeof(*b));
    b->slots = kmalloc(sizeof(struct mesa_outslot) * GPU_BATCH_MAX_CMDS);
    if (!b->slots) {
        kfree(b);
        return false;
    }
    memset(b->slots, 0, sizeof(struct mesa_outslot) * GPU_BATCH_MAX_CMDS);

    gpu_batch_hdr *hdr = (gpu_batch_hdr *)slot->data;
    bool ok = true;
    uint32_t compiled = 0;
    for (uint16_t i = 0; i < hdr->cmd_count && i < GPU_BATCH_MAX_CMDS; i++) {
        gpu_cmd_rec *rec = (gpu_cmd_rec *)&slot->data[GPU_BATCH_REC_OFF + (size_t)i * sizeof(gpu_cmd_rec)];
        if (rec->opcode >= GPU_CMD_GL_FIRST && rec->opcode <= GPU_CMD_GL_LAST) {
            const uint8_t *pl = rec->payload_len ? &slot->data[GPU_BATCH_PAYLOAD_OFF + rec->payload_off] : NULL;
            if (!mesa_compile_gl(b, i, rec, pl, rec->payload_len, rec->result_cap)) {
                const char *fname = (rec->opcode - GPU_CMD_GL_FIRST >= 0 &&
                                     rec->opcode - GPU_CMD_GL_FIRST < GPU_CMD_GL_COUNT) ?
                                    mesa_gl_sigs[rec->opcode - GPU_CMD_GL_FIRST].name : "?";
                LOG_WARNING("mesa: compile failed: op 0x%x (%s) argc=%u cap=%u plen=%u",
                            rec->opcode, fname ? fname : "?", rec->argc, rec->result_cap, rec->payload_len);
                ok = false;
                break;
            }
            compiled++;
        } else if (rec->opcode >= GPU_CMD_VK_FIRST && rec->opcode <= GPU_CMD_VK_LAST) {
            const uint8_t *pl = rec->payload_len ? &slot->data[GPU_BATCH_PAYLOAD_OFF + rec->payload_off] : NULL;
            if (!mesa_compile_vk(b, i, rec, pl, rec->payload_len, rec->result_cap)) {
                LOG_WARNING("mesa: vk compile failed: op 0x%x argc=%u cap=%u plen=%u",
                            rec->opcode, rec->argc, rec->result_cap, rec->payload_len);
                ok = false;
                break;
            }
            compiled++;
        }
    }
    bool made = false;
    if (!ok) {
        kfree(b->slots);
        kfree(b);
        gpu_slot_publish(dev_idx, slot->seq, GPU_ERR_PARAM);
        return false;
    }
    if (ok && compiled) {
        struct mesa_frame *f = NULL;
        uint64_t flags = spin_lock_irqsave(&mesa_lock);
        bool alloc = mesa_frame_alloc(&f);
        spin_unlock_irqrestore(&mesa_lock, flags);
        if (alloc) {
            uint32_t seq = ++mesa_frame_seq;
            bool wants_pix = false;
            uint32_t pix_len = 0;
            for (uint16_t i = 0; i < hdr->cmd_count && i < GPU_BATCH_MAX_CMDS; i++) {
                gpu_cmd_rec *rec = (gpu_cmd_rec *)&slot->data[GPU_BATCH_REC_OFF + (size_t)i * sizeof(gpu_cmd_rec)];
                if (rec->opcode == GPU_CMD_MESA_PIX || rec->opcode == GPU_CMD_MESA_TRACK_PIX) {
                    wants_pix = true;
                    pix_len = (uint32_t)rec->args[rec->opcode == GPU_CMD_MESA_PIX ? 0 : 2];
                }
            }
            if (wants_pix) {
                mesa_pix_seq_v++;
                mesa_pix_len_v = pix_len > MESA_PIX_MAX ? MESA_PIX_MAX : pix_len;
                mesa_pix_done = 0;
            }
            if (mesa_finalize(b, seq, f, slot->seq, dev_idx, slot->api, 0, compiled)) {
                mesa_frame_publish(f);
                made = true;
            } else {
                if (wants_pix) {
                    mesa_pix_seq_v--;
                    mesa_pix_len_v = 0;
                    mesa_pix_done = 0;
                }
                mesa_frame_free(f);
            }
        }
        if (!made) {
            LOG_WARNING("mesa: batch %u not sent (%s)", slot->seq, alloc ? "frame too large" : "all frames busy");
            gpu_slot_publish(dev_idx, slot->seq, alloc ? GPU_ERR_OVERFLOW : GPU_ERR_BUSY);
        }
    }
    kfree(b->slots);
    kfree(b);
    return made;
}

struct mesa_boot_ctx {
    struct mesa_build b;
    struct mesa_outslot slots[MESA_MAX_CALLS];
};

static bool mesa_bcall(struct mesa_build *b, const char *name, uint8_t argc, const uint64_t *args,
                       const uint8_t *classes, uint32_t rsv) {
    uint32_t name_off = mb_put(b, name, (uint32_t)strlen(name) + 1);
    struct mesa_call *c = mb_call(b);
    if (!c) return false;
    c->name_off = name_off;
    c->argc = argc;
    c->rsv = rsv;
    for (int i = 0; i < argc; i++) {
        c->args[i] = args[i];
        c->fmask |= (uint32_t)classes[i] << (2 * i);
    }
    return true;
}

static uint32_t mesa_bout(struct mesa_build *b, uint32_t len, uint16_t track) {
    mb_align(b, 8);
    uint32_t area = mb_put(b, NULL, len);
    mb_out(b, area, len, track);
    return area;
}

static bool mesa_emit_bootstrap(struct mesa_build *b) {
    uint64_t a[5];
    uint32_t rsv;
    uint32_t dpy_ret;

    if (mesa_prime_resolved == MESA_PRIME_LLVM) {
        a[0] = 0x31DD; a[1] = 0; a[2] = 0xFFFE;
        if (!mesa_bcall(b, "eglGetPlatformDisplay", 3, a,
                        (const uint8_t[]){MESA_ACLS_INT, MESA_ACLS_INT, MESA_ACLS_PTR}, 0))
            return false;
        dpy_ret = 0;
    } else {
        char node[24] = "";
        if (mesa_prime_resolved >= 1) {
            struct gpu_device *pd = gpu_get_device(mesa_prime_resolved - 1);
            if (pd && pd->render[0])
                snprintf(node, sizeof(node), "/dev/dri/%s", pd->render);
            else if (pd && pd->card[0])
                snprintf(node, sizeof(node), "/dev/dri/%s", pd->card);
        }
        if (!node[0]) snprintf(node, sizeof(node), "/dev/dri/renderD128");
        uint32_t path_off = mb_put(b, node, (uint32_t)strlen(node) + 1);
        a[0] = path_off; a[1] = 0x80002;
        if (!mesa_bcall(b, "open", 2, a, (const uint8_t[]){MESA_ACLS_PTR, MESA_ACLS_INT}, 0))
            return false;
        a[0] = 0; a[1] = 0;
        if (!mesa_bcall(b, "gbm_create_device", 1, a,
                        (const uint8_t[]){MESA_ACLS_INT}, 1u << 0))
            return false;
        a[0] = 0x31D7; a[1] = 1; a[2] = 0xFFFE;
        if (!mesa_bcall(b, "eglGetPlatformDisplay", 3, a,
                        (const uint8_t[]){MESA_ACLS_INT, MESA_ACLS_INT, MESA_ACLS_PTR}, 1u << 1))
            return false;
        dpy_ret = 2;
    }

    uint32_t maj = mesa_bout(b, 4, 1);
    uint32_t min = mesa_bout(b, 4, 2);
    a[0] = dpy_ret; a[1] = maj; a[2] = min;
    if (!mesa_bcall(b, "eglInitialize", 3, a,
                    (const uint8_t[]){MESA_ACLS_INT, MESA_ACLS_PTR, MESA_ACLS_PTR}, 1u << 0))
        return false;

    a[0] = 0x30A2;
    if (!mesa_bcall(b, "eglBindAPI", 1, a, (const uint8_t[]){MESA_ACLS_INT}, 0))
        return false;

    static const uint32_t attribs[] = {0x3038};
    uint32_t attr_off = mb_blob8(b, attribs, sizeof(attribs));
    a[0] = dpy_ret; a[1] = 0; a[2] = 0; a[3] = attr_off;
    if (!mesa_bcall(b, "eglCreateContext", 4, a,
                    (const uint8_t[]){MESA_ACLS_INT, MESA_ACLS_INT, MESA_ACLS_INT, MESA_ACLS_PTR},
                    1u << 0))
        return false;
    uint32_t ctx_ret = b->calls_n - 1;

    a[0] = dpy_ret; a[1] = 0; a[2] = 0; a[3] = ctx_ret;
    if (!mesa_bcall(b, "eglMakeCurrent", 4, a,
                    (const uint8_t[]){MESA_ACLS_INT, MESA_ACLS_INT, MESA_ACLS_INT, MESA_ACLS_INT},
                    (1u << 0) | (1u << 3)))
        return false;

    mb_align(b, 8);
    uint32_t vend_area = mb_put(b, NULL, 256);
    mb_out(b, vend_area, 256, 0);
    a[0] = 0x1F00;
    rsv = vend_area | 0x80000000u;
    if (!mesa_bcall(b, "glGetString", 1, a, (const uint8_t[]){MESA_ACLS_INT}, rsv))
        return false;

    mb_align(b, 8);
    uint32_t rend_area = mb_put(b, NULL, 256);
    mb_out(b, rend_area, 256, 0);
    a[0] = 0x1F01;
    rsv = rend_area | 0x80000000u;
    if (!mesa_bcall(b, "glGetString", 1, a, (const uint8_t[]){MESA_ACLS_INT}, rsv))
        return false;
    return true;
}

static void mesa_bootstrap_push(void) {
    struct mesa_boot_ctx *ctx = kmalloc(sizeof(*ctx));
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->b.slots = ctx->slots;
    if (!mesa_emit_bootstrap(&ctx->b)) {
        kfree(ctx);
        return;
    }
    struct mesa_frame *f = NULL;
    uint64_t flags = spin_lock_irqsave(&mesa_lock);
    bool alloc = mesa_frame_alloc(&f);
    spin_unlock_irqrestore(&mesa_lock, flags);
    if (!alloc) {
        kfree(ctx);
        return;
    }
    uint32_t seq = ++mesa_frame_seq;
    if (!mesa_finalize(&ctx->b, seq, f, 0xFFFFFFFEu, -1, 0, 0, ctx->b.calls_n)) {
        mesa_frame_free(f);
        kfree(ctx);
        return;
    }
    mesa_frame_publish(f);
    kfree(ctx);
    LOG_INFO("mesa: bootstrap frame queued (%d calls)", (int)f->cmd_count);
}

#define MESA_ICD(name) "/usr/share/vulkan/icd.d/" name "_icd.x86_64.json:/usr/share/vulkan/icd.d/" name "_icd.json:"
#define MESA_ICD_LVP "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json:/usr/share/vulkan/icd.d/lvp_icd.json"

static const char *mesa_vk_icds(void) {
    static const struct {
        const char *driver;
        const char *icds;
    } map[] = {
        {"amdgpu", MESA_ICD("radeon") MESA_ICD_LVP},
        {"i915", MESA_ICD("intel") MESA_ICD("intel_hasvk") MESA_ICD_LVP},
        {"xe", MESA_ICD("intel") MESA_ICD_LVP},
        {"nouveau", MESA_ICD("nouveau") MESA_ICD_LVP},
        {"virtio_gpu", MESA_ICD("virtio") MESA_ICD_LVP},
    };
    struct gpu_device *d = mesa_prime_resolved > 0 ? gpu_get_device(mesa_prime_resolved - 1) : NULL;
    for (size_t i = 0; d && i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(d->driver, map[i].driver) == 0) return map[i].icds;
    return MESA_ICD("radeon") MESA_ICD("intel") MESA_ICD("intel_hasvk") MESA_ICD("nouveau") MESA_ICD("virtio")
        MESA_ICD_LVP;
}

static void mesa_host_task(void *arg) {
    (void)arg;
    const char *argv[2] = {MESA_HOST_BIN, NULL};
    const char *env[12];
    int e = 0;
    static char env_buf[MESA_HOST_ENV_MAX * 2];
    int n = 0;
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "PATH=/usr/bin:/bin");
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "HOME=/");
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "TERM=linux");
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "MESA_SHADER_CACHE_DISABLE=true");
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    if (mesa_prime_resolved == MESA_PRIME_LLVM) {
        n += snprintf(env_buf + n, sizeof(env_buf) - n,
                      "MESA_LOADER_DRIVER_OVERRIDE=llvmpipe");
        n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
        n += snprintf(env_buf + n, sizeof(env_buf) - n,
                      "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json:/usr/share/vulkan/icd.d/lvp_icd.json");
        n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
        n += snprintf(env_buf + n, sizeof(env_buf) - n, "GALLIUM_DRIVER=llvmpipe");
        n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    } else {
        n += snprintf(env_buf + n, sizeof(env_buf) - n, "VK_DRIVER_FILES=%s", mesa_vk_icds());
        n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    }
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "LIBGL_ALWAYS_SOFTWARE=%d",
                  mesa_prime_resolved == MESA_PRIME_LLVM ? 1 : 0);
    n += snprintf(env_buf + n, sizeof(env_buf) - n, "%c", 0);
    for (int i = 0; i < n; i++) {
        if (env_buf[i] == 1) env_buf[i] = 0;
    }
    int pos = 0;
    while (pos < n && e < 11) {
        env[e++] = &env_buf[pos];
        pos += (int)strlen(&env_buf[pos]) + 1;
    }
    env[e] = NULL;
    strncpy(mesa_env, mesa_prime_resolved == MESA_PRIME_LLVM ? "llvmpipe (LLVM)" : "card GBM",
            sizeof(mesa_env) - 1);

    struct process *p = NULL;
    mesa_host_gen = mesa_prime_gen;
    mesa_host_card = mesa_prime_resolved;
    mesa_boot_nrets = 0;
    int pid = proc_spawn(MESA_HOST_BIN, 1, argv, e, env, &p);
    if (pid < 0) {
        LOG_WARNING("mesa: host spawn failed (%d) - GL/Vulkan batches queue up", pid);
        mesa_failed_v = true;
        mesa_host_starting = 0;
        return;
    }
    uint64_t pflags = spin_lock_irqsave(&mesa_host_proc_lock);
    mesa_host_proc = p;
    spin_unlock_irqrestore(&mesa_host_proc_lock, pflags);
    mesa_host_pid = pid;
    mesa_host_starting = 0;
    LOG_INFO("mesa: host process pid %d (%s)", pid, mesa_env);
    int status = 0;
    proc_wait(p, &status);
    pflags = spin_lock_irqsave(&mesa_host_proc_lock);
    mesa_host_proc = NULL;
    spin_unlock_irqrestore(&mesa_host_proc_lock, pflags);
    proc_put(p);
    bool booted = mesa_boot_done;
    mesa_host_pid = 0;
    mesa_attached = false;
    mesa_boot_done = false;
    mesa_epoch++;

    for (int i = 0; i < MESA_MAX_FRAMES; i++) {
        struct mesa_frame *f = &mesa_frames[i];
        int fdev = -1;
        uint32_t fslot = 0;
        uint64_t flags = spin_lock_irqsave(&mesa_lock);
        if (f->used) {
            fdev = f->dev_idx;
            fslot = f->slot_seq;
            f->used = false;
            f->ready = false;
        }
        spin_unlock_irqrestore(&mesa_lock, flags);
        if (fdev != -1 && fslot != 0xFFFFFFFEu && fslot != MESA_DIRECT_SEQ)
            gpu_slot_publish(fdev, fslot, GPU_ERR_NO_TRANSLATOR);
    }
    mesa_boot_nrets = 0;
    if (mesa_dcr.seq) {
        mesa_dcr.status = 1;
        mesa_dcr.nrets = 0;
        mesa_dcr.outs_len = 0;
        __atomic_store_n(&mesa_dcr.done, 1, __ATOMIC_RELEASE);
        scheduler_wake_all((void *)&mesa_dcr.done);
    }

    if (!booted) {
        if (mesa_prime_resolved != MESA_PRIME_LLVM) {
            int fails = ++mesa_prime_boot_fails;
            if (fails >= 2) {
                LOG_WARNING("mesa: card PRIME host died before boot %d time(s) - falling back to llvmpipe",
                            fails);
                mesa_prime_resolved = MESA_PRIME_LLVM;
                mesa_prime_gen++;
                mesa_prime_boot_fails = 0;
            } else {
                LOG_WARNING("mesa: card PRIME host died before boot (status %d), retry on next use", status);
            }
        }
        LOG_WARNING("mesa: host process exited before boot (status %d)", status);
    } else {
        mesa_prime_boot_fails = 0;
        LOG_WARNING("mesa: host process exited (status %d)", status);
    }
}

int mesa_start_async(void) {
    if (mesa_host_pid || mesa_failed_v || mesa_host_starting)
        return mesa_host_pid || mesa_host_starting ? GPU_ERR_BUSY : GPU_ERR_NO_TRANSLATOR;
    mesa_host_starting = 1;
    struct task *t = task_create_ex("mesa-host", mesa_host_task, NULL, 2, -1, 64 * 1024);
    if (!t) {
        mesa_host_starting = 0;
        return GPU_ERR_NOMEM;
    }
    return GPU_OK;
}

static int mesa_ensure_prime(void) {
    int before = mesa_prime_resolved;
    if (mesa_prime_sel == MESA_PRIME_AUTO) {
        mesa_prime_resolved = MESA_PRIME_LLVM;
        gpu_core_refresh();
        for (int i = 0; i < gpu_get_device_count(); i++) {
            struct gpu_device *d = gpu_get_device(i);
            if (d && (d->flags & GPU_DEV_FLAG_BOUND) && strcmp(d->driver, "simpledrm") != 0) {
                mesa_prime_resolved = i + 1;
                break;
            }
        }
    } else if (mesa_prime_sel > 0) {
        int slot = 0;
        mesa_prime_resolved = MESA_PRIME_LLVM;
        gpu_core_refresh();
        for (int i = 0; i < gpu_get_device_count(); i++) {
            struct gpu_device *d = gpu_get_device(i);
            if (!d || !(d->flags & GPU_DEV_FLAG_BOUND) || strcmp(d->driver, "simpledrm") == 0) continue;
            if (++slot == mesa_prime_sel) {
                mesa_prime_resolved = i + 1;
                break;
            }
        }
        if (mesa_prime_resolved == MESA_PRIME_LLVM)
            LOG_WARNING("mesa: PRIME slot %d no longer matches a bound card - using llvmpipe",
                        mesa_prime_sel);
    } else {
        mesa_prime_resolved = mesa_prime_sel;
    }
    if (mesa_prime_resolved != before) {
        mesa_prime_gen++;
        LOG_INFO("mesa: PRIME now %s (gen %u)",
                 mesa_prime_resolved == MESA_PRIME_LLVM ? "llvmpipe" : "card", mesa_prime_gen);
    }
    return mesa_prime_resolved;
}

#define MESA_HOST_STALE_GRACE_MS 1500
#define MESA_HOST_STALE_KILL_MS 10000

static void mesa_host_kill(void) {
    uint64_t flags = spin_lock_irqsave(&mesa_host_proc_lock);
    struct process *p = mesa_host_proc;
    if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&mesa_host_proc_lock, flags);
    if (!p) return;
    proc_kill(p, PROC_EXIT_SIGNAL | 9);
    proc_put(p);
}

int mesa_start(void) {
    if (mesa_host_pid) {
        if (mesa_host_gen == mesa_prime_gen) return GPU_OK;
        uint64_t tw = sys_monotonic_ns();
        bool killed = false;
        while (mesa_host_pid) {
            uint64_t waited = (sys_monotonic_ns() - tw) / 1000000ULL;
            if (!killed && waited >= MESA_HOST_STALE_GRACE_MS) {
                LOG_INFO("mesa: host pid %d belongs to PRIME gen %u (now %u) - restarting it",
                         mesa_host_pid, mesa_host_gen, mesa_prime_gen);
                mesa_host_kill();
                killed = true;
            }
            if (waited >= MESA_HOST_STALE_KILL_MS) break;
            scheduler_sleep_ms(20);
        }
        if (mesa_host_pid) {
            LOG_WARNING("mesa: stale host pid %d did not exit", mesa_host_pid);
            return GPU_ERR_BUSY;
        }
    }
    mesa_failed_v = false;
    mesa_ensure_prime();
    int r = mesa_start_async();
    if (r) return r;
    uint64_t t0 = sys_monotonic_ns();
    while (!mesa_attached && !mesa_failed_v && (mesa_host_pid || mesa_host_starting)) {
        scheduler_sleep_ms(20);
        if ((sys_monotonic_ns() - t0) / 1000000ULL > MESA_HOST_BOOT_MS) return GPU_ERR_TIMEOUT;
    }
    if (!mesa_attached) return mesa_failed_v ? GPU_ERR_NO_TRANSLATOR : GPU_ERR_TIMEOUT;
    t0 = sys_monotonic_ns();
    while (!mesa_boot_done && mesa_host_pid) {
        scheduler_sleep_ms(20);
        if ((sys_monotonic_ns() - t0) / 1000000ULL > MESA_HOST_BOOT_MS) return GPU_ERR_TIMEOUT;
    }
    return mesa_boot_done ? GPU_OK : GPU_ERR_TIMEOUT;
}

bool mesa_ready(void) { return mesa_host_pid != 0 && mesa_boot_done; }
bool mesa_failed(void) { return mesa_failed_v; }
bool mesa_boot_ok(void) { return mesa_boot_done; }
const char *mesa_renderer(void) { return mesa_info_v.renderer; }

void mesa_prime_refresh(void) { mesa_ensure_prime(); }

int mesa_prime_count(void) {
    gpu_core_refresh();
    int cards = 0;
    for (int i = 0; i < gpu_get_device_count(); i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (d && (d->flags & GPU_DEV_FLAG_BOUND) && strcmp(d->driver, "simpledrm") != 0) cards++;
    }
    return 1 + cards;
}

const char *mesa_prime_card(int idx) {
    if (idx == 0) return "llvm (Mesa llvmpipe, CPU)";
    gpu_core_refresh();
    int n = 0;
    for (int i = 0; i < gpu_get_device_count(); i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (!d || !(d->flags & GPU_DEV_FLAG_BOUND) || strcmp(d->driver, "simpledrm") == 0) continue;
        if (++n == idx) return d->drm_name[0] ? d->drm_name : d->driver;
    }
    return "?";
}

int mesa_prime_set(int sel) {
    if (sel < -1 || sel >= mesa_prime_count()) return GPU_ERR_PARAM;
    if (sel == mesa_prime_sel) return GPU_OK;
    mesa_prime_sel = sel;
    mesa_ensure_prime();
    return GPU_OK;
}

int mesa_prime_get(void) { return mesa_prime_sel; }
const char *mesa_prime_name(int sel) {
    if (sel == MESA_PRIME_AUTO) return "auto";
    if (sel == MESA_PRIME_LLVM) return "llvm";
    return mesa_prime_card(sel);
}

int mesa_env_get(char *buf, int cap) {
    return snprintf(buf, cap, "%s", mesa_env);
}

bool mesa_frame_pending(int dev_idx) {
    (void)dev_idx;
    bool any = false;
    uint64_t flags = spin_lock_irqsave(&mesa_lock);
    for (int i = 0; i < MESA_MAX_FRAMES && !any; i++)
        if (mesa_frames[i].used && mesa_frames[i].ready && !mesa_frames[i].sent) any = true;
    spin_unlock_irqrestore(&mesa_lock, flags);
    return any;
}

int mesa_frame_drain(int dev_idx, uint8_t *buf, uint32_t cap, uint32_t *out_len) {
    (void)dev_idx;
    if (!buf || cap < MESA_FRAME_MAX || !out_len) return GPU_ERR_PARAM;
    uint64_t flags = spin_lock_irqsave(&mesa_lock);
    struct mesa_frame *pick = NULL;
    for (int i = 0; i < MESA_MAX_FRAMES; i++) {
        struct mesa_frame *f = &mesa_frames[i];
        if (!f->used || !f->ready || f->sent) continue;
        if (f->slot_seq == 0xFFFFFFFEu) {
            pick = f;
            break;
        }
        if (!pick || (int32_t)(f->seq - pick->seq) < 0) pick = f;
    }
    if (!pick) {
        spin_unlock_irqrestore(&mesa_lock, flags);
        return GPU_ERR_BUSY;
    }
    if (pick->len > cap) {
        spin_unlock_irqrestore(&mesa_lock, flags);
        return GPU_ERR_OVERFLOW;
    }
    memcpy(buf, pick->data, pick->len);
    pick->sent = true;
    *out_len = pick->len;
    spin_unlock_irqrestore(&mesa_lock, flags);
    return GPU_OK;
}

uint8_t *mesa_pix_target(uint32_t pix_seq, uint32_t off, uint32_t len) {
    if (!mesa_pix || off > MESA_PIX_MAX || len > MESA_PIX_MAX - off) return NULL;
    if (pix_seq != mesa_pix_seq_v) return NULL;
    return mesa_pix + off;
}

void mesa_pix_account(uint32_t len) { mesa_pix_done += len; }

int mesa_pix_write(uint32_t pix_seq, uint32_t off, const void *data, uint32_t len) {
    if (!mesa_pix || off > MESA_PIX_MAX || len > MESA_PIX_MAX - off) return GPU_ERR_PARAM;
    if (pix_seq != mesa_pix_seq_v) return GPU_ERR_PARAM;
    memcpy(mesa_pix + off, data, len);
    mesa_pix_done += len;
    return GPU_OK;
}

uint32_t mesa_pix_seq(void) { return mesa_pix_seq_v; }
uint8_t *mesa_pix_data(void) { return mesa_pix; }
uint32_t mesa_pix_len(void) { return mesa_pix_len_v; }
int mesa_host_info(struct mesa_host_info *out) {
    if (!out) return GPU_ERR_PARAM;
    memcpy(out, &mesa_info_v, sizeof(*out));
    out->pid = (uint32_t)mesa_host_pid;
    out->prime_gen = mesa_prime_gen;
    return GPU_OK;
}


static void mesa_boot_complete(const struct mesa_done_hdr *h, const uint8_t *data, uint32_t len) {
    const uint8_t *p = data + sizeof(*h);
    uint32_t rets = h->rets;
    if (sizeof(*h) + (size_t)rets * 8 > len) return;
    const uint8_t *outs = p + (size_t)rets * 8;
    uint32_t remain = len - sizeof(*h) - rets * 8;
    uint64_t first8 = 0;
    if (remain >= 8) memcpy(&first8, outs, 8);
    LOG_INFO("mesa dbg: rets=%u bytes=%u remain=%u outs0=%02x%02x%02x%02x outs4=%02x%02x%02x%02x vend=%.28s rend=%.28s",
             h->rets, h->bytes, remain,
             (unsigned)(first8 >> 0) & 0xFF, (unsigned)(first8 >> 8) & 0xFF,
             (unsigned)(first8 >> 16) & 0xFF, (unsigned)(first8 >> 24) & 0xFF,
             (unsigned)(first8 >> 32) & 0xFF, (unsigned)(first8 >> 40) & 0xFF,
             (unsigned)(first8 >> 48) & 0xFF, (unsigned)(first8 >> 56) & 0xFF,
             remain > 8 + 256 ? (const char *)outs + 8 : "",
             remain > 8 + 512 ? (const char *)outs + 8 + 256 : "");
    uint32_t off = 0;
    uint32_t maj = 0, min = 0;
    for (int k = 0; k < 2 && off + 4 <= remain; k++, off += 4) {
        uint32_t v;
        memcpy(&v, outs + off, 4);
        if (k == 0) maj = v;
        if (k == 1) min = v;
    }
    static char vendor[128];
    static char renderer_s[128];
    if (off + 256 <= remain) {
        memcpy(vendor, outs + off, 128);
        vendor[127] = 0;
    }
    if (off + 256 + 256 <= remain) {
        memcpy(renderer_s, outs + off + 256, 128);
        renderer_s[127] = 0;
    }
    uint32_t keep = rets < MESA_MAX_CALLS ? rets : MESA_MAX_CALLS;
    memcpy(mesa_boot_rets, p, (size_t)keep * 8);
    __atomic_store_n(&mesa_boot_nrets, keep, __ATOMIC_RELEASE);
    mesa_info_v.gl_ok = 1;
    mesa_info_v.vk_ok = 1;
    snprintf(mesa_info_v.renderer, sizeof(mesa_info_v.renderer), "EGL %u.%u %s / %s", maj, min,
             vendor, renderer_s);
    strncpy(mesa_info_v.env, mesa_env, sizeof(mesa_info_v.env) - 1);
    mesa_boot_done = true;
    LOG_INFO("mesa: context ready - EGL %u.%u, renderer: %s", maj, min, renderer_s);
}

int mesa_frame_complete(int dev_idx, uint32_t slot_seq, uint32_t seq, const uint8_t *data,
                        uint32_t len) {
    if (len < sizeof(struct mesa_done_hdr)) return GPU_ERR_PARAM;
    struct mesa_done_hdr h;
    memcpy(&h, data, sizeof(h));
    if (h.bytes > 56 * 1024 || h.rets > MESA_MAX_CALLS) return GPU_ERR_PARAM;
    if (sizeof(h) + (uint64_t)h.rets * 8 > len) return GPU_ERR_PARAM;

    uint64_t flags = spin_lock_irqsave(&mesa_lock);
    struct mesa_frame *f = NULL;
    for (int i = 0; i < MESA_MAX_FRAMES; i++) {
        if (mesa_frames[i].used && mesa_frames[i].sent && mesa_frames[i].slot_seq == slot_seq &&
            mesa_frames[i].seq == seq) {
            f = &mesa_frames[i];
            break;
        }
    }
    if (!f) {
        spin_unlock_irqrestore(&mesa_lock, flags);
        if (slot_seq == 0xFFFFFFFEu) {
            mesa_boot_complete(&h, data, len);
            return GPU_OK;
        }
        return GPU_ERR_PARAM;
    }
    spin_unlock_irqrestore(&mesa_lock, flags);

    if (slot_seq == 0xFFFFFFFEu) {
        mesa_boot_complete(&h, data, len);
        mesa_frame_free(f);
        return GPU_OK;
    }

    if (slot_seq == MESA_DIRECT_SEQ) {
        if (h.seq == mesa_dcr.seq && sizeof(struct mesa_done_hdr) + (size_t)h.rets * 8 <= len) {
            uint32_t n = h.rets < MESA_MAX_CALLS ? h.rets : MESA_MAX_CALLS;
            const uint8_t *rp = data + sizeof(struct mesa_done_hdr);
            memcpy(mesa_dcr.rets, rp, (size_t)n * 8);
            mesa_dcr.nrets = n;
            uint32_t olen = len - (uint32_t)sizeof(struct mesa_done_hdr) - h.rets * 8;
            if (olen > MESA_DC_OUTS_MAX) olen = MESA_DC_OUTS_MAX;
            memcpy(mesa_dcr.outs, rp + (size_t)h.rets * 8, olen);
            mesa_dcr.outs_len = olen;
            mesa_dcr.status = h.status;
            __atomic_store_n(&mesa_dcr.done, 1, __ATOMIC_RELEASE);
            scheduler_wake_all((void *)&mesa_dcr.done);
        }
        mesa_frame_free(f);
        return GPU_OK;
    }

    struct gpu_ring_slot *slot = gpu_slot_by_seq(dev_idx, slot_seq);
    if (!slot) {
        mesa_frame_free(f);
        return GPU_ERR_PARAM;
    }

    const uint8_t *rets = data + sizeof(struct mesa_done_hdr);
    uint32_t rets_bytes = (uint32_t)h.rets * 8;
    if (sizeof(struct mesa_done_hdr) + rets_bytes > len) {
        mesa_frame_free(f);
        return GPU_ERR_PARAM;
    }
    const uint8_t *outs = rets + rets_bytes;
    uint32_t outs_len = len - sizeof(struct mesa_done_hdr) - rets_bytes;

    uint32_t out_stream[MESA_OUT_MAX];
    uint32_t pos = 0;
    for (int j = 0; j < f->nout_total && j < MESA_OUT_MAX; j++) {
        out_stream[j] = pos;
        pos += f->out_len[j];
    }

    for (uint32_t ci = 0; ci < f->cmd_count && ci < GPU_BATCH_MAX_CMDS; ci++) {
        struct mesa_outslot *o = &f->slots[ci];
        if (!o->result_off) continue;
        gpu_cmd_rec *rec =
            (gpu_cmd_rec *)&slot->data[GPU_BATCH_REC_OFF + (size_t)ci * sizeof(gpu_cmd_rec)];
        if (rec->result_cap < GPU_RESULT_HDR) continue;
        uint8_t *area = &slot->data[GPU_BATCH_RESULT_OFF + rec->result_off];
        uint16_t total = 0;
        for (int k = 0; k < o->nout; k++) {
            uint32_t src = out_stream[o->out_at[k]];
            uint32_t n = o->out_len[k];
            if (src + n <= outs_len && total + n <= (uint32_t)rec->result_cap - GPU_RESULT_HDR) {
                memcpy(area + GPU_RESULT_HDR + total, outs + src, n);
                total += (uint16_t)n;
            }
        }
        if (o->has_ret && total + 8 <= rec->result_cap - GPU_RESULT_HDR) {
            if (ci < h.rets) memcpy(area + GPU_RESULT_HDR + total, rets + (size_t)ci * 8, 8);
            total += 8;
        }
        area[0] = (uint8_t)(total & 0xFF);
        area[1] = (uint8_t)(total >> 8);
        area[2] = 0;
        area[3] = 0;
    }
    mesa_frame_free(f);
    gpu_slot_publish(dev_idx, slot_seq, h.status);
    return GPU_OK;
}

bool mesa_host_pid_get(void) { return mesa_host_pid != 0; }

void mesa_host_attached(void) {
    mesa_attached = true;
    mesa_epoch++;
    mesa_failed_v = false;
    mesa_bootstrap_push();
}

uint32_t mesa_epoch_get(void) { return mesa_epoch; }

void mesa_on_flush(int dev_idx, struct gpu_ring_slot *slot) {
    gpu_batch_hdr *hdr = (gpu_batch_hdr *)slot->data;
    bool api = false;
    for (uint16_t i = 0; i < hdr->cmd_count && i < GPU_BATCH_MAX_CMDS; i++) {
        gpu_cmd_rec *rec = (gpu_cmd_rec *)&slot->data[GPU_BATCH_REC_OFF + (size_t)i * sizeof(gpu_cmd_rec)];
        if ((rec->opcode >= GPU_CMD_GL_FIRST && rec->opcode <= GPU_CMD_GL_LAST) ||
            (rec->opcode >= GPU_CMD_VK_FIRST && rec->opcode <= GPU_CMD_VK_LAST)) {
            api = true;
            break;
        }
    }
    if (!api) return;
    if (!mesa_host_pid && !mesa_failed_v) mesa_start_async();
    mesa_compile_slot(slot, dev_idx);
}

int mesa_init(void) {
    mesa_pix = kmalloc(MESA_PIX_MAX);
    if (!mesa_pix) return GPU_ERR_NOMEM;
    memset(&mesa_info_v, 0, sizeof(mesa_info_v));
    LOG_INFO("mesa: kernel-space translator ready (%d GL + %d VK signatures)",
             GPU_CMD_GL_COUNT, GPU_CMD_VK_COUNT);
    return GPU_OK;
}

struct mesa_dc {
    struct mesa_build b;
    struct mesa_outslot slots[MESA_MAX_CALLS];
    uint16_t pend_outs;
    uint16_t pend_fix;
};

struct mesa_dc *mesa_dc_new(void) {
    struct mesa_dc *d = kmalloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->b.slots = d->slots;
    return d;
}

void mesa_dc_free(struct mesa_dc *d) {
    if (d) kfree(d);
}

static int mesa_dc_add(struct mesa_dc *d, uint16_t kind, const char *name, uint8_t argc, const uint64_t *args,
                       uint32_t ptr_mask, uint32_t ret_mask) {
    if (!d || argc > MESA_FMASK_ARGS) return -1;
    uint32_t name_off = name ? mb_put(&d->b, name, (uint32_t)strlen(name) + 1) : 0;
    struct mesa_call *c = mb_call(&d->b);
    if (!c || d->b.overflow) return -1;
    c->kind = kind;
    c->name_off = name_off;
    c->argc = argc;
    for (int i = 0; i < argc; i++) {
        c->args[i] = args[i];
        if ((ptr_mask >> i) & 1) c->fmask |= (uint32_t)MESA_ACLS_PTR << (2 * i);
    }
    c->rsv = ret_mask & 0x7FFFFFFFu;
    c->nout = d->pend_outs;
    c->nfix = d->pend_fix;
    d->pend_outs = 0;
    d->pend_fix = 0;
    return d->b.calls_n - 1;
}

int mesa_dc_call(struct mesa_dc *d, const char *name, uint8_t argc, const uint64_t *args, uint32_t ptr_mask,
                 uint32_t ret_mask) {
    return mesa_dc_add(d, MESA_CALL_FN, name, argc, args, ptr_mask, ret_mask);
}

int mesa_dc_fnptr(struct mesa_dc *d, uint8_t argc, const uint64_t *args, uint32_t ptr_mask, uint32_t ret_mask) {
    if (!argc) return -1;
    return mesa_dc_add(d, MESA_CALL_FNPTR, NULL, argc, args, ptr_mask, ret_mask);
}

uint32_t mesa_dc_in(struct mesa_dc *d, const void *data, uint32_t len) {
    return mb_blob8(&d->b, data, len);
}

uint32_t mesa_dc_out(struct mesa_dc *d, uint32_t len) {
    struct mesa_call *save = d->b.cur;
    d->b.cur = NULL;
    mb_align(&d->b, 8);
    uint32_t area = mb_put(&d->b, NULL, len);
    mb_out(&d->b, area, len, 0xFFFF);
    d->b.cur = save;
    d->pend_outs++;
    return area;
}

void mesa_dc_fix(struct mesa_dc *d, uint32_t where, uint32_t target) {
    if (mb_fix(&d->b, where, target, MESA_FIX_BLOB)) d->pend_fix++;
}

int mesa_dc_run(struct mesa_dc *d, uint64_t *rets, int max_rets, void *outs, uint32_t outs_cap,
                uint32_t timeout_ms) {
    if (!d || d->b.overflow || !d->b.calls_n) return GPU_ERR_PARAM;
    if (!mesa_ready()) return GPU_ERR_NO_TRANSLATOR;
    while (__atomic_exchange_n(&mesa_dcr.busy, 1, __ATOMIC_ACQ_REL)) scheduler_sleep_ms(1);
    struct mesa_frame *f = NULL;
    uint64_t flags = spin_lock_irqsave(&mesa_lock);
    bool alloc = mesa_frame_alloc(&f);
    spin_unlock_irqrestore(&mesa_lock, flags);
    if (!alloc) {
        __atomic_store_n(&mesa_dcr.busy, 0, __ATOMIC_RELEASE);
        return GPU_ERR_BUSY;
    }
    uint32_t seq = ++mesa_frame_seq;
    mesa_dcr.status = 0;
    mesa_dcr.nrets = 0;
    mesa_dcr.outs_len = 0;
    __atomic_store_n(&mesa_dcr.done, 0, __ATOMIC_RELEASE);
    mesa_dcr.seq = seq;
    if (!mesa_finalize(&d->b, seq, f, MESA_DIRECT_SEQ, -1, 0, 0, d->b.calls_n)) {
        mesa_frame_free(f);
        mesa_dcr.seq = 0;
        __atomic_store_n(&mesa_dcr.busy, 0, __ATOMIC_RELEASE);
        return GPU_ERR_OVERFLOW;
    }
    mesa_frame_publish(f);
    int w = scheduler_wait((void *)&mesa_dcr.done, &mesa_dcr.done, timeout_ms);
    int r = GPU_OK;
    if (w) {
        r = GPU_ERR_TIMEOUT;
    } else {
        if (rets && max_rets > 0) {
            int n = (int)mesa_dcr.nrets < max_rets ? (int)mesa_dcr.nrets : max_rets;
            memcpy(rets, mesa_dcr.rets, (size_t)n * 8);
            if (n < max_rets) memset(rets + n, 0, (size_t)(max_rets - n) * 8);
        }
        if (outs && outs_cap) {
            uint32_t n = mesa_dcr.outs_len < outs_cap ? mesa_dcr.outs_len : outs_cap;
            memcpy(outs, mesa_dcr.outs, n);
        }
        if (mesa_dcr.status) r = GPU_ERR_STATE;
    }
    mesa_dcr.seq = 0;
    __atomic_store_n(&mesa_dcr.busy, 0, __ATOMIC_RELEASE);
    return r;
}

bool mesa_card_context(struct mesa_card_ctx *out) {
    if (!out || !mesa_ready() || mesa_host_card < 1) return false;
    if (__atomic_load_n(&mesa_boot_nrets, __ATOMIC_ACQUIRE) < 6) return false;
    out->dev_idx = mesa_host_card - 1;
    out->drm_fd = (int)mesa_boot_rets[0];
    out->gbm = mesa_boot_rets[1];
    out->dpy = mesa_boot_rets[2];
    out->ctx = mesa_boot_rets[5];
    if (strstr(mesa_info_v.renderer, "llvmpipe") || strstr(mesa_info_v.renderer, "softpipe")) return false;
    return out->drm_fd >= 0 && out->gbm && out->dpy && out->ctx;
}

void *mesa_host_drm_file(int fd) {
    uint64_t flags = spin_lock_irqsave(&mesa_host_proc_lock);
    struct process *p = mesa_host_proc;
    if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&mesa_host_proc_lock, flags);
    if (!p) return NULL;
    struct ufile *f = fd_get(p, fd);
    void *lx = f ? vfs_ufile_lx(f) : NULL;
    if (f) ufile_put(f);
    proc_put(p);
    return lx;
}
