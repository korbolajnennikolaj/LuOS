// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef MESA_SIGS_H
#define MESA_SIGS_H

#include <stdint.h>

#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa_vk_uapi.h"

#define MESA_SIG_ARGS 20
#define MESA_SIG_NONE 255

enum mesa_arg_type {
    M_T_I = 0,
    M_T_F = 1,
    M_T_D = 2,
    M_T_P = 3,
};

enum mesa_out_kind {
    M_OUT_NONE = 0,
    M_OUT_CAP,
    M_OUT_EXACT,
    M_OUT_CSTR,
    M_OUT_SRCS,
};

struct mesa_gl_sig {
    const char *name;
    uint8_t argc;
    uint8_t out_kind;
    uint8_t out_arg;
    uint8_t out_len_arg;
    uint8_t out_mul;
    uint8_t out2_arg;
    uint8_t out2_mul;
    uint8_t has_ret;
    uint8_t types[MESA_SIG_ARGS];
};

enum mesa_vk_arg_kind {
    M_VA_SCALAR = 0,
    M_VA_HANDLE,
    M_VA_PSTRUCT,
    M_VA_PSTRUCT_ARR,
    M_VA_POUT_STRUCT,
    M_VA_POUT_HANDLE,
    M_VA_PSCALAR_IN,
    M_VA_PSCALAR_OUT,
    M_VA_POPEN,
    M_VA_PSTR,
};

struct mesa_vk_arg {
    uint8_t kind;
    uint16_t strct;
    uint8_t count_arg;
    uint8_t fixed_len;
};

struct mesa_vk_sig {
    uint8_t argc;
    uint8_t has_ret;
    struct mesa_vk_arg a[12];
};

enum mesa_vk_fld_kind {
    M_VF_SCALAR = 0,
    M_VF_PTR = 1,
    M_VF_INLINE = 2,
};

struct mesa_vk_fld {
    uint16_t off;
    uint16_t strct;
    uint8_t kind;
};

struct mesa_vk_struct {
    uint16_t size;
    uint16_t nfld;
    const struct mesa_vk_fld *flds;
};

extern const struct mesa_gl_sig mesa_gl_sigs[GPU_CMD_GL_COUNT];
extern const struct mesa_vk_sig mesa_vk_sigs[GPU_CMD_VK_COUNT];
extern const struct mesa_vk_struct mesa_vk_structs[VK_S_COUNT];

#endif
