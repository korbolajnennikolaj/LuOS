// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "py/obj.h"
#include "py/binary.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_sigs.h"

#include <stdint.h>
#include <string.h>

static int mpy_gpu_client = -1;

static uint16_t mpy_gl_opcode(const char *name) {
    for (int i = 0; i < GPU_CMD_GL_COUNT; i++) {
        const struct mesa_gl_sig *s = &mesa_gl_sigs[i];
        if (s->name && strcmp(s->name, name) == 0) return (uint16_t)(GPU_CMD_GL_FIRST + i);
    }
    return 0;
}

static uint16_t mpy_vk_opcode(const char *name) {
    for (int i = 0; i < GPU_CMD_VK_COUNT; i++) {
        const char *n = gpu_cmd_name(GPU_API_VULKAN, GPU_CMD_VK_FIRST + i);
        if (n && strcmp(n, name) == 0) return (uint16_t)(GPU_CMD_VK_FIRST + i);
    }
    return 0;
}

static uint32_t mpy_f32(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static mp_obj_t mpy_gpu_start(size_t n_args, const mp_obj_t *args) {
    int api = n_args > 0 && mp_obj_is_integer(args[0]) ? mp_obj_get_int(args[0]) : 1;
    int r = mesa_start();
    if (r) {
        mp_obj_t tuple[2] = {mp_const_none, MP_OBJ_NEW_SMALL_INT(r)};
        return mp_obj_new_tuple(2, tuple);
    }
    int dev = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    int cl = gpu_client_open(dev, api == 3 ? GPU_API_VULKAN : GPU_API_OPENGL);
    if (cl < 0) {
        mp_obj_t tuple[2] = {mp_const_none, MP_OBJ_NEW_SMALL_INT(cl)};
        return mp_obj_new_tuple(2, tuple);
    }
    mpy_gpu_client = cl;
    return MP_OBJ_NEW_SMALL_INT(cl);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_gpu_start_obj, 0, 2, mpy_gpu_start);

static mp_obj_t mpy_gpu_close(void) {
    if (mpy_gpu_client >= 0) {
        gpu_client_close(mpy_gpu_client);
        mpy_gpu_client = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpy_gpu_close_obj, mpy_gpu_close);

static mp_obj_t mpy_gpu_gl_call(size_t n_args, const mp_obj_t *args) {
    if (n_args < 1 || !mp_obj_is_str(args[0])) {
        mp_raise_ValueError(MP_ERROR_TEXT("gl.call(name, ...)"));
    }
    const char *name = mp_obj_str_get_str(args[0]);
    uint16_t op = mpy_gl_opcode(name);
    if (!op) mp_raise_ValueError(MP_ERROR_TEXT("unknown GL command"));
    const struct mesa_gl_sig *sig = &mesa_gl_sigs[op - GPU_CMD_GL_FIRST];
    if (!sig->name || !sig->argc) mp_raise_ValueError(MP_ERROR_TEXT("unsupported command"));
    if (mpy_gpu_client < 0) mp_raise_OSError(MP_ENODEV);

    uint64_t cargs[16] = {0};
    uint8_t payload[512];
    uint32_t used = 0;
    uint32_t n_extra = sig->argc > 6 ? (uint32_t)sig->argc - 6 : 0;
    uint32_t data_off = 8 * n_extra;
    uint16_t result_cap = 0;

    for (int i = 0; i < sig->argc; i++) {
        int idx = 1 + i;
        bool have = idx < (int)n_args;
        bool is_ptr = sig->types[i] == M_T_P;
        if (i < 6) {
            if (is_ptr) {
                if (have && (mp_obj_is_str(args[idx]) || mp_obj_is_type(args[idx], &mp_type_bytes))) {
                    mp_buffer_info_t bi;
                    mp_get_buffer_raise(args[idx], &bi, MP_BUFFER_READ);
                    if (data_off + used + bi.len > sizeof(payload))
                        mp_raise_ValueError(MP_ERROR_TEXT("payload too large"));
                    memcpy(payload + data_off + used, bi.buf, bi.len);
                    used += (uint32_t)bi.len;
                    cargs[i] = used - bi.len;
                } else if (i == sig->out_arg || i == sig->out2_arg) {
                    cargs[i] = 0xFFFF;
                } else {
                    cargs[i] = 0xFFFE;
                }
            } else if (sig->types[i] == M_T_F || sig->types[i] == M_T_D) {
                float f = have ? mp_obj_get_float(args[idx]) : 0.0f;
                cargs[i] = mpy_f32(f);
                if (sig->types[i] == M_T_D) {
                    double d = have ? mp_obj_get_float(args[idx]) : 0.0;
                    memcpy(&cargs[i], &d, 8);
                }
            } else {
                cargs[i] = have && mp_obj_is_int(args[idx]) ? (uint64_t)mp_obj_get_int(args[idx]) : 0;
            }
        } else {
            uint64_t v = 0;
            if (is_ptr)
                v = (i == sig->out_arg || i == sig->out2_arg) ? 0xFFFF : 0xFFFE;
            else if (sig->types[i] == M_T_D) {
                double d = have ? mp_obj_get_float(args[idx]) : 0.0;
                memcpy(&v, &d, 8);
            } else if (sig->types[i] == M_T_F)
                v = mpy_f32(have ? mp_obj_get_float(args[idx]) : 0.0f);
            else if (have && mp_obj_is_int(args[idx]))
                v = (uint64_t)mp_obj_get_int(args[idx]);
            memcpy(payload + 8 * (size_t)(i - 6), &v, 8);
        }
    }
    if ((int)n_args == sig->argc + 2 && mp_obj_is_int(args[n_args - 1]))
        result_cap = (uint16_t)mp_obj_get_int(args[n_args - 1]);

    uint32_t total = data_off + used;
    int r = gpu_cmd_begin(mpy_gpu_client);
    if (!r)
        r = gpu_cmd_gl(mpy_gpu_client, op, (uint8_t)sig->argc, cargs, total ? payload : NULL,
                       (uint16_t)total, result_cap);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_gpu_gl_call_obj, 1, 12, mpy_gpu_gl_call);

static mp_obj_t mpy_gpu_vk_call(size_t n_args, const mp_obj_t *args) {
    if (n_args < 1 || !mp_obj_is_str(args[0])) {
        mp_raise_ValueError(MP_ERROR_TEXT("vk.call(name, ...)"));
    }
    const char *name = mp_obj_str_get_str(args[0]);
    uint16_t op = mpy_vk_opcode(name);
    if (!op) mp_raise_ValueError(MP_ERROR_TEXT("unknown Vulkan command"));
    if (mpy_gpu_client < 0) mp_raise_OSError(MP_ENODEV);
    const struct mesa_vk_sig *sig = &mesa_vk_sigs[op - GPU_CMD_VK_FIRST];
    if (!sig->argc) mp_raise_ValueError(MP_ERROR_TEXT("unsupported command"));

    uint64_t cargs[16] = {0};
    uint8_t payload[512];
    bool has_data = n_args >= 2 && (mp_obj_is_str(args[n_args - 1]) || mp_obj_is_type(args[n_args - 1], &mp_type_bytes));
    int argc = (int)n_args - 1 - (has_data ? 1 : 0);
    if (argc > 16) mp_raise_ValueError(MP_ERROR_TEXT("at most 16 arguments"));
    for (int i = 0; i < argc; i++) {
        mp_obj_t a = args[1 + i];
        cargs[i] = mp_obj_is_int(a) ? (uint64_t)mp_obj_get_int(a) : 0xFFFE;
    }
    uint32_t total = 0;
    if (has_data) {
        mp_buffer_info_t bi;
        mp_get_buffer_raise(args[n_args - 1], &bi, MP_BUFFER_READ);
        if (8 + bi.len > sizeof(payload)) mp_raise_ValueError(MP_ERROR_TEXT("data too large"));
        memset(payload, 0, 8);
        memcpy(payload + 8, bi.buf, bi.len);
        total = 8 + (uint32_t)bi.len;
    }
    int r = gpu_cmd_begin(mpy_gpu_client);
    if (!r)
        r = gpu_cmd_vk(mpy_gpu_client, op, (uint8_t)argc, cargs, total ? payload : NULL,
                       (uint16_t)total, 64);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_gpu_vk_call_obj, 1, 12, mpy_gpu_vk_call);

static mp_obj_t mpy_gpu_flush(size_t n_args, const mp_obj_t *args) {
    if (mpy_gpu_client < 0) mp_raise_OSError(MP_ENODEV);
    uint32_t to = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : 15000;
    return MP_OBJ_NEW_SMALL_INT(gpu_flush(mpy_gpu_client, to));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_gpu_flush_obj, 0, 1, mpy_gpu_flush);

static mp_obj_t mpy_gpu_result(mp_obj_t idx_obj) {
    if (mpy_gpu_client < 0) mp_raise_OSError(MP_ENODEV);
    int idx = mp_obj_get_int(idx_obj);
    uint8_t out[256];
    uint16_t len = 0;
    int r = gpu_result(mpy_gpu_client, (uint16_t)idx, out, sizeof(out), &len);
    if (r) return mp_const_none;
    if (len == 8) {
        uint64_t v = 0;
        memcpy(&v, out, 8);
        return mp_obj_new_int_from_uint((mp_uint_t)v);
    }
    return mp_obj_new_bytes(out, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpy_gpu_result_obj, mpy_gpu_result);

static mp_obj_t mpy_gpu_info(void) {
    struct mesa_host_info info;
    if (mesa_host_info(&info) || !mesa_host_pid_get()) return mp_const_none;
    mp_obj_dict_t *d = mp_obj_new_dict(4);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_pid),
                      MP_OBJ_NEW_SMALL_INT(info.pid));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_renderer),
                      mp_obj_new_str(info.renderer, strlen(info.renderer)));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_engine),
                      mp_obj_new_str(info.env, strlen(info.env)));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_prime),
                      mp_obj_new_str(mesa_prime_name(mesa_prime_get()),
                                     strlen(mesa_prime_name(mesa_prime_get()))));
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpy_gpu_info_obj, mpy_gpu_info);

static mp_obj_t mpy_gpu_cube(size_t n_args, const mp_obj_t *args) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : 5;
    uint32_t frames = 0, fps = 0;
    bool want_vk = n_args > 1 && mp_obj_is_str(args[1]) &&
                   strcmp(mp_obj_str_get_str(args[1]), "vk") == 0;
    int r = want_vk ? gpu_vkkmv_run(&opts, &frames, &fps)
                    : gpu_kmscube_run(&opts, &frames, &fps);
    mp_obj_t tuple[3] = {MP_OBJ_NEW_SMALL_INT(r), MP_OBJ_NEW_SMALL_INT(frames),
                         MP_OBJ_NEW_SMALL_INT(fps)};
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_gpu_cube_obj, 0, 2, mpy_gpu_cube);

static mp_obj_t mpy_gpu_gears(mp_obj_t sec_obj) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = (uint32_t)mp_obj_get_int(sec_obj);
    uint32_t frames = 0, fps = 0;
    int r = gpu_glxgears_run(&opts, &frames, &fps);
    mp_obj_t tuple[3] = {MP_OBJ_NEW_SMALL_INT(r), MP_OBJ_NEW_SMALL_INT(frames),
                         MP_OBJ_NEW_SMALL_INT(fps)};
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpy_gpu_gears_obj, mpy_gpu_gears);

static mp_obj_t mpy_gpu_shader_test(mp_obj_t which_obj) {
    uint32_t crc = 0;
    char note[96] = "";
    bool want_vk = mp_obj_is_str(which_obj) && strcmp(mp_obj_str_get_str(which_obj), "vk") == 0;
    int r = want_vk ? gpu_shader_test_vk(&crc, note, sizeof(note))
                    : gpu_shader_test_gl(&crc, note, sizeof(note));
    mp_obj_t tuple[2] = {MP_OBJ_NEW_SMALL_INT(r), MP_OBJ_NEW_SMALL_INT(crc)};
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpy_gpu_shader_test_obj, mpy_gpu_shader_test);

static mp_obj_t mpy_gpu_prime(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) {
        mp_obj_list_t *l = mp_obj_new_list(0, NULL);
        for (int i = 0; i < mesa_prime_count(); i++)
            mp_obj_list_append(MP_OBJ_FROM_PTR(l), mp_obj_new_str(mesa_prime_name(i),
                                                strlen(mesa_prime_name(i))));
        return MP_OBJ_FROM_PTR(l);
    }
    const char *s = mp_obj_str_get_str(args[0]);
    int sel = strcmp(s, "llvm") == 0 || strcmp(s, "llvmpipe") == 0
                  ? MESA_PRIME_LLVM
                  : (strcmp(s, "auto") == 0 ? MESA_PRIME_AUTO : mp_obj_get_int(args[0]));
    return MP_OBJ_NEW_SMALL_INT(mesa_prime_set(sel));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_gpu_prime_obj, 0, 1, mpy_gpu_prime);

static const mp_rom_map_elem_t mpy_gpu_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gpu)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mpy_gpu_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mpy_gpu_close_obj)},
    {MP_ROM_QSTR(MP_QSTR_gl_call), MP_ROM_PTR(&mpy_gpu_gl_call_obj)},
    {MP_ROM_QSTR(MP_QSTR_vk_call), MP_ROM_PTR(&mpy_gpu_vk_call_obj)},
    {MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mpy_gpu_flush_obj)},
    {MP_ROM_QSTR(MP_QSTR_result), MP_ROM_PTR(&mpy_gpu_result_obj)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&mpy_gpu_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_cube), MP_ROM_PTR(&mpy_gpu_cube_obj)},
    {MP_ROM_QSTR(MP_QSTR_gears), MP_ROM_PTR(&mpy_gpu_gears_obj)},
    {MP_ROM_QSTR(MP_QSTR_shader_test), MP_ROM_PTR(&mpy_gpu_shader_test_obj)},
    {MP_ROM_QSTR(MP_QSTR_prime), MP_ROM_PTR(&mpy_gpu_prime_obj)},
    {MP_ROM_QSTR(MP_QSTR_OPENGL), MP_OBJ_NEW_SMALL_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_VULKAN), MP_OBJ_NEW_SMALL_INT(3)},
};
static MP_DEFINE_CONST_DICT(mpy_gpu_globals, mpy_gpu_globals_table);

const mp_obj_module_t mp_module_gpu = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mpy_gpu_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gpu, mp_module_gpu);
