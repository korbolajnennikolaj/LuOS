// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef MESA_CONST_TABLES_H
#define MESA_CONST_TABLES_H

#include <stdint.h>

struct mesa_const {
    const char *name;
    uint32_t value;
};

extern const struct mesa_const mesa_gl_consts[];
extern const uint32_t mesa_gl_const_count;
extern const struct mesa_const mesa_vk_consts[];
extern const uint32_t mesa_vk_const_count;

const struct mesa_const *mesa_const_find(const struct mesa_const *tab, uint32_t n, const char *name);

#endif
