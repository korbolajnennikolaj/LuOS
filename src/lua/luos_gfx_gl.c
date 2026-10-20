// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "lauxlib.h"
#include "lua.h"

#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_const_tables.h"
#include "drivers/GPU/mesa_gl_defs.h"
#include "drivers/GPU/mesa_sigs.h"

#include "components/Memory/heap.h"
#include "components/logger.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t f32l(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint64_t f64l(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static int lgl_simple(lua_State *L, uint16_t op, int argc, const uint64_t *args) {
    lua_pushinteger(L, gfx_gl_simple_op(op, argc, args));
    return 1;
}

static int lgl_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    LOG_INFO("gfx-gl: %s", msg);
    return 0;
}

static int lgl_start(lua_State *L) {
    int dev = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : -1;
    int r = gfx_gl_start(dev);
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_gl_error());
        return 2;
    }
    lua_pushinteger(L, gfx_gl_client());
    return 1;
}

static int lgl_close(lua_State *L) {
    (void)L;
    gfx_gl_close();
    gfx_gl_shutdown();
    return 0;
}

static int lgl_open(lua_State *L) {
    int w = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : 256;
    int h = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 256;
    int r = gfx_gl_open(w, h);
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_gl_error());
        return 2;
    }
    lua_pushinteger(L, gfx_gl_width());
    lua_pushinteger(L, gfx_gl_height());
    return 2;
}

static int lgl_present(lua_State *L) {
    int r = gfx_gl_present();
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_gl_error()[0] ? gfx_gl_error() : "present unavailable");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lgl_size(lua_State *L) {
    lua_pushinteger(L, gfx_gl_width());
    lua_pushinteger(L, gfx_gl_height());
    return 2;
}

static int lgl_err(lua_State *L) {
    const char *e = gfx_gl_error();
    if (e && e[0]) {
        lua_pushstring(L, e);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int lgl_info(lua_State *L) {
    struct mesa_host_info info;
    if (mesa_host_info(&info) || !mesa_host_pid_get()) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, info.pid);
    lua_setfield(L, -2, "pid");
    lua_pushstring(L, info.renderer);
    lua_setfield(L, -2, "renderer");
    lua_pushstring(L, info.env);
    lua_setfield(L, -2, "engine");
    lua_pushstring(L, mesa_prime_name(mesa_prime_get()));
    lua_setfield(L, -2, "prime");
    lua_pushinteger(L, info.gl_ok);
    lua_setfield(L, -2, "gl");
    lua_pushinteger(L, info.vk_ok);
    lua_setfield(L, -2, "vk");
    return 1;
}

static int lgl_prime(lua_State *L) {
    if (lua_isnone(L, 1)) {
        lua_pushstring(L, mesa_prime_name(mesa_prime_get()));
        return 1;
    }
    int sel = MESA_PRIME_AUTO;
    if (lua_isnumber(L, 1)) {
        sel = (int)lua_tointeger(L, 1);
    } else {
        const char *s = lua_tostring(L, 1);
        if (s && (strcmp(s, "llvm") == 0 || strcmp(s, "llvmpipe") == 0)) sel = MESA_PRIME_LLVM;
        else if (s && strcmp(s, "auto") == 0) sel = MESA_PRIME_AUTO;
        else sel = MESA_PRIME_AUTO;
    }
    lua_pushinteger(L, mesa_prime_set(sel));
    return 1;
}

static int lgl_get_error(lua_State *L) { lua_pushinteger(L, gfx_gl_get_error()); return 1; }

static int lgl_string(lua_State *L) {
    uint32_t name = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : GL_RENDERER;
    char buf[GFX_MAX_TEXT];
    if (gfx_gl_string(name, buf, sizeof(buf))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    return 1;
}

static int lgl_finish(lua_State *L) {
    lua_pushinteger(L, gfx_gl_finish());
    return 1;
}

static int lgl_clear_color(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1)), f32l((float)luaL_checknumber(L, 2)),
                     f32l((float)luaL_checknumber(L, 3)), f32l((float)luaL_optnumber(L, 4, 1.0))};
    return lgl_simple(L, GPU_CMD_GL_CLEAR_COLOR, 4, a);
}

static int lgl_clear(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)};
    return lgl_simple(L, GPU_CMD_GL_CLEAR, 1, a);
}

static int lgl_read_pixels(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    if (w <= 0 || h <= 0 || (uint32_t)w * h * 4 > MESA_PIX_MAX) {
        lua_pushnil(L);
        lua_pushstring(L, "bad read size");
        return 2;
    }
    uint8_t *buf = malloc((size_t)w * h * 4);
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, "oom");
        return 2;
    }
    int r = gfx_gl_read_pixels(x, y, w, h, buf, (uint32_t)w * h * 4);
    if (r) {
        free(buf);
        lua_pushnil(L);
        lua_pushstring(L, gfx_gl_error());
        return 2;
    }
    lua_pushlstring(L, (const char *)buf, (size_t)w * h * 4);
    free(buf);
    return 1;
}

static int lgl_pixel(lua_State *L) {
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    if (x < 0 || y < 0 || w <= 0 || ((size_t)y * w + (size_t)x + 1) * 4 > len) {
        lua_pushnil(L);
        return 1;
    }
    const uint8_t *p = (const uint8_t *)data + ((size_t)y * w + (size_t)x) * 4;
    lua_pushinteger(L, p[0]);
    lua_pushinteger(L, p[1]);
    lua_pushinteger(L, p[2]);
    lua_pushinteger(L, p[3]);
    return 4;
}

static int lgl_crc(lua_State *L) {
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    lua_pushinteger(L, (lua_Integer)gfx_crc32((const uint8_t *)data, (uint32_t)len));
    return 1;
}

static int lgl_begin(lua_State *L) {
    int mode = (int)luaL_checkinteger(L, 1);
    int r = gfx_im_begin(mode);
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_gl_error());
        return 2;
    }
    return 0;
}

static int lgl_end(lua_State *L) {
    int r = gfx_im_end();
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_gl_error());
        return 2;
    }
    return 0;
}

static int lgl_vertex2f(lua_State *L) {
    gfx_im_vertex((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), 0.0f, 1.0f);
    return 0;
}

static int lgl_vertex3f(lua_State *L) {
    gfx_im_vertex((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                  (float)luaL_checknumber(L, 3), 1.0f);
    return 0;
}

static int lgl_vertex4f(lua_State *L) {
    gfx_im_vertex((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                  (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    return 0;
}

static int lgl_color3f(lua_State *L) {
    gfx_im_color((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                 (float)luaL_checknumber(L, 3), 1.0f);
    return 0;
}

static int lgl_color4f(lua_State *L) {
    gfx_im_color((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                 (float)luaL_checknumber(L, 3), (float)luaL_optnumber(L, 4, 1.0));
    return 0;
}

static int lgl_normal3f(lua_State *L) {
    gfx_im_normal((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                  (float)luaL_checknumber(L, 3));
    return 0;
}

static int lgl_texcoord2f(lua_State *L) {
    gfx_im_texcoord((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}

static int lgl_create_shader(lua_State *L) {
    uint32_t type = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : GL_FRAGMENT_SHADER;
    const char *src = luaL_checkstring(L, 2);
    char log[1024];
    uint64_t id = gfx_gl_create_shader(type, src, log, sizeof(log));
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, log[0] ? log : gfx_gl_error());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int lgl_create_program(lua_State *L) {
    uint64_t vs = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t fs = (uint64_t)luaL_checkinteger(L, 2);
    const char *attribs[16];
    int n = 0;
    if (lua_istable(L, 3)) {
        int len = (int)lua_rawlen(L, 3);
        if (len > 15) len = 15;
        for (int i = 0; i < len; i++) {
            lua_rawgeti(L, 3, i + 1);
            if (lua_isstring(L, -1)) attribs[n++] = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
    }
    attribs[n] = NULL;
    char log[1024];
    uint64_t prog = gfx_gl_create_program(vs, fs, attribs, n, log, sizeof(log));
    if (!prog) {
        lua_pushnil(L);
        lua_pushstring(L, log[0] ? log : gfx_gl_error());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)prog);
    return 1;
}

static int lgl_uniform_location(lua_State *L) {
    uint64_t prog = (uint64_t)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lua_pushinteger(L, (lua_Integer)gfx_gl_uniform_location(prog, name));
    return 1;
}

static int lgl_uniform_matrix4fv(lua_State *L) {
    int64_t loc = (int64_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    float m[16];
    for (int i = 0; i < 16; i++) {
        lua_rawgeti(L, 2, i + 1);
        m[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pushinteger(L, gfx_gl_uniform_matrix(GPU_CMD_GL_UNIFORM_MATRIX4FV, loc, m, 1));
    return 1;
}

static int lgl_uniform1f(lua_State *L) {
    float v = (float)luaL_checknumber(L, 2);
    lua_pushinteger(L,
                    gfx_gl_uniformv(GPU_CMD_GL_UNIFORM1F, (int64_t)luaL_checkinteger(L, 1), &v, 1));
    return 1;
}

static int lgl_uniform2f(lua_State *L) {
    float v[2] = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)};
    lua_pushinteger(L,
                    gfx_gl_uniformv(GPU_CMD_GL_UNIFORM2F, (int64_t)luaL_checkinteger(L, 1), v, 2));
    return 1;
}

static int lgl_uniform3f(lua_State *L) {
    float v[3] = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                  (float)luaL_checknumber(L, 4)};
    lua_pushinteger(L,
                    gfx_gl_uniformv(GPU_CMD_GL_UNIFORM3F, (int64_t)luaL_checkinteger(L, 1), v, 3));
    return 1;
}

static int lgl_uniform4f(lua_State *L) {
    float v[4] = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                  (float)luaL_checknumber(L, 4), (float)luaL_optnumber(L, 5, 1.0)};
    lua_pushinteger(L,
                    gfx_gl_uniformv(GPU_CMD_GL_UNIFORM4F, (int64_t)luaL_checkinteger(L, 1), v, 4));
    return 1;
}

static int lgl_uniform1i(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1), (uint64_t)luaL_checkinteger(L, 2)};
    return lgl_simple(L, GPU_CMD_GL_UNIFORM1I, 2, a);
}

static int lgl_perspective(lua_State *L) {
    double fovy = luaL_checknumber(L, 1);
    double aspect = luaL_checknumber(L, 2);
    double znear = luaL_checknumber(L, 3);
    double zfar = luaL_checknumber(L, 4);
    float m[16];
    memset(m, 0, sizeof(m));
    double f = 1.0 / tan(fovy / 2.0);
    m[0] = (float)(f / aspect);
    m[5] = (float)f;
    m[10] = (float)((zfar + znear) / (zfar - znear));
    m[11] = -1.0f;
    m[14] = (float)(2.0 * zfar * znear / (zfar - znear));
    uint64_t a[6] = {0};
    int r = gfx_gl_uniform_matrix(GPU_CMD_GL_MULT_MATRIXF, 0, m, 1);
    (void)a;
    lua_pushinteger(L, r);
    return 1;
}

static int lgl_ortho(lua_State *L) {
    uint64_t a[6];
    for (int i = 0; i < 6; i++) a[i] = f64l(luaL_checknumber(L, i + 1));
    return lgl_simple(L, GPU_CMD_GL_ORTHO, 6, a);
}

static int lgl_look_at(lua_State *L) {
    float ex = (float)luaL_checknumber(L, 1), ey = (float)luaL_checknumber(L, 2),
          ez = (float)luaL_checknumber(L, 3);
    float cx = (float)luaL_checknumber(L, 4), cy = (float)luaL_checknumber(L, 5),
          cz = (float)luaL_checknumber(L, 6);
    float ux = (float)luaL_checknumber(L, 7), uy = (float)luaL_checknumber(L, 8),
          uz = (float)luaL_checknumber(L, 9);
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = (float)sqrt((double)(fx * fx + fy * fy + fz * fz));
    if (fl < 1e-9f) fl = 1.0f;
    fx /= fl;
    fy /= fl;
    fz /= fl;
    float sx = fy * uz - fz * uy, sy = fz * ux - fx * uz, sz = fx * uy - fy * ux;
    float sl = (float)sqrt((double)(sx * sx + sy * sy + sz * sz));
    if (sl < 1e-9f) sl = 1.0f;
    sx /= sl;
    sy /= sl;
    sz /= sl;
    ux = sy * fz - sz * fy;
    uy = sz * fx - sx * fz;
    uz = sx * fy - sy * fx;
    float m[16];
    m[0] = sx;
    m[4] = sy;
    m[8] = sz;
    m[1] = ux;
    m[5] = uy;
    m[9] = uz;
    m[2] = -fx;
    m[6] = -fy;
    m[10] = -fz;
    m[3] = 0.0f;
    m[7] = 0.0f;
    m[11] = 0.0f;
    m[12] = -(sx * ex + sy * ey + sz * ez);
    m[13] = -(ux * ex + uy * ey + uz * ez);
    m[14] = fx * ex + fy * ey + fz * ez;
    m[15] = 1.0f;
    lua_pushinteger(L, gfx_gl_uniform_matrix(GPU_CMD_GL_MULT_MATRIXF, 0, m, 1));
    return 1;
}

static int lgl_gen_buffers(lua_State *L) {
    uint32_t n = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 1;
    lua_pushinteger(L, (lua_Integer)gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, n));
    return 1;
}

static int lgl_gen_textures(lua_State *L) {
    uint32_t n = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 1;
    lua_pushinteger(L, (lua_Integer)gfx_gl_gen(GPU_CMD_GL_GEN_TEXTURES, n));
    return 1;
}

static int lgl_bind_buffer(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_ARRAY_BUFFER),
                     (uint64_t)luaL_optinteger(L, 2, 0)};
    return lgl_simple(L, GPU_CMD_GL_BIND_BUFFER, 2, a);
}

static int lgl_bind_texture(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_TEXTURE_2D),
                     (uint64_t)luaL_optinteger(L, 2, 0)};
    return lgl_simple(L, GPU_CMD_GL_BIND_TEXTURE, 2, a);
}

static int lgl_buffer_data(lua_State *L) {
    uint32_t target = (uint32_t)luaL_optinteger(L, 1, GL_ARRAY_BUFFER);
    uint32_t usage = (uint32_t)luaL_optinteger(L, 4, GL_STATIC_DRAW);
    if (lua_istable(L, 2)) {
        int n = (int)lua_rawlen(L, 2);
        if (n <= 0 || (uint32_t)n * 4 > GPU_BATCH_PAYLOAD) {
            lua_pushinteger(L, GFX_ERR_PARAM);
            return 1;
        }
        float *v = malloc((size_t)n * 4);
        if (!v) {
            lua_pushinteger(L, GFX_ERR_NOMEM);
            return 1;
        }
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            v[i] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        int r = gfx_gl_buffer_data(target, v, (uint32_t)n * 4, usage);
        free(v);
        lua_pushinteger(L, r);
        return 1;
    }
    size_t len = 0;
    const char *data = lua_isstring(L, 2) ? lua_tolstring(L, 2, &len) : NULL;
    if (!data || len == 0) {
        lua_pushinteger(L, GFX_ERR_PARAM);
        return 1;
    }
    lua_pushinteger(L, gfx_gl_buffer_data(target, data, (uint32_t)len, usage));
    return 1;
}

static int lgl_tex_image_2d(lua_State *L) {
    uint32_t target = (uint32_t)luaL_optinteger(L, 1, GL_TEXTURE_2D);
    int level = (int)luaL_optinteger(L, 2, 0);
    uint32_t internal = (uint32_t)luaL_optinteger(L, 3, GL_RGBA8);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);
    uint32_t format = (uint32_t)luaL_optinteger(L, 6, GL_RGBA);
    uint32_t type = (uint32_t)luaL_optinteger(L, 7, GL_UNSIGNED_BYTE);
    size_t len = 0;
    const char *data = lua_isstring(L, 8) ? lua_tolstring(L, 8, &len) : NULL;
    if (w <= 0 || h <= 0) {
        lua_pushinteger(L, GFX_ERR_PARAM);
        return 1;
    }
    uint32_t need = (uint32_t)w * (uint32_t)h * 4;
    if (need > GPU_BATCH_PAYLOAD) {
        lua_pushinteger(L, GFX_ERR_PARAM);
        return 1;
    }
    if (data && len < need) {
        lua_pushinteger(L, GFX_ERR_PARAM);
        return 1;
    }
    int r = gfx_gl_tex_image_2d(target, level, internal, (uint32_t)w, (uint32_t)h, format, type,
                                (const uint8_t *)data, data ? need : 0);
    lua_pushinteger(L, r);
    return 1;
}

static int lgl_tex_parameteri(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_TEXTURE_2D),
                     (uint64_t)luaL_checkinteger(L, 2), (uint64_t)luaL_checkinteger(L, 3)};
    return lgl_simple(L, GPU_CMD_GL_TEX_PARAMETERI, 3, a);
}

#define LGL_ATTRIB_FLOATS 4096

static int lgl_vertex_attrib_pointer(lua_State *L) {
    uint32_t index = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t size = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t type = (uint32_t)luaL_optinteger(L, 3, GL_FLOAT);
    uint32_t norm = (uint32_t)luaL_optinteger(L, 4, 0);
    uint32_t stride = (uint32_t)luaL_optinteger(L, 5, 0);
    if (lua_istable(L, 6)) {
        /* client-side array: the numbers are uploaded as floats into a library buffer */
        size_t n = lua_rawlen(L, 6);
        if (n == 0 || n > LGL_ATTRIB_FLOATS) {
            lua_pushinteger(L, GFX_ERR_PARAM);
            return 1;
        }
        float *data = kmalloc(n * sizeof(float));
        if (!data) {
            lua_pushinteger(L, GFX_ERR_NOMEM);
            return 1;
        }
        for (size_t k = 0; k < n; k++) {
            lua_rawgeti(L, 6, (int)k + 1);
            data[k] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        int r = gfx_gl_attrib_data(index, size, GL_FLOAT, norm, stride, data, (uint32_t)(n * sizeof(float)));
        kfree(data);
        lua_pushinteger(L, r);
        return 1;
    }
    /* a byte offset into the bound GL_ARRAY_BUFFER; 0 is a real offset */
    uint64_t off = (uint64_t)luaL_optinteger(L, 6, 0);
    lua_pushinteger(L, gfx_gl_attrib_pointer(index, size, type, norm, stride, off));
    return 1;
}

static int lgl_enable_vertex_attrib_array(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1)};
    return lgl_simple(L, GPU_CMD_GL_ENABLE_VERTEX_ATTRIB_ARRAY, 1, a);
}

static int lgl_disable_vertex_attrib_array(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1)};
    return lgl_simple(L, GPU_CMD_GL_DISABLE_VERTEX_ATTRIB_ARRAY, 1, a);
}

static int lgl_draw_arrays(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1), (uint64_t)luaL_optinteger(L, 2, 0),
                     (uint64_t)luaL_checkinteger(L, 3)};
    return lgl_simple(L, GPU_CMD_GL_DRAW_ARRAYS, 3, a);
}

static int lgl_use_program(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1)};
    return lgl_simple(L, GPU_CMD_GL_USE_PROGRAM, 1, a);
}


static int lgl_matrix_mode(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_MODELVIEW)};
    return lgl_simple(L, GPU_CMD_GL_MATRIX_MODE, 1, a);
}

static int lgl_load_identity(lua_State *L) {
    (void)L;
    return lgl_simple(L, GPU_CMD_GL_LOAD_IDENTITY, 0, NULL);
}

static int lgl_push_matrix(lua_State *L) {
    (void)L;
    return lgl_simple(L, GPU_CMD_GL_PUSH_MATRIX, 0, NULL);
}

static int lgl_pop_matrix(lua_State *L) {
    (void)L;
    return lgl_simple(L, GPU_CMD_GL_POP_MATRIX, 0, NULL);
}

static int lgl_translatef(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1)), f32l((float)luaL_checknumber(L, 2)),
                     f32l((float)luaL_checknumber(L, 3))};
    return lgl_simple(L, GPU_CMD_GL_TRANSLATEF, 3, a);
}

static int lgl_rotatef(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1)), f32l((float)luaL_checknumber(L, 2)),
                     f32l((float)luaL_checknumber(L, 3)), f32l((float)luaL_checknumber(L, 4))};
    return lgl_simple(L, GPU_CMD_GL_ROTATEF, 4, a);
}

static int lgl_scalef(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1)), f32l((float)luaL_optnumber(L, 2, 1.0)),
                     f32l((float)luaL_optnumber(L, 3, 1.0))};
    return lgl_simple(L, GPU_CMD_GL_SCALEF, 3, a);
}

static int lgl_shade_model(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_SMOOTH)};
    return lgl_simple(L, GPU_CMD_GL_SHADE_MODEL, 1, a);
}

static int lgl_enable(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1)};
    return lgl_simple(L, GPU_CMD_GL_ENABLE, 1, a);
}

static int lgl_disable(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1)};
    return lgl_simple(L, GPU_CMD_GL_DISABLE, 1, a);
}

static int lgl_viewport(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, 0), (uint64_t)luaL_optinteger(L, 2, 0),
                     (uint64_t)luaL_optinteger(L, 3, gfx_gl_width()),
                     (uint64_t)luaL_optinteger(L, 4, gfx_gl_height())};
    return lgl_simple(L, GPU_CMD_GL_VIEWPORT, 4, a);
}

static int lgl_depth_func(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_LESS)};
    return lgl_simple(L, GPU_CMD_GL_DEPTH_FUNC, 1, a);
}

static int lgl_cull_face(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_BACK)};
    return lgl_simple(L, GPU_CMD_GL_CULL_FACE, 1, a);
}

static int lgl_front_face(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_optinteger(L, 1, GL_CCW)};
    return lgl_simple(L, GPU_CMD_GL_FRONT_FACE, 1, a);
}

static int lgl_line_width(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1))};
    return lgl_simple(L, GPU_CMD_GL_LINE_WIDTH, 1, a);
}

static int lgl_point_size(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1))};
    return lgl_simple(L, GPU_CMD_GL_POINT_SIZE, 1, a);
}

static int lgl_blend_func(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1), (uint64_t)luaL_checkinteger(L, 2)};
    return lgl_simple(L, GPU_CMD_GL_BLEND_FUNC, 2, a);
}

static int lgl_rectf(lua_State *L) {
    uint64_t a[6] = {f32l((float)luaL_checknumber(L, 1)), f32l((float)luaL_checknumber(L, 2)),
                     f32l((float)luaL_checknumber(L, 3)), f32l((float)luaL_checknumber(L, 4))};
    return lgl_simple(L, GPU_CMD_GL_RECTF, 4, a);
}

static int lgl_lightfv(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1), (uint64_t)luaL_checkinteger(L, 2), 0};
    float v[4] = {0, 0, 0, 1};
    if (lua_istable(L, 3)) {
        for (int i = 0; i < 4; i++) {
            lua_rawgeti(L, 3, i + 1);
            v[i] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, gfx_gl_payload(GPU_CMD_GL_LIGHTFV, 3, a, v, 16, 0));
    return 1;
}

static int lgl_materialfv(lua_State *L) {
    uint64_t a[6] = {(uint64_t)luaL_checkinteger(L, 1), (uint64_t)luaL_checkinteger(L, 2), 0};
    float v[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    if (lua_istable(L, 3)) {
        for (int i = 0; i < 4; i++) {
            lua_rawgeti(L, 3, i + 1);
            v[i] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, gfx_gl_payload(GPU_CMD_GL_MATERIALFV, 3, a, v, 16, 0));
    return 1;
}

static uint16_t lgl_opcode(const char *name) {
    for (int i = 0; i < GPU_CMD_GL_COUNT; i++) {
        const struct mesa_gl_sig *s = &mesa_gl_sigs[i];
        if (s->name && strcmp(s->name, name) == 0) return (uint16_t)(GPU_CMD_GL_FIRST + i);
    }
    return 0;
}

static int lgl_call(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    char full[80];
    if (name[0] == 'g' && name[1] == 'l') snprintf(full, sizeof(full), "%s", name);
    else snprintf(full, sizeof(full), "gl%s", name);
    uint16_t op = lgl_opcode(full);
    if (!op) {
        luaL_error(L, "unknown GL command: %s", full);
        return 0;
    }
    if (!gfx_gl_ready()) {
        luaL_error(L, "no GL context - call gl.start() first");
        return 0;
    }
    const struct mesa_gl_sig *sig = &mesa_gl_sigs[op - GPU_CMD_GL_FIRST];
    if (!sig->argc) {
        luaL_error(L, "command not supported: %s", full);
        return 0;
    }
    int top = lua_gettop(L);
    uint64_t args[16] = {0};
    uint8_t payload[512];
    uint32_t used = 0;
    uint32_t n_extra = sig->argc > 6 ? (uint32_t)sig->argc - 6 : 0;
    uint32_t data_off = 8 * n_extra;
    uint16_t result_cap = 0;
    int argi = 2;
    for (int i = 0; i < sig->argc; i++) {
        bool is_ptr = sig->types[i] == M_T_P;
        bool have = argi <= top;
        if (i < 6) {
            if (is_ptr) {
                if (have && lua_type(L, argi) == LUA_TSTRING) {
                    size_t len = 0;
                    const char *data = lua_tolstring(L, argi, &len);
                    if (data_off + used + len > sizeof(payload))
                        luaL_error(L, "payload too large for %s", full);
                    memcpy(payload + data_off + used, data, len);
                    used += (uint32_t)len;
                    args[i] = used - len;
                } else if (have && lua_istable(L, argi)) {
                    size_t n = lua_rawlen(L, argi);
                    size_t len = n * 4;
                    if (data_off + used + len > sizeof(payload))
                        luaL_error(L, "payload too large for %s", full);
                    for (size_t k = 0; k < n; k++) {
                        lua_rawgeti(L, argi, (int)k + 1);
                        float f = (float)lua_tonumber(L, -1);
                        uint32_t bits;
                        memcpy(&bits, &f, 4);
                        memcpy(payload + data_off + used + k * 4, &bits, 4);
                        lua_pop(L, 1);
                    }
                    used += (uint32_t)len;
                    args[i] = used - len;
                } else if (have && lua_isnumber(L, argi)) {
                    args[i] = (uint64_t)lua_tointeger(L, argi);
                } else {
                    args[i] = 0xFFFE;
                }
            } else if (sig->types[i] == M_T_F) {
                float f = have ? (float)lua_tonumber(L, argi) : 0.0f;
                args[i] = f32l(f);
            } else if (sig->types[i] == M_T_D) {
                double d = have ? (double)lua_tonumber(L, argi) : 0.0;
                args[i] = f64l(d);
            } else {
                args[i] = have ? (uint64_t)lua_tointeger(L, argi) : 0;
            }
        } else {
            uint64_t v = 0;
            if (is_ptr)
                v = 0xFFFE;
            else if (sig->types[i] == M_T_F)
                v = f32l(have ? (float)lua_tonumber(L, argi) : 0.0f);
            else if (sig->types[i] == M_T_D)
                v = f64l(have ? (double)lua_tonumber(L, argi) : 0.0);
            else
                v = have ? (uint64_t)lua_tointeger(L, argi) : 0;
            memcpy(payload + 8 * (size_t)(i - 6), &v, 8);
        }
        argi++;
    }
    if (top == (int)sig->argc + 2 && lua_isnumber(L, top))
        result_cap = (uint16_t)lua_tointeger(L, top);
    uint32_t total = data_off + used;
    int r = gfx_gl_payload(op, (uint8_t)sig->argc, args, total ? payload : NULL,
                           (uint16_t)total, result_cap);
    lua_pushinteger(L, r);
    return 1;
}

static int lgl_const(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const char *short_name = name;
    if (strncmp(name, "GL_", 3) == 0) short_name = name + 3;
    const struct mesa_const *c = mesa_const_find(mesa_gl_consts, mesa_gl_const_count, short_name);
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)c->value);
    return 1;
}

static int lgl_index(lua_State *L) {
    const char *key = lua_tostring(L, 2);
    if (!key) return 0;
    const struct mesa_const *c = mesa_const_find(mesa_gl_consts, mesa_gl_const_count, key);
    if (c) {
        lua_pushinteger(L, (lua_Integer)c->value);
        lua_rawset(L, 1);
        lua_pushinteger(L, (lua_Integer)c->value);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static const luaL_Reg gl_funcs[] = {
    {"log", lgl_log},
    {"start", lgl_start},
    {"close", lgl_close},
    {"open", lgl_open},
    {"present", lgl_present},
    {"size", lgl_size},
    {"info", lgl_info},
    {"prime", lgl_prime},
    {"error", lgl_err},
    {"getError", lgl_get_error},
    {"string", lgl_string},
    {"finish", lgl_finish},
    {"flush", lgl_finish},
    {"clearColor", lgl_clear_color},
    {"clear", lgl_clear},
    {"readPixels", lgl_read_pixels},
    {"pixel", lgl_pixel},
    {"crc", lgl_crc},
    {"begin", lgl_begin},
    {"end", lgl_end},
    {"End", lgl_end},
    {"vertex2f", lgl_vertex2f},
    {"vertex3f", lgl_vertex3f},
    {"vertex4f", lgl_vertex4f},
    {"color3f", lgl_color3f},
    {"color4f", lgl_color4f},
    {"normal3f", lgl_normal3f},
    {"texCoord2f", lgl_texcoord2f},
    {"createShader", lgl_create_shader},
    {"createProgram", lgl_create_program},
    {"getUniformLocation", lgl_uniform_location},
    {"uniformMatrix4fv", lgl_uniform_matrix4fv},
    {"uniform1f", lgl_uniform1f},
    {"uniform2f", lgl_uniform2f},
    {"uniform3f", lgl_uniform3f},
    {"uniform4f", lgl_uniform4f},
    {"uniform1i", lgl_uniform1i},
    {"perspective", lgl_perspective},
    {"ortho", lgl_ortho},
    {"lookAt", lgl_look_at},
    {"genBuffers", lgl_gen_buffers},
    {"genTextures", lgl_gen_textures},
    {"bindBuffer", lgl_bind_buffer},
    {"bindTexture", lgl_bind_texture},
    {"bufferData", lgl_buffer_data},
    {"texImage2D", lgl_tex_image_2d},
    {"texParameteri", lgl_tex_parameteri},
    {"vertexAttribPointer", lgl_vertex_attrib_pointer},
    {"enableVertexAttribArray", lgl_enable_vertex_attrib_array},
    {"disableVertexAttribArray", lgl_disable_vertex_attrib_array},
    {"drawArrays", lgl_draw_arrays},
    {"useProgram", lgl_use_program},
    {"matrixMode", lgl_matrix_mode},
    {"loadIdentity", lgl_load_identity},
    {"pushMatrix", lgl_push_matrix},
    {"popMatrix", lgl_pop_matrix},
    {"translatef", lgl_translatef},
    {"rotatef", lgl_rotatef},
    {"scalef", lgl_scalef},
    {"shadeModel", lgl_shade_model},
    {"enable", lgl_enable},
    {"disable", lgl_disable},
    {"viewport", lgl_viewport},
    {"depthFunc", lgl_depth_func},
    {"cullFace", lgl_cull_face},
    {"frontFace", lgl_front_face},
    {"lineWidth", lgl_line_width},
    {"pointSize", lgl_point_size},
    {"blendFunc", lgl_blend_func},
    {"rect", lgl_rectf},
    {"lightfv", lgl_lightfv},
    {"materialfv", lgl_materialfv},
    {"call", lgl_call},
    {"const", lgl_const},
    {NULL, NULL},
};

void luos_register_gfx_gl(lua_State *L) {
    lua_newtable(L);
    for (const luaL_Reg *p = gl_funcs; p->name; p++) {
        lua_pushcfunction(L, p->func);
        lua_setfield(L, -2, p->name);
    }
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, lgl_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "gl");
}
