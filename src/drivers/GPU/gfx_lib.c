// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_gl_defs.h"
#include "drivers/GPU/mesa_vk_defs.h"
#include "drivers/GPU/mesa_vk_pack.h"

#include "components/Memory/heap.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static struct gfx_gl G = {.cl = -1, .im_mode = -1};
static int vk_cl = -1;
static uint8_t *vk_pk_buf;
static float *im_pos, *im_col, *im_nrm, *im_tex;

static void gfx_fail(const char *fmt, ...) {
    if (G.err[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(G.err, sizeof(G.err), fmt, ap);
    va_end(ap);
}

int gfx_gl_client(void) { return G.cl; }

bool gfx_gl_ready(void) { return G.cl >= 0; }

uint32_t gfx_gl_width(void) { return G.w; }

uint32_t gfx_gl_height(void) { return G.h; }

const char *gfx_gl_error(void) { return G.err; }

int gfx_gl_flush(uint32_t timeout_ms) {
    if (G.cl < 0) return GFX_ERR_STATE;
    int r = gpu_flush(G.cl, timeout_ms);
    if (r) gfx_fail("gpu_flush: %d", r);
    return r ? GFX_ERR_FAIL : GFX_ERR_NONE;
}

int gfx_gl_result(uint16_t idx, void *out, uint16_t cap, uint16_t *out_len) {
    if (G.cl < 0) return GFX_ERR_STATE;
    return gpu_result(G.cl, idx, out, cap, out_len);
}

int gfx_gl_simple_op(uint16_t op, int argc, const uint64_t *args) {
    if (G.cl < 0) return GFX_ERR_STATE;
    if (gpu_cmd_begin(G.cl)) return GFX_ERR_STATE;
    int r = gpu_cmd_gl(G.cl, op, (uint8_t)argc, args, NULL, 0, 0);
    if (r) {
        gfx_fail("cmd 0x%x encode: %d", op, r);
        return GFX_ERR_FAIL;
    }
    return gfx_gl_flush(15000);
}

int gfx_gl_simple(int argc, const uint64_t *args) { return gfx_gl_simple_op(0, argc, args); }

int gfx_gl_payload(uint16_t op, int argc, const uint64_t *args, const void *payload,
                   uint16_t plen, uint16_t cap) {
    if (G.cl < 0) return GFX_ERR_STATE;
    if (gpu_cmd_begin(G.cl)) return GFX_ERR_STATE;
    int r = gpu_cmd_gl(G.cl, op, (uint8_t)argc, args, payload, plen, cap);
    if (r) {
        gfx_fail("cmd 0x%x encode: %d", op, r);
        return GFX_ERR_FAIL;
    }
    return gfx_gl_flush(15000);
}

uint64_t gfx_gl_query(uint16_t op, int argc, const uint64_t *args, uint16_t cap,
                      uint16_t *out_len, uint8_t *rbuf, uint16_t rbuf_cap) {
    if (G.cl < 0) return 0;
    uint16_t use_cap = cap ? (uint16_t)(cap + 8) : 8;
    if (use_cap > rbuf_cap) use_cap = rbuf_cap;
    if (gpu_cmd_begin(G.cl)) return 0;
    if (gpu_cmd_gl(G.cl, op, (uint8_t)argc, args, NULL, 0, use_cap)) {
        gfx_fail("query 0x%x encode", op);
        return 0;
    }
    if (gfx_gl_flush(15000)) return 0;
    uint16_t len = 0;
    memset(rbuf, 0, rbuf_cap);
    if (gpu_result(G.cl, 0, rbuf, use_cap, &len)) {
        gfx_fail("query 0x%x result", op);
        return 0;
    }
    if (out_len) *out_len = len;
    uint64_t v = 0;
    memcpy(&v, rbuf, len < 8 ? len : 8);
    return v;
}

static uint64_t gfx_f32(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint64_t gfx_f64(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

int gfx_gl_reset_client(int client) {
    static const uint32_t caps_off[] = {
        GL_DEPTH_TEST, GL_CULL_FACE, GL_BLEND,        GL_SCISSOR_TEST, GL_LIGHTING,
        GL_LIGHT0,     GL_TEXTURE_2D, GL_COLOR_MATERIAL, GL_NORMALIZE,
    };
    static const uint32_t client_arrays[] = {GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY,
                                             GL_TEXTURE_COORD_ARRAY};
    struct {
        uint16_t op;
        uint8_t argc;
        uint64_t a[4];
    } cmds[64];
    int n = 0;
#define GFX_RESET_CMD(o, c, ...) \
    do { cmds[n].op = (o); cmds[n].argc = (c); memcpy(cmds[n].a, (uint64_t[4]){__VA_ARGS__}, sizeof(cmds[n].a)); n++; } while (0)
    GFX_RESET_CMD(GPU_CMD_GL_BIND_FRAMEBUFFER, 2, GL_FRAMEBUFFER, 0);
    GFX_RESET_CMD(GPU_CMD_GL_BIND_RENDERBUFFER, 2, GL_RENDERBUFFER, 0);
    GFX_RESET_CMD(GPU_CMD_GL_USE_PROGRAM, 1, 0);
    GFX_RESET_CMD(GPU_CMD_GL_BIND_BUFFER, 2, GL_ARRAY_BUFFER, 0);
    GFX_RESET_CMD(GPU_CMD_GL_BIND_BUFFER, 2, GL_ELEMENT_ARRAY_BUFFER, 0);
    GFX_RESET_CMD(GPU_CMD_GL_ACTIVE_TEXTURE, 1, GL_TEXTURE0);
    GFX_RESET_CMD(GPU_CMD_GL_BIND_TEXTURE, 2, GL_TEXTURE_2D, 0);
    for (uint32_t i = 0; i < 16; i++) GFX_RESET_CMD(GPU_CMD_GL_DISABLE_VERTEX_ATTRIB_ARRAY, 1, i);
    for (size_t i = 0; i < sizeof(client_arrays) / sizeof(client_arrays[0]); i++)
        GFX_RESET_CMD(GPU_CMD_GL_DISABLE_CLIENT_STATE, 1, client_arrays[i]);
    for (size_t i = 0; i < sizeof(caps_off) / sizeof(caps_off[0]); i++)
        GFX_RESET_CMD(GPU_CMD_GL_DISABLE, 1, caps_off[i]);
    GFX_RESET_CMD(GPU_CMD_GL_FRONT_FACE, 1, GL_CCW);
    GFX_RESET_CMD(GPU_CMD_GL_DEPTH_FUNC, 1, GL_LESS);
    GFX_RESET_CMD(GPU_CMD_GL_DEPTH_MASK, 1, 1);
    GFX_RESET_CMD(GPU_CMD_GL_COLOR_MASK, 4, 1, 1, 1, 1);
    GFX_RESET_CMD(GPU_CMD_GL_POLYGON_MODE, 2, GL_FRONT_AND_BACK, GL_FILL);
    GFX_RESET_CMD(GPU_CMD_GL_SHADE_MODEL, 1, GL_SMOOTH);
    GFX_RESET_CMD(GPU_CMD_GL_CLEAR_COLOR, 4, gfx_f32(0.0f), gfx_f32(0.0f), gfx_f32(0.0f), gfx_f32(0.0f));
    GFX_RESET_CMD(GPU_CMD_GL_CLEAR_DEPTH, 1, gfx_f64(1.0));
    GFX_RESET_CMD(GPU_CMD_GL_COLOR4F, 4, gfx_f32(1.0f), gfx_f32(1.0f), gfx_f32(1.0f), gfx_f32(1.0f));
    GFX_RESET_CMD(GPU_CMD_GL_MATRIX_MODE, 1, GL_TEXTURE);
    GFX_RESET_CMD(GPU_CMD_GL_LOAD_IDENTITY, 0, 0);
    GFX_RESET_CMD(GPU_CMD_GL_MATRIX_MODE, 1, GL_PROJECTION);
    GFX_RESET_CMD(GPU_CMD_GL_LOAD_IDENTITY, 0, 0);
    GFX_RESET_CMD(GPU_CMD_GL_MATRIX_MODE, 1, GL_MODELVIEW);
    GFX_RESET_CMD(GPU_CMD_GL_LOAD_IDENTITY, 0, 0);
#undef GFX_RESET_CMD
    if (gpu_cmd_begin(client)) return GFX_ERR_STATE;
    for (int i = 0; i < n; i++) {
        if (gpu_cmd_gl(client, cmds[i].op, cmds[i].argc, cmds[i].a, NULL, 0, 0)) return GFX_ERR_FAIL;
    }
    return gpu_flush(client, 15000) ? GFX_ERR_FAIL : GFX_ERR_NONE;
}

int gfx_gl_start(int dev) {
    if (G.cl >= 0) return GFX_ERR_NONE;
    int r = mesa_start();
    if (r) {
        gfx_fail("mesa_start: %d", r);
        return GFX_ERR_FAIL;
    }
    int cl = gpu_client_open(dev >= 0 ? dev : 0, GPU_API_OPENGL);
    if (cl < 0) {
        gfx_fail("gpu_client_open: %d", cl);
        return GFX_ERR_FAIL;
    }
    G.cl = cl;
    G.dev = dev >= 0 ? dev : 0;
    G.err[0] = 0;
    if (gfx_gl_reset_client(cl)) gfx_fail("GL state reset failed");
    return GFX_ERR_NONE;
}

void gfx_gl_shutdown(void) {
    if (G.cl >= 0) {
        gfx_gl_reset_client(G.cl);
        gpu_client_close(G.cl);
        G.cl = -1;
    }
    G.w = G.h = 0;
    G.fbo = G.tex = G.depth = G.vbo = 0;
    G.vbo_col = G.vbo_nrm = G.vbo_tex = 0;
    memset(G.attr_vbo, 0, sizeof(G.attr_vbo));
    G.im_count = 0;
    G.im_mode = -1;
}

uint64_t gfx_gl_gen(uint16_t op, uint32_t count) {
    uint8_t rbuf[64];
    uint64_t args[6] = {count, 0};
    uint16_t len = 0;
    memset(rbuf, 0, sizeof(rbuf));
    gfx_gl_query(op, 2, args, 8, &len, rbuf, sizeof(rbuf));
    if (len < 4 && !rbuf[0] && !rbuf[1]) return 0;
    uint32_t id = 0;
    memcpy(&id, rbuf, 4);
    return id;
}

static void gfx_gl_delete(uint16_t op, uint64_t *name) {
    if (!*name) return;
    uint32_t id = (uint32_t)*name;
    uint64_t args[6] = {1, 0};
    gfx_gl_payload(op, 2, args, &id, sizeof(id), 0);
    *name = 0;
}

static uint32_t gfx_gl_get_int(uint32_t pname) {
    uint8_t rbuf[32];
    uint64_t args[6] = {pname, 0};
    uint16_t len = 0;
    memset(rbuf, 0, sizeof(rbuf));
    gfx_gl_query(GPU_CMD_GL_GET_INTEGERV, 2, args, 8, &len, rbuf, sizeof(rbuf));
    uint32_t v = 0;
    memcpy(&v, rbuf, 4);
    return v;
}

static int gfx_gl_bind_array(uint64_t buf) {
    uint64_t args[6] = {GL_ARRAY_BUFFER, buf};
    return gfx_gl_simple_op(GPU_CMD_GL_BIND_BUFFER, 2, args);
}

int gfx_gl_string(uint32_t name, char *out, int cap) {
    uint8_t rbuf[GFX_MAX_TEXT + 16];
    uint64_t args[6] = {name};
    uint16_t len = 0;
    memset(rbuf, 0, sizeof(rbuf));
    gfx_gl_query(GPU_CMD_GL_GET_STRING, 1, args, GFX_MAX_TEXT, &len, rbuf, sizeof(rbuf));
    if (!len) {
        out[0] = 0;
        return GFX_ERR_FAIL;
    }
    int n = len < cap - 1 ? (int)len : cap - 1;
    if (n < 0) n = 0;
    memcpy(out, rbuf, (size_t)n);
    out[n] = 0;
    for (int i = 0; i < n; i++)
        if (!out[i]) break;
    return GFX_ERR_NONE;
}

int gfx_gl_get_error(void) {
    uint8_t rbuf[16];
    uint64_t v = gfx_gl_query(GPU_CMD_GL_GET_ERROR, 0, NULL, 8, NULL, rbuf, sizeof(rbuf));
    return (int)(uint32_t)v;
}

int gfx_gl_finish(void) {
    return gfx_gl_simple_op(GPU_CMD_GL_FINISH, 0, NULL);
}

int gfx_gl_tex_image_2d(uint32_t target, int level, uint32_t internal, uint32_t w, uint32_t h,
                        uint32_t format, uint32_t type, const uint8_t *data, uint32_t data_len) {
    if (G.cl < 0) return GFX_ERR_STATE;
    uint8_t payload[GPU_BATCH_PAYLOAD];
    uint32_t n_extra = 3;
    uint32_t data_off = 8 * n_extra;
    if (data_len + data_off > sizeof(payload)) {
        gfx_fail("texture too large: %u bytes", data_len);
        return GFX_ERR_PARAM;
    }
    memset(payload, 0, data_off);
    uint64_t extra[3] = {format, type, data ? 0 : 0xFFFE};
    memcpy(payload, extra, sizeof(extra));
    if (data && data_len) memcpy(payload + data_off, data, data_len);
    uint64_t args[6] = {target, (uint32_t)level, internal, w, h, 0};
    return gfx_gl_payload(GPU_CMD_GL_TEX_IMAGE2D, 9, args, payload,
                          (uint16_t)(data_off + (data ? data_len : 0)), 0);
}

int gfx_gl_buffer_data(uint32_t target, const void *data, uint32_t len, uint32_t usage) {
    if (G.cl < 0) return GFX_ERR_STATE;
    if (len > GPU_BATCH_PAYLOAD) {
        gfx_fail("buffer too large: %u bytes", len);
        return GFX_ERR_PARAM;
    }
    uint64_t args[6] = {target, len, 0, usage};
    return gfx_gl_payload(GPU_CMD_GL_BUFFER_DATA, 4, args, data, (uint16_t)len, 0);
}

uint64_t gfx_gl_create_shader(uint32_t type, const char *src, char *log, int log_cap) {
    if (G.cl < 0) return 0;
    if (log) log[0] = 0;
    uint8_t rbuf[64];
    uint64_t a1[6] = {type};
    uint64_t id = gfx_gl_query(GPU_CMD_GL_CREATE_SHADER, 1, a1, 8, NULL, rbuf, sizeof(rbuf));
    if (!id) {
        gfx_fail("glCreateShader(0x%x)", type);
        return 0;
    }
    size_t srclen = strlen(src);
    uint8_t payload[GPU_BATCH_PAYLOAD];
    if (srclen + 16 > sizeof(payload)) {
        gfx_fail("shader source too long: %zu", srclen);
        return 0;
    }
    memset(payload, 0, sizeof(payload));
    uint32_t strp = 8;
    memcpy(payload, &strp, 4);
    memcpy(payload + 8, src, srclen + 1);
    uint64_t a2[6] = {id, 1, 0, 0xFFFE};
    if (gfx_gl_payload(GPU_CMD_GL_SHADER_SOURCE, 4, a2, payload, (uint16_t)(8 + srclen + 1), 0))
        return 0;
    uint64_t a3[6] = {id};
    if (gfx_gl_simple_op(GPU_CMD_GL_COMPILE_SHADER, 1, a3)) return 0;
    uint64_t ok = 0;
    uint64_t a4[6] = {id, GL_COMPILE_STATUS, 0};
    gfx_gl_query(GPU_CMD_GL_GET_SHADERIV, 3, a4, 8, NULL, rbuf, sizeof(rbuf));
    memcpy(&ok, rbuf, 8);
    if (!ok) {
        if (log) {
            uint64_t loglen = 0;
            uint64_t a5[6] = {id, GL_INFO_LOG_LENGTH, 0};
            gfx_gl_query(GPU_CMD_GL_GET_SHADERIV, 3, a5, 8, NULL, rbuf, sizeof(rbuf));
            memcpy(&loglen, rbuf, 8);
            if (loglen > 1) {
                uint64_t a6[6] = {id, 1023, 0xFFFE, 0};
                uint16_t llen = 0;
                uint8_t lbuf[1032];
                memset(lbuf, 0, sizeof(lbuf));
                gfx_gl_query(GPU_CMD_GL_GET_SHADER_INFO_LOG, 4, a6, 1024, &llen, lbuf,
                             (uint16_t)(sizeof(lbuf) - 8));
                int n = (int)llen;
                if (n > log_cap - 1) n = log_cap - 1;
                if (n > 0) memcpy(log, lbuf, (size_t)n);
                log[n > 0 ? n : 0] = 0;
            }
        }
        gfx_fail("shader 0x%llx compile failed", (unsigned long long)id);
        return 0;
    }
    return id;
}

int64_t gfx_gl_uniform_location(uint64_t prog, const char *name) {
    if (G.cl < 0) return -1;
    uint8_t payload[256];
    uint8_t rbuf[32];
    size_t nlen = strlen(name);
    if (nlen + 1 > sizeof(payload)) return -1;
    memset(payload, 0, sizeof(payload));
    memcpy(payload, name, nlen + 1);
    uint64_t args[6] = {prog, 0};
    if (gpu_cmd_begin(G.cl)) return -1;
    if (gpu_cmd_gl(G.cl, GPU_CMD_GL_GET_UNIFORM_LOCATION, 2, args, payload,
                   (uint16_t)(nlen + 1), 8)) return -1;
    if (gfx_gl_flush(15000)) return -1;
    uint16_t len = 0;
    memset(rbuf, 0, sizeof(rbuf));
    if (gpu_result(G.cl, 0, rbuf, 8, &len)) return -1;
    if (len < 4) return -1;
    int32_t loc = -1;
    memcpy(&loc, rbuf, 4);
    return loc;
}

uint64_t gfx_gl_create_program(uint64_t vs, uint64_t fs, const char *const *attribs,
                               int attrib_count, char *log, int log_cap) {
    if (G.cl < 0) return 0;
    if (log) log[0] = 0;
    uint8_t rbuf[64];
    uint64_t prog = gfx_gl_query(GPU_CMD_GL_CREATE_PROGRAM, 0, NULL, 8, NULL, rbuf, sizeof(rbuf));
    if (!prog) {
        gfx_fail("glCreateProgram");
        return 0;
    }
    uint64_t aa[6] = {prog, vs};
    gfx_gl_simple_op(GPU_CMD_GL_ATTACH_SHADER, 2, aa);
    uint64_t ab[6] = {prog, fs};
    gfx_gl_simple_op(GPU_CMD_GL_ATTACH_SHADER, 2, ab);
    for (int i = 0; i < attrib_count && attribs && attribs[i]; i++) {
        size_t nlen = strlen(attribs[i]);
        if (nlen > 120) continue;
        uint8_t payload[136];
        memset(payload, 0, sizeof(payload));
        memcpy(payload, attribs[i], nlen + 1);
        uint64_t ac[6] = {prog, (uint64_t)i, 0};
        gfx_gl_payload(GPU_CMD_GL_BIND_ATTRIB_LOCATION, 3, ac, payload, (uint16_t)(nlen + 1), 0);
    }
    uint64_t ad[6] = {prog};
    gfx_gl_simple_op(GPU_CMD_GL_LINK_PROGRAM, 1, ad);
    uint64_t ok = 0;
    uint64_t ae[6] = {prog, GL_LINK_STATUS, 0};
    gfx_gl_query(GPU_CMD_GL_GET_PROGRAMIV, 3, ae, 8, NULL, rbuf, sizeof(rbuf));
    memcpy(&ok, rbuf, 8);
    if (!ok) {
        if (log) {
            uint64_t loglen = 0;
            uint64_t af[6] = {prog, GL_INFO_LOG_LENGTH, 0};
            gfx_gl_query(GPU_CMD_GL_GET_PROGRAMIV, 3, af, 8, NULL, rbuf, sizeof(rbuf));
            memcpy(&loglen, rbuf, 8);
            if (loglen > 1) {
                uint64_t ag[6] = {prog, 1023, 0xFFFE, 0};
                uint16_t llen = 0;
                uint8_t lbuf[1032];
                memset(lbuf, 0, sizeof(lbuf));
                gfx_gl_query(GPU_CMD_GL_GET_PROGRAM_INFO_LOG, 4, ag, 1024, &llen, lbuf,
                             (uint16_t)(sizeof(lbuf) - 8));
                int n = (int)llen;
                if (n > log_cap - 1) n = log_cap - 1;
                if (n > 0) memcpy(log, lbuf, (size_t)n);
                log[n > 0 ? n : 0] = 0;
            }
        }
        gfx_fail("program link failed");
        return 0;
    }
    return prog;
}

int gfx_gl_uniform_matrix(uint16_t op, int64_t loc, const float *m, uint32_t count) {
    if (G.cl < 0) return GFX_ERR_STATE;
    uint64_t args[6] = {(uint64_t)loc, count, 0};
    return gfx_gl_payload(op, 3, args, m, (uint16_t)(16 * sizeof(float) * count), 0);
}

int gfx_gl_uniformv(uint16_t op, int64_t loc, const float *v, uint32_t comps) {
    if (G.cl < 0) return GFX_ERR_STATE;
    uint64_t args[6] = {(uint64_t)loc, (uint64_t)comps};
    return gfx_gl_payload(op, 2, args, v, (uint16_t)(comps * sizeof(float)), 0);
}

int gfx_gl_attrib_pointer(uint32_t index, uint32_t size, uint32_t type, uint32_t normalized,
                          uint32_t stride, uint64_t offset) {
    uint64_t args[6] = {index, size, type, normalized, stride, offset};
    return gfx_gl_simple_op(GPU_CMD_GL_VERTEX_ATTRIB_POINTER, 6, args);
}

int gfx_gl_attrib_data(uint32_t index, uint32_t size, uint32_t type, uint32_t normalized,
                       uint32_t stride, const void *data, uint32_t len) {
    if (G.cl < 0) return GFX_ERR_STATE;
    if (index >= GFX_ATTR_MAX) {
        gfx_fail("attribute index %u out of range", index);
        return GFX_ERR_PARAM;
    }
    if (!G.attr_vbo[index]) {
        G.attr_vbo[index] = gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, 1);
        if (!G.attr_vbo[index]) return GFX_ERR_FAIL;
    }
    uint32_t prev = gfx_gl_get_int(GL_ARRAY_BUFFER_BINDING);
    int r = gfx_gl_bind_array(G.attr_vbo[index]);
    if (!r) r = gfx_gl_buffer_data(GL_ARRAY_BUFFER, data, len, GL_DYNAMIC_DRAW);
    if (!r) r = gfx_gl_attrib_pointer(index, size, type, normalized, stride, 0);
    gfx_gl_bind_array(prev);
    return r;
}

static int gl_im_alloc(void) {
    if (G.im_buf) return GFX_ERR_NONE;
    G.im_buf = kmalloc(GFX_IM_MAX_VERT * GFX_IM_STRIDE * sizeof(float));
    im_pos = kmalloc(GFX_IM_MAX_VERT * 3 * sizeof(float));
    im_col = kmalloc(GFX_IM_MAX_VERT * 4 * sizeof(float));
    im_nrm = kmalloc(GFX_IM_MAX_VERT * 3 * sizeof(float));
    im_tex = kmalloc(GFX_IM_MAX_VERT * 2 * sizeof(float));
    if (!G.im_buf || !im_pos || !im_col || !im_nrm || !im_tex) return GFX_ERR_NOMEM;
    return GFX_ERR_NONE;
}

bool gfx_im_active(void) { return G.im_mode >= 0; }

int gfx_im_begin(int mode) {
    if (G.cl < 0) return GFX_ERR_STATE;
    if (G.im_mode >= 0) {
        gfx_fail("begin inside begin");
        return GFX_ERR_STATE;
    }
    if (gl_im_alloc()) return GFX_ERR_NOMEM;
    G.im_mode = (int)mode;
    G.im_count = 0;
    G.im_any_color = false;
    G.im_any_normal = false;
    G.im_any_tex = false;
    return GFX_ERR_NONE;
}

int gfx_im_color(float r, float g, float b, float a) {
    if (G.im_mode < 0) return GFX_ERR_STATE;
    G.cur_color[0] = r;
    G.cur_color[1] = g;
    G.cur_color[2] = b;
    G.cur_color[3] = a;
    G.im_any_color = true;
    return GFX_ERR_NONE;
}

int gfx_im_normal(float x, float y, float z) {
    if (G.im_mode < 0) return GFX_ERR_STATE;
    G.cur_normal[0] = x;
    G.cur_normal[1] = y;
    G.cur_normal[2] = z;
    G.im_any_normal = true;
    return GFX_ERR_NONE;
}

int gfx_im_texcoord(float s, float t) {
    if (G.im_mode < 0) return GFX_ERR_STATE;
    G.cur_tex[0] = s;
    G.cur_tex[1] = t;
    G.im_any_tex = true;
    return GFX_ERR_NONE;
}

int gfx_im_vertex(float x, float y, float z, float w) {
    if (G.im_mode < 0) return GFX_ERR_STATE;
    if (G.im_count >= GFX_IM_MAX_VERT) {
        gfx_fail("too many vertices in one begin/end");
        return GFX_ERR_PARAM;
    }
    float *p = G.im_buf + (size_t)G.im_count * GFX_IM_STRIDE;
    p[0] = x;
    p[1] = y;
    p[2] = z;
    p[3] = w;
    memcpy(p + 4, G.cur_color, 4 * sizeof(float));
    memcpy(p + 8, G.cur_normal, 3 * sizeof(float));
    memcpy(p + 11, G.cur_tex, 2 * sizeof(float));
    G.im_count++;
    return GFX_ERR_NONE;
}

int gfx_im_end(void) {
    if (G.im_mode < 0) return GFX_ERR_STATE;
    int mode = G.im_mode;
    uint32_t count = G.im_count;
    G.im_mode = -1;
    if (!count) return GFX_ERR_NONE;
    for (uint32_t i = 0; i < count; i++) {
        const float *p = G.im_buf + (size_t)i * GFX_IM_STRIDE;
        memcpy(im_pos + (size_t)i * 3, p, 3 * sizeof(float));
        if (G.im_any_color) memcpy(im_col + (size_t)i * 4, p + 4, 4 * sizeof(float));
        if (G.im_any_normal) memcpy(im_nrm + (size_t)i * 3, p + 8, 3 * sizeof(float));
        if (G.im_any_tex) memcpy(im_tex + (size_t)i * 2, p + 11, 2 * sizeof(float));
    }
    if (!G.vbo) {
        uint64_t id = gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, 1);
        if (!id) return GFX_ERR_FAIL;
        G.vbo = id;
    }
    if (G.im_any_color && !G.vbo_col) {
        uint64_t id = gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, 1);
        if (!id) return GFX_ERR_FAIL;
        G.vbo_col = id;
    }
    if (G.im_any_normal && !G.vbo_nrm) {
        uint64_t id = gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, 1);
        if (!id) return GFX_ERR_FAIL;
        G.vbo_nrm = id;
    }
    if (G.im_any_tex && !G.vbo_tex) {
        uint64_t id = gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, 1);
        if (!id) return GFX_ERR_FAIL;
        G.vbo_tex = id;
    }
    uint32_t prev_array = gfx_gl_get_int(GL_ARRAY_BUFFER_BINDING);
    uint64_t bindp[6] = {GL_ARRAY_BUFFER, G.vbo};
    if (gfx_gl_simple_op(GPU_CMD_GL_BIND_BUFFER, 2, bindp)) return GFX_ERR_FAIL;
    if (gfx_gl_buffer_data(GL_ARRAY_BUFFER, im_pos, count * 12, GL_DYNAMIC_DRAW)) return GFX_ERR_FAIL;
    uint64_t vp[6] = {3, GL_FLOAT, 0, 0};
    if (gfx_gl_simple_op(GPU_CMD_GL_VERTEX_POINTER, 4, vp)) return GFX_ERR_FAIL;
    if (G.im_any_color) {
        uint64_t bindc[6] = {GL_ARRAY_BUFFER, G.vbo_col};
        if (gfx_gl_simple_op(GPU_CMD_GL_BIND_BUFFER, 2, bindc)) return GFX_ERR_FAIL;
        if (gfx_gl_buffer_data(GL_ARRAY_BUFFER, im_col, count * 16, GL_DYNAMIC_DRAW))
            return GFX_ERR_FAIL;
        uint64_t cp[6] = {4, GL_FLOAT, 0, 0};
        if (gfx_gl_simple_op(GPU_CMD_GL_COLOR_POINTER, 4, cp)) return GFX_ERR_FAIL;
    }
    if (G.im_any_normal) {
        uint64_t bindn[6] = {GL_ARRAY_BUFFER, G.vbo_nrm};
        if (gfx_gl_simple_op(GPU_CMD_GL_BIND_BUFFER, 2, bindn)) return GFX_ERR_FAIL;
        if (gfx_gl_buffer_data(GL_ARRAY_BUFFER, im_nrm, count * 12, GL_DYNAMIC_DRAW))
            return GFX_ERR_FAIL;
        uint64_t np[6] = {GL_FLOAT, 0, 0};
        if (gfx_gl_simple_op(GPU_CMD_GL_NORMAL_POINTER, 3, np)) return GFX_ERR_FAIL;
    }
    if (G.im_any_tex) {
        uint64_t bindt[6] = {GL_ARRAY_BUFFER, G.vbo_tex};
        if (gfx_gl_simple_op(GPU_CMD_GL_BIND_BUFFER, 2, bindt)) return GFX_ERR_FAIL;
        if (gfx_gl_buffer_data(GL_ARRAY_BUFFER, im_tex, count * 8, GL_DYNAMIC_DRAW))
            return GFX_ERR_FAIL;
        uint64_t tp[6] = {2, GL_FLOAT, 0, 0};
        if (gfx_gl_simple_op(GPU_CMD_GL_TEX_COORD_POINTER, 4, tp)) return GFX_ERR_FAIL;
    }
    uint64_t ec[6] = {GL_VERTEX_ARRAY};
    gfx_gl_simple_op(GPU_CMD_GL_ENABLE_CLIENT_STATE, 1, ec);
    if (G.im_any_color) {
        uint64_t e[6] = {GL_COLOR_ARRAY};
        gfx_gl_simple_op(GPU_CMD_GL_ENABLE_CLIENT_STATE, 1, e);
    }
    if (G.im_any_normal) {
        uint64_t e[6] = {GL_NORMAL_ARRAY};
        gfx_gl_simple_op(GPU_CMD_GL_ENABLE_CLIENT_STATE, 1, e);
    }
    if (G.im_any_tex) {
        uint64_t e[6] = {GL_TEXTURE_COORD_ARRAY};
        gfx_gl_simple_op(GPU_CMD_GL_ENABLE_CLIENT_STATE, 1, e);
    }
    uint64_t da[6] = {(uint64_t)mode, 0, count};
    int r = gfx_gl_simple_op(GPU_CMD_GL_DRAW_ARRAYS, 3, da);
    uint64_t dc[6] = {GL_VERTEX_ARRAY};
    gfx_gl_simple_op(GPU_CMD_GL_DISABLE_CLIENT_STATE, 1, dc);
    if (G.im_any_color) {
        uint64_t e[6] = {GL_COLOR_ARRAY};
        gfx_gl_simple_op(GPU_CMD_GL_DISABLE_CLIENT_STATE, 1, e);
    }
    if (G.im_any_normal) {
        uint64_t e[6] = {GL_NORMAL_ARRAY};
        gfx_gl_simple_op(GPU_CMD_GL_DISABLE_CLIENT_STATE, 1, e);
    }
    if (G.im_any_tex) {
        uint64_t e[6] = {GL_TEXTURE_COORD_ARRAY};
        gfx_gl_simple_op(GPU_CMD_GL_DISABLE_CLIENT_STATE, 1, e);
    }
    gfx_gl_bind_array(prev_array);
    return r;
}

static int gfx_fbo_setup(uint32_t w, uint32_t h) {
    uint64_t tex = gfx_gl_gen(GPU_CMD_GL_GEN_TEXTURES, 1);
    if (!tex) return GFX_ERR_FAIL;
    G.tex = tex;
    uint64_t bt[6] = {GL_TEXTURE_2D, tex};
    if (gfx_gl_simple_op(GPU_CMD_GL_BIND_TEXTURE, 2, bt)) return GFX_ERR_FAIL;
    uint64_t t1[6] = {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST};
    gfx_gl_simple_op(GPU_CMD_GL_TEX_PARAMETERI, 3, t1);
    uint64_t t2[6] = {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST};
    gfx_gl_simple_op(GPU_CMD_GL_TEX_PARAMETERI, 3, t2);
    if (gfx_gl_tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL, 0))
        return GFX_ERR_FAIL;
    uint64_t fbo = gfx_gl_gen(GPU_CMD_GL_GEN_FRAMEBUFFERS, 1);
    if (!fbo) return GFX_ERR_FAIL;
    G.fbo = fbo;
    uint64_t bf[6] = {GL_FRAMEBUFFER, fbo};
    if (gfx_gl_simple_op(GPU_CMD_GL_BIND_FRAMEBUFFER, 2, bf)) return GFX_ERR_FAIL;
    uint64_t fa[6] = {GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0};
    if (gfx_gl_simple_op(GPU_CMD_GL_FRAMEBUFFER_TEXTURE2D, 5, fa)) return GFX_ERR_FAIL;
    uint64_t depth = gfx_gl_gen(GPU_CMD_GL_GEN_RENDERBUFFERS, 1);
    if (!depth) return GFX_ERR_FAIL;
    G.depth = depth;
    uint64_t br[6] = {GL_RENDERBUFFER, depth};
    if (gfx_gl_simple_op(GPU_CMD_GL_BIND_RENDERBUFFER, 2, br)) return GFX_ERR_FAIL;
    uint64_t rs[6] = {GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h};
    if (gfx_gl_simple_op(GPU_CMD_GL_RENDERBUFFER_STORAGE, 4, rs)) return GFX_ERR_FAIL;
    uint64_t fr[6] = {GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth};
    if (gfx_gl_simple_op(GPU_CMD_GL_FRAMEBUFFER_RENDERBUFFER, 4, fr)) return GFX_ERR_FAIL;
    uint8_t rbuf[16];
    uint64_t cs[6] = {GL_FRAMEBUFFER};
    uint64_t st = gfx_gl_query(GPU_CMD_GL_CHECK_FRAMEBUFFER_STATUS, 1, cs, 8, NULL, rbuf,
                               sizeof(rbuf));
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        gfx_fail("framebuffer incomplete: 0x%llx", (unsigned long long)st);
        return GFX_ERR_FAIL;
    }
    return GFX_ERR_NONE;
}

int gfx_gl_open(int w, int h) {
    if (G.cl < 0) {
        gfx_fail("no GL context");
        return GFX_ERR_STATE;
    }
    if (G.fbo) return GFX_ERR_NONE;
    if (w < 1 || h < 1) {
        gfx_fail("bad canvas size %dx%d", w, h);
        return GFX_ERR_PARAM;
    }
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = 0;
    opts.width = (uint16_t)(w > 65535 ? 65535 : w);
    opts.height = (uint16_t)(h > 65535 ? 65535 : h);
    gpu_present pt;
    int pr = gpu_present_open(&pt, &opts, (uint32_t)opts.width, (uint32_t)opts.height);
    if (pr == GPU_OK) {
        G.pt = pt;
        G.present_open = true;
        if (pt.width < (uint32_t)w) w = pt.width;
        if (pt.height < (uint32_t)h) h = pt.height;
    }
    G.w = (uint32_t)w;
    G.h = (uint32_t)h;
    uint64_t vp[6] = {0, 0, G.w, G.h};
    gfx_gl_simple_op(GPU_CMD_GL_VIEWPORT, 4, vp);
    uint64_t ps[6] = {GL_UNPACK_ALIGNMENT, 1};
    gfx_gl_simple_op(GPU_CMD_GL_PIXEL_STOREI, 2, ps);
    uint64_t ps2[6] = {GL_PACK_ALIGNMENT, 1};
    gfx_gl_simple_op(GPU_CMD_GL_PIXEL_STOREI, 2, ps2);
    if (gl_im_alloc()) return GFX_ERR_NOMEM;
    if (gfx_fbo_setup(G.w, G.h)) return GFX_ERR_FAIL;
    return GFX_ERR_NONE;
}

int gfx_gl_close(void) {
    if (G.present_open) {
        gpu_present_close(&G.pt);
        G.present_open = false;
    }
    if (G.fbo) {
        uint64_t bf[6] = {GL_FRAMEBUFFER, 0};
        gfx_gl_simple_op(GPU_CMD_GL_BIND_FRAMEBUFFER, 2, bf);
    }
    gfx_gl_delete(GPU_CMD_GL_DELETE_FRAMEBUFFERS, &G.fbo);
    gfx_gl_delete(GPU_CMD_GL_DELETE_RENDERBUFFERS, &G.depth);
    gfx_gl_delete(GPU_CMD_GL_DELETE_TEXTURES, &G.tex);
    gfx_gl_delete(GPU_CMD_GL_DELETE_BUFFERS, &G.vbo);
    gfx_gl_delete(GPU_CMD_GL_DELETE_BUFFERS, &G.vbo_col);
    gfx_gl_delete(GPU_CMD_GL_DELETE_BUFFERS, &G.vbo_nrm);
    gfx_gl_delete(GPU_CMD_GL_DELETE_BUFFERS, &G.vbo_tex);
    for (int i = 0; i < GFX_ATTR_MAX; i++) gfx_gl_delete(GPU_CMD_GL_DELETE_BUFFERS, &G.attr_vbo[i]);
    if (G.cl >= 0) gfx_gl_reset_client(G.cl);
    G.w = G.h = 0;
    return GFX_ERR_NONE;
}

int gfx_gl_read_pixels(int x, int y, int w, int h, uint8_t *out, uint32_t cap) {
    if (G.cl < 0) return GFX_ERR_STATE;
    uint32_t need = (uint32_t)w * (uint32_t)h * 4;
    if (!out || cap < need) {
        gfx_fail("readPixels buffer too small: %u < %u", cap, need);
        return GFX_ERR_PARAM;
    }
    if (need > MESA_PIX_MAX) {
        gfx_fail("readPixels too large: %u bytes", need);
        return GFX_ERR_PARAM;
    }
    uint64_t args[6] = {(uint64_t)(uint32_t)x, (uint64_t)(uint32_t)y, (uint64_t)(uint32_t)w,
                        (uint64_t)(uint32_t)h, GL_RGBA, GL_UNSIGNED_BYTE};
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    uint64_t marker = 0xFFFF;
    memcpy(payload, &marker, 8);
    int r = gfx_gl_payload(GPU_CMD_GL_READ_PIXELS, 7, args, payload, 8, 0);
    if (r) return r;
    uint64_t pixargs[6] = {need};
    r = gfx_gl_simple_op(GPU_CMD_MESA_PIX, 1, pixargs);
    if (r) return r;
    memcpy(out, mesa_pix_data(), need);
    return GFX_ERR_NONE;
}

int gfx_gl_present(void) {
    if (G.cl < 0 || !G.fbo) return GFX_ERR_STATE;
    uint8_t *pix = kmalloc((size_t)G.w * G.h * 4);
    if (!pix) return GFX_ERR_NOMEM;
    int r = gfx_gl_read_pixels(0, 0, (int)G.w, (int)G.h, pix, G.w * G.h * 4);
    if (r) {
        kfree(pix);
        return r;
    }
    if (G.present_open) {
        uint32_t *rows = (uint32_t *)pix;
        for (uint32_t y = 0; y < G.h / 2; y++) {
            uint32_t *a = rows + (size_t)y * G.w, *b = rows + (size_t)(G.h - 1 - y) * G.w;
            for (uint32_t x = 0; x < G.w; x++) {
                uint32_t t = a[x];
                a[x] = b[x];
                b[x] = t;
            }
        }
        gpu_present_frame(&G.pt, (const uint32_t *)pix, G.w, G.h);
        r = GFX_ERR_NONE;
    }
    kfree(pix);
    return r;
}

int gfx_vk_start(int dev) {
    if (vk_cl >= 0) return GFX_ERR_NONE;
    int r = mesa_start();
    if (r) {
        gfx_fail("mesa_start: %d", r);
        return GFX_ERR_FAIL;
    }
    int cl = gpu_client_open(dev >= 0 ? dev : 0, GPU_API_VULKAN);
    if (cl < 0) {
        gfx_fail("gpu_client_open(vk): %d", cl);
        return GFX_ERR_FAIL;
    }
    vk_cl = cl;
    G.err[0] = 0;
    if (!vk_pk_buf) {
        vk_pk_buf = kmalloc(GPU_BATCH_PAYLOAD);
        if (!vk_pk_buf) {
            gpu_client_close(vk_cl);
            vk_cl = -1;
            return GFX_ERR_NOMEM;
        }
    }
    return GFX_ERR_NONE;
}

int gfx_vk_client(void) { return vk_cl; }

bool gfx_vk_ready(void) { return vk_cl >= 0; }

void gfx_vk_shutdown(void) {
    if (vk_cl >= 0) {
        gpu_client_close(vk_cl);
        vk_cl = -1;
    }
}

const char *gfx_vk_error(void) { return G.err; }

int gfx_vk_flush(uint32_t timeout_ms) {
    if (vk_cl < 0) return GFX_ERR_STATE;
    int r = gpu_flush(vk_cl, timeout_ms);
    if (r) gfx_fail("vk flush: %d", r);
    return r ? GFX_ERR_FAIL : GFX_ERR_NONE;
}

int gfx_vk_one(uint16_t op, int argc, const uint64_t *args, const void *payload,
               uint16_t payload_len, uint16_t cap) {
    if (vk_cl < 0) return GFX_ERR_STATE;
    if (gpu_cmd_begin(vk_cl)) return GFX_ERR_STATE;
    int r = gpu_cmd_vk(vk_cl, op, (uint8_t)argc, args, payload, payload_len,
                       cap ? (uint16_t)(cap + 8) : 0);
    if (r) {
        gfx_fail("vk cmd 0x%x: %d", op, r);
        return GFX_ERR_FAIL;
    }
    return gfx_vk_flush(180000);
}

uint64_t gfx_vk_query(uint16_t op, int argc, const uint64_t *args, const void *payload,
                      uint16_t payload_len, uint16_t cap, uint16_t *out_len, uint8_t *rbuf,
                      uint16_t rbuf_cap) {
    if (vk_cl < 0) return 0;
    if (gpu_cmd_begin(vk_cl)) return 0;
    if (gpu_cmd_vk(vk_cl, op, (uint8_t)argc, args, payload, payload_len,
                   cap ? (uint16_t)(cap + 8) : 8)) {
        gfx_fail("vk query 0x%x", op);
        return 0;
    }
    if (gfx_vk_flush(180000)) return 0;
    uint16_t len = 0;
    memset(rbuf, 0, rbuf_cap);
    if (gpu_result(vk_cl, 0, rbuf, rbuf_cap, &len)) {
        gfx_fail("vk query 0x%x result", op);
        return 0;
    }
    if (out_len) *out_len = len;
    uint64_t v = 0;
    memcpy(&v, rbuf, len < 8 ? len : 8);
    return v;
}

int gfx_vk_instance_version(uint32_t *version) {
    uint8_t rbuf[16];
    uint64_t args[6] = {0};
    uint64_t v = gfx_vk_query(GPU_CMD_VK_ENUMERATE_INSTANCE_VERSION, 1, args, NULL, 0, 4, NULL,
                              rbuf, sizeof(rbuf));
    if (version) *version = (uint32_t)v;
    return GFX_ERR_NONE;
}

int gfx_vk_create_instance(const char *app_name, uint32_t api_version, uint64_t *inst) {
    if (vk_cl < 0 || !inst) return GFX_ERR_STATE;
    struct vkp pk;
    vkp_init(&pk, vk_pk_buf, GPU_BATCH_PAYLOAD);
    uint32_t ai_off;
    VkApplicationInfo *ai = vkp_alloc(&pk, VK_S_VKAPPLICATIONINFO, sizeof(VkApplicationInfo),
                                      &ai_off);
    if (!ai) return GFX_ERR_FAIL;
    ai->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai->apiVersion = api_version;
    ai->pApplicationName = (const char *)(uint64_t)vkp_str(&pk, app_name ? app_name : "LuOS gfx");
    uint32_t ici_off;
    VkInstanceCreateInfo *ici = vkp_alloc(&pk, VK_S_VKINSTANCECREATEINFO,
                                          sizeof(VkInstanceCreateInfo), &ici_off);
    if (!ici) return GFX_ERR_FAIL;
    ici->sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici->pApplicationInfo = (const void *)(uint64_t)ai_off;
    uint32_t plen = vkp_finish(&pk);
    if (!plen) return GFX_ERR_FAIL;
    uint8_t rbuf[32];
    uint64_t args[6] = {ici_off, 0xFFFE, 0};
    uint64_t v = gfx_vk_query(GPU_CMD_VK_CREATE_INSTANCE, 3, args, vk_pk_buf, (uint16_t)plen, 8,
                              NULL, rbuf, sizeof(rbuf));
    if (!v) {
        gfx_fail("vkCreateInstance failed");
        return GFX_ERR_FAIL;
    }
    *inst = v;
    return GFX_ERR_NONE;
}

int gfx_vk_destroy_instance(uint64_t inst) {
    uint64_t args[6] = {inst, 0xFFFE};
    return gfx_vk_one(GPU_CMD_VK_DESTROY_INSTANCE, 2, args, NULL, 0, 0);
}

int gfx_vk_enumerate_physical(uint64_t inst, uint64_t *out, uint32_t max, uint32_t *count) {
    if (vk_cl < 0 || !count) return GFX_ERR_STATE;
    uint8_t rbuf[64];
    uint64_t a0[6] = {inst, 0, 0xFFFE};
    uint64_t n = gfx_vk_query(GPU_CMD_VK_ENUMERATE_PHYSICAL_DEVICES, 3, a0, NULL, 0, 8, NULL,
                              rbuf, sizeof(rbuf));
    uint32_t total = (uint32_t)n;
    if (!total) {
        *count = 0;
        return GFX_ERR_NONE;
    }
    uint8_t pl[16];
    memset(pl, 0, sizeof(pl));
    memcpy(pl + 8, &total, 4);
    uint64_t a1[6] = {inst, 0, 0};
    uint16_t len = 0;
    memset(rbuf, 0, sizeof(rbuf));
    gfx_vk_query(GPU_CMD_VK_ENUMERATE_PHYSICAL_DEVICES, 3, a1, pl, 12, 16, &len, rbuf,
                 sizeof(rbuf));
    uint32_t got = 0;
    if (len >= 12) memcpy(&got, rbuf, 4);
    if (got > total) got = total;
    uint32_t store = got < max ? got : max;
    for (uint32_t i = 0; i < store; i++) {
        uint64_t dev = 0;
        memcpy(&dev, rbuf + 4 + (size_t)i * 8, 8);
        out[i] = dev;
    }
    *count = total;
    return GFX_ERR_NONE;
}

int gfx_vk_physical_device_properties(uint64_t pd, VkPhysicalDeviceProperties *out) {
    if (vk_cl < 0 || !out) return GFX_ERR_STATE;
    uint8_t rbuf[sizeof(VkPhysicalDeviceProperties) + 16];
    uint64_t args[6] = {pd, 0};
    uint16_t len = 0;
    gfx_vk_query(GPU_CMD_VK_GET_PHYSICAL_DEVICE_PROPERTIES, 2, args, NULL, 0,
                 (uint16_t)sizeof(VkPhysicalDeviceProperties), &len, rbuf, sizeof(rbuf));
    if (len < sizeof(VkPhysicalDeviceProperties)) {
        gfx_fail("vkGetPhysicalDeviceProperties short result: %u", len);
        return GFX_ERR_FAIL;
    }
    memcpy(out, rbuf, sizeof(VkPhysicalDeviceProperties));
    return GFX_ERR_NONE;
}

int gfx_vk_queue_family_properties(uint64_t pd, VkQueueFamilyProperties *out, uint32_t max,
                                   uint32_t *count) {
    if (vk_cl < 0 || !out || !count || !max) return GFX_ERR_STATE;
    uint8_t rbuf[16];
    uint64_t a0[6] = {pd, 0, 0xFFFE};
    uint64_t n = gfx_vk_query(GPU_CMD_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, 3, a0, NULL,
                              0, 4, NULL, rbuf, sizeof(rbuf));
    uint32_t total = (uint32_t)n;
    if (!total) {
        *count = 0;
        return GFX_ERR_NONE;
    }
    uint8_t pl[16];
    memset(pl, 0, sizeof(pl));
    memcpy(pl + 8, &total, 4);
    uint64_t a1[6] = {pd, 0, 0};
    uint8_t pbuf[sizeof(VkQueueFamilyProperties) + 16];
    uint16_t len = 0;
    gfx_vk_query(GPU_CMD_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, 3, a1, pl, 12,
                 (uint16_t)sizeof(VkQueueFamilyProperties), &len, pbuf, sizeof(pbuf));
    if (len >= 4) memcpy(count, pbuf, 4);
    else *count = total;
    if (*count > total) *count = total;
    memcpy(out, pbuf + 4, sizeof(VkQueueFamilyProperties));
    return GFX_ERR_NONE;
}

int gfx_vk_memory_properties(uint64_t pd, VkPhysicalDeviceMemoryProperties *out) {
    if (vk_cl < 0 || !out) return GFX_ERR_STATE;
    uint8_t rbuf[sizeof(VkPhysicalDeviceMemoryProperties) + 16];
    uint64_t args[6] = {pd, 0};
    uint16_t len = 0;
    gfx_vk_query(GPU_CMD_VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES, 2, args, NULL, 0,
                 (uint16_t)sizeof(VkPhysicalDeviceMemoryProperties), &len, rbuf, sizeof(rbuf));
    if (len < sizeof(VkPhysicalDeviceMemoryProperties)) return GFX_ERR_FAIL;
    memcpy(out, rbuf, sizeof(VkPhysicalDeviceMemoryProperties));
    return GFX_ERR_NONE;
}

int gfx_vk_features(uint64_t pd, VkPhysicalDeviceFeatures *out) {
    if (vk_cl < 0 || !out) return GFX_ERR_STATE;
    uint8_t rbuf[sizeof(VkPhysicalDeviceFeatures) + 16];
    uint64_t args[6] = {pd, 0};
    uint16_t len = 0;
    gfx_vk_query(GPU_CMD_VK_GET_PHYSICAL_DEVICE_FEATURES, 2, args, NULL, 0,
                 (uint16_t)sizeof(VkPhysicalDeviceFeatures), &len, rbuf, sizeof(rbuf));
    if (len < sizeof(VkPhysicalDeviceFeatures)) return GFX_ERR_FAIL;
    memcpy(out, rbuf, sizeof(VkPhysicalDeviceFeatures));
    return GFX_ERR_NONE;
}

int gfx_vk_create_device(uint64_t pd, uint32_t family, uint32_t queue_count, uint64_t *dev) {
    if (vk_cl < 0 || !dev) return GFX_ERR_STATE;
    struct vkp pk;
    vkp_init(&pk, vk_pk_buf, GPU_BATCH_PAYLOAD);
    float prio = 1.0f;
    uint32_t prio_off = vkp_put(&pk, &prio, 4);
    uint32_t qi_off;
    VkDeviceQueueCreateInfo *qi = vkp_alloc(&pk, VK_S_VKDEVICEQUEUECREATEINFO,
                                            sizeof(VkDeviceQueueCreateInfo), &qi_off);
    if (!qi) return GFX_ERR_FAIL;
    qi->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi->queueFamilyIndex = family;
    qi->queueCount = queue_count ? queue_count : 1;
    qi->pQueuePriorities = (const float *)(uint64_t)prio_off;
    uint32_t dci_off;
    VkDeviceCreateInfo *dci = vkp_alloc(&pk, VK_S_VKDEVICECREATEINFO,
                                        sizeof(VkDeviceCreateInfo), &dci_off);
    if (!dci) return GFX_ERR_FAIL;
    dci->sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci->queueCreateInfoCount = 1;
    dci->pQueueCreateInfos = (const void *)(uint64_t)qi_off;
    uint32_t plen = vkp_finish(&pk);
    if (!plen) return GFX_ERR_FAIL;
    uint8_t rbuf[32];
    uint64_t args[6] = {pd, dci_off, 0xFFFE, 0};
    uint64_t v = gfx_vk_query(GPU_CMD_VK_CREATE_DEVICE, 3, args, vk_pk_buf, (uint16_t)plen, 8,
                              NULL, rbuf, sizeof(rbuf));
    if (!v) {
        gfx_fail("vkCreateDevice failed");
        return GFX_ERR_FAIL;
    }
    *dev = v;
    return GFX_ERR_NONE;
}

int gfx_vk_get_device_queue(uint64_t dev, uint32_t family, uint32_t index, uint64_t *queue) {
    if (vk_cl < 0 || !queue) return GFX_ERR_STATE;
    uint8_t rbuf[32];
    uint64_t args[6] = {dev, family, index, 0};
    uint64_t v = gfx_vk_query(GPU_CMD_VK_GET_DEVICE_QUEUE, 4, args, NULL, 0, 8, NULL, rbuf,
                              sizeof(rbuf));
    if (!v) {
        gfx_fail("vkGetDeviceQueue failed");
        return GFX_ERR_FAIL;
    }
    *queue = v;
    return GFX_ERR_NONE;
}

int gfx_vk_device_wait_idle(uint64_t dev) {
    uint64_t args[6] = {dev};
    return gfx_vk_one(GPU_CMD_VK_DEVICE_WAIT_IDLE, 1, args, NULL, 0, 0);
}

int gfx_vk_destroy_device(uint64_t dev) {
    uint64_t args[6] = {dev, 0xFFFE};
    return gfx_vk_one(GPU_CMD_VK_DESTROY_DEVICE, 2, args, NULL, 0, 0);
}

uint32_t gfx_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ 0xFFFFFFFFu;
}
