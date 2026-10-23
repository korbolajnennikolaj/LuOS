// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "lauxlib.h"
#include "lua.h"

#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_const_tables.h"
#include "drivers/GPU/mesa_vk_defs.h"
#include "drivers/GPU/mesa_vk_uapi.h"

#include "components/logger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_API_VERSION_1_1
#define VK_API_VERSION_1_1 0x00401000u
#endif
#define GFX_VK_VER_MAJOR(v) ((uint32_t)(v) >> 22)
#define GFX_VK_VER_MINOR(v) (((uint32_t)(v) >> 12) & 0x3FFu)
#define GFX_VK_VER_PATCH(v) ((uint32_t)(v) & 0xFFFu)

static int lvk_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    LOG_INFO("gfx-vk: %s", msg);
    return 0;
}

static int lvk_start(lua_State *L) {
    int dev = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : -1;
    int r = gfx_vk_start(dev);
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_pushinteger(L, gfx_vk_client());
    return 1;
}

static int lvk_close(lua_State *L) {
    (void)L;
    gfx_vk_shutdown();
    return 0;
}

static int lvk_err(lua_State *L) {
    const char *e = gfx_vk_error();
    if (e && e[0]) {
        lua_pushstring(L, e);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int lvk_info(lua_State *L) {
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

static int lvk_prime(lua_State *L) {
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
    }
    lua_pushinteger(L, mesa_prime_set(sel));
    return 1;
}

static void lvk_push_version(lua_State *L, uint32_t v) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)GFX_VK_VER_MAJOR(v));
    lua_setfield(L, -2, "major");
    lua_pushinteger(L, (lua_Integer)GFX_VK_VER_MINOR(v));
    lua_setfield(L, -2, "minor");
    lua_pushinteger(L, (lua_Integer)GFX_VK_VER_PATCH(v));
    lua_setfield(L, -2, "patch");
    lua_pushinteger(L, (lua_Integer)v);
    lua_setfield(L, -2, "raw");
}

static int lvk_instance_version(lua_State *L) {
    uint32_t v = 0;
    if (gfx_vk_instance_version(&v)) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lvk_push_version(L, v);
    return 1;
}

static int lvk_create_instance(lua_State *L) {
    const char *app = lua_isstring(L, 1) ? lua_tostring(L, 1) : "LuOS gfx";
    uint32_t api = lua_isnumber(L, 2) ? (uint32_t)lua_tointeger(L, 2) : VK_API_VERSION_1_1;
    uint64_t inst = 0;
    int r = gfx_vk_create_instance(app, api, &inst);
    if (r || !inst) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)inst);
    return 1;
}

static int lvk_destroy_instance(lua_State *L) {
    uint64_t inst = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, gfx_vk_destroy_instance(inst));
    return 1;
}

static int lvk_enumerate_physical_devices(lua_State *L) {
    uint64_t inst = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t devs[8];
    uint32_t count = 0;
    int r = gfx_vk_enumerate_physical(inst, devs, 8, &count);
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_createtable(L, (int)(count < 8 ? count : 8), 0);
    for (uint32_t i = 0; i < count && i < 8; i++) {
        lua_pushinteger(L, (lua_Integer)devs[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_pushinteger(L, count);
    return 2;
}

static int lvk_physical_device_properties(lua_State *L) {
    uint64_t pd = (uint64_t)luaL_checkinteger(L, 1);
    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof(p));
    if (gfx_vk_physical_device_properties(pd, &p)) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_createtable(L, 0, 10);
    lvk_push_version(L, p.apiVersion);
    lua_setfield(L, -2, "apiVersion");
    lua_pushinteger(L, (lua_Integer)p.driverVersion);
    lua_setfield(L, -2, "driverVersion");
    lua_pushinteger(L, (lua_Integer)p.vendorID);
    lua_setfield(L, -2, "vendorID");
    lua_pushinteger(L, (lua_Integer)p.deviceID);
    lua_setfield(L, -2, "deviceID");
    lua_pushinteger(L, (lua_Integer)p.deviceType);
    lua_setfield(L, -2, "deviceType");
    char namebuf[257];
    memcpy(namebuf, p.deviceName, 256);
    namebuf[256] = 0;
    lua_pushstring(L, namebuf);
    lua_setfield(L, -2, "deviceName");
    lua_pushinteger(L, (lua_Integer)p.limits.maxImageDimension2D);
    lua_setfield(L, -2, "maxImageDimension2D");
    lua_pushinteger(L, (lua_Integer)p.limits.maxComputeWorkGroupCount[0]);
    lua_setfield(L, -2, "maxComputeWorkGroupCountX");
    lua_pushinteger(L, (lua_Integer)p.limits.maxMemoryAllocationCount);
    lua_setfield(L, -2, "maxMemoryAllocationCount");
    lua_pushinteger(L, (lua_Integer)p.limits.timestampPeriod);
    lua_setfield(L, -2, "timestampPeriod");
    return 1;
}

static int lvk_queue_family_properties(lua_State *L) {
    uint64_t pd = (uint64_t)luaL_checkinteger(L, 1);
    VkQueueFamilyProperties q;
    memset(&q, 0, sizeof(q));
    uint32_t count = 0;
    int r = gfx_vk_queue_family_properties(pd, &q, 1, &count);
    if (r) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, count);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer)q.queueFlags);
    lua_setfield(L, -2, "queueFlags");
    lua_pushinteger(L, (lua_Integer)q.queueCount);
    lua_setfield(L, -2, "queueCount");
    lua_pushinteger(L, (lua_Integer)q.timestampValidBits);
    lua_setfield(L, -2, "timestampValidBits");
    lua_pushboolean(L, (q.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0);
    lua_setfield(L, -2, "graphics");
    lua_pushboolean(L, (q.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0);
    lua_setfield(L, -2, "compute");
    lua_pushboolean(L, (q.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0);
    lua_setfield(L, -2, "transfer");
    lua_pushboolean(L, (q.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0);
    lua_setfield(L, -2, "sparse");
    return 1;
}

static int lvk_memory_properties(lua_State *L) {
    uint64_t pd = (uint64_t)luaL_checkinteger(L, 1);
    VkPhysicalDeviceMemoryProperties m;
    memset(&m, 0, sizeof(m));
    if (gfx_vk_memory_properties(pd, &m)) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, m.memoryTypeCount);
    lua_setfield(L, -2, "memoryTypeCount");
    lua_createtable(L, 0, 0);
    for (uint32_t i = 0; i < m.memoryTypeCount && i < 32; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)m.memoryTypes[i].propertyFlags);
        lua_setfield(L, -2, "flags");
        lua_pushinteger(L, (lua_Integer)m.memoryTypes[i].heapIndex);
        lua_setfield(L, -2, "heap");
        lua_pushboolean(L, (m.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
        lua_setfield(L, -2, "hostVisible");
        lua_pushboolean(L, (m.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0);
        lua_setfield(L, -2, "deviceLocal");
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "types");
    lua_pushinteger(L, m.memoryHeapCount);
    lua_setfield(L, -2, "memoryHeapCount");
    lua_createtable(L, 0, 0);
    for (uint32_t i = 0; i < m.memoryHeapCount && i < 16; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)(m.memoryHeaps[i].size >> 20));
        lua_setfield(L, -2, "sizeMB");
        lua_pushinteger(L, (lua_Integer)m.memoryHeaps[i].flags);
        lua_setfield(L, -2, "flags");
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "heaps");
    return 1;
}

static int lvk_create_device(lua_State *L) {
    uint64_t pd = (uint64_t)luaL_checkinteger(L, 1);
    uint32_t family = lua_isnumber(L, 2) ? (uint32_t)lua_tointeger(L, 2) : 0;
    uint32_t qc = lua_isnumber(L, 3) ? (uint32_t)lua_tointeger(L, 3) : 1;
    uint64_t dev = 0;
    int r = gfx_vk_create_device(pd, family, qc, &dev);
    if (r || !dev) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)dev);
    return 1;
}

static int lvk_get_device_queue(lua_State *L) {
    uint64_t dev = (uint64_t)luaL_checkinteger(L, 1);
    uint32_t family = lua_isnumber(L, 2) ? (uint32_t)lua_tointeger(L, 2) : 0;
    uint32_t index = lua_isnumber(L, 3) ? (uint32_t)lua_tointeger(L, 3) : 0;
    uint64_t q = 0;
    int r = gfx_vk_get_device_queue(dev, family, index, &q);
    if (r || !q) {
        lua_pushnil(L);
        lua_pushstring(L, gfx_vk_error());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)q);
    return 1;
}

static int lvk_device_wait_idle(lua_State *L) {
    uint64_t dev = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, gfx_vk_device_wait_idle(dev));
    return 1;
}

static int lvk_destroy_device(lua_State *L) {
    uint64_t dev = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, gfx_vk_destroy_device(dev));
    return 1;
}

static uint16_t lvk_opcode(const char *name) {
    for (int i = 0; i < GPU_CMD_VK_COUNT; i++) {
        const char *n = gpu_cmd_name(GPU_API_VULKAN, GPU_CMD_VK_FIRST + i);
        if (n && strcmp(n, name) == 0) return (uint16_t)(GPU_CMD_VK_FIRST + i);
    }
    return 0;
}

static int lvk_call(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    char full[96];
    if (name[0] == 'v' && name[1] == 'k') snprintf(full, sizeof(full), "%s", name);
    else snprintf(full, sizeof(full), "vk%s", name);
    uint16_t op = lvk_opcode(full);
    if (!op) {
        luaL_error(L, "unknown Vulkan command: %s", full);
        return 0;
    }
    if (!gfx_vk_ready()) {
        luaL_error(L, "no Vulkan context - call vk.start() first");
        return 0;
    }
    int top = lua_gettop(L);
    uint64_t args[16] = {0};
    uint8_t payload[512];
    bool has_data = top >= 2 && lua_type(L, top) == LUA_TSTRING;
    int argc = top - 1 - (has_data ? 1 : 0);
    if (argc > 16) luaL_error(L, "%s: at most 16 arguments", full);
    for (int i = 0; i < argc; i++) {
        int idx = 2 + i;
        args[i] = lua_isnumber(L, idx) ? (uint64_t)lua_tointeger(L, idx) : 0xFFFE;
    }
    uint32_t total = 0;
    if (has_data) {
        size_t len = 0;
        const char *data = lua_tolstring(L, top, &len);
        if (8 + len > sizeof(payload)) luaL_error(L, "%s: data too large (%d bytes max)", full, (int)sizeof(payload) - 8);
        memset(payload, 0, 8);
        memcpy(payload + 8, data, len);
        total = 8 + (uint32_t)len;
    }
    int r = gfx_vk_one(op, argc, args, total ? payload : NULL, (uint16_t)total, 64);
    lua_pushinteger(L, r);
    return 1;
}

static int lvk_const(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const char *short_name = name;
    if (strncmp(name, "VK_", 3) == 0) short_name = name + 3;
    const struct mesa_const *c = mesa_const_find(mesa_vk_consts, mesa_vk_const_count, short_name);
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)c->value);
    return 1;
}

static int lvk_index(lua_State *L) {
    const char *key = lua_tostring(L, 2);
    if (!key) return 0;
    const struct mesa_const *c = mesa_const_find(mesa_vk_consts, mesa_vk_const_count, key);
    if (c) {
        lua_pushinteger(L, (lua_Integer)c->value);
        lua_rawset(L, 1);
        lua_pushinteger(L, (lua_Integer)c->value);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int lvk_shader_test(lua_State *L) {
    uint32_t crc = 0;
    char note[96] = "";
    /* the vk module tests the Vulkan pipeline unless "gl" is asked for explicitly */
    bool want_vk = !(lua_isstring(L, 1) && strcmp(lua_tostring(L, 1), "gl") == 0);
    int r = want_vk ? gpu_shader_test_vk(&crc, note, sizeof(note))
                    : gpu_shader_test_gl(&crc, note, sizeof(note));
    lua_pushinteger(L, r);
    lua_pushinteger(L, (lua_Integer)crc);
    return 2;
}

static int lvk_cube(lua_State *L) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 5;
    uint32_t frames = 0, fps = 0;
    int r = gpu_vkkmv_run(&opts, &frames, &fps);
    lua_pushinteger(L, r);
    lua_pushinteger(L, frames);
    lua_pushinteger(L, fps);
    return 3;
}

static const luaL_Reg vk_funcs[] = {
    {"log", lvk_log},
    {"start", lvk_start},
    {"close", lvk_close},
    {"info", lvk_info},
    {"prime", lvk_prime},
    {"error", lvk_err},
    {"instanceVersion", lvk_instance_version},
    {"createInstance", lvk_create_instance},
    {"destroyInstance", lvk_destroy_instance},
    {"enumeratePhysicalDevices", lvk_enumerate_physical_devices},
    {"physicalDeviceProperties", lvk_physical_device_properties},
    {"queueFamilyProperties", lvk_queue_family_properties},
    {"memoryProperties", lvk_memory_properties},
    {"createDevice", lvk_create_device},
    {"getDeviceQueue", lvk_get_device_queue},
    {"deviceWaitIdle", lvk_device_wait_idle},
    {"destroyDevice", lvk_destroy_device},
    {"call", lvk_call},
    {"const", lvk_const},
    {"shaderTest", lvk_shader_test},
    {"cube", lvk_cube},
    {NULL, NULL},
};

void luos_register_gfx_vk(lua_State *L) {
    lua_newtable(L);
    for (const luaL_Reg *p = vk_funcs; p->name; p++) {
        lua_pushcfunction(L, p->func);
        lua_setfield(L, -2, p->name);
    }
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, lvk_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "vk");
}
