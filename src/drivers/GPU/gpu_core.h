// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef GPU_CORE_H
#define GPU_CORE_H


#include "components/drivers.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stdint.h>


#define GPU_CORE_SLOT 0
#define GPU_DEVICE_FIRST_SLOT 1
#define MAX_GPU_DEVICES (MAX_DRIVERS_PER_TYPE - GPU_DEVICE_FIRST_SLOT)
#define GPU_DEVICE_MAX 16


#define GPU_MAX_CLIENTS 8
#define GPU_BATCH_BYTES (40 * 1024)
#define GPU_BATCH_PAYLOAD 22976
#define GPU_BATCH_RESULT (4 * 1024)
#define GPU_BATCH_MAX_CMDS 96
#define GPU_CMD_MAX_ARGS 16
#define GPU_RING_SLOTS 4
#define GPU_TRANS_NAME_MAX 24


enum GPU_API {
    GPU_API_NONE = 0,
    GPU_API_OPENGL = 1,
    GPU_API_OPENGL_ES = 2,
    GPU_API_VULKAN = 3,
};
#define GPU_API_COUNT 4


#define GPU_OK 0
#define GPU_ERR_NODEV -1
#define GPU_ERR_PARAM -2
#define GPU_ERR_BUSY -3
#define GPU_ERR_NOMEM -4
#define GPU_ERR_NO_DRIVER -5
#define GPU_ERR_NO_TRANSLATOR -6
#define GPU_ERR_TIMEOUT -7
#define GPU_ERR_OVERFLOW -8
#define GPU_ERR_DRM -9
#define GPU_ERR_STATE -10
#define GPU_ERR_NOT_STARTED -11


#define GPU_DEV_FLAG_BOOT_VGA (1u << 0)
#define GPU_DEV_FLAG_BOUND (1u << 1)
#define GPU_DEV_FLAG_MODESET (1u << 2)
#define GPU_DEV_FLAG_RENDER (1u << 3)
#define GPU_DEV_FLAG_NO_PCI (1u << 4)
#define GPU_DEV_FLAG_TRANSLATOR (1u << 5)

typedef struct gpu_device {
    int index;
    char pci_name[16];
    uint16_t vendor;
    uint16_t device;
    uint32_t class_code;
    uint8_t revision;
    uint8_t flags;
    char driver[24];
    char card[12];
    char render[16];
    int primary_minor;
    int render_minor;
    uint32_t primary_devt;
    uint32_t render_devt;
    uint64_t vram_bytes;
    int num_crtcs;
    int num_connectors;

    int drm_major;
    int drm_minor;
    int drm_patch;
    char drm_name[48];
    char drm_desc[96];

    uint32_t generation;
    uint8_t valid;
} gpu_device;


struct gpu_client;

typedef struct gpu_client gpu_client;


#define GPU_CMD_MAGIC 0x37D6u

#define GPU_BATCH_HDR_OFF 0
#define GPU_BATCH_REC_OFF 64
#define GPU_BATCH_PAYLOAD_OFF 13888
#define GPU_BATCH_RESULT_OFF 36864
#define GPU_BATCH_TOTAL_OFF 40960

typedef struct gpu_cmd_rec {
    uint16_t opcode;
    uint8_t argc;
    uint8_t flags;
    uint16_t payload_off;
    uint16_t payload_len;
    uint16_t result_off;
    uint16_t result_cap;
    uint32_t pad;
    uint64_t args[GPU_CMD_MAX_ARGS];
} gpu_cmd_rec;

#define GPU_REC_FLAG_RESULT (1u << 0)
#define GPU_REC_FLAG_SYNC (1u << 1)

typedef struct gpu_batch_hdr {
    uint16_t magic;
    uint8_t api;
    uint8_t pad0;
    uint32_t device;
    uint32_t client_id;
    uint32_t seq;
    uint32_t cmd_count;
    uint32_t payload_used;
    uint32_t result_used;
    uint32_t reserved;
    uint64_t submit_ns;
} gpu_batch_hdr;

typedef struct gpu_ring_slot {
    uint8_t used;
    uint8_t api;
    uint16_t bytes;
    uint16_t payload_used;
    uint16_t result_used;
    uint32_t seq;
    uint32_t device;
    uint64_t submit_ns;
    uint64_t complete_ns;
    uint32_t status;
    uint16_t fail_opcode;
    volatile int32_t done_flag;
    uint8_t data[GPU_BATCH_BYTES];
} gpu_ring_slot;


#define GPU_IOC_NRBITS 0
#define GPU_IOC_TYPEBITS 8
#define GPU_IOC_SIZEBITS 14
#define GPU_IOC_DIRBITS 2
#define GPU_IOC_NRSHIFT 0
#define GPU_IOC_TYPESHIFT 8
#define GPU_IOC_SIZESHIFT 16
#define GPU_IOC_DIRSHIFT 30
#define GPU_IOC_NONE 0U
#define GPU_IOC_WRITE 1U
#define GPU_IOC_READ 2U
#define GPU_IOC(dir, type, nr, size) \
    (((dir) << GPU_IOC_DIRSHIFT) | ((type) << GPU_IOC_TYPESHIFT) | \
     ((nr) << GPU_IOC_NRSHIFT) | ((size) << GPU_IOC_SIZESHIFT))
#define GPU_IOWR(nr, type) GPU_IOC(GPU_IOC_READ | GPU_IOC_WRITE, 'G', nr, sizeof(type))
#define GPU_IOW(nr, type) GPU_IOC(GPU_IOC_WRITE, 'G', nr, sizeof(type))
#define GPU_IOR(nr, type) GPU_IOC(GPU_IOC_READ, 'G', nr, sizeof(type))
#define GPU_IO(nr) GPU_IOC(GPU_IOC_NONE, 'G', nr, 0)
#define GPU_DRM_IOCTL_BASE 'd'
#define GPU_DRM_IOWR(nr, type) GPU_IOC(GPU_IOC_READ | GPU_IOC_WRITE, GPU_DRM_IOCTL_BASE, nr, sizeof(type))
#define GPU_DRM_IOW(nr, type) GPU_IOC(GPU_IOC_WRITE, GPU_DRM_IOCTL_BASE, nr, sizeof(type))
#define GPU_DRM_IOR(nr, type) GPU_IOC(GPU_IOC_READ, GPU_DRM_IOCTL_BASE, nr, sizeof(type))
#define GPU_DRM_IO(nr) GPU_IOC(GPU_IOC_NONE, GPU_DRM_IOCTL_BASE, nr, 0)


#define GPU_IOC_MAGIC 'G'
#define GPU_IOC_ATTACH GPU_IOW(0x00, int)
#define GPU_IOC_DETACH GPU_IOW(0x01, int)
#define GPU_IOC_RING GPU_IOR(0x02, struct gpu_ring_info)
#define GPU_IOC_DONE GPU_IOW(0x03, struct gpu_done)
#define GPU_IOC_API GPU_IOR(0x04, struct gpu_api_info)

typedef struct gpu_ring_info {
    uint32_t device;
    uint32_t slots;
    uint32_t pending;
    uint32_t total_flushed;
    uint32_t total_completed;
    uint32_t dropped;
    uint32_t largest_batch;
    uint32_t last_seq;
} gpu_ring_info;

typedef struct gpu_done {
    uint32_t device;
    uint32_t seq;
    uint32_t status;
    uint32_t fail_opcode;
    uint32_t result_used;
} gpu_done;

typedef struct gpu_api_info {
    uint32_t gl_commands;
    uint32_t vk_commands;
    uint32_t drm_commands;
    uint32_t batch_bytes;
    uint32_t ring_slots;
    uint32_t protocol;
    char drv_name[48];
    char drv_desc[96];
    int drm_version[3];
} gpu_api_info;

#define GPU_PROTOCOL_VERSION 1


typedef struct gpu_drm_ioctl_args {
    uint32_t cmd;
    uint32_t size;
    void *karg;
    long ret;
} gpu_drm_ioctl_args;

typedef struct gpu_core_driver {
    void (*scan_all)(void);
    int (*refresh)(void);
    int (*get_device_count)(void);
    struct gpu_device *(*get_device)(int idx);

    int (*start_stack)(void);
    int (*restart_stack)(int *unbound, int *bound);
    int (*bind)(int dev_idx);
    int (*unbind)(int dev_idx);
    int (*set_debug)(unsigned long mask);
    int (*driver_list)(char *names, int rows, int row_len);

    int (*drm_open)(int dev_idx, bool render);
    int (*drm_ioctl)(int dev_idx, struct gpu_drm_ioctl_args *a);
    int (*drm_close)(int dev_idx);

    int (*client_open)(int dev_idx, uint8_t api);
    int (*client_close)(int client_id);
    int (*cmd_begin)(int client_id);
    int (*cmd_gl)(int client_id, uint16_t opcode, uint8_t argc,
                  const uint64_t *args, const void *payload, uint16_t payload_len, uint16_t result_cap);
    int (*cmd_vk)(int client_id, uint16_t opcode, uint8_t argc,
                  const uint64_t *args, const void *payload, uint16_t payload_len, uint16_t result_cap);
    int (*cmd_drm)(int client_id, uint16_t opcode, uint8_t argc,
                   const uint64_t *args, const void *payload, uint16_t payload_len, uint16_t result_cap);
    int (*flush)(int client_id, uint32_t timeout_ms);
    int (*result)(int client_id, uint16_t cmd_index, void *out, uint16_t cap, uint16_t *out_len);

    int (*api_info)(struct gpu_api_info *info);
    const char *(*cmd_name)(uint8_t api, uint16_t opcode);
} gpu_core_driver;

struct gpu_core_driver *return_gpu_core_driver(void);
struct driver *return_meta_gpu_core_driver(void);


void gpu_core_refresh(void);
int gpu_get_device_count(void);
struct gpu_device *gpu_get_device(int idx);
const struct gpu_device *gpu_find_card(const char *card_name);

int gpu_start_stack(void);
int gpu_restart_stack(int *unbound, int *bound);
int gpu_bind_device(int dev_idx);
int gpu_unbind_device(int dev_idx);
int gpu_set_debug_mask(unsigned long mask);
int gpu_driver_list(char *names, int rows, int row_len);

int gpu_drm_open(int dev_idx, bool render);
int gpu_drm_ioctl(int dev_idx, struct gpu_drm_ioctl_args *a);
int gpu_drm_close(int dev_idx);

int gpu_client_open(int dev_idx, uint8_t api);
int gpu_client_close(int client_id);
int gpu_cmd_begin(int client_id);
int gpu_cmd_gl(int client_id, uint16_t opcode, uint8_t argc,
               const uint64_t *args, const void *payload, uint16_t payload_len, uint16_t result_cap);
int gpu_cmd_vk(int client_id, uint16_t opcode, uint8_t argc,
               const uint64_t *args, const void *payload, uint16_t payload_len, uint16_t result_cap);
int gpu_cmd_drm(int client_id, uint16_t opcode, uint8_t argc,
                const uint64_t *args, const void *payload, uint16_t payload_len, uint16_t result_cap);
int gpu_flush(int client_id, uint32_t timeout_ms);
int gpu_result(int client_id, uint16_t cmd_index, void *out, uint16_t cap, uint16_t *out_len);

int gpu_core_api_info(struct gpu_api_info *info);
const char *gpu_cmd_name(uint8_t api, uint16_t opcode);

int gpu_translator_attach(int dev_idx);
int gpu_translator_detach(int dev_idx);
bool gpu_translator_attached(int dev_idx);
int gpu_translator_ring_info(int dev_idx, struct gpu_ring_info *out);
int gpu_translator_drain(int dev_idx, void *buf, uint32_t cap, uint32_t *out_len);
int gpu_translator_complete(int dev_idx, const struct gpu_done *done, const void *result_data,
                            uint32_t result_len);
uint32_t gpu_translator_total_flushed(int dev_idx);
uint32_t gpu_translator_dropped(int dev_idx);


enum gpu_drm_cmd {
    GPU_CMD_DRM_FIRST = 0x4000,
    GPU_CMD_DRM_VERSION = 0x4000,
    GPU_CMD_DRM_GET_CAP,
    GPU_CMD_DRM_SET_CLIENT_CAP,
    GPU_CMD_DRM_GEM_CLOSE,
    GPU_CMD_DRM_GET_MAGIC,
    GPU_CMD_DRM_AUTH_MAGIC,
    GPU_CMD_DRM_GET_UNIQUE,
    GPU_CMD_DRM_DROP_MASTER,
    GPU_CMD_DRM_SET_MASTER,
    GPU_CMD_DRM_MODE_GETRESOURCES,
    GPU_CMD_DRM_IOCTL,
    GPU_CMD_DRM_LAST
};


typedef struct gpu_drm_version_uapi {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    uint64_t name_len;
    uint64_t date_len;
    uint64_t desc_len;
    uint64_t name;
    uint64_t date;
    uint64_t desc;
} gpu_drm_version_uapi;

typedef struct gpu_drm_get_cap_uapi {
    uint64_t capability;
    uint64_t value;
} gpu_drm_get_cap_uapi;

typedef struct gpu_drm_gem_close_uapi {
    uint32_t handle;
    uint32_t pad;
} gpu_drm_gem_close_uapi;

typedef struct gpu_drm_auth_uapi {
    uint32_t magic;
    uint32_t pad;
} gpu_drm_auth_uapi;

typedef struct gpu_drm_magic_uapi {
    uint32_t magic;
    uint32_t pad;
} gpu_drm_magic_uapi;

typedef struct gpu_drm_card_res_uapi {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
} gpu_drm_card_res_uapi;

#define GPU_DRM_CAP_DUMB_BUFFER 0x1
#define GPU_DRM_CAP_VBLANK_HIGH_CRTC 0x2
#define GPU_DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define GPU_DRM_CAP_DUMB_PREFER_SHADOW 0x4
#define GPU_DRM_CAP_PRIME 0x5
#define GPU_DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define GPU_DRM_CAP_CURSOR_WIDTH 0x8
#define GPU_DRM_CAP_CURSOR_HEIGHT 0x9
#define GPU_DRM_CAP_ADDFB2_MODIFIERS 0x10
#define GPU_DRM_CAP_PAGE_FLIP_TARGET 0x11
#define GPU_DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define GPU_DRM_CAP_CURSOR_WIDTH2 0x13
#define GPU_DRM_CAP_ASYNC_PAGE_FLIP 0x14
#define GPU_DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15


#define GPU_DRM_MODE_NAME 32
#define GPU_DRM_MODE_TYPE_PREFERRED (1u << 2)
#define GPU_DRM_CONN_CONNECTED 1
#define GPU_DRM_CONN_DISCONNECTED 2
#define GPU_DRM_PAGE_FLIP_EVENT 1u
#define GPU_DRM_PAGE_FLIP_ASYNC 2u
typedef struct gpu_drm_modeinfo_uapi {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[GPU_DRM_MODE_NAME];
} gpu_drm_modeinfo_uapi;

typedef struct gpu_drm_get_connector_uapi {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
} gpu_drm_get_connector_uapi;

typedef struct gpu_drm_crtc_uapi {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    gpu_drm_modeinfo_uapi mode;
} gpu_drm_crtc_uapi;

typedef struct gpu_drm_crtc_lut_uapi {
    uint32_t crtc_id;
    uint32_t gamma_size;
    uint64_t red;
    uint64_t green;
    uint64_t blue;
} gpu_drm_crtc_lut_uapi;

typedef struct gpu_drm_get_encoder_uapi {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} gpu_drm_get_encoder_uapi;

typedef struct gpu_drm_fb_cmd_uapi {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
} gpu_drm_fb_cmd_uapi;

typedef struct gpu_drm_page_flip_uapi {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
} gpu_drm_page_flip_uapi;

typedef struct gpu_drm_fb_dirty_uapi {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
} gpu_drm_fb_dirty_uapi;

typedef struct gpu_drm_create_dumb_uapi {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} gpu_drm_create_dumb_uapi;

typedef struct gpu_drm_map_dumb_uapi {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
} gpu_drm_map_dumb_uapi;

typedef struct gpu_drm_destroy_dumb_uapi {
    uint32_t handle;
} gpu_drm_destroy_dumb_uapi;
#define GPU_DRM_IOC_GETCRTC       GPU_DRM_IOWR(0xA1, gpu_drm_crtc_uapi)
#define GPU_DRM_IOC_SETCRTC       GPU_DRM_IOWR(0xA2, gpu_drm_crtc_uapi)
#define GPU_DRM_IOC_SETGAMMA      GPU_DRM_IOWR(0xA5, gpu_drm_crtc_lut_uapi)
#define GPU_DRM_IOC_GETENCODER    GPU_DRM_IOWR(0xA6, gpu_drm_get_encoder_uapi)
#define GPU_DRM_IOC_GETCONNECTOR  GPU_DRM_IOWR(0xA7, gpu_drm_get_connector_uapi)
#define GPU_DRM_IOC_ADDFB         GPU_DRM_IOWR(0xAE, gpu_drm_fb_cmd_uapi)
#define GPU_DRM_IOC_RMFB          GPU_DRM_IOWR(0xAF, uint32_t)
#define GPU_DRM_IOC_PAGE_FLIP     GPU_DRM_IOWR(0xB0, gpu_drm_page_flip_uapi)
#define GPU_DRM_IOC_DIRTYFB       GPU_DRM_IOWR(0xB1, gpu_drm_fb_dirty_uapi)
#define GPU_DRM_IOC_CREATE_DUMB   GPU_DRM_IOWR(0xB2, gpu_drm_create_dumb_uapi)
#define GPU_DRM_IOC_MAP_DUMB      GPU_DRM_IOWR(0xB3, gpu_drm_map_dumb_uapi)
#define GPU_DRM_IOC_DESTROY_DUMB  GPU_DRM_IOWR(0xB4, gpu_drm_destroy_dumb_uapi)
#define GPU_MON_MAX 8
#define GPU_MON_MODES 32
#define GPU_MON_NAME 36
typedef struct gpu_monitor {
    int dev_idx;
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    char name[GPU_MON_NAME];
    uint8_t connected;
    uint8_t enabled;
    uint8_t cloned;
    int mode_count;
    int mode_current;
    gpu_drm_modeinfo_uapi modes[GPU_MON_MODES];
    uint32_t encoders[8];
    int encoder_count;
    uint32_t x, y;
    uint32_t fb_id;
    uint32_t gem_handle;
    void *map_token;
    uint32_t *pixels;
    uint64_t map_size;
    uint32_t pitch;
    uint8_t brightness;
    uint8_t light_kind;
} gpu_monitor;

int gpu_monitor_count(void);
const gpu_monitor *gpu_monitor_get(int idx);
int gpu_monitor_probe(int dev_idx);
const char *gpu_monitor_light_name(int idx);
int gpu_monitor_enable(int idx, int mode_idx);
int gpu_monitor_disable(int idx);
int gpu_monitor_set_mode(int idx, int mode_idx);
int gpu_monitor_set_mode_wh(int idx, uint32_t width, uint32_t height, uint32_t vrefresh);
int gpu_monitor_find_mode(int idx, uint32_t width, uint32_t height, uint32_t vrefresh);
int gpu_monitor_set_brightness(int idx, uint32_t percent);
int gpu_monitor_move(int idx, uint32_t x, uint32_t y);
int gpu_monitor_layout_auto(void);
int gpu_monitor_clone(int dst_idx, int src_idx);
uint32_t gpu_monitor_desktop_width(void);
uint32_t gpu_monitor_desktop_height(void);

int gpu_backlight_count(void);
int gpu_backlight_get(int idx, char *name, int name_cap, uint32_t *cur, uint32_t *max);
int gpu_backlight_set(int idx, uint32_t value);

bool gpu_stack_started(void);
#define GPU_CUBE_DEFAULT_SECONDS 10
typedef struct gpu_cube_opts {
    int dev_idx;
    int connector;
    uint32_t seconds;
    uint16_t width, height;
    void (*progress)(void *user, const char *line);
    void *user;
} gpu_cube_opts;

int gpu_kmscube_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out);
int gpu_vkkmv_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out);
int gpu_glxgears_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out);
int gpu_gl_cube_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out);
int gpu_vk_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out);
int gpu_vk_run_crc(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out, uint32_t *crc_out);
int gpu_shader_test_gl(uint32_t *crc_out, char *note, int note_cap);
int gpu_shader_test_vk(uint32_t *crc_out, char *note, int note_cap);

#define GPU_ZC_MAX 4

typedef struct gpu_present {
    bool kms;
    int dev_idx;
    int mon_idx;
    uint32_t width, height, pitch;
    uint32_t *pixels;
    uint32_t *pix[2];
    uint32_t fb_id[2], gem_handle[2];
    void *map_token[2];
    uint64_t map_size[2];
    uint32_t crtc_id, connector_id;
    uint32_t fb_w, fb_h;
    bool restore_mon;
    bool src_bgra;
    bool can_flip;
    uint32_t cur;
    int64_t period_ms;
    uint8_t *snapshot;
    int zc_count;
    uint32_t zc_fb[GPU_ZC_MAX];
    uint32_t zc_gem[GPU_ZC_MAX];
} gpu_present;

int gpu_present_open(gpu_present *p, const gpu_cube_opts *opts, uint32_t want_w, uint32_t want_h);
int gpu_present_frame(gpu_present *p, const uint32_t *rgba, uint32_t w, uint32_t h);
void gpu_present_close(gpu_present *p);
int gpu_present_zc_add(gpu_present *p, void *host_file, uint32_t host_gem, uint32_t pitch);
int gpu_present_zc_show(gpu_present *p, int slot);
void gpu_present_zc_reset(gpu_present *p);
#define GPU_RESULT_HDR 4

#define GPU_CMD_GL_FIRST 0x1000
#define GPU_CMD_GL_LAST 0x2FFF
#define GPU_CMD_VK_FIRST 0x3000
#define GPU_CMD_VK_LAST 0x3EFF
#define GPU_CMD_DRM_RANGE_FIRST 0x4000
#define GPU_CMD_DRM_RANGE_LAST 0x40FF


#define GPU_GL_CMD_LIST(X) \
 \
X(GL_ACTIVE_TEXTURE, glActiveTexture) \
X(GL_ALPHA_FUNC, glAlphaFunc) \
X(GL_ACCUM, glAccum) \
X(GL_BIND_TEXTURE, glBindTexture) \
X(GL_BLEND_COLOR, glBlendColor) \
X(GL_BLEND_EQUATION, glBlendEquation) \
X(GL_BLEND_EQUATION_SEPARATE, glBlendEquationSeparate) \
X(GL_BLEND_FUNC, glBlendFunc) \
X(GL_BLEND_FUNC_SEPARATE, glBlendFuncSeparate) \
X(GL_CLEAR, glClear) \
X(GL_CLEAR_ACCUM, glClearAccum) \
X(GL_CLEAR_COLOR, glClearColor) \
X(GL_CLEAR_DEPTH, glClearDepth) \
X(GL_CLEAR_DEPTHF, glClearDepthf) \
X(GL_CLEAR_INDEX, glClearIndex) \
X(GL_CLEAR_STENCIL, glClearStencil) \
X(GL_CLIP_PLANE, glClipPlane) \
X(GL_COLOR_MASK, glColorMask) \
X(GL_COLOR_MATERIAL, glColorMaterial) \
X(GL_CULL_FACE, glCullFace) \
X(GL_DEPTH_BOUNDS, glDepthBounds) \
X(GL_DEPTH_FUNC, glDepthFunc) \
X(GL_DEPTH_MASK, glDepthMask) \
X(GL_DEPTH_RANGE, glDepthRange) \
X(GL_DEPTH_RANGEF, glDepthRangef) \
X(GL_DISABLE, glDisable) \
X(GL_DISABLEI, glDisablei) \
X(GL_ENABLE, glEnable) \
X(GL_ENABLEI, glEnablei) \
X(GL_FINISH, glFinish) \
X(GL_FLUSH, glFlush) \
X(GL_FRONT_FACE, glFrontFace) \
X(GL_HINT, glHint) \
X(GL_LINE_STIPPLE, glLineStipple) \
X(GL_LINE_WIDTH, glLineWidth) \
X(GL_LOGIC_OP, glLogicOp) \
X(GL_MIN_SAMPLE_SHADING, glMinSampleShading) \
X(GL_PIXEL_STOREF, glPixelStoref) \
X(GL_PIXEL_STOREI, glPixelStorei) \
X(GL_PIXEL_TRANSFERF, glPixelTransferf) \
X(GL_PIXEL_TRANSFERI, glPixelTransferi) \
X(GL_PIXEL_ZOOM, glPixelZoom) \
X(GL_POINT_SIZE, glPointSize) \
X(GL_POINT_PARAMETERF, glPointParameterf) \
X(GL_POINT_PARAMETERI, glPointParameteri) \
X(GL_POINT_PARAMETERFV, glPointParameterfv) \
X(GL_POINT_PARAMETERIV, glPointParameteriv) \
X(GL_POLYGON_MODE, glPolygonMode) \
X(GL_POLYGON_OFFSET, glPolygonOffset) \
X(GL_POLYGON_OFFSET_CLAMP, glPolygonOffsetClamp) \
X(GL_POLYGON_STIPPLE, glPolygonStipple) \
X(GL_SAMPLE_COVERAGE, glSampleCoverage) \
X(GL_SAMPLE_MASKI, glSampleMaski) \
X(GL_SCISSOR, glScissor) \
X(GL_SCISSOR_INDEXED, glScissorIndexed) \
X(GL_SCISSOR_INDEXEDV, glScissorIndexedv) \
X(GL_SCISSOR_ARRAYV, glScissorArrayv) \
X(GL_STENCIL_FUNC, glStencilFunc) \
X(GL_STENCIL_FUNC_SEPARATE, glStencilFuncSeparate) \
X(GL_STENCIL_MASK, glStencilMask) \
X(GL_STENCIL_MASK_SEPARATE, glStencilMaskSeparate) \
X(GL_STENCIL_OP, glStencilOp) \
X(GL_STENCIL_OP_SEPARATE, glStencilOpSeparate) \
X(GL_VIEWPORT, glViewport) \
X(GL_VIEWPORT_INDEXEDF, glViewportIndexedf) \
X(GL_VIEWPORT_INDEXEDFV, glViewportIndexedfv) \
X(GL_VIEWPORT_ARRAYV, glViewportArrayv) \
X(GL_CLIP_CONTROL, glClipControl) \
 \
X(GL_CALL_LIST, glCallList) \
X(GL_CALL_LISTS, glCallLists) \
X(GL_DELETE_LISTS, glDeleteLists) \
X(GL_END_LIST, glEndList) \
X(GL_GEN_LISTS, glGenLists) \
X(GL_IS_LIST, glIsList) \
X(GL_LIST_BASE, glListBase) \
X(GL_NEW_LIST, glNewList) \
X(GL_POP_ATTRIB, glPopAttrib) \
X(GL_POP_CLIENT_ATTRIB, glPopClientAttrib) \
X(GL_PUSH_ATTRIB, glPushAttrib) \
X(GL_PUSH_CLIENT_ATTRIB, glPushClientAttrib) \
X(GL_INIT_NAMES, glInitNames) \
X(GL_LOAD_NAME, glLoadName) \
X(GL_PUSH_NAME, glPushName) \
X(GL_POP_NAME, glPopName) \
X(GL_RENDER_MODE, glRenderMode) \
X(GL_SELECT_BUFFER, glSelectBuffer) \
X(GL_FEEDBACK_BUFFER, glFeedbackBuffer) \
X(GL_PASS_THROUGH, glPassThrough) \
 \
X(GL_MATRIX_MODE, glMatrixMode) \
X(GL_LOAD_IDENTITY, glLoadIdentity) \
X(GL_LOAD_MATRIXD, glLoadMatrixd) \
X(GL_LOAD_MATRIXF, glLoadMatrixf) \
X(GL_LOAD_TRANSPOSE_MATRIXD, glLoadTransposeMatrixd) \
X(GL_LOAD_TRANSPOSE_MATRIXF, glLoadTransposeMatrixf) \
X(GL_MULT_MATRIXD, glMultMatrixd) \
X(GL_MULT_MATRIXF, glMultMatrixf) \
X(GL_MULT_TRANSPOSE_MATRIXD, glMultTransposeMatrixd) \
X(GL_MULT_TRANSPOSE_MATRIXF, glMultTransposeMatrixf) \
X(GL_PUSH_MATRIX, glPushMatrix) \
X(GL_POP_MATRIX, glPopMatrix) \
X(GL_ROTATED, glRotated) \
X(GL_ROTATEF, glRotatef) \
X(GL_SCALED, glScaled) \
X(GL_SCALEF, glScalef) \
X(GL_TRANSLATED, glTranslated) \
X(GL_TRANSLATEF, glTranslatef) \
X(GL_FRUSTUM, glFrustum) \
X(GL_ORTHO, glOrtho) \
 \
X(GL_LIGHTF, glLightf) \
X(GL_LIGHTI, glLighti) \
X(GL_LIGHTFV, glLightfv) \
X(GL_LIGHTIV, glLightiv) \
X(GL_LIGHT_MODELF, glLightModelf) \
X(GL_LIGHT_MODELI, glLightModeli) \
X(GL_LIGHT_MODELFV, glLightModelfv) \
X(GL_LIGHT_MODELIV, glLightModeliv) \
X(GL_MATERIALF, glMaterialf) \
X(GL_MATERIALI, glMateriali) \
X(GL_MATERIALFV, glMaterialfv) \
X(GL_MATERIALIV, glMaterialiv) \
X(GL_SHADE_MODEL, glShadeModel) \
 \
X(GL_BEGIN, glBegin) \
X(GL_END, glEnd) \
X(GL_EDGE_FLAG, glEdgeFlag) \
X(GL_EDGE_FLAGV, glEdgeFlagv) \
X(GL_ARRAY_ELEMENT, glArrayElement) \
X(GL_SECONDARY_COLOR3B, glSecondaryColor3b) \
X(GL_SECONDARY_COLOR3BV, glSecondaryColor3bv) \
X(GL_SECONDARY_COLOR3D, glSecondaryColor3d) \
X(GL_SECONDARY_COLOR3DV, glSecondaryColor3dv) \
X(GL_SECONDARY_COLOR3F, glSecondaryColor3f) \
X(GL_SECONDARY_COLOR3FV, glSecondaryColor3fv) \
X(GL_SECONDARY_COLOR3I, glSecondaryColor3i) \
X(GL_SECONDARY_COLOR3IV, glSecondaryColor3iv) \
X(GL_SECONDARY_COLOR3S, glSecondaryColor3s) \
X(GL_SECONDARY_COLOR3SV, glSecondaryColor3sv) \
X(GL_SECONDARY_COLOR3UB, glSecondaryColor3ub) \
X(GL_SECONDARY_COLOR3UBV, glSecondaryColor3ubv) \
X(GL_SECONDARY_COLOR3UI, glSecondaryColor3ui) \
X(GL_SECONDARY_COLOR3UIV, glSecondaryColor3uiv) \
X(GL_SECONDARY_COLOR3US, glSecondaryColor3us) \
X(GL_SECONDARY_COLOR3USV, glSecondaryColor3usv) \
X(GL_FOG_COORDD, glFogCoordd) \
X(GL_FOG_COORDDV, glFogCoorddv) \
X(GL_FOG_COORDF, glFogCoordf) \
X(GL_FOG_COORDFV, glFogCoordfv) \
X(GL_COLOR3B, glColor3b) \
X(GL_COLOR3BV, glColor3bv) \
X(GL_COLOR3D, glColor3d) \
X(GL_COLOR3DV, glColor3dv) \
X(GL_COLOR3F, glColor3f) \
X(GL_COLOR3FV, glColor3fv) \
X(GL_COLOR3I, glColor3i) \
X(GL_COLOR3IV, glColor3iv) \
X(GL_COLOR3S, glColor3s) \
X(GL_COLOR3SV, glColor3sv) \
X(GL_COLOR3UB, glColor3ub) \
X(GL_COLOR3UBV, glColor3ubv) \
X(GL_COLOR3UI, glColor3ui) \
X(GL_COLOR3UIV, glColor3uiv) \
X(GL_COLOR3US, glColor3us) \
X(GL_COLOR3USV, glColor3usv) \
X(GL_COLOR4B, glColor4b) \
X(GL_COLOR4BV, glColor4bv) \
X(GL_COLOR4D, glColor4d) \
X(GL_COLOR4DV, glColor4dv) \
X(GL_COLOR4F, glColor4f) \
X(GL_COLOR4FV, glColor4fv) \
X(GL_COLOR4I, glColor4i) \
X(GL_COLOR4IV, glColor4iv) \
X(GL_COLOR4S, glColor4s) \
X(GL_COLOR4SV, glColor4sv) \
X(GL_COLOR4UB, glColor4ub) \
X(GL_COLOR4UBV, glColor4ubv) \
X(GL_COLOR4UI, glColor4ui) \
X(GL_COLOR4UIV, glColor4uiv) \
X(GL_COLOR4US, glColor4us) \
X(GL_COLOR4USV, glColor4usv) \
X(GL_INDEXD, glIndexd) \
X(GL_INDEXDV, glIndexdv) \
X(GL_INDEXF, glIndexf) \
X(GL_INDEXFV, glIndexfv) \
X(GL_INDEXI, glIndexi) \
X(GL_INDEXIV, glIndexiv) \
X(GL_INDEXS, glIndexs) \
X(GL_INDEXSV, glIndexsv) \
X(GL_INDEXUB, glIndexub) \
X(GL_INDEXUBV, glIndexubv) \
X(GL_NORMAL3B, glNormal3b) \
X(GL_NORMAL3BV, glNormal3bv) \
X(GL_NORMAL3D, glNormal3d) \
X(GL_NORMAL3DV, glNormal3dv) \
X(GL_NORMAL3F, glNormal3f) \
X(GL_NORMAL3FV, glNormal3fv) \
X(GL_NORMAL3I, glNormal3i) \
X(GL_NORMAL3IV, glNormal3iv) \
X(GL_NORMAL3S, glNormal3s) \
X(GL_NORMAL3SV, glNormal3sv) \
X(GL_RASTER_POS2D, glRasterPos2d) \
X(GL_RASTER_POS2DV, glRasterPos2dv) \
X(GL_RASTER_POS2F, glRasterPos2f) \
X(GL_RASTER_POS2FV, glRasterPos2fv) \
X(GL_RASTER_POS2I, glRasterPos2i) \
X(GL_RASTER_POS2IV, glRasterPos2iv) \
X(GL_RASTER_POS2S, glRasterPos2s) \
X(GL_RASTER_POS2SV, glRasterPos2sv) \
X(GL_RASTER_POS3D, glRasterPos3d) \
X(GL_RASTER_POS3DV, glRasterPos3dv) \
X(GL_RASTER_POS3F, glRasterPos3f) \
X(GL_RASTER_POS3FV, glRasterPos3fv) \
X(GL_RASTER_POS3I, glRasterPos3i) \
X(GL_RASTER_POS3IV, glRasterPos3iv) \
X(GL_RASTER_POS3S, glRasterPos3s) \
X(GL_RASTER_POS3SV, glRasterPos3sv) \
X(GL_RASTER_POS4D, glRasterPos4d) \
X(GL_RASTER_POS4DV, glRasterPos4dv) \
X(GL_RASTER_POS4F, glRasterPos4f) \
X(GL_RASTER_POS4FV, glRasterPos4fv) \
X(GL_RASTER_POS4I, glRasterPos4i) \
X(GL_RASTER_POS4IV, glRasterPos4iv) \
X(GL_RASTER_POS4S, glRasterPos4s) \
X(GL_RASTER_POS4SV, glRasterPos4sv) \
X(GL_WINDOW_POS2D, glWindowPos2d) \
X(GL_WINDOW_POS2DV, glWindowPos2dv) \
X(GL_WINDOW_POS2F, glWindowPos2f) \
X(GL_WINDOW_POS2FV, glWindowPos2fv) \
X(GL_WINDOW_POS2I, glWindowPos2i) \
X(GL_WINDOW_POS2IV, glWindowPos2iv) \
X(GL_WINDOW_POS2S, glWindowPos2s) \
X(GL_WINDOW_POS2SV, glWindowPos2sv) \
X(GL_WINDOW_POS3D, glWindowPos3d) \
X(GL_WINDOW_POS3DV, glWindowPos3dv) \
X(GL_WINDOW_POS3F, glWindowPos3f) \
X(GL_WINDOW_POS3FV, glWindowPos3fv) \
X(GL_WINDOW_POS3I, glWindowPos3i) \
X(GL_WINDOW_POS3IV, glWindowPos3iv) \
X(GL_WINDOW_POS3S, glWindowPos3s) \
X(GL_WINDOW_POS3SV, glWindowPos3sv) \
X(GL_RECTD, glRectd) \
X(GL_RECTDV, glRectdv) \
X(GL_RECTF, glRectf) \
X(GL_RECTFV, glRectfv) \
X(GL_RECTI, glRecti) \
X(GL_RECTIV, glRectiv) \
X(GL_RECTS, glRects) \
X(GL_RECTSV, glRectsv) \
X(GL_TEX_COORD1D, glTexCoord1d) \
X(GL_TEX_COORD1DV, glTexCoord1dv) \
X(GL_TEX_COORD1F, glTexCoord1f) \
X(GL_TEX_COORD1FV, glTexCoord1fv) \
X(GL_TEX_COORD1I, glTexCoord1i) \
X(GL_TEX_COORD1IV, glTexCoord1iv) \
X(GL_TEX_COORD1S, glTexCoord1s) \
X(GL_TEX_COORD1SV, glTexCoord1sv) \
X(GL_TEX_COORD2D, glTexCoord2d) \
X(GL_TEX_COORD2DV, glTexCoord2dv) \
X(GL_TEX_COORD2F, glTexCoord2f) \
X(GL_TEX_COORD2FV, glTexCoord2fv) \
X(GL_TEX_COORD2I, glTexCoord2i) \
X(GL_TEX_COORD2IV, glTexCoord2iv) \
X(GL_TEX_COORD2S, glTexCoord2s) \
X(GL_TEX_COORD2SV, glTexCoord2sv) \
X(GL_TEX_COORD3D, glTexCoord3d) \
X(GL_TEX_COORD3DV, glTexCoord3dv) \
X(GL_TEX_COORD3F, glTexCoord3f) \
X(GL_TEX_COORD3FV, glTexCoord3fv) \
X(GL_TEX_COORD3I, glTexCoord3i) \
X(GL_TEX_COORD3IV, glTexCoord3iv) \
X(GL_TEX_COORD3S, glTexCoord3s) \
X(GL_TEX_COORD3SV, glTexCoord3sv) \
X(GL_TEX_COORD4D, glTexCoord4d) \
X(GL_TEX_COORD4DV, glTexCoord4dv) \
X(GL_TEX_COORD4F, glTexCoord4f) \
X(GL_TEX_COORD4FV, glTexCoord4fv) \
X(GL_TEX_COORD4I, glTexCoord4i) \
X(GL_TEX_COORD4IV, glTexCoord4iv) \
X(GL_TEX_COORD4S, glTexCoord4s) \
X(GL_TEX_COORD4SV, glTexCoord4sv) \
X(GL_VERTEX2D, glVertex2d) \
X(GL_VERTEX2DV, glVertex2dv) \
X(GL_VERTEX2F, glVertex2f) \
X(GL_VERTEX2FV, glVertex2fv) \
X(GL_VERTEX2I, glVertex2i) \
X(GL_VERTEX2IV, glVertex2iv) \
X(GL_VERTEX2S, glVertex2s) \
X(GL_VERTEX2SV, glVertex2sv) \
X(GL_VERTEX3D, glVertex3d) \
X(GL_VERTEX3DV, glVertex3dv) \
X(GL_VERTEX3F, glVertex3f) \
X(GL_VERTEX3FV, glVertex3fv) \
X(GL_VERTEX3I, glVertex3i) \
X(GL_VERTEX3IV, glVertex3iv) \
X(GL_VERTEX3S, glVertex3s) \
X(GL_VERTEX3SV, glVertex3sv) \
X(GL_VERTEX4D, glVertex4d) \
X(GL_VERTEX4DV, glVertex4dv) \
X(GL_VERTEX4F, glVertex4f) \
X(GL_VERTEX4FV, glVertex4fv) \
X(GL_VERTEX4I, glVertex4i) \
X(GL_VERTEX4IV, glVertex4iv) \
X(GL_VERTEX4S, glVertex4s) \
X(GL_VERTEX4SV, glVertex4sv) \
X(GL_MULTI_TEX_COORD1D, glMultiTexCoord1d) \
X(GL_MULTI_TEX_COORD1DV, glMultiTexCoord1dv) \
X(GL_MULTI_TEX_COORD1F, glMultiTexCoord1f) \
X(GL_MULTI_TEX_COORD1FV, glMultiTexCoord1fv) \
X(GL_MULTI_TEX_COORD1I, glMultiTexCoord1i) \
X(GL_MULTI_TEX_COORD1IV, glMultiTexCoord1iv) \
X(GL_MULTI_TEX_COORD1S, glMultiTexCoord1s) \
X(GL_MULTI_TEX_COORD1SV, glMultiTexCoord1sv) \
X(GL_MULTI_TEX_COORD2D, glMultiTexCoord2d) \
X(GL_MULTI_TEX_COORD2DV, glMultiTexCoord2dv) \
X(GL_MULTI_TEX_COORD2F, glMultiTexCoord2f) \
X(GL_MULTI_TEX_COORD2FV, glMultiTexCoord2fv) \
X(GL_MULTI_TEX_COORD2I, glMultiTexCoord2i) \
X(GL_MULTI_TEX_COORD2IV, glMultiTexCoord2iv) \
X(GL_MULTI_TEX_COORD2S, glMultiTexCoord2s) \
X(GL_MULTI_TEX_COORD2SV, glMultiTexCoord2sv) \
X(GL_MULTI_TEX_COORD3D, glMultiTexCoord3d) \
X(GL_MULTI_TEX_COORD3DV, glMultiTexCoord3dv) \
X(GL_MULTI_TEX_COORD3F, glMultiTexCoord3f) \
X(GL_MULTI_TEX_COORD3FV, glMultiTexCoord3fv) \
X(GL_MULTI_TEX_COORD3I, glMultiTexCoord3i) \
X(GL_MULTI_TEX_COORD3IV, glMultiTexCoord3iv) \
X(GL_MULTI_TEX_COORD3S, glMultiTexCoord3s) \
X(GL_MULTI_TEX_COORD3SV, glMultiTexCoord3sv) \
X(GL_MULTI_TEX_COORD4D, glMultiTexCoord4d) \
X(GL_MULTI_TEX_COORD4DV, glMultiTexCoord4dv) \
X(GL_MULTI_TEX_COORD4F, glMultiTexCoord4f) \
X(GL_MULTI_TEX_COORD4FV, glMultiTexCoord4fv) \
X(GL_MULTI_TEX_COORD4I, glMultiTexCoord4i) \
X(GL_MULTI_TEX_COORD4IV, glMultiTexCoord4iv) \
X(GL_MULTI_TEX_COORD4S, glMultiTexCoord4s) \
X(GL_MULTI_TEX_COORD4SV, glMultiTexCoord4sv) \
 \
X(GL_EVAL_COORD1D, glEvalCoord1d) \
X(GL_EVAL_COORD1DV, glEvalCoord1dv) \
X(GL_EVAL_COORD1F, glEvalCoord1f) \
X(GL_EVAL_COORD1FV, glEvalCoord1fv) \
X(GL_EVAL_COORD2D, glEvalCoord2d) \
X(GL_EVAL_COORD2DV, glEvalCoord2dv) \
X(GL_EVAL_COORD2F, glEvalCoord2f) \
X(GL_EVAL_COORD2FV, glEvalCoord2fv) \
X(GL_EVAL_MESH1, glEvalMesh1) \
X(GL_EVAL_MESH2, glEvalMesh2) \
X(GL_EVAL_POINT1, glEvalPoint1) \
X(GL_EVAL_POINT2, glEvalPoint2) \
X(GL_MAP1D, glMap1d) \
X(GL_MAP1F, glMap1f) \
X(GL_MAP2D, glMap2d) \
X(GL_MAP2F, glMap2f) \
X(GL_MAP_GRID1D, glMapGrid1d) \
X(GL_MAP_GRID1F, glMapGrid1f) \
X(GL_MAP_GRID2D, glMapGrid2d) \
X(GL_MAP_GRID2F, glMapGrid2f) \
 \
X(GL_BITMAP, glBitmap) \
X(GL_COPY_PIXELS, glCopyPixels) \
X(GL_DRAW_PIXELS, glDrawPixels) \
X(GL_READ_PIXELS, glReadPixels) \
X(GL_READ_BUFFER, glReadBuffer) \
X(GL_DRAW_BUFFER, glDrawBuffer) \
X(GL_DRAW_BUFFERS, glDrawBuffers) \
X(GL_PIXEL_MAPFV, glPixelMapfv) \
X(GL_PIXEL_MAPUIV, glPixelMapuiv) \
X(GL_PIXEL_MAPUSV, glPixelMapusv) \
X(GL_COPY_TEX_IMAGE1D, glCopyTexImage1D) \
X(GL_COPY_TEX_IMAGE2D, glCopyTexImage2D) \
X(GL_COPY_TEX_SUB_IMAGE1D, glCopyTexSubImage1D) \
X(GL_COPY_TEX_SUB_IMAGE2D, glCopyTexSubImage2D) \
X(GL_COPY_TEX_SUB_IMAGE3D, glCopyTexSubImage3D) \
 \
X(GL_COLOR_TABLE, glColorTable) \
X(GL_COLOR_TABLE_PARAMETERFV, glColorTableParameterfv) \
X(GL_COLOR_TABLE_PARAMETERIV, glColorTableParameteriv) \
X(GL_COLOR_SUB_TABLE, glColorSubTable) \
X(GL_COPY_COLOR_SUB_TABLE, glCopyColorSubTable) \
X(GL_COPY_COLOR_TABLE, glCopyColorTable) \
X(GL_CONVOLUTION_FILTER1D, glConvolutionFilter1D) \
X(GL_CONVOLUTION_FILTER2D, glConvolutionFilter2D) \
X(GL_CONVOLUTION_PARAMETERF, glConvolutionParameterf) \
X(GL_CONVOLUTION_PARAMETERI, glConvolutionParameteri) \
X(GL_CONVOLUTION_PARAMETERFV, glConvolutionParameterfv) \
X(GL_CONVOLUTION_PARAMETERIV, glConvolutionParameteriv) \
X(GL_COPY_CONVOLUTION_FILTER1D, glCopyConvolutionFilter1D) \
X(GL_COPY_CONVOLUTION_FILTER2D, glCopyConvolutionFilter2D) \
X(GL_SEPARABLE_FILTER2D, glSeparableFilter2D) \
X(GL_HISTOGRAM, glHistogram) \
X(GL_RESET_HISTOGRAM, glResetHistogram) \
X(GL_MINMAX, glMinmax) \
X(GL_RESET_MINMAX, glResetMinmax) \
 \
X(GL_DELETE_TEXTURES, glDeleteTextures) \
X(GL_GEN_TEXTURES, glGenTextures) \
X(GL_IS_TEXTURE, glIsTexture) \
X(GL_PRIORITIZE_TEXTURES, glPrioritizeTextures) \
X(GL_TEX_IMAGE1D, glTexImage1D) \
X(GL_TEX_IMAGE2D, glTexImage2D) \
X(GL_TEX_IMAGE3D, glTexImage3D) \
X(GL_TEX_SUB_IMAGE1D, glTexSubImage1D) \
X(GL_TEX_SUB_IMAGE2D, glTexSubImage2D) \
X(GL_TEX_SUB_IMAGE3D, glTexSubImage3D) \
X(GL_COMPRESSED_TEX_IMAGE1D, glCompressedTexImage1D) \
X(GL_COMPRESSED_TEX_IMAGE2D, glCompressedTexImage2D) \
X(GL_COMPRESSED_TEX_IMAGE3D, glCompressedTexImage3D) \
X(GL_COMPRESSED_TEX_SUB_IMAGE1D, glCompressedTexSubImage1D) \
X(GL_COMPRESSED_TEX_SUB_IMAGE2D, glCompressedTexSubImage2D) \
X(GL_COMPRESSED_TEX_SUB_IMAGE3D, glCompressedTexSubImage3D) \
X(GL_TEX_PARAMETERF, glTexParameterf) \
X(GL_TEX_PARAMETERI, glTexParameteri) \
X(GL_TEX_PARAMETERFV, glTexParameterfv) \
X(GL_TEX_PARAMETERIV, glTexParameteriv) \
X(GL_TEX_PARAMETERIIV, glTexParameterIiv) \
X(GL_TEX_PARAMETERIUIV, glTexParameterIuiv) \
X(GL_TEX_BUFFER, glTexBuffer) \
X(GL_TEX_BUFFER_RANGE, glTexBufferRange) \
X(GL_TEX_IMAGE2D_MULTISAMPLE, glTexImage2DMultisample) \
X(GL_TEX_IMAGE3D_MULTISAMPLE, glTexImage3DMultisample) \
X(GL_TEX_STORAGE1D, glTexStorage1D) \
X(GL_TEX_STORAGE2D, glTexStorage2D) \
X(GL_TEX_STORAGE3D, glTexStorage3D) \
X(GL_TEX_STORAGE2D_MULTISAMPLE, glTexStorage2DMultisample) \
X(GL_TEX_STORAGE3D_MULTISAMPLE, glTexStorage3DMultisample) \
X(GL_TEX_VIEW, glTextureView) \
X(GL_TEX_ENVF, glTexEnvf) \
X(GL_TEX_ENVI, glTexEnvi) \
X(GL_TEX_ENVFV, glTexEnvfv) \
X(GL_TEX_ENVIV, glTexEnviv) \
X(GL_TEX_GEND, glTexGend) \
X(GL_TEX_GENDV, glTexGendv) \
X(GL_TEX_GENF, glTexGenf) \
X(GL_TEX_GENFV, glTexGenfv) \
X(GL_TEX_GENI, glTexGeni) \
X(GL_TEX_GENIV, glTexGeniv) \
X(GL_GENERATE_MIPMAP, glGenerateMipmap) \
X(GL_TEXTURE_BARRIER, glTextureBarrier) \
 \
X(GL_ATTACH_SHADER, glAttachShader) \
X(GL_BIND_ATTRIB_LOCATION, glBindAttribLocation) \
X(GL_COMPILE_SHADER, glCompileShader) \
X(GL_CREATE_PROGRAM, glCreateProgram) \
X(GL_CREATE_SHADER, glCreateShader) \
X(GL_DELETE_PROGRAM, glDeleteProgram) \
X(GL_DELETE_SHADER, glDeleteShader) \
X(GL_DETACH_SHADER, glDetachShader) \
X(GL_LINK_PROGRAM, glLinkProgram) \
X(GL_SHADER_SOURCE, glShaderSource) \
X(GL_SPECIALIZE_SHADER, glSpecializeShader) \
X(GL_USE_PROGRAM, glUseProgram) \
X(GL_VALIDATE_PROGRAM, glValidateProgram) \
X(GL_UNIFORM1F, glUniform1f) \
X(GL_UNIFORM2F, glUniform2f) \
X(GL_UNIFORM3F, glUniform3f) \
X(GL_UNIFORM4F, glUniform4f) \
X(GL_UNIFORM1I, glUniform1i) \
X(GL_UNIFORM2I, glUniform2i) \
X(GL_UNIFORM3I, glUniform3i) \
X(GL_UNIFORM4I, glUniform4i) \
X(GL_UNIFORM1UI, glUniform1ui) \
X(GL_UNIFORM2UI, glUniform2ui) \
X(GL_UNIFORM3UI, glUniform3ui) \
X(GL_UNIFORM4UI, glUniform4ui) \
X(GL_UNIFORM1FV, glUniform1fv) \
X(GL_UNIFORM2FV, glUniform2fv) \
X(GL_UNIFORM3FV, glUniform3fv) \
X(GL_UNIFORM4FV, glUniform4fv) \
X(GL_UNIFORM1IV, glUniform1iv) \
X(GL_UNIFORM2IV, glUniform2iv) \
X(GL_UNIFORM3IV, glUniform3iv) \
X(GL_UNIFORM4IV, glUniform4iv) \
X(GL_UNIFORM1UIV, glUniform1uiv) \
X(GL_UNIFORM2UIV, glUniform2uiv) \
X(GL_UNIFORM3UIV, glUniform3uiv) \
X(GL_UNIFORM4UIV, glUniform4uiv) \
X(GL_UNIFORM_MATRIX2FV, glUniformMatrix2fv) \
X(GL_UNIFORM_MATRIX3FV, glUniformMatrix3fv) \
X(GL_UNIFORM_MATRIX4FV, glUniformMatrix4fv) \
X(GL_UNIFORM_MATRIX2X3FV, glUniformMatrix2x3fv) \
X(GL_UNIFORM_MATRIX3X2FV, glUniformMatrix3x2fv) \
X(GL_UNIFORM_MATRIX2X4FV, glUniformMatrix2x4fv) \
X(GL_UNIFORM_MATRIX4X2FV, glUniformMatrix4x2fv) \
X(GL_UNIFORM_MATRIX3X4FV, glUniformMatrix3x4fv) \
X(GL_UNIFORM_MATRIX4X3FV, glUniformMatrix4x3fv) \
X(GL_UNIFORM1D, glUniform1d) \
X(GL_UNIFORM2D, glUniform2d) \
X(GL_UNIFORM3D, glUniform3d) \
X(GL_UNIFORM4D, glUniform4d) \
X(GL_UNIFORM1DV, glUniform1dv) \
X(GL_UNIFORM2DV, glUniform2dv) \
X(GL_UNIFORM3DV, glUniform3dv) \
X(GL_UNIFORM4DV, glUniform4dv) \
X(GL_UNIFORM_MATRIX2DV, glUniformMatrix2dv) \
X(GL_UNIFORM_MATRIX3DV, glUniformMatrix3dv) \
X(GL_UNIFORM_MATRIX4DV, glUniformMatrix4dv) \
X(GL_UNIFORM_BLOCK_BINDING, glUniformBlockBinding) \
X(GL_UNIFORM_SUBROUTINESUIV, glUniformSubroutinesuiv) \
X(GL_SHADER_STORAGE_BLOCK_BINDING, glShaderStorageBlockBinding) \
 \
X(GL_CREATE_SHADER_PROGRAMV, glCreateShaderProgramv) \
X(GL_USE_PROGRAM_STAGES, glUseProgramStages) \
X(GL_VALID_PROGRAM_PIPELINE, glValidateProgramPipeline) \
X(GL_PROGRAM_PARAMETERI, glProgramParameteri) \
X(GL_PROGRAM_BINARY, glProgramBinary) \
X(GL_PROGRAM_UNIFORM1F, glProgramUniform1f) \
X(GL_PROGRAM_UNIFORM2F, glProgramUniform2f) \
X(GL_PROGRAM_UNIFORM3F, glProgramUniform3f) \
X(GL_PROGRAM_UNIFORM4F, glProgramUniform4f) \
X(GL_PROGRAM_UNIFORM1I, glProgramUniform1i) \
X(GL_PROGRAM_UNIFORM2I, glProgramUniform2i) \
X(GL_PROGRAM_UNIFORM3I, glProgramUniform3i) \
X(GL_PROGRAM_UNIFORM4I, glProgramUniform4i) \
X(GL_PROGRAM_UNIFORM1UI, glProgramUniform1ui) \
X(GL_PROGRAM_UNIFORM2UI, glProgramUniform2ui) \
X(GL_PROGRAM_UNIFORM3UI, glProgramUniform3ui) \
X(GL_PROGRAM_UNIFORM4UI, glProgramUniform4ui) \
X(GL_PROGRAM_UNIFORM1FV, glProgramUniform1fv) \
X(GL_PROGRAM_UNIFORM2FV, glProgramUniform2fv) \
X(GL_PROGRAM_UNIFORM3FV, glProgramUniform3fv) \
X(GL_PROGRAM_UNIFORM4FV, glProgramUniform4fv) \
X(GL_PROGRAM_UNIFORM1IV, glProgramUniform1iv) \
X(GL_PROGRAM_UNIFORM2IV, glProgramUniform2iv) \
X(GL_PROGRAM_UNIFORM3IV, glProgramUniform3iv) \
X(GL_PROGRAM_UNIFORM4IV, glProgramUniform4iv) \
X(GL_PROGRAM_UNIFORM1UIV, glProgramUniform1uiv) \
X(GL_PROGRAM_UNIFORM2UIV, glProgramUniform2uiv) \
X(GL_PROGRAM_UNIFORM3UIV, glProgramUniform3uiv) \
X(GL_PROGRAM_UNIFORM4UIV, glProgramUniform4uiv) \
X(GL_PROGRAM_UNIFORM_MATRIX2FV, glProgramUniformMatrix2fv) \
X(GL_PROGRAM_UNIFORM_MATRIX3FV, glProgramUniformMatrix3fv) \
X(GL_PROGRAM_UNIFORM_MATRIX4FV, glProgramUniformMatrix4fv) \
X(GL_PROGRAM_UNIFORM_MATRIX2X3FV, glProgramUniformMatrix2x3fv) \
X(GL_PROGRAM_UNIFORM_MATRIX3X2FV, glProgramUniformMatrix3x2fv) \
X(GL_PROGRAM_UNIFORM_MATRIX2X4FV, glProgramUniformMatrix2x4fv) \
X(GL_PROGRAM_UNIFORM_MATRIX4X2FV, glProgramUniformMatrix4x2fv) \
X(GL_PROGRAM_UNIFORM_MATRIX3X4FV, glProgramUniformMatrix3x4fv) \
X(GL_PROGRAM_UNIFORM_MATRIX4X3FV, glProgramUniformMatrix4x3fv) \
 \
X(GL_DISABLE_VERTEX_ATTRIB_ARRAY, glDisableVertexAttribArray) \
X(GL_ENABLE_VERTEX_ATTRIB_ARRAY, glEnableVertexAttribArray) \
X(GL_VERTEX_ATTRIB1D, glVertexAttrib1d) \
X(GL_VERTEX_ATTRIB1DV, glVertexAttrib1dv) \
X(GL_VERTEX_ATTRIB1F, glVertexAttrib1f) \
X(GL_VERTEX_ATTRIB1FV, glVertexAttrib1fv) \
X(GL_VERTEX_ATTRIB1S, glVertexAttrib1s) \
X(GL_VERTEX_ATTRIB1SV, glVertexAttrib1sv) \
X(GL_VERTEX_ATTRIB2D, glVertexAttrib2d) \
X(GL_VERTEX_ATTRIB2DV, glVertexAttrib2dv) \
X(GL_VERTEX_ATTRIB2F, glVertexAttrib2f) \
X(GL_VERTEX_ATTRIB2FV, glVertexAttrib2fv) \
X(GL_VERTEX_ATTRIB2S, glVertexAttrib2s) \
X(GL_VERTEX_ATTRIB2SV, glVertexAttrib2sv) \
X(GL_VERTEX_ATTRIB3D, glVertexAttrib3d) \
X(GL_VERTEX_ATTRIB3DV, glVertexAttrib3dv) \
X(GL_VERTEX_ATTRIB3F, glVertexAttrib3f) \
X(GL_VERTEX_ATTRIB3FV, glVertexAttrib3fv) \
X(GL_VERTEX_ATTRIB3S, glVertexAttrib3s) \
X(GL_VERTEX_ATTRIB3SV, glVertexAttrib3sv) \
X(GL_VERTEX_ATTRIB4D, glVertexAttrib4d) \
X(GL_VERTEX_ATTRIB4DV, glVertexAttrib4dv) \
X(GL_VERTEX_ATTRIB4F, glVertexAttrib4f) \
X(GL_VERTEX_ATTRIB4FV, glVertexAttrib4fv) \
X(GL_VERTEX_ATTRIB4S, glVertexAttrib4s) \
X(GL_VERTEX_ATTRIB4SV, glVertexAttrib4sv) \
X(GL_VERTEX_ATTRIB4BV, glVertexAttrib4bv) \
X(GL_VERTEX_ATTRIB4UBV, glVertexAttrib4ubv) \
X(GL_VERTEX_ATTRIB4UB, glVertexAttrib4ub) \
X(GL_VERTEX_ATTRIB4UI, glVertexAttrib4ui) \
X(GL_VERTEX_ATTRIB4UIV, glVertexAttrib4uiv) \
X(GL_VERTEX_ATTRIB4US, glVertexAttrib4us) \
X(GL_VERTEX_ATTRIB4USV, glVertexAttrib4usv) \
X(GL_VERTEX_ATTRIB4NBV, glVertexAttrib4Nbv) \
X(GL_VERTEX_ATTRIB4NIV, glVertexAttrib4Niv) \
X(GL_VERTEX_ATTRIB4NSV, glVertexAttrib4Nsv) \
X(GL_VERTEX_ATTRIB4NUB, glVertexAttrib4Nub) \
X(GL_VERTEX_ATTRIB4NUBV, glVertexAttrib4Nubv) \
X(GL_VERTEX_ATTRIB4NUIV, glVertexAttrib4Nuiv) \
X(GL_VERTEX_ATTRIB4NUSV, glVertexAttrib4Nusv) \
X(GL_VERTEX_ATTRIB_POINTER, glVertexAttribPointer) \
X(GL_VERTEX_ATTRIB_I1I, glVertexAttribI1i) \
X(GL_VERTEX_ATTRIB_I1UI, glVertexAttribI1ui) \
X(GL_VERTEX_ATTRIB_I2I, glVertexAttribI2i) \
X(GL_VERTEX_ATTRIB_I2UI, glVertexAttribI2ui) \
X(GL_VERTEX_ATTRIB_I3I, glVertexAttribI3i) \
X(GL_VERTEX_ATTRIB_I3UI, glVertexAttribI3ui) \
X(GL_VERTEX_ATTRIB_I4I, glVertexAttribI4i) \
X(GL_VERTEX_ATTRIB_I4UI, glVertexAttribI4ui) \
X(GL_VERTEX_ATTRIB_I4BV, glVertexAttribI4bv) \
X(GL_VERTEX_ATTRIB_I4SV, glVertexAttribI4sv) \
X(GL_VERTEX_ATTRIB_I4UBV, glVertexAttribI4ubv) \
X(GL_VERTEX_ATTRIB_I4USV, glVertexAttribI4usv) \
X(GL_VERTEX_ATTRIB_I4IV, glVertexAttribI4iv) \
X(GL_VERTEX_ATTRIB_I4UIV, glVertexAttribI4uiv) \
X(GL_VERTEX_ATTRIB_I_POINTER, glVertexAttribIPointer) \
X(GL_VERTEX_ATTRIB_L1D, glVertexAttribL1d) \
X(GL_VERTEX_ATTRIB_L2D, glVertexAttribL2d) \
X(GL_VERTEX_ATTRIB_L3D, glVertexAttribL3d) \
X(GL_VERTEX_ATTRIB_L4D, glVertexAttribL4d) \
X(GL_VERTEX_ATTRIB_L1DV, glVertexAttribL1dv) \
X(GL_VERTEX_ATTRIB_L2DV, glVertexAttribL2dv) \
X(GL_VERTEX_ATTRIB_L3DV, glVertexAttribL3dv) \
X(GL_VERTEX_ATTRIB_L4DV, glVertexAttribL4dv) \
X(GL_VERTEX_ATTRIB_L_POINTER, glVertexAttribLPointer) \
X(GL_VERTEX_ATTRIB_FORMAT, glVertexAttribFormat) \
X(GL_VERTEX_ATTRIB_I_FORMAT, glVertexAttribIFormat) \
X(GL_VERTEX_ATTRIB_L_FORMAT, glVertexAttribLFormat) \
X(GL_VERTEX_ATTRIB_BINDING, glVertexAttribBinding) \
X(GL_VERTEX_BINDING_DIVISOR, glVertexBindingDivisor) \
X(GL_BIND_VERTEX_BUFFER, glBindVertexBuffer) \
 \
X(GL_BIND_BUFFER, glBindBuffer) \
X(GL_BIND_BUFFER_BASE, glBindBufferBase) \
X(GL_BIND_BUFFER_RANGE, glBindBufferRange) \
X(GL_BIND_BUFFERS_BASE, glBindBuffersBase) \
X(GL_BIND_BUFFERS_RANGE, glBindBuffersRange) \
X(GL_BUFFER_DATA, glBufferData) \
X(GL_BUFFER_STORAGE, glBufferStorage) \
X(GL_BUFFER_SUB_DATA, glBufferSubData) \
X(GL_COPY_BUFFER_SUB_DATA, glCopyBufferSubData) \
X(GL_CLEAR_BUFFER_DATA, glClearBufferData) \
X(GL_CLEAR_BUFFER_SUB_DATA, glClearBufferSubData) \
X(GL_CLEAR_BUFFERF, glClearBufferfi) \
X(GL_CLEAR_BUFFERFV, glClearBufferfv) \
X(GL_CLEAR_BUFFERIV, glClearBufferiv) \
X(GL_CLEAR_BUFFERSV, glClearBuffersv) \
X(GL_CLEAR_BUFFERUIV, glClearBufferuiv) \
X(GL_DELETE_BUFFERS, glDeleteBuffers) \
X(GL_GEN_BUFFERS, glGenBuffers) \
X(GL_IS_BUFFER, glIsBuffer) \
X(GL_FLUSH_MAPPED_BUFFER_RANGE, glFlushMappedBufferRange) \
X(GL_MAP_BUFFER, glMapBuffer) \
X(GL_MAP_BUFFER_RANGE, glMapBufferRange) \
X(GL_UNMAP_BUFFER, glUnmapBuffer) \
X(GL_INVALIDATE_BUFFER_DATA, glInvalidateBufferData) \
X(GL_INVALIDATE_BUFFER_SUB_DATA, glInvalidateBufferSubData) \
X(GL_MEMORY_BARRIER, glMemoryBarrier) \
X(GL_MEMORY_BARRIER_BY_REGION, glMemoryBarrierByRegion) \
 \
X(GL_BIND_VERTEX_ARRAY, glBindVertexArray) \
X(GL_DELETE_VERTEX_ARRAYS, glDeleteVertexArrays) \
X(GL_GEN_VERTEX_ARRAYS, glGenVertexArrays) \
X(GL_IS_VERTEX_ARRAY, glIsVertexArray) \
X(GL_DISABLE_VERTEX_ARRAY_ATTRIB, glDisableVertexArrayAttrib) \
X(GL_ENABLE_VERTEX_ARRAY_ATTRIB, glEnableVertexArrayAttrib) \
X(GL_VERTEX_ARRAY_ATTRIB_FORMAT, glVertexArrayAttribFormat) \
X(GL_VERTEX_ARRAY_ATTRIB_I_FORMAT, glVertexArrayAttribIFormat) \
X(GL_VERTEX_ARRAY_ATTRIB_L_FORMAT, glVertexArrayAttribLFormat) \
X(GL_VERTEX_ARRAY_ATTRIB_BINDING, glVertexArrayAttribBinding) \
X(GL_VERTEX_ARRAY_BINDING_DIVISOR, glVertexArrayBindingDivisor) \
X(GL_VERTEX_ARRAY_VERTEX_BUFFER, glVertexArrayVertexBuffer) \
X(GL_VERTEX_ARRAY_VERTEX_BUFFERS, glVertexArrayVertexBuffers) \
X(GL_VERTEX_ARRAY_ELEMENT_BUFFER, glVertexArrayElementBuffer) \
X(GL_BIND_VERTEX_BUFFERS, glBindVertexBuffers) \
X(GL_VERTEX_DIVISOR, glVertexDivisor) \
X(GL_COLOR_POINTER, glColorPointer) \
X(GL_EDGE_FLAG_POINTER, glEdgeFlagPointer) \
X(GL_FOG_COORD_POINTER, glFogCoordPointer) \
X(GL_INDEX_POINTER, glIndexPointer) \
X(GL_INTERLEAVED_ARRAYS, glInterleavedArrays) \
X(GL_NORMAL_POINTER, glNormalPointer) \
X(GL_SECONDARY_COLOR_POINTER, glSecondaryColorPointer) \
X(GL_TEX_COORD_POINTER, glTexCoordPointer) \
X(GL_VERTEX_POINTER, glVertexPointer) \
X(GL_ENABLE_CLIENT_STATE, glEnableClientState) \
X(GL_DISABLE_CLIENT_STATE, glDisableClientState) \
 \
X(GL_DRAW_ARRAYS, glDrawArrays) \
X(GL_DRAW_ARRAYS_INSTANCED, glDrawArraysInstanced) \
X(GL_DRAW_ARRAYS_INSTANCED_BASE_INSTANCE, glDrawArraysInstancedBaseInstance) \
X(GL_DRAW_ARRAYS_INDIRECT, glDrawArraysIndirect) \
X(GL_DRAW_ELEMENTS, glDrawElements) \
X(GL_DRAW_ELEMENTS_BASE_VERTEX, glDrawElementsBaseVertex) \
X(GL_DRAW_ELEMENTS_INSTANCED, glDrawElementsInstanced) \
X(GL_DRAW_ELEMENTS_INSTANCED_BASE_INSTANCE, glDrawElementsInstancedBaseInstance) \
X(GL_DRAW_ELEMENTS_INSTANCED_BASE_VERTEX, glDrawElementsInstancedBaseVertex) \
X(GL_DRAW_ELEMENTS_INSTANCED_BASE_VERTEX_BASE_INSTANCE, glDrawElementsInstancedBaseVertexBaseInstance) \
X(GL_DRAW_ELEMENTS_INDIRECT, glDrawElementsIndirect) \
X(GL_DRAW_RANGE_ELEMENTS, glDrawRangeElements) \
X(GL_DRAW_RANGE_ELEMENTS_BASE_VERTEX, glDrawRangeElementsBaseVertex) \
X(GL_MULTI_DRAW_ARRAYS, glMultiDrawArrays) \
X(GL_MULTI_DRAW_ELEMENTS, glMultiDrawElements) \
X(GL_MULTI_DRAW_ARRAYS_INDIRECT, glMultiDrawArraysIndirect) \
X(GL_MULTI_DRAW_ELEMENTS_INDIRECT, glMultiDrawElementsIndirect) \
X(GL_MULTI_DRAW_ARRAYS_INDIRECT_COUNT, glMultiDrawArraysIndirectCount) \
X(GL_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT, glMultiDrawElementsIndirectCount) \
X(GL_PRIMITIVE_RESTART_INDEX, glPrimitiveRestartIndex) \
X(GL_PROVOKING_VERTEX, glProvokingVertex) \
 \
X(GL_BEGIN_QUERY, glBeginQuery) \
X(GL_BEGIN_QUERY_INDEXED, glBeginQueryIndexed) \
X(GL_BEGIN_CONDITIONAL_RENDER, glBeginConditionalRender) \
X(GL_END_QUERY, glEndQuery) \
X(GL_END_QUERY_INDEXED, glEndQueryIndexed) \
X(GL_END_CONDITIONAL_RENDER, glEndConditionalRender) \
X(GL_DELETE_QUERIES, glDeleteQueries) \
X(GL_GEN_QUERIES, glGenQueries) \
X(GL_IS_QUERY, glIsQuery) \
X(GL_QUERY_COUNTER, glQueryCounter) \
 \
X(GL_BEGIN_TRANSFORM_FEEDBACK, glBeginTransformFeedback) \
X(GL_END_TRANSFORM_FEEDBACK, glEndTransformFeedback) \
X(GL_BIND_TRANSFORM_FEEDBACK, glBindTransformFeedback) \
X(GL_CREATE_TRANSFORM_FEEDBACKS, glCreateTransformFeedbacks) \
X(GL_DELETE_TRANSFORM_FEEDBACKS, glDeleteTransformFeedbacks) \
X(GL_IS_TRANSFORM_FEEDBACK, glIsTransformFeedback) \
X(GL_PAUSE_TRANSFORM_FEEDBACK, glPauseTransformFeedback) \
X(GL_RESUME_TRANSFORM_FEEDBACK, glResumeTransformFeedback) \
X(GL_TRANSFORM_FEEDBACK_VARYINGS, glTransformFeedbackVaryings) \
X(GL_TRANSFORM_FEEDBACK_BUFFER_BASE, glTransformFeedbackBufferBase) \
X(GL_TRANSFORM_FEEDBACK_BUFFER_RANGE, glTransformFeedbackBufferRange) \
X(GL_DRAW_TRANSFORM_FEEDBACK, glDrawTransformFeedback) \
X(GL_DRAW_TRANSFORM_FEEDBACK_INSTANCED, glDrawTransformFeedbackInstanced) \
X(GL_DRAW_TRANSFORM_FEEDBACK_STREAM, glDrawTransformFeedbackStream) \
X(GL_DRAW_TRANSFORM_FEEDBACK_STREAM_INSTANCED, glDrawTransformFeedbackStreamInstanced) \
 \
X(GL_BIND_FRAMEBUFFER, glBindFramebuffer) \
X(GL_BIND_RENDERBUFFER, glBindRenderbuffer) \
X(GL_BIND_FRAG_DATA_LOCATION, glBindFragDataLocation) \
X(GL_BIND_FRAG_DATA_LOCATION_INDEXED, glBindFragDataLocationIndexed) \
X(GL_BLIT_FRAMEBUFFER, glBlitFramebuffer) \
X(GL_CHECK_FRAMEBUFFER_STATUS, glCheckFramebufferStatus) \
X(GL_CLAMP_COLOR, glClampColor) \
X(GL_DELETE_FRAMEBUFFERS, glDeleteFramebuffers) \
X(GL_DELETE_RENDERBUFFERS, glDeleteRenderbuffers) \
X(GL_GEN_FRAMEBUFFERS, glGenFramebuffers) \
X(GL_GEN_RENDERBUFFERS, glGenRenderbuffers) \
X(GL_IS_FRAMEBUFFER, glIsFramebuffer) \
X(GL_IS_RENDERBUFFER, glIsRenderbuffer) \
X(GL_FRAMEBUFFER_RENDERBUFFER, glFramebufferRenderbuffer) \
X(GL_FRAMEBUFFER_TEXTURE1D, glFramebufferTexture1D) \
X(GL_FRAMEBUFFER_TEXTURE2D, glFramebufferTexture2D) \
X(GL_FRAMEBUFFER_TEXTURE3D, glFramebufferTexture3D) \
X(GL_FRAMEBUFFER_TEXTURE, glFramebufferTexture) \
X(GL_FRAMEBUFFER_TEXTURE_LAYER, glFramebufferTextureLayer) \
X(GL_FRAMEBUFFER_PARAMETERI, glFramebufferParameteri) \
X(GL_RENDERBUFFER_STORAGE, glRenderbufferStorage) \
X(GL_RENDERBUFFER_STORAGE_MULTISAMPLE, glRenderbufferStorageMultisample) \
X(GL_INVALIDATE_FRAMEBUFFER, glInvalidateFramebuffer) \
X(GL_INVALIDATE_SUB_FRAMEBUFFER, glInvalidateSubFramebuffer) \
X(GL_INVALIDATE_TEX_IMAGE, glInvalidateTexImage) \
X(GL_INVALIDATE_TEX_SUB_IMAGE, glInvalidateTexSubImage) \
 \
X(GL_FENCE_SYNC, glFenceSync) \
X(GL_CLIENT_WAIT_SYNC, glClientWaitSync) \
X(GL_WAIT_SYNC, glWaitSync) \
X(GL_DELETE_SYNC, glDeleteSync) \
X(GL_IS_SYNC, glIsSync) \
 \
X(GL_DISPATCH_COMPUTE, glDispatchCompute) \
X(GL_DISPATCH_COMPUTE_INDIRECT, glDispatchComputeIndirect) \
X(GL_BIND_IMAGE_TEXTURE, glBindImageTexture) \
X(GL_BIND_IMAGE_TEXTURES, glBindImageTextures) \
 \
X(GL_GET_ACTIVE_ATOMIC_COUNTER_BUFFERIV, glGetActiveAtomicCounterBufferiv) \
 \
X(GL_DEBUG_MESSAGE_CALLBACK, glDebugMessageCallback) \
X(GL_DEBUG_MESSAGE_CONTROL, glDebugMessageControl) \
X(GL_DEBUG_MESSAGE_INSERT, glDebugMessageInsert) \
X(GL_OBJECT_LABEL, glObjectLabel) \
X(GL_OBJECT_PTR_LABEL, glObjectPtrLabel) \
X(GL_POP_DEBUG_GROUP, glPopDebugGroup) \
X(GL_PUSH_DEBUG_GROUP, glPushDebugGroup) \
 \
X(GL_PATCH_PARAMETERF, glPatchParameterfv) \
X(GL_PATCH_PARAMETERI, glPatchParameteri) \
X(GL_GET_PROGRAM_BINARY, glGetProgramBinary) \
X(GL_GET_ACTIVE_SUBROUTINE_UNIFORM_NAME, glGetActiveSubroutineUniformName) \
X(GL_GET_ACTIVE_SUBROUTINE_UNIFORMIV, glGetActiveSubroutineUniformiv) \
X(GL_GET_ACTIVE_SUBROUTINE_NAME, glGetActiveSubroutineName) \
X(GL_GET_SUBROUTINE_INDEX, glGetSubroutineIndex) \
X(GL_GET_SUBROUTINE_UNIFORM_LOCATION, glGetSubroutineUniformLocation) \
X(GL_GET_PROGRAM_STAGEIV, glGetProgramStageiv) \
X(GL_BIND_PROGRAM_PIPELINE, glBindProgramPipeline) \
X(GL_DELETE_PROGRAM_PIPELINES, glDeleteProgramPipelines) \
X(GL_GEN_PROGRAM_PIPELINES, glGenProgramPipelines) \
X(GL_IS_PROGRAM_PIPELINE, glIsProgramPipeline) \
 \
X(GL_CREATE_BUFFERS, glCreateBuffers) \
X(GL_CREATE_FRAMEBUFFERS, glCreateFramebuffers) \
X(GL_CREATE_PROGRAM_PIPELINES, glCreateProgramPipelines) \
X(GL_CREATE_QUERIES, glCreateQueries) \
X(GL_CREATE_RENDERBUFFERS, glCreateRenderbuffers) \
X(GL_CREATE_SAMPLERS, glCreateSamplers) \
X(GL_CREATE_TEXTURES, glCreateTextures) \
X(GL_CREATE_VERTEX_ARRAYS, glCreateVertexArrays) \
X(GL_NAMED_BUFFER_DATA, glNamedBufferData) \
X(GL_NAMED_BUFFER_STORAGE, glNamedBufferStorage) \
X(GL_NAMED_BUFFER_SUB_DATA, glNamedBufferSubData) \
X(GL_NAMED_FRAMEBUFFER_DRAW_BUFFER, glNamedFramebufferDrawBuffer) \
X(GL_NAMED_FRAMEBUFFER_DRAW_BUFFERS, glNamedFramebufferDrawBuffers) \
X(GL_NAMED_FRAMEBUFFER_PARAMETERI, glNamedFramebufferParameteri) \
X(GL_NAMED_FRAMEBUFFER_READ_BUFFER, glNamedFramebufferReadBuffer) \
X(GL_NAMED_FRAMEBUFFER_RENDERBUFFER, glNamedFramebufferRenderbuffer) \
X(GL_NAMED_FRAMEBUFFER_TEXTURE, glNamedFramebufferTexture) \
X(GL_NAMED_FRAMEBUFFER_TEXTURE_LAYER, glNamedFramebufferTextureLayer) \
X(GL_NAMED_RENDERBUFFER_STORAGE, glNamedRenderbufferStorage) \
X(GL_NAMED_RENDERBUFFER_STORAGE_MULTISAMPLE, glNamedRenderbufferStorageMultisample) \
X(GL_TEXTURE_BUFFER2, glTextureBuffer) \
X(GL_TEXTURE_BUFFER_RANGE2, glTextureBufferRange) \
X(GL_TEXTURE_PARAMETERF2, glTextureParameterf) \
X(GL_TEXTURE_PARAMETERI2, glTextureParameteri) \
X(GL_TEXTURE_PARAMETERFV2, glTextureParameterfv) \
X(GL_TEXTURE_PARAMETERIV2, glTextureParameteriv) \
X(GL_TEXTURE_PARAMETERIIV2, glTextureParameterIiv) \
X(GL_TEXTURE_PARAMETERIUIV2, glTextureParameterIuiv) \
X(GL_TEXTURE_STORAGE1D2, glTextureStorage1D) \
X(GL_TEXTURE_STORAGE2D2, glTextureStorage2D) \
X(GL_TEXTURE_STORAGE3D2, glTextureStorage3D) \
X(GL_TEXTURE_STORAGE2D_MULTISAMPLE2, glTextureStorage2DMultisample) \
X(GL_TEXTURE_STORAGE3D_MULTISAMPLE2, glTextureStorage3DMultisample) \
X(GL_TEXTURE_SUB_IMAGE1D2, glTextureSubImage1D) \
X(GL_TEXTURE_SUB_IMAGE2D2, glTextureSubImage2D) \
X(GL_TEXTURE_SUB_IMAGE3D2, glTextureSubImage3D) \
X(GL_GENERATE_TEXTURE_MIPMAP, glGenerateTextureMipmap) \
X(GL_CLEAR_TEX_IMAGE, glClearTexImage) \
X(GL_CLEAR_TEX_SUB_IMAGE, glClearTexSubImage) \
X(GL_COPY_IMAGE_SUB_DATA, glCopyImageSubData) \
X(GL_INVALIDATE_NAMED_FRAMEBUFFER_DATA, glInvalidateNamedFramebufferData) \
X(GL_INVALIDATE_NAMED_FRAMEBUFFER_SUB_DATA, glInvalidateNamedFramebufferSubData) \
 \
X(GL_BIND_SAMPLER, glBindSampler) \
X(GL_BIND_SAMPLERS, glBindSamplers) \
X(GL_DELETE_SAMPLERS, glDeleteSamplers) \
X(GL_GEN_SAMPLERS, glGenSamplers) \
X(GL_IS_SAMPLER, glIsSampler) \
X(GL_SAMPLER_PARAMETERF, glSamplerParameterf) \
X(GL_SAMPLER_PARAMETERI, glSamplerParameteri) \
X(GL_SAMPLER_PARAMETERFV, glSamplerParameterfv) \
X(GL_SAMPLER_PARAMETERIV, glSamplerParameteriv) \
X(GL_SAMPLER_PARAMETERIIV, glSamplerParameterIiv) \
X(GL_SAMPLER_PARAMETERIUIV, glSamplerParameterIuiv) \
X(GL_BIND_TEXTURES, glBindTextures) \
 \
X(GL_GET_ERROR, glGetError) \
X(GL_GET_STRING, glGetString) \
X(GL_GET_STRINGI, glGetStringi) \
X(GL_GET_BOOLEANV, glGetBooleanv) \
X(GL_GET_BOOLEANI_V, glGetBooleani_v) \
X(GL_GET_DOUBLEV, glGetDoublev) \
X(GL_GET_FLOATV, glGetFloatv) \
X(GL_GET_INTEGERV, glGetIntegerv) \
X(GL_GET_INTEGERI_V, glGetIntegeri_v) \
X(GL_GET_INTEGER64V, glGetInteger64v) \
X(GL_GET_INTEGER64I_V, glGetInteger64i_v) \
X(GL_GET_DOUBLEI_V, glGetDoublei_v) \
X(GL_GET_FLOATI_V, glGetFloati_v) \
X(GL_GET_POINTERV, glGetPointerv) \
X(GL_GET_TEX_IMAGE, glGetTexImage) \
X(GL_GET_COMPRESSED_TEX_IMAGE, glGetCompressedTexImage) \
X(GL_GET_TEX_LEVEL_PARAMETERFV, glGetTexLevelParameterfv) \
X(GL_GET_TEX_LEVEL_PARAMETERIV, glGetTexLevelParameteriv) \
X(GL_GET_TEX_PARAMETERFV, glGetTexParameterfv) \
X(GL_GET_TEX_PARAMETERIV, glGetTexParameteriv) \
X(GL_GET_TEX_PARAMETERIIV, glGetTexParameterIiv) \
X(GL_GET_TEX_PARAMETERIUIV, glGetTexParameterIuiv) \
X(GL_GET_TEX_ENVFV, glGetTexEnvfv) \
X(GL_GET_TEX_ENVIV, glGetTexEnviv) \
X(GL_GET_TEX_GENDV, glGetTexGendv) \
X(GL_GET_TEX_GENFV, glGetTexGenfv) \
X(GL_GET_TEX_GENIV, glGetTexGeniv) \
X(GL_GET_LIGHTFV, glGetLightfv) \
X(GL_GET_LIGHTIV, glGetLightiv) \
X(GL_GET_MATERIALFV, glGetMaterialfv) \
X(GL_GET_MATERIALIV, glGetMaterialiv) \
X(GL_GET_MAPDV, glGetMapdv) \
X(GL_GET_MAPFV, glGetMapfv) \
X(GL_GET_MAPIV, glGetMapiv) \
X(GL_GET_PIXEL_MAPFV, glGetPixelMapfv) \
X(GL_GET_PIXEL_MAPUIV, glGetPixelMapuiv) \
X(GL_GET_PIXEL_MAPUSV, glGetPixelMapusv) \
X(GL_GET_POLYGON_STIPPLE, glGetPolygonStipple) \
X(GL_GET_BUFFER_PARAMETERIV, glGetBufferParameteriv) \
X(GL_GET_BUFFER_PARAMETERI64V, glGetBufferParameteri64v) \
X(GL_GET_BUFFER_POINTERV, glGetBufferPointerv) \
X(GL_GET_BUFFER_SUB_DATA, glGetBufferSubData) \
X(GL_GET_NAMED_BUFFER_PARAMETERIV, glGetNamedBufferParameteriv) \
X(GL_GET_NAMED_BUFFER_PARAMETERI64V, glGetNamedBufferParameteri64v) \
X(GL_GET_NAMED_BUFFER_POINTERV, glGetNamedBufferPointerv) \
X(GL_GET_NAMED_BUFFER_SUB_DATA, glGetNamedBufferSubData) \
X(GL_GET_PROGRAMIV, glGetProgramiv) \
X(GL_GET_PROGRAM_INFO_LOG, glGetProgramInfoLog) \
X(GL_GET_SHADERIV, glGetShaderiv) \
X(GL_GET_SHADER_INFO_LOG, glGetShaderInfoLog) \
X(GL_GET_SHADER_SOURCE, glGetShaderSource) \
X(GL_GET_SHADER_PRECISION_FORMAT, glGetShaderPrecisionFormat) \
X(GL_GET_ATTACHED_SHADERS, glGetAttachedShaders) \
X(GL_GET_ACTIVE_ATTRIB, glGetActiveAttrib) \
X(GL_GET_ACTIVE_UNIFORM, glGetActiveUniform) \
X(GL_GET_ACTIVE_UNIFORMSIV, glGetActiveUniformsiv) \
X(GL_GET_ACTIVE_UNIFORM_NAME, glGetActiveUniformName) \
X(GL_GET_ACTIVE_UNIFORM_BLOCKIV, glGetActiveUniformBlockiv) \
X(GL_GET_ACTIVE_UNIFORM_BLOCK_NAME, glGetActiveUniformBlockName) \
X(GL_GET_UNIFORMINDICES, glGetUniformIndices) \
X(GL_GET_UNIFORM_BLOCK_INDEX, glGetUniformBlockIndex) \
X(GL_GET_UNIFORMFV, glGetUniformfv) \
X(GL_GET_UNIFORMIV, glGetUniformiv) \
X(GL_GET_UNIFORMUIV, glGetUniformuiv) \
X(GL_GET_UNIFORMDV, glGetUniformdv) \
X(GL_GET_UNIFORM_LOCATION, glGetUniformLocation) \
X(GL_GET_ATTRIB_LOCATION, glGetAttribLocation) \
X(GL_GET_FRAMEBUFFER_ATTACHMENT_PARAMETERIV, glGetFramebufferAttachmentParameteriv) \
X(GL_GET_RENDERBUFFER_PARAMETERIV, glGetRenderbufferParameteriv) \
X(GL_GET_VERTEX_ATTRIBDV, glGetVertexAttribdv) \
X(GL_GET_VERTEX_ATTRIBFV, glGetVertexAttribfv) \
X(GL_GET_VERTEX_ATTRIBIV, glGetVertexAttribiv) \
X(GL_GET_VERTEX_ATTRIB_IIV, glGetVertexAttribIiv) \
X(GL_GET_VERTEX_ATTRIB_IUIV, glGetVertexAttribIuiv) \
X(GL_GET_VERTEX_ATTRIB_LDV, glGetVertexAttribLdv) \
X(GL_GET_VERTEX_ATTRIB_POINTERV, glGetVertexAttribPointerv) \
X(GL_GET_QUERYIV, glGetQueryiv) \
X(GL_GET_QUERY_OBJECTIV, glGetQueryObjectiv) \
X(GL_GET_QUERY_OBJECTUIV, glGetQueryObjectuiv) \
X(GL_GET_QUERY_BUFFER_OBJECTIV, glGetQueryBufferObjectiv) \
X(GL_GET_QUERY_BUFFER_OBJECTUIV, glGetQueryBufferObjectuiv) \
X(GL_GET_QUERY_OBJECTI64V, glGetQueryObjecti64v) \
X(GL_GET_QUERY_OBJECTUI64V, glGetQueryObjectui64v) \
X(GL_GET_QUERY_INDEXEDIV, glGetQueryIndexediv) \
X(GL_GET_SUBROUTINE_UNIFORM_LOCATIONS, glGetSubroutineUniformLocation) \
X(GL_GET_MULTISAMPLEFV, glGetMultisamplefv) \
X(GL_GET_INTERNALFORMATIV, glGetInternalformativ) \
X(GL_GET_INTERNALFORMATI64V, glGetInternalformati64v) \
X(GL_GET_FRAMEBUFFER_PARAMETERIV, glGetFramebufferParameteriv) \
X(GL_GET_OBJECT_LABEL, glGetObjectLabel) \
X(GL_GET_OBJECT_PTR_LABEL, glGetObjectPtrLabel) \
X(GL_GET_DEBUG_MESSAGE_LOG, glGetDebugMessageLog) \
X(GL_GET_NAMED_FRAMEBUFFER_ATTACHMENT_PARAMETERIV, glGetNamedFramebufferAttachmentParameteriv) \
X(GL_GET_NAMED_FRAMEBUFFER_PARAMETERIV, glGetNamedFramebufferParameteriv) \
X(GL_GET_NAMED_RENDERBUFFER_PARAMETERIV, glGetNamedRenderbufferParameteriv) \
X(GL_GET_TEXTURE_PARAMETERFV, glGetTextureParameterfv) \
X(GL_GET_TEXTURE_PARAMETERIV, glGetTextureParameteriv) \
X(GL_GET_TEXTURE_PARAMETERIIV, glGetTextureParameterIiv) \
X(GL_GET_TEXTURE_PARAMETERIUIV, glGetTextureParameterIuiv) \
X(GL_GET_TEXTURE_LEVEL_PARAMETERFV, glGetTextureLevelParameterfv) \
X(GL_GET_TEXTURE_LEVEL_PARAMETERIV, glGetTextureLevelParameteriv) \
X(GL_GET_TEXTURE_IMAGE2, glGetTextureImage) \
X(GL_GET_TEXTURE_SUB_IMAGE, glGetTextureSubImage) \
X(GL_GET_COMPRESSED_TEXTURE_IMAGE, glGetCompressedTextureImage) \
X(GL_GET_COMPRESSED_TEXTURE_SUB_IMAGE, glGetCompressedTextureSubImage) \
X(GL_GET_SAMPLER_PARAMETERFV, glGetSamplerParameterfv) \
X(GL_GET_SAMPLER_PARAMETERIV, glGetSamplerParameteriv) \
X(GL_GET_SAMPLER_PARAMETERIIV, glGetSamplerParameterIiv) \
X(GL_GET_SAMPLER_PARAMETERIUIV, glGetSamplerParameterIuiv) \
X(GL_GET_TRANSFORM_FEEDBACK_VARYING, glGetTransformFeedbackVarying) \
X(GL_GET_TRANSFORM_FEEDBACKIV, glGetTransformFeedbackiv) \
X(GL_GET_TRANSFORM_FEEDBACKI_V, glGetTransformFeedbacki_v) \
X(GL_GET_TRANSFORM_FEEDBACKI64_V, glGetTransformFeedbacki64_v) \
X(GL_GET_TRANSFORM_FEEDBACK_BUFFERIV, glGetTransformFeedbackBufferiv) \
X(GL_GET_VERTEX_ARRAYIV, glGetVertexArrayiv) \
X(GL_GET_VERTEX_ARRAY_INDEXEDIV, glGetVertexArrayIndexediv) \
X(GL_GET_VERTEX_ARRAY_INDEXED64IV, glGetVertexArrayIndexed64iv) \
X(GL_GET_PROGRAM_PIPELINEIV, glGetProgramPipelineiv) \
X(GL_GET_PROGRAM_PIPELINE_INFO_LOG, glGetProgramPipelineInfoLog) \
X(GL_GET_PROGRAM_RESOURCEIV, glGetProgramResourceiv) \
X(GL_GET_PROGRAM_RESOURCE_NAME, glGetProgramResourceName) \
X(GL_GET_PROGRAM_RESOURCE_INDEX, glGetProgramResourceIndex) \
X(GL_GET_PROGRAM_RESOURCE_LOCATION, glGetProgramResourceLocation) \
X(GL_GET_PROGRAM_RESOURCE_LOCATION_INDEX, glGetProgramResourceLocationIndex) \
X(GL_GET_PROGRAM_INTERFACEIV, glGetProgramInterfaceiv) \
X(GL_GET_GRAPHICS_RESET_STATUS, glGetGraphicsResetStatus) \
X(GL_GET_NUNIFORMFV, glGetnUniformfv) \
X(GL_GET_NUNIFORMIV, glGetnUniformiv) \
X(GL_GET_NUNIFORMUIV, glGetnUniformuiv) \
X(GL_GET_NUNIFORMDV, glGetnUniformdv) \
X(GL_GET_NTEX_IMAGE, glGetnTexImage) \
X(GL_GET_NCOMPRESSED_TEX_IMAGE, glGetnCompressedTexImage) \
X(GL_IS_ENABLED, glIsEnabled) \
X(GL_IS_ENABLEDI, glIsEnabledi) \
X(GL_IS_PROGRAM, glIsProgram) \
X(GL_IS_SHADER, glIsShader) \
X(GL_GET_CLIP_PLANE, glGetClipPlane)

enum gpu_gl_cmd {
    GPU_CMD_GL_NONE = GPU_CMD_GL_FIRST - 1,
#define GPU_GL_ENUM_ROW(name, fn) GPU_CMD_##name,
    GPU_GL_CMD_LIST(GPU_GL_ENUM_ROW)
#undef GPU_GL_ENUM_ROW
    GPU_CMD_GL_SENTINEL,
};

#define GPU_CMD_GL_COUNT ((int)(GPU_CMD_GL_SENTINEL - GPU_CMD_GL_FIRST))


#define GPU_VK_CMD_LIST(X) \
 \
X(VK_CREATE_INSTANCE, vkCreateInstance) \
X(VK_DESTROY_INSTANCE, vkDestroyInstance) \
X(VK_ENUMERATE_PHYSICAL_DEVICES, vkEnumeratePhysicalDevices) \
X(VK_ENUMERATE_INSTANCE_VERSION, vkEnumerateInstanceVersion) \
X(VK_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES, vkEnumerateInstanceExtensionProperties) \
X(VK_ENUMERATE_INSTANCE_LAYER_PROPERTIES, vkEnumerateInstanceLayerProperties) \
X(VK_ENUMERATE_DEVICE_EXTENSION_PROPERTIES, vkEnumerateDeviceExtensionProperties) \
X(VK_ENUMERATE_DEVICE_LAYER_PROPERTIES, vkEnumerateDeviceLayerProperties) \
X(VK_CREATE_DEVICE, vkCreateDevice) \
X(VK_DESTROY_DEVICE, vkDestroyDevice) \
X(VK_GET_DEVICE_PROC_ADDR, vkGetDeviceProcAddr) \
X(VK_GET_INSTANCE_PROC_ADDR, vkGetInstanceProcAddr) \
X(VK_GET_DEVICE_QUEUE, vkGetDeviceQueue) \
X(VK_GET_DEVICE_QUEUE2, vkGetDeviceQueue2) \
X(VK_DEVICE_WAIT_IDLE, vkDeviceWaitIdle) \
X(VK_QUEUE_WAIT_IDLE, vkQueueWaitIdle) \
 \
X(VK_GET_PHYSICAL_DEVICE_FEATURES, vkGetPhysicalDeviceFeatures) \
X(VK_GET_PHYSICAL_DEVICE_FEATURES2, vkGetPhysicalDeviceFeatures2) \
X(VK_GET_PHYSICAL_DEVICE_PROPERTIES, vkGetPhysicalDeviceProperties) \
X(VK_GET_PHYSICAL_DEVICE_PROPERTIES2, vkGetPhysicalDeviceProperties2) \
X(VK_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES, vkGetPhysicalDeviceFormatProperties) \
X(VK_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES2, vkGetPhysicalDeviceFormatProperties2) \
X(VK_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES, vkGetPhysicalDeviceImageFormatProperties) \
X(VK_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2, vkGetPhysicalDeviceImageFormatProperties2) \
X(VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES, vkGetPhysicalDeviceMemoryProperties) \
X(VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES2, vkGetPhysicalDeviceMemoryProperties2) \
X(VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, vkGetPhysicalDeviceQueueFamilyProperties) \
X(VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES2, vkGetPhysicalDeviceQueueFamilyProperties2) \
X(VK_GET_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_PROPERTIES, vkGetPhysicalDeviceSparseImageFormatProperties) \
X(VK_GET_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_PROPERTIES2, vkGetPhysicalDeviceSparseImageFormatProperties2) \
X(VK_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES, vkGetPhysicalDeviceExternalBufferProperties) \
X(VK_GET_PHYSICAL_DEVICE_EXTERNAL_FENCE_PROPERTIES, vkGetPhysicalDeviceExternalFenceProperties) \
X(VK_GET_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_PROPERTIES, vkGetPhysicalDeviceExternalSemaphoreProperties) \
X(VK_GET_PHYSICAL_DEVICE_TOOL_PROPERTIES, vkGetPhysicalDeviceToolProperties) \
X(VK_ENUMERATE_PHYSICAL_DEVICE_GROUPS, vkEnumeratePhysicalDeviceGroups) \
X(VK_GET_PHYSICAL_DEVICE_SURFACE_SUPPORT_KHR, vkGetPhysicalDeviceSurfaceSupportKHR) \
X(VK_GET_PHYSICAL_DEVICE_SURFACE_CAPABILITIES_KHR, vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
X(VK_GET_PHYSICAL_DEVICE_SURFACE_FORMATS_KHR, vkGetPhysicalDeviceSurfaceFormatsKHR) \
X(VK_GET_PHYSICAL_DEVICE_SURFACE_PRESENT_MODES_KHR, vkGetPhysicalDeviceSurfacePresentModesKHR) \
 \
X(VK_ALLOCATE_MEMORY, vkAllocateMemory) \
X(VK_FREE_MEMORY, vkFreeMemory) \
X(VK_MAP_MEMORY, vkMapMemory) \
X(VK_UNMAP_MEMORY, vkUnmapMemory) \
X(VK_FLUSH_MAPPED_MEMORY_RANGES, vkFlushMappedMemoryRanges) \
X(VK_INVALIDATE_MAPPED_MEMORY_RANGES, vkInvalidateMappedMemoryRanges) \
X(VK_GET_DEVICE_MEMORY_COMMITMENT, vkGetDeviceMemoryCommitment) \
X(VK_BIND_BUFFER_MEMORY, vkBindBufferMemory) \
X(VK_BIND_BUFFER_MEMORY2, vkBindBufferMemory2) \
X(VK_BIND_IMAGE_MEMORY, vkBindImageMemory) \
X(VK_BIND_IMAGE_MEMORY2, vkBindImageMemory2) \
X(VK_GET_BUFFER_MEMORY_REQUIREMENTS, vkGetBufferMemoryRequirements) \
X(VK_GET_BUFFER_MEMORY_REQUIREMENTS2, vkGetBufferMemoryRequirements2) \
X(VK_GET_IMAGE_MEMORY_REQUIREMENTS, vkGetImageMemoryRequirements) \
X(VK_GET_IMAGE_MEMORY_REQUIREMENTS2, vkGetImageMemoryRequirements2) \
X(VK_GET_DEVICE_IMAGE_MEMORY_REQUIREMENTS, vkGetDeviceImageMemoryRequirements) \
X(VK_GET_IMAGE_SPARSE_MEMORY_REQUIREMENTS, vkGetImageSparseMemoryRequirements) \
X(VK_GET_IMAGE_SPARSE_MEMORY_REQUIREMENTS2, vkGetImageSparseMemoryRequirements2) \
X(VK_GET_DEVICE_IMAGE_SPARSE_MEMORY_REQUIREMENTS, vkGetDeviceImageSparseMemoryRequirements) \
X(VK_GET_BUFFER_DEVICE_ADDRESS, vkGetBufferDeviceAddress) \
X(VK_GET_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS, vkGetDeviceMemoryOpaqueCaptureAddress) \
X(VK_GET_BUFFER_OPAQUE_CAPTURE_ADDRESS, vkGetBufferOpaqueCaptureAddress) \
X(VK_GET_PHYSICAL_DEVICE_CALIBRATED_TIMESTAMPS_EXT, vkGetPhysicalDeviceCalibratedTimestampsEXT) \
 \
X(VK_CREATE_BUFFER, vkCreateBuffer) \
X(VK_DESTROY_BUFFER, vkDestroyBuffer) \
X(VK_CREATE_BUFFER_VIEW, vkCreateBufferView) \
X(VK_DESTROY_BUFFER_VIEW, vkDestroyBufferView) \
X(VK_CREATE_IMAGE, vkCreateImage) \
X(VK_DESTROY_IMAGE, vkDestroyImage) \
X(VK_CREATE_IMAGE_VIEW, vkCreateImageView) \
X(VK_DESTROY_IMAGE_VIEW, vkDestroyImageView) \
X(VK_GET_IMAGE_SUBRESOURCE_LAYOUT, vkGetImageSubresourceLayout) \
X(VK_CREATE_SAMPLER, vkCreateSampler) \
X(VK_DESTROY_SAMPLER, vkDestroySampler) \
X(VK_CREATE_SAMPLER_YCBCR_CONVERSION, vkCreateSamplerYcbcrConversion) \
X(VK_DESTROY_SAMPLER_YCBCR_CONVERSION, vkDestroySamplerYcbcrConversion) \
X(VK_CREATE_ACCELERATION_STRUCTURE_KHR, vkCreateAccelerationStructureKHR) \
X(VK_DESTROY_ACCELERATION_STRUCTURE_KHR, vkDestroyAccelerationStructureKHR) \
 \
X(VK_CREATE_COMMAND_POOL, vkCreateCommandPool) \
X(VK_DESTROY_COMMAND_POOL, vkDestroyCommandPool) \
X(VK_RESET_COMMAND_POOL, vkResetCommandPool) \
X(VK_ALLOCATE_COMMAND_BUFFERS, vkAllocateCommandBuffers) \
X(VK_FREE_COMMAND_BUFFERS, vkFreeCommandBuffers) \
X(VK_BEGIN_COMMAND_BUFFER, vkBeginCommandBuffer) \
X(VK_END_COMMAND_BUFFER, vkEndCommandBuffer) \
X(VK_RESET_COMMAND_BUFFER, vkResetCommandBuffer) \
 \
X(VK_QUEUE_SUBMIT, vkQueueSubmit) \
X(VK_QUEUE_SUBMIT2, vkQueueSubmit2) \
X(VK_QUEUE_BIND_SPARSE, vkQueueBindSparse) \
X(VK_QUEUE_PRESENT_KHR, vkQueuePresentKHR) \
 \
X(VK_CREATE_FENCE, vkCreateFence) \
X(VK_DESTROY_FENCE, vkDestroyFence) \
X(VK_RESET_FENCES, vkResetFences) \
X(VK_GET_FENCE_STATUS, vkGetFenceStatus) \
X(VK_SIGNAL_FENCE, vkSignalFence) \
X(VK_CREATE_SEMAPHORE, vkCreateSemaphore) \
X(VK_DESTROY_SEMAPHORE, vkDestroySemaphore) \
X(VK_GET_SEMAPHORE_COUNTER_VALUE, vkGetSemaphoreCounterValue) \
X(VK_SIGNAL_SEMAPHORE, vkSignalSemaphore) \
X(VK_WAIT_SEMAPHORES, vkWaitSemaphores) \
X(VK_CREATE_EVENT, vkCreateEvent) \
X(VK_DESTROY_EVENT, vkDestroyEvent) \
X(VK_GET_EVENT_STATUS, vkGetEventStatus) \
X(VK_SET_EVENT, vkSetEvent) \
X(VK_RESET_EVENT, vkResetEvent) \
 \
X(VK_CREATE_DESCRIPTOR_SET_LAYOUT, vkCreateDescriptorSetLayout) \
X(VK_DESTROY_DESCRIPTOR_SET_LAYOUT, vkDestroyDescriptorSetLayout) \
X(VK_CREATE_DESCRIPTOR_POOL, vkCreateDescriptorPool) \
X(VK_DESTROY_DESCRIPTOR_POOL, vkDestroyDescriptorPool) \
X(VK_RESET_DESCRIPTOR_POOL, vkResetDescriptorPool) \
X(VK_ALLOCATE_DESCRIPTOR_SETS, vkAllocateDescriptorSets) \
X(VK_FREE_DESCRIPTOR_SETS, vkFreeDescriptorSets) \
X(VK_UPDATE_DESCRIPTOR_SETS, vkUpdateDescriptorSets) \
X(VK_CREATE_DESCRIPTOR_UPDATE_TEMPLATE, vkCreateDescriptorUpdateTemplate) \
X(VK_DESTROY_DESCRIPTOR_UPDATE_TEMPLATE, vkDestroyDescriptorUpdateTemplate) \
X(VK_UPDATE_DESCRIPTOR_SET_WITH_TEMPLATE, vkUpdateDescriptorSetWithTemplate) \
X(VK_GET_DESCRIPTOR_SET_LAYOUT_SUPPORT, vkGetDescriptorSetLayoutSupport) \
 \
X(VK_CREATE_SHADER_MODULE, vkCreateShaderModule) \
X(VK_DESTROY_SHADER_MODULE, vkDestroyShaderModule) \
X(VK_CREATE_PIPELINE_CACHE, vkCreatePipelineCache) \
X(VK_DESTROY_PIPELINE_CACHE, vkDestroyPipelineCache) \
X(VK_GET_PIPELINE_CACHE_DATA, vkGetPipelineCacheData) \
X(VK_MERGE_PIPELINE_CACHES, vkMergePipelineCaches) \
X(VK_CREATE_GRAPHICS_PIPELINES, vkCreateGraphicsPipelines) \
X(VK_CREATE_COMPUTE_PIPELINES, vkCreateComputePipelines) \
X(VK_CREATE_RAY_TRACING_PIPELINES_KHR, vkCreateRayTracingPipelinesKHR) \
X(VK_DESTROY_PIPELINE, vkDestroyPipeline) \
X(VK_CREATE_PIPELINE_LAYOUT, vkCreatePipelineLayout) \
X(VK_DESTROY_PIPELINE_LAYOUT, vkDestroyPipelineLayout) \
X(VK_CREATE_QUERY_POOL, vkCreateQueryPool) \
X(VK_DESTROY_QUERY_POOL, vkDestroyQueryPool) \
X(VK_GET_QUERY_POOL_RESULTS, vkGetQueryPoolResults) \
X(VK_RESET_QUERY_POOL, vkResetQueryPool) \
 \
X(VK_CREATE_RENDER_PASS, vkCreateRenderPass) \
X(VK_CREATE_RENDER_PASS2, vkCreateRenderPass2) \
X(VK_DESTROY_RENDER_PASS, vkDestroyRenderPass) \
X(VK_CREATE_FRAMEBUFFER, vkCreateFramebuffer) \
X(VK_DESTROY_FRAMEBUFFER, vkDestroyFramebuffer) \
 \
X(VK_CREATE_SWAPCHAIN_KHR, vkCreateSwapchainKHR) \
X(VK_DESTROY_SWAPCHAIN_KHR, vkDestroySwapchainKHR) \
X(VK_GET_SWAPCHAIN_IMAGES_KHR, vkGetSwapchainImagesKHR) \
X(VK_ACQUIRE_NEXT_IMAGE_KHR, vkAcquireNextImageKHR) \
X(VK_ACQUIRE_NEXT_IMAGE2_KHR, vkAcquireNextImage2KHR) \
 \
X(VK_CREATE_PRIVATE_DATA_SLOT, vkCreatePrivateDataSlot) \
X(VK_DESTROY_PRIVATE_DATA_SLOT, vkDestroyPrivateDataSlot) \
X(VK_SET_PRIVATE_DATA, vkSetPrivateData) \
X(VK_GET_PRIVATE_DATA, vkGetPrivateData) \
 \
X(VK_CMD_BIND_PIPELINE, vkCmdBindPipeline) \
X(VK_CMD_SET_VIEWPORT, vkCmdSetViewport) \
X(VK_CMD_SET_SCISSOR, vkCmdSetScissor) \
X(VK_CMD_SET_LINE_WIDTH, vkCmdSetLineWidth) \
X(VK_CMD_SET_DEPTH_BIAS, vkCmdSetDepthBias) \
X(VK_CMD_SET_BLEND_CONSTANTS, vkCmdSetBlendConstants) \
X(VK_CMD_SET_DEPTH_BOUNDS, vkCmdSetDepthBounds) \
X(VK_CMD_SET_STENCIL_COMPARE_MASK, vkCmdSetStencilCompareMask) \
X(VK_CMD_SET_STENCIL_WRITE_MASK, vkCmdSetStencilWriteMask) \
X(VK_CMD_SET_STENCIL_REFERENCE, vkCmdSetStencilReference) \
X(VK_CMD_SET_CULL_MODE, vkCmdSetCullMode) \
X(VK_CMD_SET_FRONT_FACE, vkCmdSetFrontFace) \
X(VK_CMD_SET_PRIMITIVE_TOPOLOGY, vkCmdSetPrimitiveTopology) \
X(VK_CMD_SET_VIEWPORT_WITH_COUNT, vkCmdSetViewportWithCount) \
X(VK_CMD_SET_SCISSOR_WITH_COUNT, vkCmdSetScissorWithCount) \
X(VK_CMD_BIND_VERTEX_BUFFERS, vkCmdBindVertexBuffers) \
X(VK_CMD_BIND_VERTEX_BUFFERS2, vkCmdBindVertexBuffers2) \
X(VK_CMD_SET_DEPTH_TEST_ENABLE, vkCmdSetDepthTestEnable) \
X(VK_CMD_SET_DEPTH_WRITE_ENABLE, vkCmdSetDepthWriteEnable) \
X(VK_CMD_SET_DEPTH_COMPARE_OP, vkCmdSetDepthCompareOp) \
X(VK_CMD_SET_DEPTH_BOUNDS_TEST_ENABLE, vkCmdSetDepthBoundsTestEnable) \
X(VK_CMD_SET_STENCIL_TEST_ENABLE, vkCmdSetStencilTestEnable) \
X(VK_CMD_SET_STENCIL_OP, vkCmdSetStencilOp) \
X(VK_CMD_SET_RASTERIZER_DISCARD_ENABLE, vkCmdSetRasterizerDiscardEnable) \
X(VK_CMD_SET_DEPTH_BIAS_ENABLE, vkCmdSetDepthBiasEnable) \
X(VK_CMD_SET_PRIMITIVE_RESTART_ENABLE, vkCmdSetPrimitiveRestartEnable) \
X(VK_CMD_BIND_DESCRIPTOR_SETS, vkCmdBindDescriptorSets) \
X(VK_CMD_BIND_INDEX_BUFFER, vkCmdBindIndexBuffer) \
X(VK_CMD_DRAW, vkCmdDraw) \
X(VK_CMD_DRAW_INDEXED, vkCmdDrawIndexed) \
X(VK_CMD_DRAW_INDIRECT, vkCmdDrawIndirect) \
X(VK_CMD_DRAW_INDEXED_INDIRECT, vkCmdDrawIndexedIndirect) \
X(VK_CMD_DRAW_INDIRECT_COUNT, vkCmdDrawIndirectCount) \
X(VK_CMD_DRAW_INDEXED_INDIRECT_COUNT, vkCmdDrawIndexedIndirectCount) \
X(VK_CMD_DISPATCH, vkCmdDispatch) \
X(VK_CMD_DISPATCH_INDIRECT, vkCmdDispatchIndirect) \
X(VK_CMD_TRACE_RAYS_KHR, vkCmdTraceRaysKHR) \
X(VK_CMD_TRACE_RAYS_INDIRECT_KHR, vkCmdTraceRaysIndirectKHR) \
X(VK_CMD_COPY_BUFFER, vkCmdCopyBuffer) \
X(VK_CMD_COPY_BUFFER2, vkCmdCopyBuffer2) \
X(VK_CMD_COPY_IMAGE, vkCmdCopyImage) \
X(VK_CMD_COPY_IMAGE2, vkCmdCopyImage2) \
X(VK_CMD_COPY_BUFFER_TO_IMAGE, vkCmdCopyBufferToImage) \
X(VK_CMD_COPY_BUFFER_TO_IMAGE2, vkCmdCopyBufferToImage2) \
X(VK_CMD_COPY_IMAGE_TO_BUFFER, vkCmdCopyImageToBuffer) \
X(VK_CMD_COPY_IMAGE_TO_BUFFER2, vkCmdCopyImageToBuffer2) \
X(VK_CMD_BLIT_IMAGE, vkCmdBlitImage) \
X(VK_CMD_BLIT_IMAGE2, vkCmdBlitImage2) \
X(VK_CMD_RESOLVE_IMAGE, vkCmdResolveImage) \
X(VK_CMD_UPDATE_BUFFER, vkCmdUpdateBuffer) \
X(VK_CMD_FILL_BUFFER, vkCmdFillBuffer) \
X(VK_CMD_CLEAR_COLOR_IMAGE, vkCmdClearColorImage) \
X(VK_CMD_CLEAR_DEPTH_STENCIL_IMAGE, vkCmdClearDepthStencilImage) \
X(VK_CMD_CLEAR_ATTACHMENTS, vkCmdClearAttachments) \
X(VK_CMD_BEGIN_QUERY, vkCmdBeginQuery) \
X(VK_CMD_END_QUERY, vkCmdEndQuery) \
X(VK_CMD_RESET_QUERY_POOL, vkCmdResetQueryPool) \
X(VK_CMD_WRITE_TIMESTAMP, vkCmdWriteTimestamp) \
X(VK_CMD_COPY_QUERY_POOL_RESULTS, vkCmdCopyQueryPoolResults) \
X(VK_CMD_BEGIN_RENDER_PASS, vkCmdBeginRenderPass) \
X(VK_CMD_BEGIN_RENDER_PASS2, vkCmdBeginRenderPass2) \
X(VK_CMD_BEGIN_RENDERING, vkCmdBeginRendering) \
X(VK_CMD_END_RENDER_PASS, vkCmdEndRenderPass) \
X(VK_CMD_END_RENDER_PASS2, vkCmdEndRenderPass2) \
X(VK_CMD_END_RENDERING, vkCmdEndRendering) \
X(VK_CMD_NEXT_SUBPASS, vkCmdNextSubpass) \
X(VK_CMD_NEXT_SUBPASS2, vkCmdNextSubpass2) \
X(VK_CMD_EXECUTE_COMMANDS, vkCmdExecuteCommands) \
X(VK_CMD_PIPELINE_BARRIER, vkCmdPipelineBarrier) \
X(VK_CMD_PIPELINE_BARRIER2, vkCmdPipelineBarrier2) \
X(VK_CMD_WAIT_EVENTS, vkCmdWaitEvents) \
X(VK_CMD_WAIT_EVENTS2, vkCmdWaitEvents2) \
X(VK_CMD_SET_EVENT, vkCmdSetEvent) \
X(VK_CMD_SET_EVENT2, vkCmdSetEvent2) \
X(VK_CMD_RESET_EVENT, vkCmdResetEvent) \
X(VK_CMD_RESET_EVENT2, vkCmdResetEvent2) \
X(VK_CMD_PUSH_CONSTANTS, vkCmdPushConstants) \
X(VK_CMD_PUSH_DESCRIPTOR_SET_KHR, vkCmdPushDescriptorSetKHR) \
X(VK_CMD_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_KHR, vkCmdPushDescriptorSetWithTemplateKHR) \
X(VK_CMD_BEGIN_DEBUG_UTILS_LABEL_EXT, vkCmdBeginDebugUtilsLabelEXT) \
X(VK_CMD_END_DEBUG_UTILS_LABEL_EXT, vkCmdEndDebugUtilsLabelEXT) \
X(VK_CMD_INSERT_DEBUG_UTILS_LABEL_EXT, vkCmdInsertDebugUtilsLabelEXT) \
X(VK_CMD_SET_SAMPLE_LOCATIONS_EXT, vkCmdSetSampleLocationsEXT) \
X(VK_CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS_EXT, vkCmdBindTransformFeedbackBuffersEXT) \
X(VK_CMD_BEGIN_TRANSFORM_FEEDBACK_EXT, vkCmdBeginTransformFeedbackEXT) \
X(VK_CMD_END_TRANSFORM_FEEDBACK_EXT, vkCmdEndTransformFeedbackEXT) \
X(VK_CMD_DRAW_INDIRECT_BYTE_COUNT_EXT, vkCmdDrawIndirectByteCountEXT) \
X(VK_CMD_SET_LINE_STIPPLE_EXT, vkCmdSetLineStippleEXT) \
X(VK_CMD_SET_DEVICE_MASK, vkCmdSetDeviceMask) \
 \
X(VK_SET_DEBUG_UTILS_OBJECT_NAME_EXT, vkSetDebugUtilsObjectNameEXT) \
X(VK_SET_DEBUG_UTILS_OBJECT_TAG_EXT, vkSetDebugUtilsObjectTagEXT) \
X(VK_CREATE_DEBUG_UTILS_MESSENGER_EXT, vkCreateDebugUtilsMessengerEXT) \
X(VK_DESTROY_DEBUG_UTILS_MESSENGER_EXT, vkDestroyDebugUtilsMessengerEXT) \
X(VK_SUBMIT_DEBUG_UTILS_MESSAGE_EXT, vkSubmitDebugUtilsMessageEXT) \
X(VK_DESTROY_SURFACE_KHR, vkDestroySurfaceKHR)

enum gpu_vk_cmd {
    GPU_CMD_VK_NONE = GPU_CMD_VK_FIRST - 1,
#define GPU_VK_ENUM_ROW(name, fn) GPU_CMD_##name,
    GPU_VK_CMD_LIST(GPU_VK_ENUM_ROW)
#undef GPU_VK_ENUM_ROW
    GPU_CMD_VK_SENTINEL,
};

#define GPU_CMD_VK_COUNT ((int)(GPU_CMD_VK_SENTINEL - GPU_CMD_VK_FIRST))

#endif

