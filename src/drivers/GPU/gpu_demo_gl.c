// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/gpu_demo_math.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_gl_defs.h"

#include "components/Memory/heap.h"
#include "components/logger.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/lkpi/lkpi.h"
#include "kernel/proc/syscall.h"
#include "kernel/scheduler/scheduler.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GLS_ERR 1
#define GLS_OK 0

#define GLS_TIMEOUT_MS 1180000
#define GLS_FRAME_TIMEOUT_MS 180000
#define GLS_PIX_TIMEOUT_MS 240000

#define GLS_UPLOAD_CHUNK (16 * 1024)

struct gls {
    int cl;
    char err[128];
    uint64_t r64;
    uint16_t rlen;
    uint8_t rbuf[512];
};

#define GLS_A(...) \
    (uint8_t)(sizeof((uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t)), (const uint64_t[]){__VA_ARGS__}
#define GLS_A0 0, NULL

struct gls_fbo {
    uint32_t tex;
    uint32_t depth;
    uint32_t fbo;
};

static uint64_t msnow(void) { return sys_monotonic_ns() / 1000000ULL; }

static inline uint64_t f32b(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void gls_fail(struct gls *g, const char *what) {
    if (!g->err[0]) snprintf(g->err, sizeof(g->err), "%s", what);
}

static int gls_flush(struct gls *g, uint32_t timeout_ms) {
    int r = gpu_flush(g->cl, timeout_ms);
    if (r != GPU_OK) {
        if (!g->err[0]) snprintf(g->err, sizeof(g->err), "gpu_flush: %d", r);
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_put(struct gls *g, uint16_t op, uint8_t argc, const uint64_t *args,
                   const void *payload, uint32_t payload_len) {
    if (payload_len > 0xFFFF) {
        gls_fail(g, "payload too large");
        return GLS_ERR;
    }
    int r = gpu_cmd_gl(g->cl, op, argc, args, payload, (uint16_t)payload_len, 0);
    if (r) {
        if (!g->err[0]) snprintf(g->err, sizeof(g->err), "encode op 0x%x: %d", op, r);
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_call(struct gls *g, uint16_t op, uint8_t argc, const uint64_t *args,
                    const void *payload, uint32_t payload_len) {
    if (gpu_cmd_begin(g->cl)) {
        gls_fail(g, "cmd begin");
        return GLS_ERR;
    }
    if (gls_put(g, op, argc, args, payload, payload_len)) return GLS_ERR;
    return gls_flush(g, GLS_TIMEOUT_MS);
}

static int gls_one(struct gls *g, uint16_t op, uint8_t argc, const uint64_t *args) {
    return gls_call(g, op, argc, args, NULL, 0);
}

static int gls_res(struct gls *g, uint16_t op, uint8_t argc, const uint64_t *args,
                   const void *payload, uint32_t payload_len, uint16_t cap) {
    if (gpu_cmd_begin(g->cl)) {
        gls_fail(g, "cmd begin");
        return GLS_ERR;
    }
    if (payload_len > 0xFFFF ||
        gpu_cmd_gl(g->cl, op, argc, args, payload, (uint16_t)payload_len, cap + 8)) {
        gls_fail(g, "cmd encode");
        return GLS_ERR;
    }
    if (gls_flush(g, GLS_TIMEOUT_MS)) return GLS_ERR;
    uint16_t len = 0;
    memset(g->rbuf, 0, sizeof(g->rbuf));
    if (gpu_result(g->cl, 0, g->rbuf, cap, &len)) {
        gls_fail(g, "result fetch");
        return GLS_ERR;
    }
    g->rlen = len;
    g->r64 = 0;
    memcpy(&g->r64, g->rbuf, len < 8 ? len : 8);
    return GLS_OK;
}

static uint32_t gls_ret32(struct gls *g) { return (uint32_t)g->r64; }

static int gls_gen(struct gls *g, uint16_t op, uint32_t *out) {
    if (gls_res(g, op, GLS_A(1, 0), NULL, 0, 8)) return GLS_ERR;
    memcpy(out, g->rbuf, 4);
    if (!*out) {
        gls_fail(g, "object name 0");
        return GLS_ERR;
    }
    return GLS_OK;
}

static void gls_delete(struct gls *g, uint16_t op, uint32_t *name) {
    if (!*name) return;
    gls_call(g, op, GLS_A(1, 0), name, 4);
    *name = 0;
}

static int gls_check(struct gls *g, const char *what) {
    if (gls_res(g, GPU_CMD_GL_GET_ERROR, GLS_A0, NULL, 0, 8)) return GLS_ERR;
    uint32_t e = gls_ret32(g);
    if (e != 0) {
        snprintf(g->err, sizeof(g->err), "%s: glGetError=0x%x", what, e);
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_reset_state(struct gls *g) {
    if (gfx_gl_reset_client(g->cl)) {
        gls_fail(g, "GL state reset");
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_shader(struct gls *g, uint32_t type, const char *src, uint32_t *id_out) {
    if (gls_res(g, GPU_CMD_GL_CREATE_SHADER, GLS_A(type), NULL, 0, 8)) return GLS_ERR;
    uint32_t id = gls_ret32(g);
    if (!id) {
        gls_fail(g, "glCreateShader returned 0");
        return GLS_ERR;
    }
    *id_out = id;

    uint32_t srclen = (uint32_t)strlen(src);
    uint8_t payload[1024];
    const uint32_t str_off = 8;
    uint32_t total = str_off + srclen + 1;
    if (total > sizeof(payload)) {
        gls_fail(g, "shader source too long");
        return GLS_ERR;
    }
    memset(payload, 0, str_off);
    memcpy(payload, &str_off, 4);
    memcpy(payload + str_off, src, srclen + 1);
    if (gls_call(g, GPU_CMD_GL_SHADER_SOURCE, GLS_A(id, 1, 0, 0xFFFE), payload, total))
        return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_COMPILE_SHADER, GLS_A(id))) return GLS_ERR;

    if (gls_res(g, GPU_CMD_GL_GET_SHADERIV, GLS_A(id, GL_COMPILE_STATUS, 0), NULL, 0, 8))
        return GLS_ERR;
    uint32_t ok = 0;
    memcpy(&ok, g->rbuf, 4);
    if (!ok) {
        uint32_t loglen = 0;
        if (!gls_res(g, GPU_CMD_GL_GET_SHADERIV, GLS_A(id, GL_INFO_LOG_LENGTH, 0), NULL, 0, 8))
            memcpy(&loglen, g->rbuf, 4);
        snprintf(g->err, sizeof(g->err), "shader 0x%x compile failed (log %u bytes)", type, loglen);
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_program(struct gls *g, uint32_t vs, uint32_t fs, const char *names, int nattr,
                       uint32_t *prog_out) {
    if (gls_res(g, GPU_CMD_GL_CREATE_PROGRAM, GLS_A0, NULL, 0, 8)) return GLS_ERR;
    uint32_t prog = gls_ret32(g);
    if (!prog) {
        gls_fail(g, "glCreateProgram returned 0");
        return GLS_ERR;
    }
    *prog_out = prog;
    if (gls_one(g, GPU_CMD_GL_ATTACH_SHADER, GLS_A(prog, vs))) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_ATTACH_SHADER, GLS_A(prog, fs))) return GLS_ERR;

    uint32_t names_len = 0;
    for (int i = 0; i < nattr; i++) names_len += (uint32_t)strlen(names + names_len) + 1;
    uint32_t off = 0;
    for (int i = 0; i < nattr; i++) {
        if (gls_call(g, GPU_CMD_GL_BIND_ATTRIB_LOCATION, GLS_A(prog, (uint64_t)i, off), names,
                     names_len))
            return GLS_ERR;
        off += (uint32_t)strlen(names + off) + 1;
    }
    if (gls_one(g, GPU_CMD_GL_LINK_PROGRAM, GLS_A(prog))) return GLS_ERR;
    if (gls_res(g, GPU_CMD_GL_GET_PROGRAMIV, GLS_A(prog, GL_LINK_STATUS, 0), NULL, 0, 8))
        return GLS_ERR;
    uint32_t ok = 0;
    memcpy(&ok, g->rbuf, 4);
    if (!ok) {
        gls_fail(g, "program link failed");
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_uniform_location(struct gls *g, uint32_t prog, const char *name, int32_t *loc) {
    uint32_t len = (uint32_t)strlen(name) + 1;
    if (gls_res(g, GPU_CMD_GL_GET_UNIFORM_LOCATION, GLS_A(prog, 0), name, len, 8)) return GLS_ERR;
    if (g->rlen < 4) {
        gls_fail(g, "glGetUniformLocation: no result");
        return GLS_ERR;
    }
    *loc = (int32_t)gls_ret32(g);
    if (*loc < 0) {
        snprintf(g->err, sizeof(g->err), "uniform '%s' not found", name);
        return GLS_ERR;
    }
    return GLS_OK;
}

static int gls_upload(struct gls *g, const void *data, uint32_t bytes) {
    if (gls_one(g, GPU_CMD_GL_BUFFER_DATA, GLS_A(GL_ARRAY_BUFFER, bytes, 0xFFFE, GL_STATIC_DRAW)))
        return GLS_ERR;
    const uint8_t *p = data;
    for (uint32_t off = 0; off < bytes; off += GLS_UPLOAD_CHUNK) {
        uint32_t n = bytes - off < GLS_UPLOAD_CHUNK ? bytes - off : GLS_UPLOAD_CHUNK;
        if (gls_call(g, GPU_CMD_GL_BUFFER_SUB_DATA, GLS_A(GL_ARRAY_BUFFER, off, n, 0), p + off, n))
            return GLS_ERR;
    }
    return gls_check(g, "vertex upload");
}

static int gls_setup_fbo(struct gls *g, uint32_t w, uint32_t h, struct gls_fbo *f) {
    memset(f, 0, sizeof(*f));
    if (gls_gen(g, GPU_CMD_GL_GEN_TEXTURES, &f->tex)) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_BIND_TEXTURE, GLS_A(GL_TEXTURE_2D, f->tex))) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_TEX_PARAMETERI, GLS_A(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)))
        return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_TEX_PARAMETERI, GLS_A(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)))
        return GLS_ERR;

    uint64_t texargs[6] = {GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0};
    uint64_t extra[3] = {GL_RGBA, GL_UNSIGNED_BYTE, 0xFFFE};
    if (gpu_cmd_begin(g->cl)) return GLS_ERR;
    if (gls_put(g, GPU_CMD_GL_TEX_IMAGE2D, 9, texargs, extra, sizeof(extra))) return GLS_ERR;
    if (gls_flush(g, GLS_TIMEOUT_MS)) return GLS_ERR;

    if (gls_gen(g, GPU_CMD_GL_GEN_RENDERBUFFERS, &f->depth)) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_BIND_RENDERBUFFER, GLS_A(GL_RENDERBUFFER, f->depth))) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_RENDERBUFFER_STORAGE, GLS_A(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h)))
        return GLS_ERR;

    if (gls_gen(g, GPU_CMD_GL_GEN_FRAMEBUFFERS, &f->fbo)) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_BIND_FRAMEBUFFER, GLS_A(GL_FRAMEBUFFER, f->fbo))) return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_FRAMEBUFFER_TEXTURE2D,
                GLS_A(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0)))
        return GLS_ERR;
    if (gls_one(g, GPU_CMD_GL_FRAMEBUFFER_RENDERBUFFER,
                GLS_A(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f->depth)))
        return GLS_ERR;
    if (gls_res(g, GPU_CMD_GL_CHECK_FRAMEBUFFER_STATUS, GLS_A(GL_FRAMEBUFFER), NULL, 0, 8))
        return GLS_ERR;
    uint32_t st = gls_ret32(g);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(g->err, sizeof(g->err), "FBO incomplete (0x%x)", st);
        return GLS_ERR;
    }
    if (gls_one(g, GPU_CMD_GL_VIEWPORT, GLS_A(0, 0, w, h))) return GLS_ERR;
    return gls_check(g, "FBO setup");
}

static void gls_free_fbo(struct gls *g, struct gls_fbo *f) {
    gls_one(g, GPU_CMD_GL_BIND_FRAMEBUFFER, GLS_A(GL_FRAMEBUFFER, 0));
    gls_delete(g, GPU_CMD_GL_DELETE_FRAMEBUFFERS, &f->fbo);
    gls_delete(g, GPU_CMD_GL_DELETE_RENDERBUFFERS, &f->depth);
    gls_delete(g, GPU_CMD_GL_DELETE_TEXTURES, &f->tex);
}

static int gls_readback(struct gls *g, uint32_t w, uint32_t h) {
    uint64_t marker = 0xFFFF;
    if (gls_put(g, GPU_CMD_GL_READ_PIXELS, GLS_A(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, 0xFFFF),
                &marker, 8))
        return GLS_ERR;
    if (gls_put(g, GPU_CMD_MESA_PIX, GLS_A((uint64_t)w * h * 4), NULL, 0)) return GLS_ERR;
    return gls_flush(g, GLS_FRAME_TIMEOUT_MS > GLS_PIX_TIMEOUT_MS ? GLS_FRAME_TIMEOUT_MS : GLS_PIX_TIMEOUT_MS);
}

#define GLS_ZC_BUFS 3
#define GLS_ZC_TIMEOUT_MS 20000

struct gls_zc {
    int n;
    int cur;
    uint32_t w, h;
    uint64_t dpy;
    uint64_t bo[GLS_ZC_BUFS];
    uint64_t img[GLS_ZC_BUFS];
    uint32_t rb[GLS_ZC_BUFS];
    uint32_t fbo[GLS_ZC_BUFS];
    int slot[GLS_ZC_BUFS];
    uint32_t depth;
};

static void gls_zc_free(struct gls *g, struct gls_zc *z) {
    if (!z->dpy) return;
    gls_one(g, GPU_CMD_GL_BIND_FRAMEBUFFER, GLS_A(GL_FRAMEBUFFER, 0));
    for (int i = 0; i < GLS_ZC_BUFS; i++) {
        gls_delete(g, GPU_CMD_GL_DELETE_FRAMEBUFFERS, &z->fbo[i]);
        gls_delete(g, GPU_CMD_GL_DELETE_RENDERBUFFERS, &z->rb[i]);
    }
    gls_delete(g, GPU_CMD_GL_DELETE_RENDERBUFFERS, &z->depth);
    struct mesa_dc *d = mesa_dc_new();
    if (d) {
        int calls = 0;
        for (int i = 0; i < GLS_ZC_BUFS; i++) {
            if (z->img[i] && mesa_dc_call(d, "eglDestroyImageKHR", 2, (uint64_t[]){z->dpy, z->img[i]}, 0, 0) >= 0)
                calls++;
            if (z->bo[i] && mesa_dc_call(d, "gbm_bo_destroy", 1, (uint64_t[]){z->bo[i]}, 0, 0) >= 0) calls++;
        }
        if (calls) mesa_dc_run(d, NULL, 0, NULL, 0, GLS_ZC_TIMEOUT_MS);
        mesa_dc_free(d);
    }
    memset(z, 0, sizeof(*z));
}

static int gls_zc_setup(struct gls *g, gpu_present *pt, struct gls_zc *z) {
    memset(z, 0, sizeof(*z));
    struct mesa_card_ctx mc;
    if (!pt->kms || !mesa_card_context(&mc) || mc.dev_idx != pt->dev_idx) return GLS_ERR;
    z->w = pt->fb_w;
    z->h = pt->fb_h;
    z->dpy = mc.dpy;
    struct mesa_dc *d = mesa_dc_new();
    if (!d) return GLS_ERR;
    uint64_t rets[4 * GLS_ZC_BUFS];
    for (int i = 0; i < GLS_ZC_BUFS; i++)
        mesa_dc_call(d, "gbm_bo_create", 5, (uint64_t[]){mc.gbm, z->w, z->h, 0x34325258u, 1u | 4u | 16u}, 0, 0);
    int r = mesa_dc_run(d, rets, GLS_ZC_BUFS, NULL, 0, GLS_ZC_TIMEOUT_MS);
    mesa_dc_free(d);
    for (int i = 0; i < GLS_ZC_BUFS; i++) z->bo[i] = r ? 0 : rets[i];
    for (int i = 0; i < GLS_ZC_BUFS; i++)
        if (!z->bo[i]) goto fail;

    d = mesa_dc_new();
    if (!d) goto fail;
    for (int i = 0; i < GLS_ZC_BUFS; i++) {
        mesa_dc_call(d, "gbm_bo_get_handle", 1, (uint64_t[]){z->bo[i]}, 0, 0);
        mesa_dc_call(d, "gbm_bo_get_stride", 1, (uint64_t[]){z->bo[i]}, 0, 0);
        mesa_dc_call(d, "eglCreateImageKHR", 5, (uint64_t[]){mc.dpy, 0, 0x30B0, z->bo[i], 0}, 0, 0);
    }
    r = mesa_dc_run(d, rets, 3 * GLS_ZC_BUFS, NULL, 0, GLS_ZC_TIMEOUT_MS);
    mesa_dc_free(d);
    if (r) goto fail;
    for (int i = 0; i < GLS_ZC_BUFS; i++) z->img[i] = rets[3 * i + 2];

    void *host = mesa_host_drm_file(mc.drm_fd);
    if (!host) goto fail;
    for (int i = 0; i < GLS_ZC_BUFS; i++) {
        z->slot[i] = -1;
        if (!z->img[i]) continue;
        z->slot[i] = gpu_present_zc_add(pt, host, (uint32_t)rets[3 * i], (uint32_t)rets[3 * i + 1]);
    }
    lkpi_file_put(host);
    for (int i = 0; i < GLS_ZC_BUFS; i++)
        if (!z->img[i] || z->slot[i] < 0) goto fail;

    if (gls_gen(g, GPU_CMD_GL_GEN_RENDERBUFFERS, &z->depth)) goto fail;
    if (gls_one(g, GPU_CMD_GL_BIND_RENDERBUFFER, GLS_A(GL_RENDERBUFFER, z->depth))) goto fail;
    if (gls_one(g, GPU_CMD_GL_RENDERBUFFER_STORAGE, GLS_A(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, z->w, z->h)))
        goto fail;
    for (int i = 0; i < GLS_ZC_BUFS; i++) {
        if (gls_gen(g, GPU_CMD_GL_GEN_RENDERBUFFERS, &z->rb[i])) goto fail;
        if (gls_one(g, GPU_CMD_GL_BIND_RENDERBUFFER, GLS_A(GL_RENDERBUFFER, z->rb[i]))) goto fail;
        d = mesa_dc_new();
        if (!d) goto fail;
        mesa_dc_call(d, "glEGLImageTargetRenderbufferStorageOES", 2, (uint64_t[]){GL_RENDERBUFFER, z->img[i]}, 0,
                     0);
        r = mesa_dc_run(d, NULL, 0, NULL, 0, GLS_ZC_TIMEOUT_MS);
        mesa_dc_free(d);
        if (r) goto fail;
        if (gls_gen(g, GPU_CMD_GL_GEN_FRAMEBUFFERS, &z->fbo[i])) goto fail;
        if (gls_one(g, GPU_CMD_GL_BIND_FRAMEBUFFER, GLS_A(GL_FRAMEBUFFER, z->fbo[i]))) goto fail;
        if (gls_one(g, GPU_CMD_GL_FRAMEBUFFER_RENDERBUFFER,
                    GLS_A(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, z->rb[i])))
            goto fail;
        if (gls_one(g, GPU_CMD_GL_FRAMEBUFFER_RENDERBUFFER,
                    GLS_A(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, z->depth)))
            goto fail;
        if (gls_res(g, GPU_CMD_GL_CHECK_FRAMEBUFFER_STATUS, GLS_A(GL_FRAMEBUFFER), NULL, 0, 8)) goto fail;
        if (gls_ret32(g) != GL_FRAMEBUFFER_COMPLETE) goto fail;
    }
    if (gls_one(g, GPU_CMD_GL_VIEWPORT, GLS_A(0, 0, z->w, z->h))) goto fail;
    if (gls_check(g, "scanout buffers")) goto fail;
    z->n = GLS_ZC_BUFS;
    LOG_INFO("present: GPU renders straight into %d scanout buffers %ux%u, no CPU copy", z->n, z->w, z->h);
    return GLS_OK;

fail:
    LOG_WARNING("present: scanout buffers unavailable on this renderer, using the CPU copy path");
    g->err[0] = '\0';
    gls_zc_free(g, z);
    gpu_present_zc_reset(pt);
    return GLS_ERR;
}

static int gls_zc_bind(struct gls *g, struct gls_zc *z) {
    return gls_put(g, GPU_CMD_GL_BIND_FRAMEBUFFER, GLS_A(GL_FRAMEBUFFER, z->fbo[z->cur]), NULL, 0);
}

static int gls_zc_show(struct gls *g, gpu_present *pt, struct gls_zc *z) {
    if (gls_put(g, GPU_CMD_GL_FINISH, GLS_A0, NULL, 0)) return GLS_ERR;
    if (gls_flush(g, GLS_FRAME_TIMEOUT_MS)) return GLS_ERR;
    if (gpu_present_zc_show(pt, z->slot[z->cur])) {
        gls_fail(g, "scanout flip");
        return GLS_ERR;
    }
    z->cur = (z->cur + 1) % z->n;
    return GLS_OK;
}

static bool gls_abort(void) {
    struct keyboard_driver *kbd = get_self_driver(KEYBOARD_DRIVER, VIRTUAL_KEYBOARD);
    if (!kbd) return false;
    struct key_event ev;
    bool stop = false;
    while (kbd->get_key_event(&ev))
        if (ev.code == KEY_ESC) stop = true;
    return stop;
}

static uint32_t gls_fps(uint32_t frames, uint64_t elapsed) {
    return elapsed > 500 ? (uint32_t)(frames * 1000ULL / elapsed) : frames;
}

static const char cube_vs[] =
    "#version 120\n"
    "attribute vec3 pos;\n"
    "attribute vec3 col;\n"
    "uniform mat4 mvp;\n"
    "varying vec3 vcol;\n"
    "void main() { vcol = col; gl_Position = mvp * vec4(pos, 1.0); }\n";
static const char cube_fs[] =
    "#version 120\n"
    "varying vec3 vcol;\n"
    "void main() { gl_FragColor = vec4(vcol, 1.0); }\n";
static const char cube_attrs[] = "pos\0col";

int gpu_gl_cube_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out) {
    int ms = mesa_start();
    if (ms) return ms;
    struct gls g;
    memset(&g, 0, sizeof(g));
    g.cl = gpu_client_open(opts->dev_idx >= 0 ? opts->dev_idx : 0, GPU_API_OPENGL);
    if (g.cl < 0) return GPU_ERR_BUSY;

    gpu_present pt;
    int pr = gpu_present_open(&pt, opts, opts->width, opts->height);
    pt.src_bgra = true;
    if (pr) {
        LOG_WARNING("kmscube(GL): present open failed (%d)", pr);
        gpu_client_close(g.cl);
        return pr;
    }

    float verts[DEMO_CUBE_FLOATS];
    dm_cube_build(verts);

    uint32_t vs = 0, fs = 0, prog = 0, vbo = 0;
    struct gls_fbo fb;
    memset(&fb, 0, sizeof(fb));
    int32_t mvp_loc = -1;
    uint32_t frames = 0;
    const char *why = "";
    int rc = GLS_OK;

    if (gls_reset_state(&g)) { rc = GLS_ERR; why = "state reset"; }
    if (!rc && gls_shader(&g, GL_VERTEX_SHADER, cube_vs, &vs)) { rc = GLS_ERR; why = "vertex shader"; }
    if (!rc && gls_shader(&g, GL_FRAGMENT_SHADER, cube_fs, &fs)) { rc = GLS_ERR; why = "fragment shader"; }
    if (!rc && gls_program(&g, vs, fs, cube_attrs, 2, &prog)) { rc = GLS_ERR; why = "program"; }
    if (!rc && gls_uniform_location(&g, prog, "mvp", &mvp_loc)) { rc = GLS_ERR; why = "uniform"; }
    if (!rc && gls_gen(&g, GPU_CMD_GL_GEN_BUFFERS, &vbo)) { rc = GLS_ERR; why = "glGenBuffers"; }
    if (!rc && gls_one(&g, GPU_CMD_GL_BIND_BUFFER, GLS_A(GL_ARRAY_BUFFER, vbo))) { rc = GLS_ERR; why = "bind VBO"; }
    if (!rc && gls_upload(&g, verts, sizeof(verts))) { rc = GLS_ERR; why = "VBO upload"; }
    struct gls_zc zc;
    memset(&zc, 0, sizeof(zc));
    if (!rc && gls_zc_setup(&g, &pt, &zc) == GLS_OK) {
        pt.width = zc.w;
        pt.height = zc.h;
    } else if (!rc && gls_setup_fbo(&g, pt.width, pt.height, &fb)) {
        rc = GLS_ERR;
        why = "FBO";
    }
    if (!rc) {
        int r = gpu_cmd_begin(g.cl);
        r |= gls_put(&g, GPU_CMD_GL_USE_PROGRAM, GLS_A(prog), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE, GLS_A(GL_DEPTH_TEST), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_CLEAR_COLOR,
                     GLS_A(f32b(0.05f), f32b(0.05f), f32b(0.08f), f32b(1.0f)), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE_VERTEX_ATTRIB_ARRAY, GLS_A(0), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE_VERTEX_ATTRIB_ARRAY, GLS_A(1), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_VERTEX_ATTRIB_POINTER, GLS_A(0, 3, GL_FLOAT, 0, 24, 0), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_VERTEX_ATTRIB_POINTER, GLS_A(1, 3, GL_FLOAT, 0, 24, 12), NULL, 0);
        if (r || gls_flush(&g, GLS_TIMEOUT_MS) || gls_check(&g, "cube state")) {
            rc = GLS_ERR;
            why = "pipeline state";
        }
    }

    uint32_t run_ms = opts->seconds ? opts->seconds * 1000 : GPU_CUBE_DEFAULT_SECONDS * 1000;
    uint64_t t0 = msnow();
    while (!rc) {
        uint64_t now = msnow();
        if (now - t0 >= run_ms || gls_abort()) break;

        float mvp[16];
        dm_cube_mvp(mvp, (float)((now - t0) / 1000.0), pt.width, pt.height, 0);

        if (gpu_cmd_begin(g.cl)) break;
        int r = zc.n ? gls_zc_bind(&g, &zc) : GLS_OK;
        r |= gls_put(&g, GPU_CMD_GL_CLEAR, GLS_A(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_UNIFORM_MATRIX4FV, GLS_A((uint32_t)mvp_loc, 1, 0, 0), mvp, sizeof(mvp));
        r |= gls_put(&g, GPU_CMD_GL_DRAW_ARRAYS, GLS_A(GL_TRIANGLES, 0, DEMO_CUBE_VERTS), NULL, 0);
        if (r) break;
        if (zc.n) {
            if (gls_zc_show(&g, &pt, &zc)) break;
        } else {
            if (gls_readback(&g, pt.width, pt.height)) break;
            gpu_present_frame(&pt, (const uint32_t *)mesa_pix_data(), pt.width, pt.height);
        }
        frames++;
    }
    uint64_t elapsed = msnow() - t0;
    if (!rc && !frames && g.err[0]) {
        rc = GLS_ERR;
        why = "frame";
    }

    gls_zc_free(&g, &zc);
    gls_free_fbo(&g, &fb);
    gls_delete(&g, GPU_CMD_GL_DELETE_BUFFERS, &vbo);
    if (prog) gls_one(&g, GPU_CMD_GL_DELETE_PROGRAM, GLS_A(prog));
    if (vs) gls_one(&g, GPU_CMD_GL_DELETE_SHADER, GLS_A(vs));
    if (fs) gls_one(&g, GPU_CMD_GL_DELETE_SHADER, GLS_A(fs));
    gls_reset_state(&g);

    gpu_present_close(&pt);
    if (frames_out) *frames_out = frames;
    if (fps_out) *fps_out = gls_fps(frames, elapsed);
    if (rc) {
        LOG_WARNING("kmscube(GL): failed at %s: %s", why, g.err);
        if (opts->progress) {
            char line[200];
            snprintf(line, sizeof(line), "  OpenGL path failed (%s: %s) - see dmesg", why, g.err);
            opts->progress(opts->user, line);
        }
    } else if (g.err[0]) {
        LOG_WARNING("kmscube(GL): stopped after %u frames: %s", frames, g.err);
    }
    gpu_client_close(g.cl);
    return rc ? GLS_ERR : GLS_OK;
}

#define GEAR_FLOATS_PER_VERT 6
#define GEAR_MAX_VERTS 4096

struct gear_mesh {
    float *data;
    uint32_t verts;
    bool overflow;
};

static void gear_vert(struct gear_mesh *m, const float n[3], float x, float y, float z) {
    if (m->verts >= GEAR_MAX_VERTS) {
        m->overflow = true;
        return;
    }
    float *p = m->data + (size_t)m->verts * GEAR_FLOATS_PER_VERT;
    p[0] = n[0];
    p[1] = n[1];
    p[2] = n[2];
    p[3] = x;
    p[4] = y;
    p[5] = z;
    m->verts++;
}

static void gear_tri(struct gear_mesh *m, const float n[3], const float a[3], const float b[3],
                     const float c[3]) {
    gear_vert(m, n, a[0], a[1], a[2]);
    gear_vert(m, n, b[0], b[1], b[2]);
    gear_vert(m, n, c[0], c[1], c[2]);
}

static void gear_pt(float *p, float r, float ang, float z) {
    p[0] = r * (float)cos((double)ang);
    p[1] = r * (float)sin((double)ang);
    p[2] = z;
}

static void gear_build(struct gear_mesh *m, float inner, float outer, float width, int teeth,
                       float tooth_depth) {
    const float r0 = inner;
    const float r1 = outer - tooth_depth / 2.0f;
    const float r2 = outer + tooth_depth / 2.0f;
    const float da = 2.0f * DEMO_PI / (float)teeth / 4.0f;
    const float hw = width * 0.5f;

    for (int i = 0; i < teeth; i++) {
        float a = (float)i * 2.0f * DEMO_PI / (float)teeth;
        float ang[5] = {a, a + da, a + 2 * da, a + 3 * da, a + 4 * da};

        for (int side = 0; side < 2; side++) {
            float z = side ? -hw : hw;
            float n[3] = {0.0f, 0.0f, side ? -1.0f : 1.0f};
            float i0[3], i4[3], o0[3], o3[3], o4[3], t1[3], t2[3];
            gear_pt(i0, r0, ang[0], z);
            gear_pt(i4, r0, ang[4], z);
            gear_pt(o0, r1, ang[0], z);
            gear_pt(o3, r1, ang[3], z);
            gear_pt(o4, r1, ang[4], z);
            gear_pt(t1, r2, ang[1], z);
            gear_pt(t2, r2, ang[2], z);
            gear_tri(m, n, i0, o0, o3);
            gear_tri(m, n, i0, o3, i4);
            gear_tri(m, n, i4, o3, o4);
            gear_tri(m, n, o0, t1, t2);
            gear_tri(m, n, o0, t2, o3);
        }

        const float pr[5] = {r1, r2, r2, r1, r1};
        for (int s = 0; s < 4; s++) {
            float p[3], q[3], pb[3], qb[3];
            gear_pt(p, pr[s], ang[s], hw);
            gear_pt(q, pr[s + 1], ang[s + 1], hw);
            gear_pt(pb, pr[s], ang[s], -hw);
            gear_pt(qb, pr[s + 1], ang[s + 1], -hw);
            float dx = q[0] - p[0], dy = q[1] - p[1];
            float len = (float)sqrt((double)(dx * dx + dy * dy));
            if (len <= 0.0f) continue;
            float n[3] = {dy / len, -dx / len, 0.0f};
            gear_tri(m, n, p, q, qb);
            gear_tri(m, n, p, qb, pb);
        }

        float c0[3], c1[3], c0b[3], c1b[3];
        gear_pt(c0, r0, ang[0], hw);
        gear_pt(c1, r0, ang[4], hw);
        gear_pt(c0b, r0, ang[0], -hw);
        gear_pt(c1b, r0, ang[4], -hw);
        float mid = (ang[0] + ang[4]) * 0.5f;
        float n[3] = {-(float)cos((double)mid), -(float)sin((double)mid), 0.0f};
        gear_tri(m, n, c0, c1b, c1);
        gear_tri(m, n, c0, c0b, c1b);
    }
}

int gpu_glxgears_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out) {
    int ms = mesa_start();
    if (ms) return ms;
    struct gls g;
    memset(&g, 0, sizeof(g));
    g.cl = gpu_client_open(opts->dev_idx >= 0 ? opts->dev_idx : 0, GPU_API_OPENGL);
    if (g.cl < 0) return GPU_ERR_BUSY;

    gpu_present pt;
    int pr = gpu_present_open(&pt, opts, opts->width, opts->height);
    pt.src_bgra = true;
    if (pr) {
        LOG_WARNING("glxgears(GL): present open failed (%d)", pr);
        gpu_client_close(g.cl);
        return pr;
    }

    float *mesh = kmalloc((size_t)GEAR_MAX_VERTS * GEAR_FLOATS_PER_VERT * sizeof(float));
    if (!mesh) {
        gpu_present_close(&pt);
        gpu_client_close(g.cl);
        return GPU_ERR_NOMEM;
    }

    static const struct {
        float inner, outer, width;
        int teeth;
        float color[4];
        float x, y;
        float speed, phase;
    } gear_def[3] = {
        {1.0f, 4.0f, 1.0f, 20, {0.8f, 0.1f, 0.0f, 1.0f}, -3.0f, -2.0f, 1.0f, 0.0f},
        {0.5f, 2.0f, 2.0f, 10, {0.0f, 0.8f, 0.2f, 1.0f}, 3.1f, -2.0f, -2.0f, -9.0f},
        {1.3f, 2.0f, 0.5f, 10, {0.2f, 0.2f, 1.0f, 1.0f}, -3.1f, 4.2f, -2.0f, -25.0f},
    };
    struct gear_mesh gm = {.data = mesh, .verts = 0, .overflow = false};
    uint32_t gear_first[3], gear_count[3];
    for (int i = 0; i < 3; i++) {
        gear_first[i] = gm.verts;
        gear_build(&gm, gear_def[i].inner, gear_def[i].outer, gear_def[i].width,
                   gear_def[i].teeth, 0.7f);
        gear_count[i] = gm.verts - gear_first[i];
    }

    uint32_t frames = 0, vbo = 0;
    struct gls_fbo fb;
    memset(&fb, 0, sizeof(fb));
    int rc = GLS_OK;
    const char *why = "";

    if (gm.overflow) {
        gls_fail(&g, "gear mesh overflow");
        rc = GLS_ERR;
        why = "mesh";
    }
    if (!rc && gls_reset_state(&g)) { rc = GLS_ERR; why = "state reset"; }
    if (!rc && gls_gen(&g, GPU_CMD_GL_GEN_BUFFERS, &vbo)) { rc = GLS_ERR; why = "glGenBuffers"; }
    if (!rc && gls_one(&g, GPU_CMD_GL_BIND_BUFFER, GLS_A(GL_ARRAY_BUFFER, vbo))) { rc = GLS_ERR; why = "bind VBO"; }
    if (!rc && gls_upload(&g, mesh, gm.verts * GEAR_FLOATS_PER_VERT * (uint32_t)sizeof(float))) {
        rc = GLS_ERR;
        why = "VBO upload";
    }
    struct gls_zc zc;
    memset(&zc, 0, sizeof(zc));
    if (!rc && gls_zc_setup(&g, &pt, &zc) == GLS_OK) {
        pt.width = zc.w;
        pt.height = zc.h;
    } else if (!rc && gls_setup_fbo(&g, pt.width, pt.height, &fb)) {
        rc = GLS_ERR;
        why = "FBO";
    }

    float proj[16];
    {
        float aspect = (float)pt.width / (float)(pt.height ? pt.height : 1);
        float top = aspect >= 1.0f ? 1.0f : 1.0f / aspect;
        float fovy = 2.0f * (float)atan((double)(top / 5.0f));
        dm_perspective(proj, fovy, aspect, 5.0f, 60.0f, 0, 1);
    }
    if (!rc) {
        static const float light_pos[4] = {5.0f, 5.0f, 10.0f, 0.0f};
        int r = gpu_cmd_begin(g.cl);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE, GLS_A(GL_DEPTH_TEST), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE, GLS_A(GL_LIGHTING), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE, GLS_A(GL_LIGHT0), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_FRONT_FACE, GLS_A(GL_CW), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_SHADE_MODEL, GLS_A(GL_FLAT), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_CLEAR_COLOR,
                     GLS_A(f32b(0.0f), f32b(0.0f), f32b(0.0f), f32b(1.0f)), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_MATRIX_MODE, GLS_A(GL_PROJECTION), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_LOAD_MATRIXF, GLS_A(0), proj, sizeof(proj));
        r |= gls_put(&g, GPU_CMD_GL_MATRIX_MODE, GLS_A(GL_MODELVIEW), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_LOAD_IDENTITY, GLS_A0, NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_LIGHTFV, GLS_A(GL_LIGHT0, GL_POSITION, 0), light_pos,
                     sizeof(light_pos));
        r |= gls_put(&g, GPU_CMD_GL_ENABLE_CLIENT_STATE, GLS_A(GL_NORMAL_ARRAY), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE_CLIENT_STATE, GLS_A(GL_VERTEX_ARRAY), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_NORMAL_POINTER, GLS_A(GL_FLOAT, 24, 0), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_VERTEX_POINTER, GLS_A(3, GL_FLOAT, 24, 12), NULL, 0);
        if (r || gls_flush(&g, GLS_TIMEOUT_MS) || gls_check(&g, "gears state")) {
            rc = GLS_ERR;
            why = "pipeline state";
        }
    }

    uint32_t run_ms = opts->seconds ? opts->seconds * 1000 : GPU_CUBE_DEFAULT_SECONDS * 1000;
    uint64_t t0 = msnow();
    while (!rc) {
        uint64_t now = msnow();
        if (now - t0 >= run_ms || gls_abort()) break;
        float angle = (float)((now - t0) / 1000.0) * 70.0f;

        float view[16], rx[16], ry[16], cam[16];
        dm_translate(cam, 0.0f, 0.0f, -40.0f);
        dm_rotate(rx, 20.0f * DEMO_PI / 180.0f, 1.0f, 0.0f, 0.0f);
        dm_rotate(ry, 30.0f * DEMO_PI / 180.0f, 0.0f, 1.0f, 0.0f);
        dm_mul(view, cam, rx);
        dm_mul(view, view, ry);

        if (gpu_cmd_begin(g.cl)) break;
        int r = zc.n ? gls_zc_bind(&g, &zc) : GLS_OK;
        r |= gls_put(&g, GPU_CMD_GL_CLEAR, GLS_A(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT), NULL, 0);
        for (int gi = 0; gi < 3 && !r; gi++) {
            float mv[16], tr[16], rz[16];
            float deg = gear_def[gi].speed * angle + gear_def[gi].phase;
            dm_translate(tr, gear_def[gi].x, gear_def[gi].y, 0.0f);
            dm_rotate(rz, deg * DEMO_PI / 180.0f, 0.0f, 0.0f, 1.0f);
            dm_mul(mv, view, tr);
            dm_mul(mv, mv, rz);
            r |= gls_put(&g, GPU_CMD_GL_MATERIALFV,
                         GLS_A(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, 0), gear_def[gi].color, 16);
            r |= gls_put(&g, GPU_CMD_GL_LOAD_MATRIXF, GLS_A(0), mv, sizeof(mv));
            r |= gls_put(&g, GPU_CMD_GL_DRAW_ARRAYS, GLS_A(GL_TRIANGLES, gear_first[gi], gear_count[gi]),
                         NULL, 0);
        }
        if (r) break;
        if (zc.n) {
            if (gls_zc_show(&g, &pt, &zc)) break;
        } else {
            if (gls_readback(&g, pt.width, pt.height)) break;
            gpu_present_frame(&pt, (const uint32_t *)mesa_pix_data(), pt.width, pt.height);
        }
        frames++;
    }
    uint64_t elapsed = msnow() - t0;
    if (!rc && !frames && g.err[0]) {
        rc = GLS_ERR;
        why = "frame";
    }

    gls_zc_free(&g, &zc);
    gls_free_fbo(&g, &fb);
    gls_delete(&g, GPU_CMD_GL_DELETE_BUFFERS, &vbo);
    gls_reset_state(&g);
    kfree(mesh);

    gpu_present_close(&pt);
    if (frames_out) *frames_out = frames;
    if (fps_out) *fps_out = gls_fps(frames, elapsed);
    if (rc) {
        LOG_WARNING("glxgears(GL): failed at %s: %s", why, g.err);
        if (opts->progress) {
            char line[200];
            snprintf(line, sizeof(line), "  glxgears(GL) failed (%s: %s) - see dmesg", why, g.err);
            opts->progress(opts->user, line);
        }
    } else if (g.err[0]) {
        LOG_WARNING("glxgears(GL): stopped after %u frames: %s", frames, g.err);
    }
    gpu_client_close(g.cl);
    return rc ? GLS_ERR : GLS_OK;
}

static uint32_t shader_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ 0xFFFFFFFFu;
}

static const char test_vs[] =
    "#version 120\n"
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char test_fs[] =
    "#version 120\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = vec4(uv.x, uv.y, 0.5 + 0.5 * sin(uv.x * 6.28), 1.0); }\n";

static const float test_verts[] = {
    -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
};

int gpu_shader_test_gl(uint32_t *crc_out, char *note, int note_cap) {
    int ms = mesa_start();
    if (ms) return ms;
    struct gls g;
    memset(&g, 0, sizeof(g));
    g.cl = gpu_client_open(0, GPU_API_OPENGL);
    if (g.cl < 0) return GPU_ERR_BUSY;

    uint32_t vs = 0, fs = 0, prog = 0, vbo = 0;
    struct gls_fbo fb;
    memset(&fb, 0, sizeof(fb));
    int rc = GLS_OK;
    const uint32_t w = 128, h = 128;
    if (gls_reset_state(&g)) rc = GLS_ERR;
    if (!rc && gls_shader(&g, GL_VERTEX_SHADER, test_vs, &vs)) rc = GLS_ERR;
    if (!rc && gls_shader(&g, GL_FRAGMENT_SHADER, test_fs, &fs)) rc = GLS_ERR;
    if (!rc && gls_program(&g, vs, fs, "pos", 1, &prog)) rc = GLS_ERR;
    if (!rc && gls_gen(&g, GPU_CMD_GL_GEN_BUFFERS, &vbo)) rc = GLS_ERR;
    if (!rc && gls_one(&g, GPU_CMD_GL_BIND_BUFFER, GLS_A(GL_ARRAY_BUFFER, vbo))) rc = GLS_ERR;
    if (!rc && gls_upload(&g, test_verts, sizeof(test_verts))) rc = GLS_ERR;
    if (!rc && gls_setup_fbo(&g, w, h, &fb)) rc = GLS_ERR;
    if (!rc) {
        int r = gpu_cmd_begin(g.cl);
        r |= gls_put(&g, GPU_CMD_GL_CLEAR, GLS_A(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_USE_PROGRAM, GLS_A(prog), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_ENABLE_VERTEX_ATTRIB_ARRAY, GLS_A(0), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_VERTEX_ATTRIB_POINTER, GLS_A(0, 2, GL_FLOAT, 0, 8, 0), NULL, 0);
        r |= gls_put(&g, GPU_CMD_GL_DRAW_ARRAYS, GLS_A(GL_TRIANGLES, 0, 6), NULL, 0);
        if (r || gls_readback(&g, w, h)) rc = GLS_ERR;
    }
    if (!rc && crc_out) *crc_out = shader_crc32(mesa_pix_data(), w * h * 4);
    if (!rc && note) snprintf(note, note_cap, "GLSL vertex+fragment linked, 128x128 gradient");
    if (rc) LOG_WARNING("gpu-shader(GL): %s", g.err);

    gls_free_fbo(&g, &fb);
    gls_delete(&g, GPU_CMD_GL_DELETE_BUFFERS, &vbo);
    if (prog) gls_one(&g, GPU_CMD_GL_DELETE_PROGRAM, GLS_A(prog));
    if (vs) gls_one(&g, GPU_CMD_GL_DELETE_SHADER, GLS_A(vs));
    if (fs) gls_one(&g, GPU_CMD_GL_DELETE_SHADER, GLS_A(fs));
    gls_reset_state(&g);
    gpu_client_close(g.cl);
    return rc ? GLS_ERR : GLS_OK;
}
