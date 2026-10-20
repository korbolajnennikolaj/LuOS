// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "lauxlib.h"
#include "lua.h"

#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/mesa.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void luos_register_gfx_gl(lua_State *L);
void luos_register_gfx_vk(lua_State *L);

static int lua_gpu_info(lua_State *L) {
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

static int lua_gpu_prime(lua_State *L) {
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
    }
    lua_pushinteger(L, mesa_prime_set(sel));
    return 1;
}

static int lua_gpu_cube(lua_State *L) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 5;
    uint32_t frames = 0, fps = 0;
    int r = (lua_isstring(L, 2) && strcmp(lua_tostring(L, 2), "vk") == 0)
                ? gpu_vkkmv_run(&opts, &frames, &fps)
                : gpu_kmscube_run(&opts, &frames, &fps);
    lua_pushinteger(L, r);
    lua_pushinteger(L, frames);
    lua_pushinteger(L, fps);
    return 3;
}

static int lua_gpu_gears(lua_State *L) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 5;
    uint32_t frames = 0, fps = 0;
    int r = gpu_glxgears_run(&opts, &frames, &fps);
    lua_pushinteger(L, r);
    lua_pushinteger(L, frames);
    lua_pushinteger(L, fps);
    return 3;
}

static int lua_gpu_shader_test(lua_State *L) {
    uint32_t crc = 0;
    char note[96] = "";
    bool want_vk = lua_isstring(L, 1) && strcmp(lua_tostring(L, 1), "vk") == 0;
    int r = want_vk ? gpu_shader_test_vk(&crc, note, sizeof(note))
                    : gpu_shader_test_gl(&crc, note, sizeof(note));
    lua_pushinteger(L, r);
    lua_pushinteger(L, (lua_Integer)crc);
    return 2;
}

static const luaL_Reg gpu_lib[] = {
    {"info", lua_gpu_info},
    {"prime", lua_gpu_prime},
    {"cube", lua_gpu_cube},
    {"gears", lua_gpu_gears},
    {"shader_test", lua_gpu_shader_test},
    {NULL, NULL},
};

void luos_register_gpu_libs(lua_State *L) {
    lua_newtable(L);
    for (const luaL_Reg *p = gpu_lib; p->name; p++) {
        lua_pushcfunction(L, p->func);
        lua_setfield(L, -2, p->name);
    }
    lua_setglobal(L, "gpu");
    luos_register_gfx_gl(L);
    luos_register_gfx_vk(L);
}
