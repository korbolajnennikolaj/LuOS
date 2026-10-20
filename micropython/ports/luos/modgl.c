// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "py/obj.h"
#include "py/binary.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_const_tables.h"
#include "drivers/GPU/mesa_gl_defs.h"

#include "components/logger.h"

#include <math.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t f32b_(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static mp_obj_t mgl_log(mp_obj_t msg_obj) {
    LOG_INFO("gfx-gl: %s", mp_obj_str_get_str(msg_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_log_obj, mgl_log);

static mp_obj_t mgl_start(size_t n_args, const mp_obj_t *args) {
    int dev = n_args > 0 && mp_obj_is_int(args[0]) ? mp_obj_get_int(args[0]) : -1;
    int r = gfx_gl_start(dev);
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_client());
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_start_obj, 0, 1, mgl_start);

static mp_obj_t mgl_close(void) {
    gfx_gl_close();
    gfx_gl_shutdown();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_close_obj, mgl_close);

static mp_obj_t mgl_open(size_t n_args, const mp_obj_t *args) {
    int w = n_args > 0 && mp_obj_is_int(args[0]) ? mp_obj_get_int(args[0]) : 256;
    int h = n_args > 1 && mp_obj_is_int(args[1]) ? mp_obj_get_int(args[1]) : 256;
    int r = gfx_gl_open(w, h);
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    mp_obj_t t[2] = {MP_OBJ_NEW_SMALL_INT(gfx_gl_width()), MP_OBJ_NEW_SMALL_INT(gfx_gl_height())};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_open_obj, 0, 2, mgl_open);

static mp_obj_t mgl_present(void) {
    int r = gfx_gl_present();
    if (r) {
        mp_obj_t t[2] = {mp_const_false, mp_obj_new_str_from_cstr(gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_present_obj, mgl_present);

static mp_obj_t mgl_size(void) {
    mp_obj_t t[2] = {MP_OBJ_NEW_SMALL_INT(gfx_gl_width()), MP_OBJ_NEW_SMALL_INT(gfx_gl_height())};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_size_obj, mgl_size);

static mp_obj_t mgl_error(void) {
    const char *e = gfx_gl_error();
    if (e && e[0]) return mp_obj_new_str_from_cstr(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_error_obj, mgl_error);

static mp_obj_t mgl_get_error(void) { return MP_OBJ_NEW_SMALL_INT(gfx_gl_get_error()); }
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_get_error_obj, mgl_get_error);

static mp_obj_t mgl_info(void) {
    struct mesa_host_info info;
    if (mesa_host_info(&info) || !mesa_host_pid_get()) return mp_const_none;
    mp_obj_dict_t *d = mp_obj_new_dict(6);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_pid),
                      MP_OBJ_NEW_SMALL_INT(info.pid));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_renderer),
                      mp_obj_new_str_from_cstr(info.renderer));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_engine),
                      mp_obj_new_str_from_cstr(info.env));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_prime),
                      mp_obj_new_str_from_cstr(mesa_prime_name(mesa_prime_get())));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_gl),
                      MP_OBJ_NEW_SMALL_INT(info.gl_ok));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_vk),
                      MP_OBJ_NEW_SMALL_INT(info.vk_ok));
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_info_obj, mgl_info);

static mp_obj_t mgl_prime(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) return mp_obj_new_str_from_cstr(mesa_prime_name(mesa_prime_get()));
    int sel = MESA_PRIME_AUTO;
    if (mp_obj_is_int(args[0])) {
        sel = mp_obj_get_int(args[0]);
    } else {
        const char *s = mp_obj_str_get_str(args[0]);
        if (strcmp(s, "llvm") == 0 || strcmp(s, "llvmpipe") == 0) sel = MESA_PRIME_LLVM;
    }
    return MP_OBJ_NEW_SMALL_INT(mesa_prime_set(sel));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_prime_obj, 0, 1, mgl_prime);

static mp_obj_t mgl_string(mp_obj_t name_obj) {
    uint32_t name = mp_obj_get_int(name_obj);
    char buf[GFX_MAX_TEXT];
    if (gfx_gl_string(name, buf, sizeof(buf))) return mp_const_none;
    return mp_obj_new_str_from_cstr(buf);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_string_obj, mgl_string);

static mp_obj_t mgl_finish(void) { return MP_OBJ_NEW_SMALL_INT(gfx_gl_finish()); }
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_finish_obj, mgl_finish);

static mp_obj_t mgl_clear_color(size_t n_args, const mp_obj_t *args) {
    float r = mp_obj_get_float(args[0]);
    float g = mp_obj_get_float(args[1]);
    float b = mp_obj_get_float(args[2]);
    float a = n_args > 3 ? mp_obj_get_float(args[3]) : 1.0f;
    uint64_t cmd[6] = {f32b_(r), f32b_(g), f32b_(b), f32b_(a)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_CLEAR_COLOR, 4, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_clear_color_obj, 3, 4, mgl_clear_color);

static mp_obj_t mgl_clear(size_t n_args, const mp_obj_t *args) {
    uint64_t mask = n_args > 0 ? (uint64_t)mp_obj_get_int(args[0])
                               : (uint64_t)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    uint64_t cmd[6] = {mask};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_CLEAR, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_clear_obj, 0, 1, mgl_clear);

static mp_obj_t mgl_read_pixels(size_t n_args, const mp_obj_t *args) {
    int x = mp_obj_get_int(args[0]);
    int y = mp_obj_get_int(args[1]);
    int w = mp_obj_get_int(args[2]);
    int h = mp_obj_get_int(args[3]);
    if (w <= 0 || h <= 0 || (uint32_t)w * h * 4 > MESA_PIX_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad read size"));
    }
    uint8_t *buf = malloc((size_t)w * h * 4);
    if (!buf) mp_raise_OSError(MP_ENOMEM);
    int r = gfx_gl_read_pixels(x, y, w, h, buf, (uint32_t)w * h * 4);
    if (r) {
        free(buf);
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    mp_obj_t b = mp_obj_new_bytes(buf, (size_t)w * h * 4);
    free(buf);
    return b;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_read_pixels_obj, 4, 4, mgl_read_pixels);

static mp_obj_t mgl_pixel(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[0], &bi, MP_BUFFER_READ);
    int x = mp_obj_get_int(args[1]);
    int y = mp_obj_get_int(args[2]);
    int w = mp_obj_get_int(args[3]);
    if (x < 0 || y < 0 || w <= 0 || ((size_t)y * w + (size_t)x + 1) * 4 > bi.len)
        return mp_const_none;
    const uint8_t *p = (const uint8_t *)bi.buf + ((size_t)y * w + (size_t)x) * 4;
    mp_obj_t t[4] = {MP_OBJ_NEW_SMALL_INT(p[0]), MP_OBJ_NEW_SMALL_INT(p[1]),
                     MP_OBJ_NEW_SMALL_INT(p[2]), MP_OBJ_NEW_SMALL_INT(p[3])};
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_pixel_obj, 4, 4, mgl_pixel);

static mp_obj_t mgl_crc(mp_obj_t data_obj) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data_obj, &bi, MP_BUFFER_READ);
    return mp_obj_new_int_from_uint(gfx_crc32(bi.buf, (uint32_t)bi.len));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_crc_obj, mgl_crc);

static mp_obj_t mgl_begin(mp_obj_t mode_obj) {
    int r = gfx_im_begin(mp_obj_get_int(mode_obj));
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_begin_obj, mgl_begin);

static mp_obj_t mgl_end(void) {
    int r = gfx_im_end();
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_end_obj, mgl_end);

static mp_obj_t mgl_vertex2f(mp_obj_t x_obj, mp_obj_t y_obj) {
    gfx_im_vertex(mp_obj_get_float(x_obj), mp_obj_get_float(y_obj), 0.0f, 1.0f);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_vertex2f_obj, mgl_vertex2f);

static mp_obj_t mgl_vertex3f(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t z_obj) {
    gfx_im_vertex(mp_obj_get_float(x_obj), mp_obj_get_float(y_obj), mp_obj_get_float(z_obj),
                  1.0f);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_vertex3f_obj, mgl_vertex3f);

static mp_obj_t mgl_vertex4f(size_t n_args, const mp_obj_t *args) {
    gfx_im_vertex(mp_obj_get_float(args[0]), mp_obj_get_float(args[1]),
                  mp_obj_get_float(args[2]),
                  n_args > 3 ? mp_obj_get_float(args[3]) : 1.0f);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_vertex4f_obj, 3, 4, mgl_vertex4f);

static mp_obj_t mgl_color3f(mp_obj_t r_obj, mp_obj_t g_obj, mp_obj_t b_obj) {
    gfx_im_color(mp_obj_get_float(r_obj), mp_obj_get_float(g_obj), mp_obj_get_float(b_obj), 1.0f);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_color3f_obj, mgl_color3f);

static mp_obj_t mgl_color4f(size_t n_args, const mp_obj_t *args) {
    gfx_im_color(mp_obj_get_float(args[0]), mp_obj_get_float(args[1]),
                 mp_obj_get_float(args[2]), n_args > 3 ? mp_obj_get_float(args[3]) : 1.0f);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_color4f_obj, 3, 4, mgl_color4f);

static mp_obj_t mgl_normal3f(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t z_obj) {
    gfx_im_normal(mp_obj_get_float(x_obj), mp_obj_get_float(y_obj), mp_obj_get_float(z_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_normal3f_obj, mgl_normal3f);

static mp_obj_t mgl_texcoord2f(mp_obj_t s_obj, mp_obj_t t_obj) {
    gfx_im_texcoord(mp_obj_get_float(s_obj), mp_obj_get_float(t_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_texcoord2f_obj, mgl_texcoord2f);

static mp_obj_t mgl_create_shader(size_t n_args, const mp_obj_t *args) {
    uint32_t type = GL_FRAGMENT_SHADER;
    const char *src = NULL;
    if (mp_obj_is_str(args[0])) {
        src = mp_obj_str_get_str(args[0]);
    } else {
        type = (uint32_t)mp_obj_get_int(args[0]);
        if (n_args > 1) src = mp_obj_str_get_str(args[1]);
    }
    if (!src) mp_raise_TypeError(MP_ERROR_TEXT("shader source expected"));
    char log[1024];
    uint64_t id = gfx_gl_create_shader(type, src, log, sizeof(log));
    if (!id) {
        mp_obj_t t[2] = {mp_const_none,
                         mp_obj_new_str_from_cstr(log[0] ? log : gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_obj_new_int_from_uint(id);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_create_shader_obj, 1, 2, mgl_create_shader);

static mp_obj_t mgl_create_program(size_t n_args, const mp_obj_t *args) {
    uint64_t vs = (uint64_t)mp_obj_get_int(args[0]);
    uint64_t fs = (uint64_t)mp_obj_get_int(args[1]);
    const char *attribs[16];
    int n = 0;
    if (n_args > 2 && mp_obj_is_type(args[2], &mp_type_list)) {
        mp_obj_list_t *lst = MP_OBJ_TO_PTR(args[2]);
        size_t len = lst->len;
        if (len > 15) len = 15;
        for (size_t i = 0; i < len; i++) {
            if (mp_obj_is_str(lst->items[i])) attribs[n++] = mp_obj_str_get_str(lst->items[i]);
        }
    }
    attribs[n] = NULL;
    char log[1024];
    uint64_t prog = gfx_gl_create_program(vs, fs, attribs, n, log, sizeof(log));
    if (!prog) {
        mp_obj_t t[2] = {mp_const_none,
                         mp_obj_new_str_from_cstr(log[0] ? log : gfx_gl_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_obj_new_int_from_uint(prog);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_create_program_obj, 2, 3, mgl_create_program);

static mp_obj_t mgl_uniform_location(mp_obj_t prog_obj, mp_obj_t name_obj) {
    int64_t loc = gfx_gl_uniform_location((uint64_t)mp_obj_get_int(prog_obj),
                                          mp_obj_str_get_str(name_obj));
    return mp_obj_new_int(loc);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_uniform_location_obj, mgl_uniform_location);

static mp_obj_t mgl_uniform_matrix4fv(mp_obj_t loc_obj, mp_obj_t table_obj) {
    mp_obj_t *items = NULL;
    size_t len = 0;
    mp_obj_get_array(table_obj, &len, &items);
    if (len < 16) mp_raise_ValueError(MP_ERROR_TEXT("need 16 floats"));
    float m[16];
    for (int i = 0; i < 16; i++) m[i] = mp_obj_get_float(items[i]);
    return MP_OBJ_NEW_SMALL_INT(
        gfx_gl_uniform_matrix(GPU_CMD_GL_UNIFORM_MATRIX4FV, (int64_t)mp_obj_get_int(loc_obj), m, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_uniform_matrix4fv_obj, mgl_uniform_matrix4fv);

static mp_obj_t mgl_uniform1f(mp_obj_t loc_obj, mp_obj_t v_obj) {
    float v = mp_obj_get_float(v_obj);
    return MP_OBJ_NEW_SMALL_INT(
        gfx_gl_uniformv(GPU_CMD_GL_UNIFORM1F, (int64_t)mp_obj_get_int(loc_obj), &v, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_uniform1f_obj, mgl_uniform1f);

static mp_obj_t mgl_uniform2f(mp_obj_t loc_obj, mp_obj_t x_obj, mp_obj_t y_obj) {
    float v[2] = {mp_obj_get_float(x_obj), mp_obj_get_float(y_obj)};
    return MP_OBJ_NEW_SMALL_INT(
        gfx_gl_uniformv(GPU_CMD_GL_UNIFORM2F, (int64_t)mp_obj_get_int(loc_obj), v, 2));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_uniform2f_obj, mgl_uniform2f);

static mp_obj_t mgl_uniform3f(size_t n_args, const mp_obj_t *args) {
    float v[3] = {mp_obj_get_float(args[1]), mp_obj_get_float(args[2]),
                  mp_obj_get_float(args[3])};
    return MP_OBJ_NEW_SMALL_INT(
        gfx_gl_uniformv(GPU_CMD_GL_UNIFORM3F, (int64_t)mp_obj_get_int(args[0]), v, 3));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_uniform3f_obj, 4, 4, mgl_uniform3f);

static mp_obj_t mgl_uniform4f(size_t n_args, const mp_obj_t *args) {
    float v[4] = {mp_obj_get_float(args[1]), mp_obj_get_float(args[2]),
                  mp_obj_get_float(args[3]), n_args > 4 ? mp_obj_get_float(args[4]) : 1.0f};
    return MP_OBJ_NEW_SMALL_INT(
        gfx_gl_uniformv(GPU_CMD_GL_UNIFORM4F, (int64_t)mp_obj_get_int(args[0]), v, 4));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_uniform4f_obj, 4, 5, mgl_uniform4f);

static mp_obj_t mgl_uniform1i(mp_obj_t loc_obj, mp_obj_t v_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(loc_obj), (uint64_t)mp_obj_get_int(v_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_UNIFORM1I, 2, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_uniform1i_obj, mgl_uniform1i);

static mp_obj_t mgl_perspective(size_t n_args, const mp_obj_t *args) {
    double fovy = mp_obj_get_float(args[0]);
    double aspect = mp_obj_get_float(args[1]);
    double znear = mp_obj_get_float(args[2]);
    double zfar = mp_obj_get_float(args[3]);
    float m[16];
    memset(m, 0, sizeof(m));
    double f = 1.0 / tan(fovy / 2.0);
    m[0] = (float)(f / aspect);
    m[5] = (float)f;
    m[10] = (float)((zfar + znear) / (zfar - znear));
    m[11] = -1.0f;
    m[14] = (float)(2.0 * zfar * znear / (zfar - znear));
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_uniform_matrix(GPU_CMD_GL_MULT_MATRIXF, 0, m, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_perspective_obj, 4, 4, mgl_perspective);

static mp_obj_t mgl_ortho(size_t n_args, const mp_obj_t *args) {
    if (n_args != 6) mp_raise_TypeError(MP_ERROR_TEXT("ortho takes 6 args"));
    uint64_t cmd[6];
    for (int i = 0; i < 6; i++) {
        double d = mp_obj_get_float(args[i]);
        uint64_t u;
        memcpy(&u, &d, 8);
        cmd[i] = u;
    }
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_ORTHO, 6, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_ortho_obj, 6, 6, mgl_ortho);

static mp_obj_t mgl_look_at(size_t n_args, const mp_obj_t *args) {
    float ex = mp_obj_get_float(args[0]), ey = mp_obj_get_float(args[1]), ez = mp_obj_get_float(args[2]);
    float cx = mp_obj_get_float(args[3]), cy = mp_obj_get_float(args[4]), cz = mp_obj_get_float(args[5]);
    float ux = mp_obj_get_float(args[6]), uy = mp_obj_get_float(args[7]), uz = mp_obj_get_float(args[8]);
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
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_uniform_matrix(GPU_CMD_GL_MULT_MATRIXF, 0, m, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_look_at_obj, 9, 9, mgl_look_at);

static mp_obj_t mgl_gen_buffers(size_t n_args, const mp_obj_t *args) {
    uint32_t n = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : 1;
    return mp_obj_new_int_from_uint(gfx_gl_gen(GPU_CMD_GL_GEN_BUFFERS, n));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_gen_buffers_obj, 0, 1, mgl_gen_buffers);

static mp_obj_t mgl_gen_textures(size_t n_args, const mp_obj_t *args) {
    uint32_t n = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : 1;
    return mp_obj_new_int_from_uint(gfx_gl_gen(GPU_CMD_GL_GEN_TEXTURES, n));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_gen_textures_obj, 0, 1, mgl_gen_textures);

static mp_obj_t mgl_bind_buffer(size_t n_args, const mp_obj_t *args) {
    uint32_t target = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : GL_ARRAY_BUFFER;
    uint64_t id = n_args > 1 ? (uint64_t)mp_obj_get_int(args[1]) : 0;
    uint64_t cmd[6] = {target, id};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_BIND_BUFFER, 2, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_bind_buffer_obj, 0, 2, mgl_bind_buffer);

static mp_obj_t mgl_bind_texture(size_t n_args, const mp_obj_t *args) {
    uint32_t target = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : GL_TEXTURE_2D;
    uint64_t id = n_args > 1 ? (uint64_t)mp_obj_get_int(args[1]) : 0;
    uint64_t cmd[6] = {target, id};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_BIND_TEXTURE, 2, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_bind_texture_obj, 0, 2, mgl_bind_texture);

static mp_obj_t mgl_buffer_data(size_t n_args, const mp_obj_t *args) {
    uint32_t target = n_args > 0 && mp_obj_is_int(args[0]) ? (uint32_t)mp_obj_get_int(args[0])
                                                           : GL_ARRAY_BUFFER;
    uint32_t usage = n_args > 2 && mp_obj_is_int(args[2]) ? (uint32_t)mp_obj_get_int(args[2])
                                                          : GL_STATIC_DRAW;
    if (mp_obj_is_type(args[1], &mp_type_list)) {
        mp_obj_list_t *lst = MP_OBJ_TO_PTR(args[1]);
        size_t n = lst->len;
        if (n == 0 || (uint32_t)n * 4 > GPU_BATCH_PAYLOAD)
            mp_raise_ValueError(MP_ERROR_TEXT("bad buffer size"));
        float *v = malloc(n * 4);
        if (!v) mp_raise_OSError(MP_ENOMEM);
        for (size_t i = 0; i < n; i++) v[i] = mp_obj_get_float(lst->items[i]);
        int r = gfx_gl_buffer_data(target, v, (uint32_t)n * 4, usage);
        free(v);
        return MP_OBJ_NEW_SMALL_INT(r);
    }
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[1], &bi, MP_BUFFER_READ);
    if (!bi.len) mp_raise_ValueError(MP_ERROR_TEXT("empty buffer"));
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_buffer_data(target, bi.buf, (uint32_t)bi.len, usage));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_buffer_data_obj, 1, 3, mgl_buffer_data);

static mp_obj_t mgl_tex_image_2d(size_t n_args, const mp_obj_t *args) {
    uint32_t target = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : GL_TEXTURE_2D;
    int level = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    uint32_t internal = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : GL_RGBA8;
    int w = mp_obj_get_int(args[3]);
    int h = mp_obj_get_int(args[4]);
    uint32_t format = n_args > 5 ? (uint32_t)mp_obj_get_int(args[5]) : GL_RGBA;
    uint32_t type = n_args > 6 ? (uint32_t)mp_obj_get_int(args[6]) : GL_UNSIGNED_BYTE;
    if (w <= 0 || h <= 0) mp_raise_ValueError(MP_ERROR_TEXT("bad texture size"));
    uint32_t need = (uint32_t)w * (uint32_t)h * 4;
    if (need > GPU_BATCH_PAYLOAD) mp_raise_ValueError(MP_ERROR_TEXT("texture too large"));
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    if (n_args > 7 && args[7] != mp_const_none) {
        mp_buffer_info_t bi;
        mp_get_buffer_raise(args[7], &bi, MP_BUFFER_READ);
        if (bi.len < need) mp_raise_ValueError(MP_ERROR_TEXT("texture data too small"));
        data = bi.buf;
        data_len = need;
    }
    int r = gfx_gl_tex_image_2d(target, level, internal, (uint32_t)w, (uint32_t)h, format, type,
                                data, data_len);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_tex_image_2d_obj, 4, 8, mgl_tex_image_2d);

static mp_obj_t mgl_tex_parameteri(size_t n_args, const mp_obj_t *args) {
    uint32_t target = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : GL_TEXTURE_2D;
    uint64_t cmd[6] = {target, (uint64_t)mp_obj_get_int(args[1]),
                       (uint64_t)mp_obj_get_int(args[2])};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_TEX_PARAMETERI, 3, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_tex_parameteri_obj, 2, 3, mgl_tex_parameteri);

#define MGL_ATTRIB_FLOATS 4096

static mp_obj_t mgl_vertex_attrib_pointer(size_t n_args, const mp_obj_t *args) {
    uint32_t index = (uint32_t)mp_obj_get_int(args[0]);
    uint32_t size = (uint32_t)mp_obj_get_int(args[1]);
    uint32_t type = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : GL_FLOAT;
    uint32_t norm = n_args > 3 ? (uint32_t)mp_obj_get_int(args[3]) : 0;
    uint32_t stride = n_args > 4 ? (uint32_t)mp_obj_get_int(args[4]) : 0;
    if (n_args > 5 && mp_obj_is_type(args[5], &mp_type_list)) {
        /* client-side array: the numbers are uploaded as floats into a library buffer */
        mp_obj_list_t *lst = MP_OBJ_TO_PTR(args[5]);
        size_t n = lst->len;
        if (n == 0 || n > MGL_ATTRIB_FLOATS) mp_raise_ValueError(MP_ERROR_TEXT("attrib data size"));
        float *data = m_new(float, n);
        for (size_t k = 0; k < n; k++) data[k] = mp_obj_get_float(lst->items[k]);
        int r = gfx_gl_attrib_data(index, size, GL_FLOAT, norm, stride, data, (uint32_t)(n * sizeof(float)));
        m_del(float, data, n);
        return MP_OBJ_NEW_SMALL_INT(r);
    }
    /* a byte offset into the bound GL_ARRAY_BUFFER; 0 is a real offset */
    uint64_t off = n_args > 5 ? (uint64_t)mp_obj_get_int(args[5]) : 0;
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_attrib_pointer(index, size, type, norm, stride, off));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_vertex_attrib_pointer_obj, 2, 6,
                                           mgl_vertex_attrib_pointer);

static mp_obj_t mgl_enable_vertex_attrib_array(mp_obj_t idx_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(idx_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_ENABLE_VERTEX_ATTRIB_ARRAY, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_enable_vertex_attrib_array_obj, mgl_enable_vertex_attrib_array);

static mp_obj_t mgl_disable_vertex_attrib_array(mp_obj_t idx_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(idx_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_DISABLE_VERTEX_ATTRIB_ARRAY, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_disable_vertex_attrib_array_obj, mgl_disable_vertex_attrib_array);

static mp_obj_t mgl_draw_arrays(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(args[0]),
                       n_args > 1 ? (uint64_t)mp_obj_get_int(args[1]) : 0,
                       (uint64_t)mp_obj_get_int(args[2])};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_DRAW_ARRAYS, 3, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_draw_arrays_obj, 3, 3, mgl_draw_arrays);

static mp_obj_t mgl_use_program(mp_obj_t prog_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(prog_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_USE_PROGRAM, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_use_program_obj, mgl_use_program);


static mp_obj_t mgl_matrix_mode(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : GL_MODELVIEW};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_MATRIX_MODE, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_matrix_mode_obj, 0, 1, mgl_matrix_mode);

static mp_obj_t mgl_load_identity(void) {
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_LOAD_IDENTITY, 0, NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_load_identity_obj, mgl_load_identity);

static mp_obj_t mgl_push_matrix(void) {
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_PUSH_MATRIX, 0, NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_push_matrix_obj, mgl_push_matrix);

static mp_obj_t mgl_pop_matrix(void) {
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_POP_MATRIX, 0, NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_pop_matrix_obj, mgl_pop_matrix);

static mp_obj_t mgl_translatef(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t z_obj) {
    uint64_t cmd[6] = {f32b_(mp_obj_get_float(x_obj)), f32b_(mp_obj_get_float(y_obj)),
                       f32b_(mp_obj_get_float(z_obj))};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_TRANSLATEF, 3, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_translatef_obj, mgl_translatef);

static mp_obj_t mgl_rotatef(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {f32b_(mp_obj_get_float(args[0])), f32b_(mp_obj_get_float(args[1])),
                       f32b_(mp_obj_get_float(args[2])), f32b_(mp_obj_get_float(args[3]))};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_ROTATEF, 4, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_rotatef_obj, 4, 4, mgl_rotatef);

static mp_obj_t mgl_scalef(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {f32b_(mp_obj_get_float(args[0])),
                       f32b_(n_args > 1 ? mp_obj_get_float(args[1]) : 1.0f),
                       f32b_(n_args > 2 ? mp_obj_get_float(args[2]) : 1.0f)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_SCALEF, 3, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_scalef_obj, 1, 3, mgl_scalef);

static mp_obj_t mgl_shade_model(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : GL_SMOOTH};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_SHADE_MODEL, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_shade_model_obj, 0, 1, mgl_shade_model);

static mp_obj_t mgl_enable(mp_obj_t cap_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(cap_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_ENABLE, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_enable_obj, mgl_enable);

static mp_obj_t mgl_disable(mp_obj_t cap_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(cap_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_DISABLE, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_disable_obj, mgl_disable);

static mp_obj_t mgl_viewport(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : 0,
                       n_args > 1 ? (uint64_t)mp_obj_get_int(args[1]) : 0,
                       n_args > 2 ? (uint64_t)mp_obj_get_int(args[2]) : gfx_gl_width(),
                       n_args > 3 ? (uint64_t)mp_obj_get_int(args[3]) : gfx_gl_height()};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_VIEWPORT, 4, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_viewport_obj, 0, 4, mgl_viewport);

static mp_obj_t mgl_depth_func(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : GL_LESS};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_DEPTH_FUNC, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_depth_func_obj, 0, 1, mgl_depth_func);

static mp_obj_t mgl_cull_face(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : GL_BACK};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_CULL_FACE, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_cull_face_obj, 0, 1, mgl_cull_face);

static mp_obj_t mgl_front_face(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : GL_CCW};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_FRONT_FACE, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_front_face_obj, 0, 1, mgl_front_face);

static mp_obj_t mgl_line_width(mp_obj_t w_obj) {
    uint64_t cmd[6] = {f32b_(mp_obj_get_float(w_obj))};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_LINE_WIDTH, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_line_width_obj, mgl_line_width);

static mp_obj_t mgl_point_size(mp_obj_t s_obj) {
    uint64_t cmd[6] = {f32b_(mp_obj_get_float(s_obj))};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_POINT_SIZE, 1, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_point_size_obj, mgl_point_size);

static mp_obj_t mgl_blend_func(mp_obj_t s_obj, mp_obj_t d_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(s_obj), (uint64_t)mp_obj_get_int(d_obj)};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_BLEND_FUNC, 2, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mgl_blend_func_obj, mgl_blend_func);

static mp_obj_t mgl_rectf(size_t n_args, const mp_obj_t *args) {
    uint64_t cmd[6] = {f32b_(mp_obj_get_float(args[0])), f32b_(mp_obj_get_float(args[1])),
                       f32b_(mp_obj_get_float(args[2])), f32b_(mp_obj_get_float(args[3]))};
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_simple_op(GPU_CMD_GL_RECTF, 4, cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mgl_rectf_obj, 4, 4, mgl_rectf);

static mp_obj_t mgl_lightfv(mp_obj_t light_obj, mp_obj_t pname_obj, mp_obj_t v_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(light_obj), (uint64_t)mp_obj_get_int(pname_obj),
                       0};
    float v[4] = {0, 0, 0, 1};
    mp_obj_t *items = NULL;
    size_t len = 0;
    mp_obj_get_array(v_obj, &len, &items);
    for (int i = 0; i < 4 && (size_t)i < len; i++) v[i] = mp_obj_get_float(items[i]);
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_payload(GPU_CMD_GL_LIGHTFV, 3, cmd, v, 16, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_lightfv_obj, mgl_lightfv);

static mp_obj_t mgl_materialfv(mp_obj_t face_obj, mp_obj_t pname_obj, mp_obj_t v_obj) {
    uint64_t cmd[6] = {(uint64_t)mp_obj_get_int(face_obj), (uint64_t)mp_obj_get_int(pname_obj),
                       0};
    float v[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    mp_obj_t *items = NULL;
    size_t len = 0;
    mp_obj_get_array(v_obj, &len, &items);
    for (int i = 0; i < 4 && (size_t)i < len; i++) v[i] = mp_obj_get_float(items[i]);
    return MP_OBJ_NEW_SMALL_INT(gfx_gl_payload(GPU_CMD_GL_MATERIALFV, 3, cmd, v, 16, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mgl_materialfv_obj, mgl_materialfv);

static mp_obj_t mgl_const(mp_obj_t name_obj) {
    const char *name = mp_obj_str_get_str(name_obj);
    const char *short_name = name;
    if (strncmp(name, "GL_", 3) == 0) short_name = name + 3;
    const struct mesa_const *c = mesa_const_find(mesa_gl_consts, mesa_gl_const_count, short_name);
    if (!c) return mp_const_none;
    return mp_obj_new_int_from_uint(c->value);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mgl_const_obj, mgl_const);

static mp_obj_t mgl_consts(void) {
    mp_obj_dict_t *d = mp_obj_new_dict(0);
    for (uint32_t i = 0; i < mesa_gl_const_count; i++) {
        mp_obj_dict_store(MP_OBJ_FROM_PTR(d), mp_obj_new_str_from_cstr(mesa_gl_consts[i].name),
                          mp_obj_new_int_from_uint(mesa_gl_consts[i].value));
    }
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mgl_consts_obj, mgl_consts);

static const mp_rom_map_elem_t mgl_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gl)},
    {MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&mgl_log_obj)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mgl_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mgl_close_obj)},
    {MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&mgl_open_obj)},
    {MP_ROM_QSTR(MP_QSTR_present), MP_ROM_PTR(&mgl_present_obj)},
    {MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&mgl_size_obj)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&mgl_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_prime), MP_ROM_PTR(&mgl_prime_obj)},
    {MP_ROM_QSTR(MP_QSTR_error), MP_ROM_PTR(&mgl_error_obj)},
    {MP_ROM_QSTR(MP_QSTR_getError), MP_ROM_PTR(&mgl_get_error_obj)},
    {MP_ROM_QSTR(MP_QSTR_string), MP_ROM_PTR(&mgl_string_obj)},
    {MP_ROM_QSTR(MP_QSTR_finish), MP_ROM_PTR(&mgl_finish_obj)},
    {MP_ROM_QSTR(MP_QSTR_clearColor), MP_ROM_PTR(&mgl_clear_color_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&mgl_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_readPixels), MP_ROM_PTR(&mgl_read_pixels_obj)},
    {MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&mgl_pixel_obj)},
    {MP_ROM_QSTR(MP_QSTR_crc), MP_ROM_PTR(&mgl_crc_obj)},
    {MP_ROM_QSTR(MP_QSTR_begin), MP_ROM_PTR(&mgl_begin_obj)},
    {MP_ROM_QSTR(MP_QSTR_end), MP_ROM_PTR(&mgl_end_obj)},
    {MP_ROM_QSTR(MP_QSTR_vertex2f), MP_ROM_PTR(&mgl_vertex2f_obj)},
    {MP_ROM_QSTR(MP_QSTR_vertex3f), MP_ROM_PTR(&mgl_vertex3f_obj)},
    {MP_ROM_QSTR(MP_QSTR_vertex4f), MP_ROM_PTR(&mgl_vertex4f_obj)},
    {MP_ROM_QSTR(MP_QSTR_color3f), MP_ROM_PTR(&mgl_color3f_obj)},
    {MP_ROM_QSTR(MP_QSTR_color4f), MP_ROM_PTR(&mgl_color4f_obj)},
    {MP_ROM_QSTR(MP_QSTR_normal3f), MP_ROM_PTR(&mgl_normal3f_obj)},
    {MP_ROM_QSTR(MP_QSTR_texCoord2f), MP_ROM_PTR(&mgl_texcoord2f_obj)},
    {MP_ROM_QSTR(MP_QSTR_createShader), MP_ROM_PTR(&mgl_create_shader_obj)},
    {MP_ROM_QSTR(MP_QSTR_createProgram), MP_ROM_PTR(&mgl_create_program_obj)},
    {MP_ROM_QSTR(MP_QSTR_getUniformLocation), MP_ROM_PTR(&mgl_uniform_location_obj)},
    {MP_ROM_QSTR(MP_QSTR_uniformMatrix4fv), MP_ROM_PTR(&mgl_uniform_matrix4fv_obj)},
    {MP_ROM_QSTR(MP_QSTR_uniform1f), MP_ROM_PTR(&mgl_uniform1f_obj)},
    {MP_ROM_QSTR(MP_QSTR_uniform2f), MP_ROM_PTR(&mgl_uniform2f_obj)},
    {MP_ROM_QSTR(MP_QSTR_uniform3f), MP_ROM_PTR(&mgl_uniform3f_obj)},
    {MP_ROM_QSTR(MP_QSTR_uniform4f), MP_ROM_PTR(&mgl_uniform4f_obj)},
    {MP_ROM_QSTR(MP_QSTR_uniform1i), MP_ROM_PTR(&mgl_uniform1i_obj)},
    {MP_ROM_QSTR(MP_QSTR_perspective), MP_ROM_PTR(&mgl_perspective_obj)},
    {MP_ROM_QSTR(MP_QSTR_lookAt), MP_ROM_PTR(&mgl_look_at_obj)},
    {MP_ROM_QSTR(MP_QSTR_ortho), MP_ROM_PTR(&mgl_ortho_obj)},
    {MP_ROM_QSTR(MP_QSTR_genBuffers), MP_ROM_PTR(&mgl_gen_buffers_obj)},
    {MP_ROM_QSTR(MP_QSTR_genTextures), MP_ROM_PTR(&mgl_gen_textures_obj)},
    {MP_ROM_QSTR(MP_QSTR_bindBuffer), MP_ROM_PTR(&mgl_bind_buffer_obj)},
    {MP_ROM_QSTR(MP_QSTR_bindTexture), MP_ROM_PTR(&mgl_bind_texture_obj)},
    {MP_ROM_QSTR(MP_QSTR_bufferData), MP_ROM_PTR(&mgl_buffer_data_obj)},
    {MP_ROM_QSTR(MP_QSTR_texImage2D), MP_ROM_PTR(&mgl_tex_image_2d_obj)},
    {MP_ROM_QSTR(MP_QSTR_texParameteri), MP_ROM_PTR(&mgl_tex_parameteri_obj)},
    {MP_ROM_QSTR(MP_QSTR_vertexAttribPointer), MP_ROM_PTR(&mgl_vertex_attrib_pointer_obj)},
    {MP_ROM_QSTR(MP_QSTR_enableVertexAttribArray), MP_ROM_PTR(&mgl_enable_vertex_attrib_array_obj)},
    {MP_ROM_QSTR(MP_QSTR_disableVertexAttribArray), MP_ROM_PTR(&mgl_disable_vertex_attrib_array_obj)},
    {MP_ROM_QSTR(MP_QSTR_drawArrays), MP_ROM_PTR(&mgl_draw_arrays_obj)},
    {MP_ROM_QSTR(MP_QSTR_useProgram), MP_ROM_PTR(&mgl_use_program_obj)},
    {MP_ROM_QSTR(MP_QSTR_matrixMode), MP_ROM_PTR(&mgl_matrix_mode_obj)},
    {MP_ROM_QSTR(MP_QSTR_loadIdentity), MP_ROM_PTR(&mgl_load_identity_obj)},
    {MP_ROM_QSTR(MP_QSTR_pushMatrix), MP_ROM_PTR(&mgl_push_matrix_obj)},
    {MP_ROM_QSTR(MP_QSTR_popMatrix), MP_ROM_PTR(&mgl_pop_matrix_obj)},
    {MP_ROM_QSTR(MP_QSTR_translatef), MP_ROM_PTR(&mgl_translatef_obj)},
    {MP_ROM_QSTR(MP_QSTR_rotatef), MP_ROM_PTR(&mgl_rotatef_obj)},
    {MP_ROM_QSTR(MP_QSTR_scalef), MP_ROM_PTR(&mgl_scalef_obj)},
    {MP_ROM_QSTR(MP_QSTR_shadeModel), MP_ROM_PTR(&mgl_shade_model_obj)},
    {MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&mgl_enable_obj)},
    {MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&mgl_disable_obj)},
    {MP_ROM_QSTR(MP_QSTR_viewport), MP_ROM_PTR(&mgl_viewport_obj)},
    {MP_ROM_QSTR(MP_QSTR_depthFunc), MP_ROM_PTR(&mgl_depth_func_obj)},
    {MP_ROM_QSTR(MP_QSTR_cullFace), MP_ROM_PTR(&mgl_cull_face_obj)},
    {MP_ROM_QSTR(MP_QSTR_frontFace), MP_ROM_PTR(&mgl_front_face_obj)},
    {MP_ROM_QSTR(MP_QSTR_lineWidth), MP_ROM_PTR(&mgl_line_width_obj)},
    {MP_ROM_QSTR(MP_QSTR_pointSize), MP_ROM_PTR(&mgl_point_size_obj)},
    {MP_ROM_QSTR(MP_QSTR_blendFunc), MP_ROM_PTR(&mgl_blend_func_obj)},
    {MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&mgl_rectf_obj)},
    {MP_ROM_QSTR(MP_QSTR_lightfv), MP_ROM_PTR(&mgl_lightfv_obj)},
    {MP_ROM_QSTR(MP_QSTR_materialfv), MP_ROM_PTR(&mgl_materialfv_obj)},
    {MP_ROM_QSTR(MP_QSTR_const), MP_ROM_PTR(&mgl_const_obj)},
    {MP_ROM_QSTR(MP_QSTR_consts), MP_ROM_PTR(&mgl_consts_obj)},
    {MP_ROM_QSTR(MP_QSTR_POINTS), MP_ROM_INT(GL_POINTS)},
    {MP_ROM_QSTR(MP_QSTR_LINES), MP_ROM_INT(GL_LINES)},
    {MP_ROM_QSTR(MP_QSTR_LINE_STRIP), MP_ROM_INT(GL_LINE_STRIP)},
    {MP_ROM_QSTR(MP_QSTR_LINE_LOOP), MP_ROM_INT(GL_LINE_LOOP)},
    {MP_ROM_QSTR(MP_QSTR_TRIANGLES), MP_ROM_INT(GL_TRIANGLES)},
    {MP_ROM_QSTR(MP_QSTR_TRIANGLE_STRIP), MP_ROM_INT(GL_TRIANGLE_STRIP)},
    {MP_ROM_QSTR(MP_QSTR_TRIANGLE_FAN), MP_ROM_INT(GL_TRIANGLE_FAN)},
    {MP_ROM_QSTR(MP_QSTR_QUADS), MP_ROM_INT(GL_QUADS)},
    {MP_ROM_QSTR(MP_QSTR_POLYGON), MP_ROM_INT(GL_POLYGON)},
    {MP_ROM_QSTR(MP_QSTR_COLOR_BUFFER_BIT), MP_ROM_INT(GL_COLOR_BUFFER_BIT)},
    {MP_ROM_QSTR(MP_QSTR_DEPTH_BUFFER_BIT), MP_ROM_INT(GL_DEPTH_BUFFER_BIT)},
    {MP_ROM_QSTR(MP_QSTR_STENCIL_BUFFER_BIT), MP_ROM_INT(GL_STENCIL_BUFFER_BIT)},
    {MP_ROM_QSTR(MP_QSTR_VERTEX_SHADER), MP_ROM_INT(GL_VERTEX_SHADER)},
    {MP_ROM_QSTR(MP_QSTR_FRAGMENT_SHADER), MP_ROM_INT(GL_FRAGMENT_SHADER)},
    {MP_ROM_QSTR(MP_QSTR_COMPILE_STATUS), MP_ROM_INT(GL_COMPILE_STATUS)},
    {MP_ROM_QSTR(MP_QSTR_LINK_STATUS), MP_ROM_INT(GL_LINK_STATUS)},
    {MP_ROM_QSTR(MP_QSTR_TEXTURE_2D), MP_ROM_INT(GL_TEXTURE_2D)},
    {MP_ROM_QSTR(MP_QSTR_TEXTURE0), MP_ROM_INT(GL_TEXTURE0)},
    {MP_ROM_QSTR(MP_QSTR_RGBA), MP_ROM_INT(GL_RGBA)},
    {MP_ROM_QSTR(MP_QSTR_RGB), MP_ROM_INT(GL_RGB)},
    {MP_ROM_QSTR(MP_QSTR_RGBA8), MP_ROM_INT(GL_RGBA8)},
    {MP_ROM_QSTR(MP_QSTR_UNSIGNED_BYTE), MP_ROM_INT(GL_UNSIGNED_BYTE)},
    {MP_ROM_QSTR(MP_QSTR_FLOAT), MP_ROM_INT(GL_FLOAT)},
    {MP_ROM_QSTR(MP_QSTR_ARRAY_BUFFER), MP_ROM_INT(GL_ARRAY_BUFFER)},
    {MP_ROM_QSTR(MP_QSTR_STATIC_DRAW), MP_ROM_INT(GL_STATIC_DRAW)},
    {MP_ROM_QSTR(MP_QSTR_DYNAMIC_DRAW), MP_ROM_INT(GL_DYNAMIC_DRAW)},
    {MP_ROM_QSTR(MP_QSTR_DEPTH_TEST), MP_ROM_INT(GL_DEPTH_TEST)},
    {MP_ROM_QSTR(MP_QSTR_LIGHTING), MP_ROM_INT(GL_LIGHTING)},
    {MP_ROM_QSTR(MP_QSTR_LIGHT0), MP_ROM_INT(GL_LIGHT0)},
    {MP_ROM_QSTR(MP_QSTR_BLEND), MP_ROM_INT(GL_BLEND)},
    {MP_ROM_QSTR(MP_QSTR_CULL_FACE), MP_ROM_INT(GL_CULL_FACE)},
    {MP_ROM_QSTR(MP_QSTR_MODELVIEW), MP_ROM_INT(GL_MODELVIEW)},
    {MP_ROM_QSTR(MP_QSTR_PROJECTION), MP_ROM_INT(GL_PROJECTION)},
    {MP_ROM_QSTR(MP_QSTR_TEXTURE), MP_ROM_INT(GL_TEXTURE)},
    {MP_ROM_QSTR(MP_QSTR_VENDOR), MP_ROM_INT(GL_VENDOR)},
    {MP_ROM_QSTR(MP_QSTR_RENDERER), MP_ROM_INT(GL_RENDERER)},
    {MP_ROM_QSTR(MP_QSTR_VERSION), MP_ROM_INT(GL_VERSION)},
    {MP_ROM_QSTR(MP_QSTR_SHADING_LANGUAGE_VERSION), MP_ROM_INT(GL_SHADING_LANGUAGE_VERSION)},
    {MP_ROM_QSTR(MP_QSTR_NEAREST), MP_ROM_INT(GL_NEAREST)},
    {MP_ROM_QSTR(MP_QSTR_LINEAR), MP_ROM_INT(GL_LINEAR)},
    {MP_ROM_QSTR(MP_QSTR_TEXTURE_MIN_FILTER), MP_ROM_INT(GL_TEXTURE_MIN_FILTER)},
    {MP_ROM_QSTR(MP_QSTR_TEXTURE_MAG_FILTER), MP_ROM_INT(GL_TEXTURE_MAG_FILTER)},
    {MP_ROM_QSTR(MP_QSTR_POSITION), MP_ROM_INT(GL_POSITION)},
    {MP_ROM_QSTR(MP_QSTR_AMBIENT), MP_ROM_INT(GL_AMBIENT)},
    {MP_ROM_QSTR(MP_QSTR_DIFFUSE), MP_ROM_INT(GL_DIFFUSE)},
    {MP_ROM_QSTR(MP_QSTR_SPECULAR), MP_ROM_INT(GL_SPECULAR)},
    {MP_ROM_QSTR(MP_QSTR_SMOOTH), MP_ROM_INT(GL_SMOOTH)},
    {MP_ROM_QSTR(MP_QSTR_FLAT), MP_ROM_INT(GL_FLAT)},
};

static MP_DEFINE_CONST_DICT(mgl_globals, mgl_globals_table);

const mp_obj_module_t mp_module_gl = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mgl_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gl, mp_module_gl);
