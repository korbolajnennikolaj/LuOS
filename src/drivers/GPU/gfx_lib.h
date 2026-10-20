// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef GFX_LIB_H
#define GFX_LIB_H

#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa_vk_uapi.h"

#include <stdbool.h>
#include <stdint.h>

#define GFX_ERR_NONE 0
#define GFX_ERR_FAIL -1
#define GFX_ERR_STATE -2
#define GFX_ERR_PARAM -3
#define GFX_ERR_NOMEM -4

#define GFX_IM_MAX_VERT 1024
#define GFX_IM_STRIDE 13
#define GFX_MAX_TEXT 256

#define GFX_ATTR_MAX 16

struct gfx_gl {
    int cl;
    int dev;
    uint32_t w, h;
    uint64_t fbo, tex, depth, vbo, vbo_col, vbo_nrm, vbo_tex;
    uint64_t attr_vbo[GFX_ATTR_MAX];
    gpu_present pt;
    bool present_open;
    int im_mode;
    float *im_buf;
    uint32_t im_count;
    bool im_color, im_normal, im_tex;
    bool im_any_color, im_any_normal, im_any_tex;
    float cur_color[4];
    float cur_normal[3];
    float cur_tex[2];
    char err[160];
};

int gfx_gl_start(int dev);
int gfx_gl_reset_client(int client);
int gfx_gl_client(void);
bool gfx_gl_ready(void);
void gfx_gl_shutdown(void);
int gfx_gl_open(int w, int h);
int gfx_gl_present(void);
int gfx_gl_close(void);
uint32_t gfx_gl_width(void);
uint32_t gfx_gl_height(void);
const char *gfx_gl_error(void);

int gfx_gl_simple(int argc, const uint64_t *args);
int gfx_gl_simple_op(uint16_t op, int argc, const uint64_t *args);
int gfx_gl_payload(uint16_t op, int argc, const uint64_t *args, const void *payload, uint16_t plen,
                   uint16_t cap);
uint64_t gfx_gl_query(uint16_t op, int argc, const uint64_t *args, uint16_t cap, uint16_t *out_len,
                      uint8_t *rbuf, uint16_t rbuf_cap);
int gfx_gl_flush(uint32_t timeout_ms);
int gfx_gl_result(uint16_t idx, void *out, uint16_t cap, uint16_t *out_len);

int gfx_gl_get_error(void);
int gfx_gl_string(uint32_t name, char *out, int cap);
int gfx_gl_finish(void);
int gfx_gl_read_pixels(int x, int y, int w, int h, uint8_t *out, uint32_t cap);
uint64_t gfx_gl_gen(uint16_t op, uint32_t count);
int gfx_gl_tex_image_2d(uint32_t target, int level, uint32_t internal, uint32_t w, uint32_t h,
                        uint32_t format, uint32_t type, const uint8_t *data, uint32_t data_len);
int gfx_gl_buffer_data(uint32_t target, const void *data, uint32_t len, uint32_t usage);
uint64_t gfx_gl_create_shader(uint32_t type, const char *src, char *log, int log_cap);
uint64_t gfx_gl_create_program(uint64_t vs, uint64_t fs, const char *const *attribs, int attrib_count,
                               char *log, int log_cap);
int64_t gfx_gl_uniform_location(uint64_t prog, const char *name);
int gfx_gl_attrib_pointer(uint32_t index, uint32_t size, uint32_t type, uint32_t normalized,
                          uint32_t stride, uint64_t offset);
int gfx_gl_attrib_data(uint32_t index, uint32_t size, uint32_t type, uint32_t normalized,
                       uint32_t stride, const void *data, uint32_t len);
int gfx_gl_uniform_matrix(uint16_t op, int64_t loc, const float *m, uint32_t count);
int gfx_gl_uniformv(uint16_t op, int64_t loc, const float *v, uint32_t comps);

int gfx_im_begin(int mode);
int gfx_im_vertex(float x, float y, float z, float w);
int gfx_im_color(float r, float g, float b, float a);
int gfx_im_normal(float x, float y, float z);
int gfx_im_texcoord(float s, float t);
int gfx_im_end(void);
bool gfx_im_active(void);

int gfx_vk_start(int dev);
int gfx_vk_client(void);
bool gfx_vk_ready(void);
void gfx_vk_shutdown(void);
const char *gfx_vk_error(void);

uint64_t gfx_vk_query(uint16_t op, int argc, const uint64_t *args, const void *payload,
                      uint16_t payload_len, uint16_t cap, uint16_t *out_len, uint8_t *rbuf,
                      uint16_t rbuf_cap);
int gfx_vk_one(uint16_t op, int argc, const uint64_t *args, const void *payload, uint16_t payload_len,
               uint16_t cap);
int gfx_vk_flush(uint32_t timeout_ms);

int gfx_vk_instance_version(uint32_t *version);
int gfx_vk_create_instance(const char *app_name, uint32_t api_version, uint64_t *inst);
int gfx_vk_destroy_instance(uint64_t inst);
int gfx_vk_enumerate_physical(uint64_t inst, uint64_t *out, uint32_t max, uint32_t *count);
int gfx_vk_physical_device_properties(uint64_t pd, VkPhysicalDeviceProperties *out);
int gfx_vk_queue_family_properties(uint64_t pd, VkQueueFamilyProperties *out, uint32_t max,
                                   uint32_t *count);
int gfx_vk_memory_properties(uint64_t pd, VkPhysicalDeviceMemoryProperties *out);
int gfx_vk_features(uint64_t pd, VkPhysicalDeviceFeatures *out);
int gfx_vk_create_device(uint64_t pd, uint32_t family, uint32_t queue_count, uint64_t *dev);
int gfx_vk_get_device_queue(uint64_t dev, uint32_t family, uint32_t index, uint64_t *queue);
int gfx_vk_device_wait_idle(uint64_t dev);
int gfx_vk_destroy_device(uint64_t dev);

uint32_t gfx_crc32(const uint8_t *data, uint32_t len);

#endif
