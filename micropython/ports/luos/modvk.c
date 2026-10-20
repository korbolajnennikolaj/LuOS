// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "py/obj.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "drivers/GPU/gfx_lib.h"
#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_const_tables.h"
#include "drivers/GPU/mesa_vk_defs.h"
#include "drivers/GPU/mesa_vk_uapi.h"

#include "components/logger.h"

#include <stdint.h>
#include <string.h>

#ifndef VK_API_VERSION_1_1
#define VK_API_VERSION_1_1 0x00401000u
#endif
#define GFX_VK_VER_MAJOR(v) ((uint32_t)(v) >> 22)
#define GFX_VK_VER_MINOR(v) (((uint32_t)(v) >> 12) & 0x3FFu)
#define GFX_VK_VER_PATCH(v) ((uint32_t)(v) & 0xFFFu)

static mp_obj_t mvk_log(mp_obj_t msg_obj) {
    LOG_INFO("gfx-vk: %s", mp_obj_str_get_str(msg_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_log_obj, mvk_log);

static mp_obj_t mvk_start(size_t n_args, const mp_obj_t *args) {
    int dev = n_args > 0 && mp_obj_is_int(args[0]) ? mp_obj_get_int(args[0]) : -1;
    int r = gfx_vk_start(dev);
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    return MP_OBJ_NEW_SMALL_INT(gfx_vk_client());
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_start_obj, 0, 1, mvk_start);

static mp_obj_t mvk_close(void) {
    gfx_vk_shutdown();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mvk_close_obj, mvk_close);

static mp_obj_t mvk_error(void) {
    const char *e = gfx_vk_error();
    if (e && e[0]) return mp_obj_new_str_from_cstr(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mvk_error_obj, mvk_error);

static mp_obj_t mvk_info(void) {
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
static MP_DEFINE_CONST_FUN_OBJ_0(mvk_info_obj, mvk_info);

static mp_obj_t mvk_prime(size_t n_args, const mp_obj_t *args) {
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
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_prime_obj, 0, 1, mvk_prime);

static mp_obj_t mvk_push_version(uint32_t v) {
    mp_obj_dict_t *d = mp_obj_new_dict(4);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_major),
                      MP_OBJ_NEW_SMALL_INT(GFX_VK_VER_MAJOR(v)));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_minor),
                      MP_OBJ_NEW_SMALL_INT(GFX_VK_VER_MINOR(v)));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_patch),
                      MP_OBJ_NEW_SMALL_INT(GFX_VK_VER_PATCH(v)));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_raw),
                      mp_obj_new_int_from_uint(v));
    return MP_OBJ_FROM_PTR(d);
}

static mp_obj_t mvk_instance_version(void) {
    uint32_t v = 0;
    if (gfx_vk_instance_version(&v)) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mvk_push_version(v);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mvk_instance_version_obj, mvk_instance_version);

static mp_obj_t mvk_create_instance(size_t n_args, const mp_obj_t *args) {
    const char *app = n_args > 0 && mp_obj_is_str(args[0]) ? mp_obj_str_get_str(args[0])
                                                           : "LuOS gfx";
    uint32_t api = n_args > 1 && mp_obj_is_int(args[1]) ? (uint32_t)mp_obj_get_int(args[1])
                                                        : VK_API_VERSION_1_1;
    uint64_t inst = 0;
    int r = gfx_vk_create_instance(app, api, &inst);
    if (r || !inst) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_obj_new_int_from_uint(inst);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_create_instance_obj, 0, 2, mvk_create_instance);

static mp_obj_t mvk_destroy_instance(mp_obj_t inst_obj) {
    return MP_OBJ_NEW_SMALL_INT(gfx_vk_destroy_instance((uint64_t)mp_obj_get_int(inst_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_destroy_instance_obj, mvk_destroy_instance);

static mp_obj_t mvk_enumerate_physical_devices(mp_obj_t inst_obj) {
    uint64_t devs[8];
    uint32_t count = 0;
    int r = gfx_vk_enumerate_physical((uint64_t)mp_obj_get_int(inst_obj), devs, 8, &count);
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    uint32_t shown = count < 8 ? count : 8;
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (uint32_t i = 0; i < shown; i++)
        mp_obj_list_append(list, mp_obj_new_int_from_uint(devs[i]));
    mp_obj_t t[2] = {list, MP_OBJ_NEW_SMALL_INT(count)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_enumerate_physical_devices_obj, mvk_enumerate_physical_devices);

static mp_obj_t mvk_physical_device_properties(mp_obj_t pd_obj) {
    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof(p));
    if (gfx_vk_physical_device_properties((uint64_t)mp_obj_get_int(pd_obj), &p)) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    mp_obj_dict_t *d = mp_obj_new_dict(10);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_apiVersion),
                      mvk_push_version(p.apiVersion));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_driverVersion),
                      mp_obj_new_int_from_uint(p.driverVersion));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_vendorID),
                      MP_OBJ_NEW_SMALL_INT(p.vendorID));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_deviceID),
                      MP_OBJ_NEW_SMALL_INT(p.deviceID));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_deviceType),
                      MP_OBJ_NEW_SMALL_INT(p.deviceType));
    char namebuf[257];
    memcpy(namebuf, p.deviceName, 256);
    namebuf[256] = 0;
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_deviceName),
                      mp_obj_new_str_from_cstr(namebuf));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_maxImageDimension2D),
                      MP_OBJ_NEW_SMALL_INT(p.limits.maxImageDimension2D));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_maxComputeWorkGroupCountX),
                      MP_OBJ_NEW_SMALL_INT(p.limits.maxComputeWorkGroupCount[0]));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_maxMemoryAllocationCount),
                      MP_OBJ_NEW_SMALL_INT(p.limits.maxMemoryAllocationCount));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_timestampPeriod),
                      mp_obj_new_float(p.limits.timestampPeriod));
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_physical_device_properties_obj, mvk_physical_device_properties);

static mp_obj_t mvk_queue_family_properties(mp_obj_t pd_obj) {
    VkQueueFamilyProperties q;
    memset(&q, 0, sizeof(q));
    uint32_t count = 0;
    int r = gfx_vk_queue_family_properties((uint64_t)mp_obj_get_int(pd_obj), &q, 1, &count);
    if (r) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    mp_obj_dict_t *d = mp_obj_new_dict(8);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_count),
                      MP_OBJ_NEW_SMALL_INT(count));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_queueFlags),
                      MP_OBJ_NEW_SMALL_INT(q.queueFlags));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_queueCount),
                      MP_OBJ_NEW_SMALL_INT(q.queueCount));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_timestampValidBits),
                      MP_OBJ_NEW_SMALL_INT(q.timestampValidBits));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_graphics),
                      mp_obj_new_bool((q.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_compute),
                      mp_obj_new_bool((q.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_transfer),
                      mp_obj_new_bool((q.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_sparse),
                      mp_obj_new_bool((q.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0));
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_queue_family_properties_obj, mvk_queue_family_properties);

static mp_obj_t mvk_memory_properties(mp_obj_t pd_obj) {
    VkPhysicalDeviceMemoryProperties m;
    memset(&m, 0, sizeof(m));
    if (gfx_vk_memory_properties((uint64_t)mp_obj_get_int(pd_obj), &m)) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    mp_obj_dict_t *d = mp_obj_new_dict(4);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_memoryTypeCount),
                      MP_OBJ_NEW_SMALL_INT(m.memoryTypeCount));
    mp_obj_t types = mp_obj_new_list(0, NULL);
    for (uint32_t i = 0; i < m.memoryTypeCount && i < 32; i++) {
        mp_obj_dict_t *t = mp_obj_new_dict(4);
        mp_obj_dict_store(MP_OBJ_FROM_PTR(t), MP_OBJ_NEW_QSTR(MP_QSTR_flags),
                          MP_OBJ_NEW_SMALL_INT(m.memoryTypes[i].propertyFlags));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(t), MP_OBJ_NEW_QSTR(MP_QSTR_heap),
                          MP_OBJ_NEW_SMALL_INT(m.memoryTypes[i].heapIndex));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(t), MP_OBJ_NEW_QSTR(MP_QSTR_hostVisible),
                          mp_obj_new_bool((m.memoryTypes[i].propertyFlags &
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(t), MP_OBJ_NEW_QSTR(MP_QSTR_deviceLocal),
                          mp_obj_new_bool((m.memoryTypes[i].propertyFlags &
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0));
        mp_obj_list_append(types, MP_OBJ_FROM_PTR(t));
    }
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_types), types);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_memoryHeapCount),
                      MP_OBJ_NEW_SMALL_INT(m.memoryHeapCount));
    mp_obj_t heaps = mp_obj_new_list(0, NULL);
    for (uint32_t i = 0; i < m.memoryHeapCount && i < 16; i++) {
        mp_obj_dict_t *t = mp_obj_new_dict(2);
        mp_obj_dict_store(MP_OBJ_FROM_PTR(t), MP_OBJ_NEW_QSTR(MP_QSTR_sizeMB),
                          mp_obj_new_int_from_uint(m.memoryHeaps[i].size >> 20));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(t), MP_OBJ_NEW_QSTR(MP_QSTR_flags),
                          MP_OBJ_NEW_SMALL_INT(m.memoryHeaps[i].flags));
        mp_obj_list_append(heaps, MP_OBJ_FROM_PTR(t));
    }
    mp_obj_dict_store(MP_OBJ_FROM_PTR(d), MP_OBJ_NEW_QSTR(MP_QSTR_heaps), heaps);
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_memory_properties_obj, mvk_memory_properties);

static mp_obj_t mvk_create_device(size_t n_args, const mp_obj_t *args) {
    uint64_t pd = (uint64_t)mp_obj_get_int(args[0]);
    uint32_t family = n_args > 1 ? (uint32_t)mp_obj_get_int(args[1]) : 0;
    uint32_t qc = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : 1;
    uint64_t dev = 0;
    int r = gfx_vk_create_device(pd, family, qc, &dev);
    if (r || !dev) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_obj_new_int_from_uint(dev);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_create_device_obj, 1, 3, mvk_create_device);

static mp_obj_t mvk_get_device_queue(size_t n_args, const mp_obj_t *args) {
    uint64_t dev = (uint64_t)mp_obj_get_int(args[0]);
    uint32_t family = n_args > 1 ? (uint32_t)mp_obj_get_int(args[1]) : 0;
    uint32_t index = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : 0;
    uint64_t q = 0;
    int r = gfx_vk_get_device_queue(dev, family, index, &q);
    if (r || !q) {
        mp_obj_t t[2] = {mp_const_none, mp_obj_new_str_from_cstr(gfx_vk_error())};
        return mp_obj_new_tuple(2, t);
    }
    return mp_obj_new_int_from_uint(q);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_get_device_queue_obj, 1, 3, mvk_get_device_queue);

static mp_obj_t mvk_device_wait_idle(mp_obj_t dev_obj) {
    return MP_OBJ_NEW_SMALL_INT(gfx_vk_device_wait_idle((uint64_t)mp_obj_get_int(dev_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_device_wait_idle_obj, mvk_device_wait_idle);

static mp_obj_t mvk_destroy_device(mp_obj_t dev_obj) {
    return MP_OBJ_NEW_SMALL_INT(gfx_vk_destroy_device((uint64_t)mp_obj_get_int(dev_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_destroy_device_obj, mvk_destroy_device);

static mp_obj_t mvk_const(mp_obj_t name_obj) {
    const char *name = mp_obj_str_get_str(name_obj);
    const char *short_name = name;
    if (strncmp(name, "VK_", 3) == 0) short_name = name + 3;
    const struct mesa_const *c = mesa_const_find(mesa_vk_consts, mesa_vk_const_count, short_name);
    if (!c) return mp_const_none;
    return mp_obj_new_int_from_uint(c->value);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mvk_const_obj, mvk_const);

static mp_obj_t mvk_shader_test(size_t n_args, const mp_obj_t *args) {
    uint32_t crc = 0;
    char note[96] = "";
    /* the vk module tests the Vulkan pipeline unless "gl" is asked for explicitly */
    bool want_vk = !(n_args > 0 && mp_obj_is_str(args[0]) &&
                     strcmp(mp_obj_str_get_str(args[0]), "gl") == 0);
    int r = want_vk ? gpu_shader_test_vk(&crc, note, sizeof(note))
                    : gpu_shader_test_gl(&crc, note, sizeof(note));
    mp_obj_t t[2] = {MP_OBJ_NEW_SMALL_INT(r), mp_obj_new_int_from_uint(crc)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_shader_test_obj, 0, 1, mvk_shader_test);

static mp_obj_t mvk_cube(size_t n_args, const mp_obj_t *args) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : 5;
    uint32_t frames = 0, fps = 0;
    int r = gpu_vkkmv_run(&opts, &frames, &fps);
    mp_obj_t t[3] = {MP_OBJ_NEW_SMALL_INT(r), MP_OBJ_NEW_SMALL_INT(frames),
                     MP_OBJ_NEW_SMALL_INT(fps)};
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mvk_cube_obj, 0, 1, mvk_cube);

static const mp_rom_map_elem_t mvk_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_vk)},
    {MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&mvk_log_obj)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mvk_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mvk_close_obj)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&mvk_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_prime), MP_ROM_PTR(&mvk_prime_obj)},
    {MP_ROM_QSTR(MP_QSTR_error), MP_ROM_PTR(&mvk_error_obj)},
    {MP_ROM_QSTR(MP_QSTR_instanceVersion), MP_ROM_PTR(&mvk_instance_version_obj)},
    {MP_ROM_QSTR(MP_QSTR_createInstance), MP_ROM_PTR(&mvk_create_instance_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroyInstance), MP_ROM_PTR(&mvk_destroy_instance_obj)},
    {MP_ROM_QSTR(MP_QSTR_enumeratePhysicalDevices), MP_ROM_PTR(&mvk_enumerate_physical_devices_obj)},
    {MP_ROM_QSTR(MP_QSTR_physicalDeviceProperties), MP_ROM_PTR(&mvk_physical_device_properties_obj)},
    {MP_ROM_QSTR(MP_QSTR_queueFamilyProperties), MP_ROM_PTR(&mvk_queue_family_properties_obj)},
    {MP_ROM_QSTR(MP_QSTR_memoryProperties), MP_ROM_PTR(&mvk_memory_properties_obj)},
    {MP_ROM_QSTR(MP_QSTR_createDevice), MP_ROM_PTR(&mvk_create_device_obj)},
    {MP_ROM_QSTR(MP_QSTR_getDeviceQueue), MP_ROM_PTR(&mvk_get_device_queue_obj)},
    {MP_ROM_QSTR(MP_QSTR_deviceWaitIdle), MP_ROM_PTR(&mvk_device_wait_idle_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroyDevice), MP_ROM_PTR(&mvk_destroy_device_obj)},
    {MP_ROM_QSTR(MP_QSTR_const), MP_ROM_PTR(&mvk_const_obj)},
    {MP_ROM_QSTR(MP_QSTR_shaderTest), MP_ROM_PTR(&mvk_shader_test_obj)},
    {MP_ROM_QSTR(MP_QSTR_cube), MP_ROM_PTR(&mvk_cube_obj)},
    {MP_ROM_QSTR(MP_QSTR_API_VERSION_1_1), MP_ROM_INT(VK_API_VERSION_1_1)},
    {MP_ROM_QSTR(MP_QSTR_QUEUE_GRAPHICS_BIT), MP_ROM_INT(VK_QUEUE_GRAPHICS_BIT)},
    {MP_ROM_QSTR(MP_QSTR_QUEUE_COMPUTE_BIT), MP_ROM_INT(VK_QUEUE_COMPUTE_BIT)},
    {MP_ROM_QSTR(MP_QSTR_QUEUE_TRANSFER_BIT), MP_ROM_INT(VK_QUEUE_TRANSFER_BIT)},
    {MP_ROM_QSTR(MP_QSTR_PHYSICAL_DEVICE_TYPE_CPU), MP_ROM_INT(VK_PHYSICAL_DEVICE_TYPE_CPU)},
    {MP_ROM_QSTR(MP_QSTR_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU), MP_ROM_INT(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)},
    {MP_ROM_QSTR(MP_QSTR_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU), MP_ROM_INT(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)},
    {MP_ROM_QSTR(MP_QSTR_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), MP_ROM_INT(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
    {MP_ROM_QSTR(MP_QSTR_MEMORY_PROPERTY_HOST_VISIBLE_BIT), MP_ROM_INT(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)},
    {MP_ROM_QSTR(MP_QSTR_MEMORY_PROPERTY_HOST_COHERENT_BIT), MP_ROM_INT(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)},
};

static MP_DEFINE_CONST_DICT(mvk_globals, mvk_globals_table);

const mp_obj_module_t mp_module_vk = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mvk_globals,
};

MP_REGISTER_MODULE(MP_QSTR_vk, mp_module_vk);
