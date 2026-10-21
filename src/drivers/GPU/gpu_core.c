// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

/*
 *                                              GPU STACK
 *
 *    +----------------+   +-------------------+   +-----------------------+   +-------------------+
 *    | gpu-init PATH  |   |  gpu-devs / info  |   |        gpu-mons       |   |  kmscube / vkkmv  |
 *    | gpu-reinit     |   | gpu-drivers       |   | mon on / off / mode   |   | hw card or cpu fb |
 *    | fw: dir / .zip |   | gpu-bind / unbind |   | move / clone / layout |   +-------------------+
 *    +----------------+   +-------------------+   | bright                |             |
 *             |                     |             +-----------------------+             |
 *             |                     |                         |                         |
 *             +---------------------+-------------------------+-------------------------+
 *                                                   |
 *                       +======================================================+
 *                       |                 g p u _ c o r e . c                  |
 *                       +------------------------------------------------------+
 *                       | registry  gpu_devices[16] <- lkpi enumerate + flags  |
 *                       | engine    GL / GLES / VK / DRM batch -> ring 4 slots |
 *                       | kms       dumb fb + SETCRTC + PAGE_FLIP + DIRTYFB    |
 *                       | monitors  probe / on / mode WxH@Hz / move / clone    |
 *                       | light     hw backlight + per-monitor software dim    |
 *                       | demos     kmscube flip / vkkmv offscreen z-buffer    |
 *                       | cpu path  boot fb + snapshot + restore, ESC aborts   |
 *                       | watchdog  probe task 20 s / enum task 3 s, no hang   |
 *                       +====+----------------------+==========================+
 *                            |
 *                 +--------------------+     +--------------+     +------------------+
 *                 |    kernel/lkpi     |     |  linux/kpi   |     |  linux/upstream  |
 *                 | glue.c + lkpi.h    |---->| pci + device |---->| DRM core + ttm   |
 *                 | boots "linux" task |     | gpu + bridge |     | i915 amd nouveau |
 *                 +--------------------+     | request_fw   |     +------------------+
 *                                            +--------------+
 *
 *   fw:       gpu-init PATH -> firmware source (dir / .zip) -> request_firmware
 *   gate:     stack off -> drm stack cmds fail fast (GL/VK go through mesa)
 *   watchdog: a stuck driver probe never freezes the shell (timeout + log)
 *   mesa:     userspace translator attaches over /dev/gpu (GPU_IOC_*)
 *   console:  Limine GOP text until a DRM driver claims a connector
 */

#include "gpu_core.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "kernel/lkpi/lkpi.h"
#include "kernel/scheduler/scheduler.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Video/limine_video_driver.h"
#include "drivers/Video/gop_font.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_gl_defs.h"

void mesa_on_flush(int dev_idx, struct gpu_ring_slot *slot);
int mesa_init(void);

#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>


#define GPU_GL_NAME_ROW(name, fn) [GPU_CMD_##name - GPU_CMD_GL_FIRST] = #fn,
static const char *gpu_gl_names[GPU_CMD_GL_COUNT] = {
    GPU_GL_CMD_LIST(GPU_GL_NAME_ROW)
};
#undef GPU_GL_NAME_ROW

#define GPU_VK_NAME_ROW(name, fn) [GPU_CMD_##name - GPU_CMD_VK_FIRST] = #fn,
static const char *gpu_vk_names[GPU_CMD_VK_COUNT] = {
    GPU_VK_CMD_LIST(GPU_VK_NAME_ROW)
};
#undef GPU_VK_NAME_ROW

static const char *gpu_drm_names[GPU_CMD_DRM_LAST - GPU_CMD_DRM_FIRST] = {
    "glDrmVersion", "glDrmGetCap", "glDrmSetClientCap", "glDrmGemClose",
    "glDrmGetMagic", "glDrmAuthMagic", "glDrmGetUnique", "glDrmDropMaster",
    "glDrmSetMaster", "glDrmModeGetResources", "glDrmIoctl",
};

const char *gpu_cmd_name(uint8_t api, uint16_t opcode) {
    if (api == GPU_API_OPENGL || api == GPU_API_OPENGL_ES) {
        int i = opcode - GPU_CMD_GL_FIRST;
        if (i >= 0 && i < GPU_CMD_GL_COUNT) return gpu_gl_names[i] ? gpu_gl_names[i] : "?";
        return NULL;
    }
    if (api == GPU_API_VULKAN) {
        int i = opcode - GPU_CMD_VK_FIRST;
        if (i >= 0 && i < GPU_CMD_VK_COUNT) return gpu_vk_names[i] ? gpu_vk_names[i] : "?";
        return NULL;
    }
    if (api == GPU_API_NONE) {
        int i = opcode - GPU_CMD_DRM_FIRST;
        if (i >= 0 && i < GPU_CMD_DRM_LAST - GPU_CMD_DRM_FIRST)
            return gpu_drm_names[i] ? gpu_drm_names[i] : "?";
        return NULL;
    }
    return NULL;
}


typedef struct gpu_devctx {
    char pci_name[16];
    uint32_t primary_devt;

    void *lx_card;
    void *lx_render;

    struct gpu_ring_slot ring[GPU_RING_SLOTS];
    volatile int32_t attached;
    uint32_t total_flushed;
    uint32_t total_completed;
    uint32_t dropped;
    uint32_t largest_batch;
    uint32_t last_seq;
} gpu_devctx;

static struct gpu_device gpu_devices[GPU_DEVICE_MAX];
static int gpu_device_count;
static gpu_devctx gpu_ctx[GPU_DEVICE_MAX];
static uint32_t gpu_generation;
static bool gpu_core_ready;
static bool gpu_stack_on;
static spinlock_t gpu_registry_lock = SPINLOCK_INIT;


struct gpu_client {
    int used;
    int dev_idx;
    uint8_t api;
    uint32_t seq;
    uint32_t tr_epoch;
    uint8_t batch_open;
    uint16_t cmd_count;
    uint16_t payload_used;
    uint16_t result_used;
    uint32_t flushed_cmds;
    uint32_t flushed_batches;
    struct gpu_ring_slot *last_slot;
    uint8_t batch[GPU_BATCH_BYTES];
};

static struct gpu_client gpu_clients[GPU_MAX_CLIENTS];
static spinlock_t gpu_client_lock = SPINLOCK_INIT;


static struct tsc_driver *gpu_tsc;

static uint64_t gpu_now_ms(void) {
    if (!gpu_tsc) gpu_tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return (gpu_tsc && gpu_tsc->get_tsc_uptime_ms) ? gpu_tsc->get_tsc_uptime_ms() : 0;
}

#define GPU_PROBE_TIMEOUT_MS 20000
#define GPU_PROBE_STALE_MS 30000
#define GPU_ENUM_TIMEOUT_MS 3000

enum { GPU_PROBE_REINIT = 0, GPU_PROBE_RESTART, GPU_PROBE_BIND, GPU_PROBE_UNBIND };

typedef struct gpu_probe_job {
    volatile int done;
    volatile int result;
    int mode;
    char pci_name[16];
    int r_unbound;
    int r_bound;
    uint32_t epoch;
} gpu_probe_job;

static gpu_probe_job gpu_probe;
static volatile int gpu_probe_live;
static volatile uint32_t gpu_probe_epoch;
static uint64_t gpu_probe_started_ms;

static void gpu_probe_task(void *arg) {
    gpu_probe_job *j = arg;
    uint32_t my_epoch = j->epoch;
    int mode = j->mode;
    char pci_name[16];
    memcpy(pci_name, j->pci_name, sizeof(pci_name));
    int r_unbound = 0, r_bound = 0;
    int result;
    switch (mode) {
    case GPU_PROBE_REINIT:  result = lkpi_gpu_reinit(); break;
    case GPU_PROBE_RESTART: result = lkpi_gpu_restart(&r_unbound, &r_bound); break;
    case GPU_PROBE_BIND:    result = lkpi_gpu_bind_one(pci_name); break;
    default:                result = lkpi_gpu_unbind_one(pci_name); break;
    }
    if (my_epoch != __atomic_load_n(&gpu_probe_epoch, __ATOMIC_ACQUIRE)) return;
    j->result = result;
    j->r_unbound = r_unbound;
    j->r_bound = r_bound;
    j->done = 1;
    scheduler_wake_all((void *)&j->done);
    __atomic_store_n(&gpu_probe_live, 0, __ATOMIC_RELEASE);
}

static int gpu_probe_run(int mode, const char *pci_name, int timeout_ms) {
    if (__atomic_exchange_n(&gpu_probe_live, 1, __ATOMIC_ACQ_REL)) {
        uint64_t now = gpu_now_ms();
        if (now && gpu_probe_started_ms && now - gpu_probe_started_ms > GPU_PROBE_STALE_MS) {
            LOG_WARNING("gpu core: the previous driver probe has been silent for over %d ms "
                        "(a wedged driver?); taking the probe lock back",
                        GPU_PROBE_STALE_MS);
            __atomic_add_fetch(&gpu_probe_epoch, 1, __ATOMIC_ACQ_REL);
            __atomic_store_n(&gpu_probe_live, 0, __ATOMIC_RELEASE);
            if (__atomic_exchange_n(&gpu_probe_live, 1, __ATOMIC_ACQ_REL)) {
                LOG_WARNING("gpu core: a driver probe is still in flight, command skipped");
                return GPU_ERR_BUSY;
            }
        } else {
            LOG_WARNING("gpu core: a driver probe is still in flight (a stuck driver?), command skipped");
            return GPU_ERR_BUSY;
        }
    }
    memset((void *)&gpu_probe, 0, sizeof(gpu_probe));
    gpu_probe.mode = mode;
    gpu_probe.epoch = __atomic_load_n(&gpu_probe_epoch, __ATOMIC_ACQUIRE);
    if (pci_name) strncpy(gpu_probe.pci_name, pci_name, sizeof(gpu_probe.pci_name) - 1);
    uint64_t started = gpu_now_ms();
    if (started) gpu_probe_started_ms = started;
    if (!task_create("gpu-probe", gpu_probe_task, &gpu_probe, PRIO_DEFAULT)) {
        gpu_probe_task(&gpu_probe);
        return gpu_probe.result;
    }
    if (scheduler_wait((void *)&gpu_probe.done, &gpu_probe.done, timeout_ms) == 0)
        return gpu_probe.result;
    LOG_ERROR("gpu core: the driver probe did not return within %d ms (the driver is stuck on this "
              "hardware); the shell stays alive and the probe task keeps running in the background",
              timeout_ms);
    return GPU_ERR_TIMEOUT;
}


static gpu_devctx *gpu_dev_ctx(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= gpu_device_count || dev_idx >= GPU_DEVICE_MAX) return NULL;
    return &gpu_ctx[dev_idx];
}

static struct gpu_device *core_dev(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= gpu_device_count) return NULL;
    if (!gpu_devices[dev_idx].valid) return NULL;
    return &gpu_devices[dev_idx];
}

typedef struct gpu_enum_stash {
    void *handles[GPU_DEVICE_MAX * 2];
    int count;
    bool reset[GPU_DEVICE_MAX];
    uint32_t old_devt[GPU_DEVICE_MAX];
} gpu_enum_stash;

static void gpu_mons_forget(int dev_idx, uint32_t old_devt);

static void gpu_reset_ctx(gpu_devctx *ctx, gpu_enum_stash *st, int n) {
    int cap = (int)(sizeof(st->handles) / sizeof(st->handles[0]));
    if (ctx->lx_card && st->count < cap) st->handles[st->count++] = ctx->lx_card;
    if (ctx->lx_render && st->count < cap) st->handles[st->count++] = ctx->lx_render;
    st->reset[n] = true;
    st->old_devt[n] = ctx->primary_devt;
    memset(ctx, 0, sizeof(*ctx));
}


static void gpu_merge_row(const struct lkpi_gpu_devinfo *row, int n, gpu_enum_stash *st) {
    struct gpu_device *d = &gpu_devices[n];
    uint8_t flags = 0;

    if (row->driver[0]) flags |= GPU_DEV_FLAG_BOUND;
    if (row->boot_vga) flags |= GPU_DEV_FLAG_BOOT_VGA;
    if (row->modeset) flags |= GPU_DEV_FLAG_MODESET;
    if (row->render_avail) flags |= GPU_DEV_FLAG_RENDER;
    if (!row->pci_name[0]) flags |= GPU_DEV_FLAG_NO_PCI;

    bool identity_changed =
        (strcmp(d->pci_name, row->pci_name) != 0 || d->primary_devt != row->primary_devt ||
         strcmp(d->driver, row->driver) != 0);

    d->index = n;
    strncpy(d->pci_name, row->pci_name, sizeof(d->pci_name) - 1);
    d->vendor = row->vendor;
    d->device = row->device;
    d->class_code = row->class_code;
    d->revision = (uint8_t)row->revision;
    strncpy(d->driver, row->driver, sizeof(d->driver) - 1);
    strncpy(d->card, row->card, sizeof(d->card) - 1);
    strncpy(d->render, row->render, sizeof(d->render) - 1);
    d->primary_minor = row->primary_minor;
    d->render_minor = row->render_minor;
    d->primary_devt = row->primary_devt;
    d->render_devt = row->render_devt;
    d->vram_bytes = row->vram_bytes;
    d->num_crtcs = row->num_crtcs;
    d->num_connectors = row->num_connectors;
    d->flags = flags;
    d->valid = 1;
    d->generation = gpu_generation;

    if (identity_changed) {
        d->drm_name[0] = '\0';
        d->drm_desc[0] = '\0';
        d->drm_major = d->drm_minor = d->drm_patch = 0;
    }

    gpu_devctx *ctx = &gpu_ctx[n];
    if (identity_changed || strcmp(ctx->pci_name, row->pci_name) != 0 || ctx->primary_devt != row->primary_devt) {
        gpu_reset_ctx(ctx, st, n);
        strncpy(ctx->pci_name, row->pci_name, sizeof(ctx->pci_name) - 1);
        ctx->primary_devt = row->primary_devt;
    }
}

static void gpu_query_drm_identity(struct gpu_device *d) {
    if (!(d->flags & GPU_DEV_FLAG_BOUND) || !d->primary_devt) return;
    if (d->drm_name[0]) return;

    void *handle = NULL;
    if (lkpi_gpu_open_dev(d->primary_devt, 2 , &handle)) return;
    char name[48] = "", date[32] = "", desc[96] = "";
    int maj = 0, min = 0, pat = 0;
    long r = lkpi_gpu_drm_version(handle, &maj, &min, &pat, name, sizeof(name), date, sizeof(date),
                                  desc, sizeof(desc));
    lkpi_gpu_close_dev(handle);
    if (r) {
        LOG_WARNING("gpu core: DRM_VERSION on %s failed (%ld)", d->pci_name, r);
        return;
    }
    d->drm_major = maj;
    d->drm_minor = min;
    d->drm_patch = pat;
    strncpy(d->drm_name, name, sizeof(d->drm_name) - 1);
    strncpy(d->drm_desc, desc, sizeof(d->drm_desc) - 1);
}

static volatile int gpu_enum_done;
static volatile int gpu_enum_live;
static uint64_t gpu_enum_started_ms;

#define GPU_ENUM_STALE_MS 30000

static void gpu_enum_task(void *arg) {
    (void)arg;
    struct lkpi_gpu_devinfo rows[GPU_DEVICE_MAX];
    LOG_DEBUG("gpu-enum: enter");
    int n = lkpi_gpu_enumerate(rows, GPU_DEVICE_MAX);
    gpu_enum_stash st;
    memset(&st, 0, sizeof(st));

    uint64_t flags = spin_lock_irqsave(&gpu_registry_lock);
    gpu_generation++;
    if (n < 0) n = 0;
    if (n > GPU_DEVICE_MAX) n = GPU_DEVICE_MAX;

    int old_count = gpu_device_count;
    for (int i = 0; i < gpu_device_count; i++) gpu_devices[i].valid = 0;
    for (int i = 0; i < n; i++) gpu_merge_row(&rows[i], i, &st);
    for (int i = n; i < old_count && i < GPU_DEVICE_MAX; i++) {
        gpu_reset_ctx(&gpu_ctx[i], &st, i);
        memset(&gpu_devices[i], 0, sizeof(gpu_devices[i]));
    }
    gpu_device_count = n;
    spin_unlock_irqrestore(&gpu_registry_lock, flags);

    for (int i = 0; i < GPU_DEVICE_MAX; i++)
        if (st.reset[i]) gpu_mons_forget(i, st.old_devt[i]);
    for (int i = 0; i < st.count; i++) lkpi_gpu_close_dev(st.handles[i]);
    bool on = false;
    for (int i = 0; i < gpu_device_count; i++) {
        gpu_query_drm_identity(&gpu_devices[i]);
        if ((gpu_devices[i].flags & GPU_DEV_FLAG_BOUND) && strcmp(gpu_devices[i].driver, "simpledrm") != 0) on = true;
    }
    gpu_stack_on = on;

    LOG_INFO("gpu core: %d device(s), generation %u", gpu_device_count, gpu_generation);
    __atomic_store_n(&gpu_enum_done, 1, __ATOMIC_RELEASE);
    scheduler_wake_all((void *)&gpu_enum_done);
    __atomic_store_n(&gpu_enum_live, 0, __ATOMIC_RELEASE);
}

void gpu_core_refresh(void) {
    uint64_t now = gpu_now_ms();
    if (__atomic_exchange_n(&gpu_enum_live, 1, __ATOMIC_ACQ_REL)) {
        if (now && gpu_enum_started_ms && now - gpu_enum_started_ms > GPU_ENUM_STALE_MS) {
            LOG_WARNING("gpu core: the previous enumeration has been silent for over %d ms; starting a new one",
                        GPU_ENUM_STALE_MS);
            __atomic_store_n(&gpu_enum_live, 0, __ATOMIC_RELEASE);
            if (__atomic_exchange_n(&gpu_enum_live, 1, __ATOMIC_ACQ_REL))
                return;
        } else {
            LOG_WARNING("gpu core: an enumeration is already running (stuck driver?), keeping the last device table");
            return;
        }
    }
    gpu_enum_started_ms = now;
    __atomic_store_n(&gpu_enum_done, 0, __ATOMIC_RELEASE);
    if (!task_create("gpu-enum", gpu_enum_task, NULL, PRIO_DEFAULT)) {
        gpu_enum_task(NULL);
        return;
    }
    if (scheduler_wait((void *)&gpu_enum_done, &gpu_enum_done, GPU_ENUM_TIMEOUT_MS) != 0)
        LOG_WARNING("gpu core: enumeration stuck for %d ms (a wedged driver holds the device lists?), "
                    "keeping the last device table", GPU_ENUM_TIMEOUT_MS);
}

bool gpu_stack_started(void) { return gpu_stack_on; }

int gpu_get_device_count(void) { return gpu_device_count; }

struct gpu_device *gpu_get_device(int idx) { return core_dev(idx); }

const struct gpu_device *gpu_find_card(const char *card_name) {
    if (!card_name || !card_name[0]) return NULL;
    for (int i = 0; i < gpu_device_count; i++) {
        if (!gpu_devices[i].valid) continue;
        if (strcmp(gpu_devices[i].card, card_name) == 0) return &gpu_devices[i];
    }
    return NULL;
}


int gpu_start_stack(void) {
    int bound = gpu_probe_run(GPU_PROBE_REINIT, NULL, GPU_PROBE_TIMEOUT_MS);
    gpu_core_refresh();
    if (bound > 0) gpu_stack_on = true;
    return bound;
}

int gpu_restart_stack(int *unbound, int *bound) {
    if (unbound) *unbound = 0;
    if (bound) *bound = 0;
    int r = gpu_probe_run(GPU_PROBE_RESTART, NULL, GPU_PROBE_TIMEOUT_MS);
    if (r != GPU_ERR_TIMEOUT && r != GPU_ERR_BUSY) {
        if (unbound) *unbound = gpu_probe.r_unbound;
        if (bound) *bound = gpu_probe.r_bound;
    }
    gpu_core_refresh();
    if (r >= 0 && bound && *bound > 0) gpu_stack_on = true;
    return r;
}

int gpu_bind_device(int dev_idx) {
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (!d->pci_name[0]) return GPU_ERR_PARAM;
    int r = gpu_probe_run(GPU_PROBE_BIND, d->pci_name, GPU_PROBE_TIMEOUT_MS);
    gpu_core_refresh();
    if (r == GPU_ERR_TIMEOUT || r == GPU_ERR_BUSY) return r;
    if (r == 0) gpu_stack_on = true;
    return r < 0 ? GPU_ERR_NO_DRIVER : GPU_OK;
}

int gpu_unbind_device(int dev_idx) {
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (!d->pci_name[0]) return GPU_ERR_PARAM;
    int r = gpu_probe_run(GPU_PROBE_UNBIND, d->pci_name, GPU_PROBE_TIMEOUT_MS);
    gpu_core_refresh();
    if (r == GPU_ERR_TIMEOUT || r == GPU_ERR_BUSY) return r;
    return r < 0 ? GPU_ERR_PARAM : GPU_OK;
}

int gpu_set_debug_mask(unsigned long mask) {
    lkpi_set_drm_debug(mask);
    return GPU_OK;
}

int gpu_driver_list(char *names, int rows, int row_len) {
    return lkpi_gpu_driver_list(names, rows, row_len);
}


static int kms_drop_master(int dev_idx);

int gpu_drm_open(int dev_idx, bool render) {
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (!(d->flags & GPU_DEV_FLAG_BOUND)) return GPU_ERR_NO_DRIVER;

    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return GPU_ERR_NODEV;

    uint32_t devt = render ? d->render_devt : d->primary_devt;
    if (!devt) return GPU_ERR_PARAM;

    void **slot = render ? &ctx->lx_render : &ctx->lx_card;
    if (*slot) return GPU_OK;

    if (lkpi_gpu_open_dev(devt, 2 , slot)) {
        *slot = NULL;
        return GPU_ERR_DRM;
    }
    if (!render) kms_drop_master(dev_idx);
    LOG_INFO("gpu core: kernel-side DRM client on %s (%s)%s", d->card, d->driver,
             render ? "" : ", master dropped (the console keeps its flusher)");
    return GPU_OK;
}

int gpu_drm_ioctl(int dev_idx, struct gpu_drm_ioctl_args *a) {
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (!a || !a->karg || !a->size) return GPU_ERR_PARAM;
    if (gpu_drm_open(dev_idx, false)) return GPU_ERR_DRM;

    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    a->ret = lkpi_gpu_ioctl(ctx->lx_card, a->cmd, a->karg);
    return a->ret ? GPU_ERR_DRM : GPU_OK;
}

static gpu_monitor gpu_mons[GPU_MON_MAX];

static int gpu_mon_count;

int gpu_drm_close(int dev_idx) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return GPU_ERR_NODEV;
    for (int i = 0; i < gpu_mon_count; i++) {
        gpu_monitor *m = &gpu_mons[i];
        if (m->dev_idx != dev_idx) continue;
        if (m->map_token) lkpi_gpu_gem_vunmap(m->map_token);
        m->map_token = NULL;
        m->pixels = NULL;
        m->map_size = 0;
        m->fb_id = 0;
        m->gem_handle = 0;
        m->enabled = 0;
        m->mode_current = -1;
        m->crtc_id = 0;
    }
    if (ctx->lx_card) lkpi_gpu_close_dev(ctx->lx_card);
    if (ctx->lx_render) lkpi_gpu_close_dev(ctx->lx_render);
    ctx->lx_card = NULL;
    ctx->lx_render = NULL;
    return GPU_OK;
}

static bool kms_dev_has_enabled(int dev_idx) {
    for (int i = 0; i < gpu_mon_count; i++)
        if (gpu_mons[i].dev_idx == dev_idx && gpu_mons[i].enabled) return true;
    return false;
}

static uint32_t kms_crtcs[GPU_DEVICE_MAX][8];

static int kms_crtc_count[GPU_DEVICE_MAX];

static long kms_ioctl(int dev_idx, uint32_t cmd, void *karg, uint32_t size) {
    long dummy = 0;
    gpu_drm_ioctl_args a;
    memset(&a, 0, sizeof(a));
    a.cmd = cmd;
    a.karg = karg ? karg : (void *)&dummy;
    a.size = karg ? size : (uint32_t)sizeof(dummy);
    gpu_drm_ioctl(dev_idx, &a);
    return a.ret;
}

static int kms_set_master(int dev_idx) {
    return (int)kms_ioctl(dev_idx, GPU_DRM_IO(0x1e), NULL, 0);
}

static int kms_drop_master(int dev_idx) {
    return (int)kms_ioctl(dev_idx, GPU_DRM_IO(0x1f), NULL, 0);
}

static const char *kms_conn_type_name(uint32_t type) {
    switch (type) {
        case 1: return "VGA";
        case 2: return "DVI-I";
        case 3: return "DVI-D";
        case 4: return "DVI-A";
        case 5: return "Composite";
        case 6: return "SVIDEO";
        case 7: return "LVDS";
        case 8: return "Component";
        case 9: return "9PinDIN";
        case 10: return "DP";
        case 11: return "HDMI-A";
        case 12: return "HDMI-B";
        case 13: return "TV";
        case 14: return "eDP";
        case 15: return "Virtual";
        case 16: return "DSI";
        case 17: return "DPI";
        case 18: return "Writeback";
        case 19: return "SPI";
        case 20: return "USB";
        default: return "UNK";
    }
}

static void kms_conn_name(const gpu_drm_get_connector_uapi *c, char *out, int cap) {
    snprintf(out, cap, "%s-%u", kms_conn_type_name(c->connector_type), c->connector_type_id);
}

static int kms_get_resources(int dev_idx, uint32_t *crtcs, int *n_crtcs,
                             uint32_t *conns, int *n_conns) {
    gpu_drm_card_res_uapi res;
    uint32_t encs[8];
    uint32_t fbs[4];
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    res.count_crtcs = 8;
    res.count_connectors = 8;
    res.count_encoders = 8;
    res.count_fbs = 4;
    long r = kms_ioctl(dev_idx, GPU_DRM_IOWR(0xA0, gpu_drm_card_res_uapi), &res, sizeof(res));
    if (r) return (int)r;
    *n_crtcs = res.count_crtcs < 8 ? res.count_crtcs : 8;
    *n_conns = res.count_connectors < 8 ? res.count_connectors : 8;
    return GPU_OK;
}

static int kms_get_connector(int dev_idx, uint32_t connector_id,
                             gpu_drm_get_connector_uapi *out,
                             gpu_drm_modeinfo_uapi *modes, int modes_cap, int *modes_got,
                             uint32_t *encoders, int enc_cap, int *enc_got) {
    gpu_drm_get_connector_uapi c;
    long r;
    memset(&c, 0, sizeof(c));
    c.connector_id = connector_id;
    r = kms_ioctl(dev_idx, GPU_DRM_IOC_GETCONNECTOR, &c, sizeof(c));
    if (r) return (int)r;
    uint32_t modes_actual = c.count_modes;
    uint32_t enc_actual = c.count_encoders;
    uint32_t modes_want = modes_actual > (uint32_t)modes_cap ? (uint32_t)modes_cap : modes_actual;
    uint32_t enc_pass = enc_actual > (uint32_t)enc_cap ? (uint32_t)enc_cap : enc_actual;
    memset(&c, 0, sizeof(c));
    c.connector_id = connector_id;
    c.modes_ptr = (uint64_t)(uintptr_t)modes;
    c.count_modes = modes_want;
    c.encoders_ptr = (uint64_t)(uintptr_t)encoders;
    c.count_encoders = enc_pass;
    r = kms_ioctl(dev_idx, GPU_DRM_IOC_GETCONNECTOR, &c, sizeof(c));
    if (r) return (int)r;
    *out = c;
    *modes_got = modes_actual <= (uint32_t)modes_cap ? (int)modes_actual : 0;
    *enc_got = enc_actual <= (uint32_t)enc_cap ? (int)enc_actual : 0;
    return GPU_OK;
}

static int kms_get_encoder(int dev_idx, uint32_t encoder_id, gpu_drm_get_encoder_uapi *out) {
    memset(out, 0, sizeof(*out));
    out->encoder_id = encoder_id;
    return (int)kms_ioctl(dev_idx, GPU_DRM_IOC_GETENCODER, out, sizeof(*out));
}

static bool kms_crtc_busy(int dev_idx, uint32_t crtc_id) {
    gpu_drm_crtc_uapi crtc;
    if (!crtc_id) return true;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtc_id;
    if (kms_ioctl(dev_idx, GPU_DRM_IOC_GETCRTC, &crtc, sizeof(crtc))) return true;
    return crtc.mode_valid != 0 || crtc.fb_id != 0;
}

static uint32_t kms_pick_crtc(int dev_idx, gpu_monitor *mon) {
    gpu_drm_get_encoder_uapi enc;
    if (mon->encoder_id && !kms_get_encoder(dev_idx, mon->encoder_id, &enc) && enc.crtc_id) {
        bool taken = false;
        for (int i = 0; i < gpu_mon_count; i++)
            if (i != (mon - gpu_mons) && gpu_mons[i].enabled && gpu_mons[i].crtc_id == enc.crtc_id)
                taken = true;
        if (!taken) return enc.crtc_id;
    }
    uint32_t best = 0;
    for (int e = 0; e < mon->encoder_count; e++) {
        if (kms_get_encoder(dev_idx, mon->encoders[e], &enc)) continue;
        for (int i = 0; i < kms_crtc_count[dev_idx]; i++) {
            uint32_t crtc_id = kms_crtcs[dev_idx][i];
            if (!(enc.possible_crtcs & (1u << i))) continue;
            bool taken = false;
            for (int m = 0; m < gpu_mon_count; m++)
                if (m != (mon - gpu_mons) && gpu_mons[m].enabled && gpu_mons[m].crtc_id == crtc_id)
                    taken = true;
            if (taken) continue;
            if (!best) best = crtc_id;
            if (!kms_crtc_busy(dev_idx, crtc_id)) return crtc_id;
        }
    }
    return best;
}

static struct {
    char name[GPU_MON_NAME];
    uint8_t brightness;
} gpu_mon_mem[GPU_MON_MAX];

static void gpu_mon_remember(const gpu_monitor *m) {
    int slot = -1, same_type = -1, same_hits = 0;
    const char *dash = strrchr(m->name, '-');
    size_t type_len = dash ? (size_t)(dash - m->name) + 1 : strlen(m->name);
    for (int i = 0; i < GPU_MON_MAX; i++) {
        if (strcmp(gpu_mon_mem[i].name, m->name) == 0) { slot = i; break; }
        if (gpu_mon_mem[i].name[0] && strncmp(gpu_mon_mem[i].name, m->name, type_len) == 0) {
            bool live = false;
            for (int k = 0; k < gpu_mon_count; k++)
                if (&gpu_mons[k] != m && strcmp(gpu_mons[k].name, gpu_mon_mem[i].name) == 0) live = true;
            if (!live) {
                same_type = i;
                same_hits++;
            }
        }
    }
    if (slot < 0 && same_hits == 1) slot = same_type;
    for (int i = 0; slot < 0 && i < GPU_MON_MAX; i++)
        if (!gpu_mon_mem[i].name[0]) slot = i;
    if (slot < 0) slot = 0;
    strncpy(gpu_mon_mem[slot].name, m->name, sizeof(gpu_mon_mem[slot].name) - 1);
    gpu_mon_mem[slot].brightness = m->brightness;
}

static int gpu_mon_recall(const char *name) {
    for (int i = 0; i < GPU_MON_MAX; i++)
        if (gpu_mon_mem[i].name[0] && strcmp(gpu_mon_mem[i].name, name) == 0) return gpu_mon_mem[i].brightness;
    const char *dash = strrchr(name, '-');
    size_t type_len = dash ? (size_t)(dash - name) + 1 : strlen(name);
    int found = -1, hits = 0;
    for (int i = 0; i < GPU_MON_MAX; i++) {
        if (!gpu_mon_mem[i].name[0] || strncmp(gpu_mon_mem[i].name, name, type_len) != 0) continue;
        found = gpu_mon_mem[i].brightness;
        hits++;
    }
    return hits == 1 ? found : -1;
}

static int kms_apply_light(int idx);

int gpu_monitor_probe(int dev_idx) {
    if (!gpu_stack_on) return GPU_ERR_NOT_STARTED;
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (!(d->flags & GPU_DEV_FLAG_BOUND)) return GPU_ERR_NO_DRIVER;
    if (!(d->flags & GPU_DEV_FLAG_MODESET)) return GPU_ERR_PARAM;
    uint32_t crtcs[8], conns[8];
    int n_crtcs = 0, n_conns = 0;
    int r = kms_get_resources(dev_idx, crtcs, &n_crtcs, conns, &n_conns);
    if (r) return r;
    for (int i = 0; i < 8; i++) kms_crtcs[dev_idx][i] = crtcs[i];
    kms_crtc_count[dev_idx] = n_crtcs;
    gpu_drm_modeinfo_uapi *modes = kmalloc(sizeof(gpu_drm_modeinfo_uapi) * 64);
    if (!modes) return GPU_ERR_NOMEM;
    for (int i = 0; i < gpu_mon_count; i++) {
        gpu_monitor *m = &gpu_mons[i];
        if (m->dev_idx != dev_idx) continue;
        bool still = false;
        for (int c = 0; c < n_conns; c++) if (conns[c] == m->connector_id) still = true;
        if (still) continue;
        if (m->enabled) gpu_monitor_disable(i);
        gpu_mon_remember(m);
        lkpi_console_hold_connector(d->primary_devt, m->connector_id, 0);
        if (i < gpu_mon_count - 1)
            memmove(&gpu_mons[i], &gpu_mons[i + 1], (size_t)(gpu_mon_count - i - 1) * sizeof(gpu_monitor));
        gpu_mon_count--;
        i--;
    }
    int added = 0;
    for (int c = 0; c < n_conns; c++) {
        int slot = -1;
        bool fresh = false;
        for (int i = 0; i < gpu_mon_count; i++)
            if (gpu_mons[i].dev_idx == dev_idx && gpu_mons[i].connector_id == conns[c]) slot = i;
        if (slot < 0) {
            if (gpu_mon_count >= GPU_MON_MAX) continue;
            fresh = true;
            slot = gpu_mon_count++;
            memset(&gpu_mons[slot], 0, sizeof(gpu_mons[slot]));
            gpu_mons[slot].dev_idx = dev_idx;
            gpu_mons[slot].connector_id = conns[c];
            gpu_mons[slot].mode_current = -1;
            gpu_mons[slot].brightness = 100;
        }
        gpu_drm_get_connector_uapi conn;
        memset(&conn, 0, sizeof(conn));
        int modes_got = 0, enc_got = 0;
        r = kms_get_connector(dev_idx, conns[c], &conn, modes, 64, &modes_got,
                              gpu_mons[slot].encoders, 8, &enc_got);
        if (r) continue;
        if (modes_got > GPU_MON_MODES) modes_got = GPU_MON_MODES;
        gpu_monitor *m = &gpu_mons[slot];
        m->connected = conn.connection == GPU_DRM_CONN_CONNECTED;
        m->encoder_id = conn.encoder_id;
        m->encoder_count = enc_got;
        m->mode_count = modes_got;
        memcpy(m->modes, modes, sizeof(gpu_drm_modeinfo_uapi) * (size_t)modes_got);
        kms_conn_name(&conn, m->name, sizeof(m->name));
        if (fresh) {
            int b = gpu_mon_recall(m->name);
            if (b >= 0 && b != 100) {
                m->brightness = (uint8_t)b;
                if (m->connected) kms_apply_light(slot);
            }
        }
        added++;
    }
    kfree(modes);
    LOG_INFO("gpu core: %d connector(s) read on %s", added, d->card);
    return GPU_OK;
}

static void gpu_mons_forget(int dev_idx, uint32_t old_devt) {
    for (int i = 0; i < gpu_mon_count; i++) {
        gpu_monitor *m = &gpu_mons[i];
        if (m->dev_idx != dev_idx) continue;
        if (m->map_token) lkpi_gpu_gem_vunmap(m->map_token);
        if (old_devt) lkpi_console_hold_connector(old_devt, m->connector_id, 0);
        if (m->name[0]) gpu_mon_remember(m);
        if (i < gpu_mon_count - 1)
            memmove(&gpu_mons[i], &gpu_mons[i + 1], (size_t)(gpu_mon_count - i - 1) * sizeof(gpu_monitor));
        gpu_mon_count--;
        i--;
    }
    if (dev_idx >= 0 && dev_idx < GPU_DEVICE_MAX) kms_crtc_count[dev_idx] = 0;
}

int gpu_monitor_count(void) { return gpu_mon_count; }
const gpu_monitor *gpu_monitor_get(int idx) {
    if (idx < 0 || idx >= gpu_mon_count) return NULL;
    return &gpu_mons[idx];
}

static uint32_t kms_dim_channel(uint32_t v, uint32_t k) {
    return (v * k) >> 8;
}

static uint32_t kms_dim_px(uint32_t px, uint32_t k) {
    if (k >= 256) return px;
    uint32_t r = kms_dim_channel((px >> 16) & 0xff, k);
    uint32_t g = kms_dim_channel((px >> 8) & 0xff, k);
    uint32_t b = kms_dim_channel(px & 0xff, k);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint32_t kms_dim_k(uint32_t percent) {
    if (percent > 100) percent = 100;
    return (percent * 256 + 50) / 100;
}

#define GPU_DRM_ERR_BUSY (-16)

enum { GPU_LIGHT_NONE = 0, GPU_LIGHT_BACKLIGHT, GPU_LIGHT_GAMMA, GPU_LIGHT_SOFT };

static void kms_draw_text(uint32_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                          uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t scale) {
    uint32_t cx = 0;
    for (const char *p = s; *p; p++, cx++) {
        uint8_t idx = (uint8_t)(*p) & 0x7f;
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint8_t bits = gop_font_bitmap[idx][row];
            for (uint32_t col = 0; col < FONT_WIDTH; col++) {
                if (!((bits >> (7 - (col & 7))) & 1)) continue;
                for (uint32_t sy = 0; sy < scale; sy++) {
                    uint32_t py = y + row * scale + sy;
                    if (py >= h) break;
                    uint32_t *line = (uint32_t *)((uint8_t *)px + (size_t)py * pitch);
                    for (uint32_t sx = 0; sx < scale; sx++) {
                        uint32_t pxx = x + (cx * FONT_WIDTH + col) * scale + sx;
                        if (pxx >= w) break;
                        line[pxx] = fg;
                    }
                }
            }
        }
    }
}

static uint32_t kms_text_w(const char *s, uint32_t scale) {
    uint32_t n = 0;
    for (const char *p = s; *p; p++) n++;
    return n * FONT_WIDTH * scale;
}

static uint32_t kms_grad(uint32_t c0, uint32_t c1, uint32_t t) {
    uint32_t r = (((c0 >> 16) & 0xff) * (256 - t) + ((c1 >> 16) & 0xff) * t) >> 8;
    uint32_t g = (((c0 >> 8) & 0xff) * (256 - t) + ((c1 >> 8) & 0xff) * t) >> 8;
    uint32_t b = ((c0 & 0xff) * (256 - t) + (c1 & 0xff) * t) >> 8;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void kms_mode_screen(uint32_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                            const char *conn, const gpu_drm_modeinfo_uapi *mode, uint32_t dim_k) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t t = h > 1 ? (y * 256) / (h - 1) : 0;
        uint32_t bg = kms_grad(0x0d1a33u, 0x05070fu, t);
        uint32_t *row = (uint32_t *)((uint8_t *)px + (size_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) row[x] = kms_dim_px(bg, dim_k);
    }
    if (w < 64 || h < 64) return;
    uint32_t frame = kms_dim_px(0xff3d5a92u, dim_k);
    for (uint32_t y = 10; y < h - 10; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)px + (size_t)y * pitch);
        row[10] = frame;
        row[11] = frame;
        row[w - 11] = frame;
        row[w - 12] = frame;
    }
    for (uint32_t x = 10; x < w - 10; x++) {
        uint32_t *top = (uint32_t *)((uint8_t *)px + (size_t)10 * pitch);
        uint32_t *top2 = (uint32_t *)((uint8_t *)px + (size_t)11 * pitch);
        uint32_t *bot = (uint32_t *)((uint8_t *)px + (size_t)(h - 11) * pitch);
        uint32_t *bot2 = (uint32_t *)((uint8_t *)px + (size_t)(h - 12) * pitch);
        top[x] = frame;
        top2[x] = frame;
        bot[x] = frame;
        bot2[x] = frame;
    }
    char line[96];
    uint32_t cy = h > 400 ? h / 4 : h / 6;
    uint32_t white = kms_dim_px(0xffe8eefcu, dim_k);
    uint32_t blue = kms_dim_px(0xff7ea6ffu, dim_k);
    uint32_t green = kms_dim_px(0xff54d47cu, dim_k);
    uint32_t gray = kms_dim_px(0xff9aa3b5u, dim_k);
    kms_draw_text(px, pitch, w, h, (w - kms_text_w("LuOS", 3)) / 2, cy, "LuOS", white, 3);
    cy += FONT_HEIGHT * 3 + 18;
    kms_draw_text(px, pitch, w, h, (w - kms_text_w(conn, 1)) / 2, cy, conn, gray, 1);
    cy += FONT_HEIGHT + 14;
    snprintf(line, sizeof(line), "%u x %u @ %u Hz", mode->hdisplay, mode->vdisplay,
             mode->vrefresh ? mode->vrefresh : 60);
    kms_draw_text(px, pitch, w, h, (w - kms_text_w(line, 2)) / 2, cy, line, blue, 2);
    cy += FONT_HEIGHT * 2 + 16;
    kms_draw_text(px, pitch, w, h, (w - kms_text_w("mode set: OK", 1)) / 2, cy,
                  "mode set: OK", green, 1);
    cy += FONT_HEIGHT + 12;
    kms_draw_text(px, pitch, w, h,
                  (w - kms_text_w("gpu-mon-off N powers off / gpu-mon-bright N 0-100", 1)) / 2, cy,
                  "gpu-mon-off N powers off / gpu-mon-bright N 0-100", gray, 1);
}

typedef struct kms_fb {
    uint32_t fb_id;
    uint32_t gem_handle;
    uint32_t pitch;
    void *map_token;
    uint32_t *pixels;
    uint64_t map_size;
} kms_fb;

static int kms_fb_create(int dev_idx, uint32_t w, uint32_t h, kms_fb *out) {
    gpu_drm_create_dumb_uapi dumb;
    gpu_drm_fb_cmd_uapi fb;
    unsigned long kaddr = 0;
    unsigned long long size = 0;
    void *token = NULL;
    gpu_devctx *ctx;
    memset(out, 0, sizeof(*out));
    if (gpu_drm_open(dev_idx, false)) return GPU_ERR_DRM;
    ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return GPU_ERR_NODEV;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = w;
    dumb.height = h;
    dumb.bpp = 32;
    long r = kms_ioctl(dev_idx, GPU_DRM_IOC_CREATE_DUMB, &dumb, sizeof(dumb));
    if (r) return (int)r;
    memset(&fb, 0, sizeof(fb));
    fb.width = w;
    fb.height = h;
    fb.pitch = dumb.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = dumb.handle;
    r = kms_ioctl(dev_idx, GPU_DRM_IOC_ADDFB, &fb, sizeof(fb));
    if (r) {
        gpu_drm_destroy_dumb_uapi dstr = { .handle = dumb.handle };
        kms_ioctl(dev_idx, GPU_DRM_IOC_DESTROY_DUMB, &dstr, sizeof(dstr));
        return (int)r;
    }
    if (lkpi_gpu_gem_vmap(ctx->lx_card, dumb.handle, &token, &kaddr, &size)) {
        uint32_t fb_id = fb.fb_id;
        kms_ioctl(dev_idx, GPU_DRM_IOC_RMFB, &fb_id, sizeof(fb_id));
        gpu_drm_destroy_dumb_uapi dstr = { .handle = dumb.handle };
        kms_ioctl(dev_idx, GPU_DRM_IOC_DESTROY_DUMB, &dstr, sizeof(dstr));
        return GPU_ERR_DRM;
    }
    out->fb_id = fb.fb_id;
    out->gem_handle = dumb.handle;
    out->pitch = dumb.pitch;
    out->map_token = token;
    out->pixels = (uint32_t *)kaddr;
    out->map_size = size;
    return GPU_OK;
}

static void kms_fb_destroy(int dev_idx, kms_fb *fb) {
    if (fb->map_token) {
        lkpi_gpu_gem_vunmap(fb->map_token);
        fb->map_token = NULL;
        fb->pixels = NULL;
    }
    if (fb->fb_id) {
        uint32_t fb_id = fb->fb_id;
        kms_ioctl(dev_idx, GPU_DRM_IOC_RMFB, &fb_id, sizeof(fb_id));
        fb->fb_id = 0;
    }
    if (fb->gem_handle) {
        gpu_drm_destroy_dumb_uapi dstr = { .handle = fb->gem_handle };
        kms_ioctl(dev_idx, GPU_DRM_IOC_DESTROY_DUMB, &dstr, sizeof(dstr));
        fb->gem_handle = 0;
    }
}

static void kms_monitor_fb_get(const gpu_monitor *m, kms_fb *fb) {
    memset(fb, 0, sizeof(*fb));
    fb->fb_id = m->fb_id;
    fb->gem_handle = m->gem_handle;
    fb->pitch = m->pitch;
    fb->map_token = m->map_token;
    fb->pixels = m->pixels;
    fb->map_size = m->map_size;
}

static void kms_monitor_fb_put(gpu_monitor *m, const kms_fb *fb) {
    m->fb_id = fb->fb_id;
    m->gem_handle = fb->gem_handle;
    m->pitch = fb->pitch;
    m->map_token = fb->map_token;
    m->pixels = fb->pixels;
    m->map_size = fb->map_size;
}

int gpu_monitor_enable(int idx, int mode_idx) {
    if (idx < 0 || idx >= gpu_mon_count) return GPU_ERR_PARAM;
    gpu_monitor *m = &gpu_mons[idx];
    if (!m->connected) return GPU_ERR_STATE;
    if (mode_idx == -1) {
        mode_idx = 0;
        for (int i = 0; i < m->mode_count; i++)
            if (m->modes[i].type & GPU_DRM_MODE_TYPE_PREFERRED) { mode_idx = i; break; }
    }
    if (mode_idx < 0 || mode_idx >= m->mode_count) return GPU_ERR_PARAM;
    int dev = m->dev_idx;
    if (m->enabled) {
        if (m->mode_current == mode_idx) return GPU_OK;
        gpu_monitor_disable(idx);
        m = &gpu_mons[idx];
    }
    kms_set_master(dev);
    uint32_t crtc_id = kms_pick_crtc(dev, m);
    if (!crtc_id) return GPU_ERR_BUSY;
    gpu_drm_modeinfo_uapi *mode = &m->modes[mode_idx];
    kms_fb fb;
    int r = kms_fb_create(dev, mode->hdisplay, mode->vdisplay, &fb);
    if (r) return r;
    gpu_drm_crtc_uapi set;
    memset(&set, 0, sizeof(set));
    set.crtc_id = crtc_id;
    set.fb_id = fb.fb_id;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&m->connector_id;
    set.count_connectors = 1;
    set.mode_valid = 1;
    set.mode = *mode;
    r = (int)kms_ioctl(dev, GPU_DRM_IOC_SETCRTC, &set, sizeof(set));
    if (r) {
        kms_fb_destroy(dev, &fb);
        return r;
    }
    kms_monitor_fb_put(m, &fb);
    m->crtc_id = crtc_id;
    m->mode_current = mode_idx;
    bool origin_taken = false;
    for (int i = 0; i < gpu_mon_count; i++)
        if (i != idx && gpu_mons[i].enabled && !gpu_mons[i].cloned && gpu_mons[i].x == 0 && gpu_mons[i].y == 0)
            origin_taken = true;
    if (m->x == 0 && m->y == 0 && origin_taken) m->x = gpu_monitor_desktop_width();
    m->enabled = 1;
    if (m->pixels) kms_mode_screen(m->pixels, mode->hdisplay, mode->vdisplay, m->pitch,
                                   m->name, mode, m->light_kind == GPU_LIGHT_SOFT ? kms_dim_k(m->brightness) : 256);
    lkpi_console_hold_connector(gpu_devices[dev].primary_devt, m->connector_id, 1);
    kms_drop_master(dev);
    if (m->brightness != 100 || m->light_kind == GPU_LIGHT_GAMMA) kms_apply_light(idx);
    LOG_INFO("gpu core: %s on %s: %ux%u@%u", m->name, gpu_devices[dev].card,
             mode->hdisplay, mode->vdisplay, mode->vrefresh);
    return GPU_OK;
}

int gpu_monitor_disable(int idx) {
    if (idx < 0 || idx >= gpu_mon_count) return GPU_ERR_PARAM;
    gpu_monitor *m = &gpu_mons[idx];
    int dev = m->dev_idx;
    if (!m->enabled) return GPU_OK;
    if (m->crtc_id) {
        gpu_drm_crtc_uapi set;
        memset(&set, 0, sizeof(set));
        set.crtc_id = m->crtc_id;
        set.mode_valid = 0;
        set.fb_id = 0;
        kms_set_master(dev);
        kms_ioctl(dev, GPU_DRM_IOC_SETCRTC, &set, sizeof(set));
    }
    kms_fb fb;
    kms_monitor_fb_get(m, &fb);
    kms_fb_destroy(dev, &fb);
    kms_monitor_fb_put(m, &fb);
    m->enabled = 0;
    m->mode_current = -1;
    m->cloned = 0;
    m->crtc_id = 0;
    lkpi_console_hold_connector(gpu_devices[dev].primary_devt, m->connector_id, 1);
    kms_drop_master(dev);
    if (!kms_dev_has_enabled(dev)) gpu_drm_close(dev);
    return GPU_OK;
}

int gpu_monitor_set_mode(int idx, int mode_idx) {
    if (idx < 0 || idx >= gpu_mon_count) return GPU_ERR_PARAM;
    gpu_monitor *m = &gpu_mons[idx];
    if (mode_idx < 0 || mode_idx >= m->mode_count) return GPU_ERR_PARAM;
    uint32_t x = m->x, y = m->y;
    int r = gpu_monitor_enable(idx, mode_idx);
    if (!r) { m->x = x; m->y = y; }
    return r;
}

int gpu_monitor_find_mode(int idx, uint32_t width, uint32_t height, uint32_t vrefresh) {
    if (idx < 0 || idx >= gpu_mon_count) return GPU_ERR_PARAM;
    gpu_monitor *m = &gpu_mons[idx];
    if (!width || !height) return GPU_ERR_PARAM;
    int best = -1;
    int best_score = -1;
    for (int i = 0; i < m->mode_count; i++) {
        if (m->modes[i].hdisplay != width || m->modes[i].vdisplay != height) continue;
        int score = 1000;
        if (vrefresh) {
            int diff = (int)m->modes[i].vrefresh - (int)vrefresh;
            if (diff < 0) diff = -diff;
            score = 1000 - diff * 10;
            if (m->modes[i].vrefresh == vrefresh) score = 10000;
        } else if (m->modes[i].type & GPU_DRM_MODE_TYPE_PREFERRED) {
            score = 5000;
        }
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

int gpu_monitor_set_mode_wh(int idx, uint32_t width, uint32_t height, uint32_t vrefresh) {
    int mode_idx = gpu_monitor_find_mode(idx, width, height, vrefresh);
    if (mode_idx < 0) return GPU_ERR_PARAM;
    return gpu_monitor_set_mode(idx, mode_idx);
}

static bool kms_conn_internal(const gpu_monitor *m) {
    return strncmp(m->name, "eDP", 3) == 0 || strncmp(m->name, "LVDS", 4) == 0 || strncmp(m->name, "DSI", 3) == 0;
}

static uint32_t kms_monitor_crtc(int dev, const gpu_monitor *m) {
    if (m->enabled && m->crtc_id) return m->crtc_id;
    gpu_drm_modeinfo_uapi one;
    gpu_drm_get_connector_uapi c;
    memset(&c, 0, sizeof(c));
    c.connector_id = m->connector_id;
    c.modes_ptr = (uint64_t)(uintptr_t)&one;
    c.count_modes = 1;
    if (kms_ioctl(dev, GPU_DRM_IOC_GETCONNECTOR, &c, sizeof(c)) || !c.encoder_id) return 0;
    gpu_drm_get_encoder_uapi enc;
    if (kms_get_encoder(dev, c.encoder_id, &enc)) return 0;
    return enc.crtc_id;
}

static int kms_set_gamma(int dev, uint32_t crtc_id, uint32_t percent) {
    gpu_drm_crtc_uapi c;
    memset(&c, 0, sizeof(c));
    c.crtc_id = crtc_id;
    if (kms_ioctl(dev, GPU_DRM_IOC_GETCRTC, &c, sizeof(c))) return GPU_ERR_DRM;
    uint32_t n = c.gamma_size;
    if (n < 2 || n > 4096) return GPU_ERR_STATE;
    uint16_t *lut = kmalloc((size_t)n * 3 * sizeof(uint16_t));
    if (!lut) return GPU_ERR_NOMEM;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(((uint64_t)i * 65535u * percent) / ((uint64_t)(n - 1) * 100u));
        lut[i] = lut[n + i] = lut[2 * n + i] = (uint16_t)v;
    }
    gpu_drm_crtc_lut_uapi lu;
    memset(&lu, 0, sizeof(lu));
    lu.crtc_id = crtc_id;
    lu.gamma_size = n;
    lu.red = (uint64_t)(uintptr_t)lut;
    lu.green = (uint64_t)(uintptr_t)(lut + n);
    lu.blue = (uint64_t)(uintptr_t)(lut + 2 * n);
    kms_set_master(dev);
    long r = kms_ioctl(dev, GPU_DRM_IOC_SETGAMMA, &lu, sizeof(lu));
    kms_drop_master(dev);
    kfree(lut);
    return r ? GPU_ERR_DRM : GPU_OK;
}

static int kms_backlight_percent(uint32_t percent) {
    if (lkpi_backlight_count() <= 0) return GPU_ERR_NODEV;
    uint32_t cur = 0, max = 0;
    if (lkpi_backlight_get(0, NULL, 0, &cur, &max) || !max) return GPU_ERR_DRM;
    uint32_t v = (uint32_t)(((uint64_t)max * percent * percent + 5000) / 10000);
    if (percent && !v) v = 1;
    return lkpi_backlight_set(0, v) ? GPU_ERR_DRM : GPU_OK;
}

static const char *kms_light_name(int kind) {
    switch (kind) {
        case GPU_LIGHT_BACKLIGHT: return "panel backlight";
        case GPU_LIGHT_GAMMA: return "CRTC gamma";
        default: return "software dim of the LuOS test screen";
    }
}

static int kms_apply_light(int idx) {
    gpu_monitor *m = &gpu_mons[idx];
    int dev = m->dev_idx;
    int kind = GPU_LIGHT_SOFT;
    if (kms_conn_internal(m) && kms_backlight_percent(m->brightness) == GPU_OK) {
        kind = GPU_LIGHT_BACKLIGHT;
    } else {
        uint32_t crtc = kms_monitor_crtc(dev, m);
        if (crtc && kms_set_gamma(dev, crtc, m->brightness) == GPU_OK) kind = GPU_LIGHT_GAMMA;
    }
    m->light_kind = (uint8_t)kind;
    if (m->enabled && m->pixels && m->mode_current >= 0 && m->pitch) {
        gpu_drm_modeinfo_uapi *mode = &m->modes[m->mode_current];
        kms_mode_screen(m->pixels, mode->hdisplay, mode->vdisplay, m->pitch, m->name, mode,
                        kind == GPU_LIGHT_SOFT ? kms_dim_k(m->brightness) : 256);
    }
    return kind;
}

int gpu_monitor_set_brightness(int idx, uint32_t percent) {
    if (idx < 0 || idx >= gpu_mon_count) return GPU_ERR_PARAM;
    if (percent > 100) percent = 100;
    gpu_monitor *m = &gpu_mons[idx];
    m->brightness = (uint8_t)percent;
    int kind = kms_apply_light(idx);
    gpu_mon_remember(m);
    LOG_INFO("gpu core: %s brightness %u%% (%s)", m->name, percent, kms_light_name(kind));
    return GPU_OK;
}

const char *gpu_monitor_light_name(int idx) {
    if (idx < 0 || idx >= gpu_mon_count) return "";
    return kms_light_name(gpu_mons[idx].light_kind);
}

int gpu_backlight_count(void) {
    return lkpi_backlight_count();
}

int gpu_backlight_get(int idx, char *name, int name_cap, uint32_t *cur, uint32_t *max) {
    return lkpi_backlight_get(idx, name, name_cap, cur, max);
}

int gpu_backlight_set(int idx, uint32_t value) {
    return lkpi_backlight_set(idx, value);
}

int gpu_monitor_move(int idx, uint32_t x, uint32_t y) {
    if (idx < 0 || idx >= gpu_mon_count) return GPU_ERR_PARAM;
    gpu_mons[idx].x = x;
    gpu_mons[idx].y = y;
    return GPU_OK;
}

uint32_t gpu_monitor_desktop_width(void) {
    uint32_t w = 0;
    for (int i = 0; i < gpu_mon_count; i++) {
        if (!gpu_mons[i].enabled || gpu_mons[i].mode_current < 0) continue;
        uint32_t rw = gpu_mons[i].x + gpu_mons[i].modes[gpu_mons[i].mode_current].hdisplay;
        if (rw > w) w = rw;
    }
    return w;
}

uint32_t gpu_monitor_desktop_height(void) {
    uint32_t h = 0;
    for (int i = 0; i < gpu_mon_count; i++) {
        if (!gpu_mons[i].enabled || gpu_mons[i].mode_current < 0) continue;
        uint32_t rh = gpu_mons[i].y + gpu_mons[i].modes[gpu_mons[i].mode_current].vdisplay;
        if (rh > h) h = rh;
    }
    return h;
}

int gpu_monitor_layout_auto(void) {
    uint32_t x = 0;
    int laid = 0;
    for (int i = 0; i < gpu_mon_count; i++) {
        if (!gpu_mons[i].enabled || gpu_mons[i].mode_current < 0) continue;
        gpu_mons[i].x = x;
        gpu_mons[i].y = 0;
        x += gpu_mons[i].modes[gpu_mons[i].mode_current].hdisplay;
        laid++;
    }
    return laid ? GPU_OK : GPU_ERR_STATE;
}

int gpu_monitor_clone(int dst_idx, int src_idx) {
    if (dst_idx < 0 || dst_idx >= gpu_mon_count || src_idx < 0 || src_idx >= gpu_mon_count)
        return GPU_ERR_PARAM;
    gpu_monitor *dst = &gpu_mons[dst_idx];
    gpu_monitor *src = &gpu_mons[src_idx];
    if (dst_idx == src_idx) return GPU_ERR_PARAM;
    if (!dst->connected || !src->connected) return GPU_ERR_STATE;
    int want = 0;
    if (src->enabled && src->mode_current >= 0) {
        uint32_t w = src->modes[src->mode_current].hdisplay;
        uint32_t h = src->modes[src->mode_current].vdisplay;
        int best = -1;
        for (int i = 0; i < dst->mode_count; i++) {
            if (dst->modes[i].hdisplay != w || dst->modes[i].vdisplay != h) continue;
            if (best < 0 || (dst->modes[i].type & GPU_DRM_MODE_TYPE_PREFERRED)) best = i;
        }
        if (best >= 0) want = best;
    }
    int r = gpu_monitor_enable(dst_idx, want);
    if (r) return r;
    dst->cloned = (uint8_t)(src_idx + 1);
    dst->x = src->x;
    dst->y = src->y;
    if (dst->pixels && src->enabled && src->pixels && dst->map_size) {
        uint32_t rows = dst->modes[dst->mode_current].vdisplay;
        uint32_t dim_k = kms_dim_k(dst->brightness);
        for (uint32_t yy = 0; yy < rows; yy++) {
            uint32_t *drow = (uint32_t *)((uint8_t *)dst->pixels + (size_t)yy * dst->pitch);
            const uint32_t *srow = (const uint32_t *)((const uint8_t *)src->pixels + (size_t)yy * src->pitch);
            uint32_t cols = dst->pitch < src->pitch ? dst->pitch : src->pitch;
            cols /= 4;
            for (uint32_t xx = 0; xx < cols; xx++) drow[xx] = kms_dim_px(srow[xx], dim_k);
        }
    }
    return GPU_OK;
}

static bool cube_console_driver_only(int idx) {
    const struct gpu_device *d = core_dev(idx);
    if (!d) return false;
    return strcmp(d->driver, "simpledrm") == 0;
}

static const gpu_drm_modeinfo_uapi *kms_mode_fit(gpu_monitor *m, uint32_t max_w, uint32_t max_h) {
    const gpu_drm_modeinfo_uapi *best = NULL;
    const gpu_drm_modeinfo_uapi *small = NULL;
    for (int i = 0; i < m->mode_count; i++) {
        const gpu_drm_modeinfo_uapi *c = &m->modes[i];
        uint64_t px = (uint64_t)c->hdisplay * (uint64_t)c->vdisplay;
        uint64_t spx = small ? (uint64_t)small->hdisplay * (uint64_t)small->vdisplay : 0;
        if (!small || px < spx) small = c;
        if (c->hdisplay > max_w || c->vdisplay > max_h) continue;
        uint64_t bpx = best ? (uint64_t)best->hdisplay * (uint64_t)best->vdisplay : 0;
        if (!best || px > bpx ||
            (px == bpx && (c->type & GPU_DRM_MODE_TYPE_PREFERRED))) best = c;
    }
    return best ? best : small;
}

int gpu_present_open(gpu_present *p, const gpu_cube_opts *opts, uint32_t want_w, uint32_t want_h) {
    memset(p, 0, sizeof(*p));
    gpu_core_refresh();
    int dev = opts->dev_idx;
    if (dev < 0) {
        for (int i = 0; i < gpu_get_device_count(); i++) {
            struct gpu_device *d = gpu_get_device(i);
            if (d && (d->flags & GPU_DEV_FLAG_BOUND) && (d->flags & GPU_DEV_FLAG_MODESET) &&
                !cube_console_driver_only(i)) {
                dev = i;
                break;
            }
        }
    }
    if (dev < 0) {
        uint8_t *fbp;
        uint32_t w, h, pitch;
        if (!limine_video_fb_info(&fbp, &w, &h, &pitch) || w < 64 || h < 64) return GPU_ERR_NOT_STARTED;
        p->kms = false;
        p->width = want_w && want_w < w ? want_w : w;
        p->height = want_h && want_h < h ? want_h : h;
        if (p->width > 1920) p->width = 1920;
        if (p->height > 1080) p->height = 1080;
        p->pitch = pitch;
        p->pixels = (uint32_t *)fbp;
        p->snapshot = kmalloc((size_t)h * pitch);
        if (p->snapshot) memcpy(p->snapshot, fbp, (size_t)h * pitch);
        return GPU_OK;
    }

    p->kms = true;
    p->dev_idx = dev;
    struct gpu_device *d = gpu_get_device(dev);
    if (!d || gpu_drm_open(dev, false)) return GPU_ERR_NODEV;
    if (gpu_monitor_probe(dev)) return GPU_ERR_DRM;
    int mon = opts->connector;
    if (mon < 0) {
        for (int i = 0; i < gpu_monitor_count(); i++) {
            const gpu_monitor *m = gpu_monitor_get(i);
            if (m && m->connected && m->mode_count) { mon = i; break; }
        }
    }
    if (mon < 0 || mon >= gpu_monitor_count()) return GPU_ERR_NODEV;
    p->mon_idx = mon;
    gpu_monitor *m = &gpu_mons[mon];
    if (!m->mode_count) return GPU_ERR_NODEV;
    bool keep = m->enabled && m->mode_current >= 0 && m->fb_id && m->crtc_id;
    const gpu_drm_modeinfo_uapi *mode = keep ? &m->modes[m->mode_current] : &m->modes[0];
    bool asked = false;
    if (opts->width && opts->height) {
        int mi = gpu_monitor_find_mode(mon, opts->width, opts->height, 0);
        if (mi >= 0) {
            mode = &m->modes[mi];
            asked = true;
        }
    }
    if (!keep && !asked && (mode->hdisplay > 1920 || mode->vdisplay > 1080)) {
        const gpu_drm_modeinfo_uapi *fit = kms_mode_fit(m, 1920, 1080);
        if (fit && (fit->hdisplay < mode->hdisplay || fit->vdisplay < mode->vdisplay)) mode = fit;
    }
    p->restore_mon = keep;
    uint32_t w = want_w ? want_w : mode->hdisplay;
    uint32_t h = want_h ? want_h : mode->vdisplay;
    if (w > mode->hdisplay) w = mode->hdisplay;
    if (h > mode->vdisplay) h = mode->vdisplay;
    if (w > 1920) w = 1920;
    if (h > 1080) h = 1080;
    p->width = w;
    p->height = h;

    long sm = kms_set_master(dev);
    if (sm) LOG_WARNING("gpu core: SET_MASTER on %s failed (%ld), present may be rejected", d->card, sm);
    kms_fb fbs[2];
    int fr = kms_fb_create(dev, mode->hdisplay, mode->vdisplay, &fbs[0]);
    if (!fr) {
        fr = kms_fb_create(dev, mode->hdisplay, mode->vdisplay, &fbs[1]);
        if (fr) kms_fb_destroy(dev, &fbs[0]);
    }
    if (fr) {
        LOG_WARNING("gpu core: dumb/ADD_FB %ux%u on %s failed (%d)", mode->hdisplay, mode->vdisplay,
                    d->card, fr);
        return GPU_ERR_DRM;
    }
    p->fb_id[0] = fbs[0].fb_id;
    p->fb_id[1] = fbs[1].fb_id;
    p->gem_handle[0] = fbs[0].gem_handle;
    p->gem_handle[1] = fbs[1].gem_handle;
    p->map_token[0] = fbs[0].map_token;
    p->map_token[1] = fbs[1].map_token;
    p->map_size[0] = fbs[0].map_size;
    p->map_size[1] = fbs[1].map_size;
    p->pitch = fbs[0].pitch;
    p->fb_w = mode->hdisplay;
    p->fb_h = mode->vdisplay;
    uint32_t off_x = (mode->hdisplay - w) / 2, off_y = (mode->vdisplay - h) / 2;
    for (int i = 0; i < 2; i++) {
        if (off_x || off_y)
            for (uint32_t y = 0; y < mode->vdisplay; y++)
                memset((uint8_t *)fbs[i].pixels + (size_t)y * fbs[i].pitch, 0, (size_t)mode->hdisplay * 4);
        p->pix[i] = (uint32_t *)((uint8_t *)fbs[i].pixels + (size_t)off_y * fbs[i].pitch) + off_x;
    }
    p->pixels = p->pix[0];
    p->cur = 0;

    uint32_t crtc = m->crtc_id;
    if (!crtc) crtc = kms_pick_crtc(dev, m);
    if (!crtc) {
        LOG_WARNING("gpu core: no CRTC on %s for connector %s", d->card, m->name);
        kms_fb_destroy(dev, &fbs[1]);
        kms_fb_destroy(dev, &fbs[0]);
        return GPU_ERR_DRM;
    }
    p->crtc_id = crtc;
    p->connector_id = m->connector_id;

    gpu_drm_crtc_uapi set;
    memset(&set, 0, sizeof(set));
    set.crtc_id = crtc;
    set.fb_id = p->fb_id[0];
    set.set_connectors_ptr = (uint64_t)&p->connector_id;
    set.count_connectors = 1;
    set.mode_valid = 1;
    set.mode = *mode;
    long sr = kms_ioctl(dev, GPU_DRM_IOC_SETCRTC, &set, sizeof(set));
    if (sr) {
        LOG_WARNING("gpu core: SETCRTC %ux%u on %s crtc %u failed (%ld)",
                    mode->hdisplay, mode->vdisplay, d->card, crtc, sr);
        kms_fb_destroy(dev, &fbs[1]);
        kms_fb_destroy(dev, &fbs[0]);
        return GPU_ERR_DRM;
    }

    gpu_drm_page_flip_uapi flip;
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = crtc;
    flip.fb_id = p->fb_id[1];
    p->can_flip = kms_ioctl(dev, GPU_DRM_IOC_PAGE_FLIP, &flip, sizeof(flip)) == 0;
    if (p->can_flip) p->cur = 1;
    p->period_ms = 16;
    if (mode->vrefresh >= 30 && mode->vrefresh <= 240) p->period_ms = 1000 / mode->vrefresh;
    lkpi_console_hold_connector(gpu_devices[dev].primary_devt, m->connector_id, 1);
    LOG_INFO("present: KMS on %s crtc %u, mode %ux%u@%u, render %ux%u, %s",
             d->card, crtc, mode->hdisplay, mode->vdisplay, mode->vrefresh, p->width, p->height,
             p->can_flip ? "page flips" : "dirtyfb");
    return GPU_OK;
}

static void kms_blit_frame_row(uint32_t *dst, const uint32_t *src, uint32_t px, bool bgra) {
    if (bgra) {
        memcpy(dst, src, (size_t)px * 4);
        return;
    }
    for (uint32_t i = 0; i < px; i++) {
        uint32_t c = src[i];
        dst[i] = (c & 0xFF000000u) | ((c & 0x000000FFu) << 16) | (c & 0x0000FF00u) | ((c & 0x00FF0000u) >> 16);
    }
}

int gpu_present_frame(gpu_present *p, const uint32_t *rgba, uint32_t w, uint32_t h) {
    if (!p->kms) {
        uint32_t *dst = p->pixels;
        for (uint32_t y = 0; y < h; y++)
            kms_blit_frame_row(dst + (size_t)y * (p->pitch / 4), rgba + (size_t)y * w, w, p->src_bgra);
        limine_video_announce_damage();
        return GPU_OK;
    }
    if (p->can_flip) {
        uint32_t back = p->cur ^ 1;
        uint32_t *bdst = p->pix[back];
        for (uint32_t y = 0; y < h; y++)
            kms_blit_frame_row(bdst + (size_t)y * (p->pitch / 4), rgba + (size_t)y * w, w, p->src_bgra);
        uint32_t busy = 0;
        gpu_drm_page_flip_uapi flip;
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = p->crtc_id;
        flip.fb_id = p->fb_id[back];
        for (;;) {
            long r = kms_ioctl(p->dev_idx, GPU_DRM_IOC_PAGE_FLIP, &flip, sizeof(flip));
            if (r == 0) {
                p->cur = back;
                return GPU_OK;
            }
            if (r == -16 && busy < 1500) {
                busy++;
                scheduler_sleep_ms(2);
                continue;
            }
            LOG_WARNING("present: page flip failed (%ld) - falling back to dirtyfb", r);
            p->can_flip = false;
            break;
        }
    }
    uint32_t *dst = p->pix[p->cur];
    for (uint32_t y = 0; y < h; y++)
        kms_blit_frame_row(dst + (size_t)y * (p->pitch / 4), rgba + (size_t)y * w, w, p->src_bgra);
    {
        gpu_drm_fb_dirty_uapi dirty;
        memset(&dirty, 0, sizeof(dirty));
        dirty.fb_id = p->fb_id[p->cur];
        kms_ioctl(p->dev_idx, GPU_DRM_IOC_DIRTYFB, &dirty, sizeof(dirty));
        scheduler_sleep_ms((uint64_t)(p->period_ms > 0 ? p->period_ms : 16));
    }
    return GPU_OK;
}

int gpu_present_zc_add(gpu_present *p, void *host_file, uint32_t host_gem, uint32_t pitch) {
    if (!p->kms || p->zc_count >= GPU_ZC_MAX || !host_file || !pitch) return GPU_ERR_PARAM;
    gpu_devctx *ctx = gpu_dev_ctx(p->dev_idx);
    if (!ctx || !ctx->lx_card) return GPU_ERR_NODEV;
    unsigned int gem = 0;
    long r = lkpi_gpu_gem_share(ctx->lx_card, host_file, host_gem, &gem);
    if (r) {
        LOG_WARNING("present: cannot take the renderer's buffer %u (%ld)", host_gem, r);
        return GPU_ERR_DRM;
    }
    gpu_drm_fb_cmd_uapi fb;
    memset(&fb, 0, sizeof(fb));
    fb.width = p->fb_w;
    fb.height = p->fb_h;
    fb.pitch = pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = gem;
    r = kms_ioctl(p->dev_idx, GPU_DRM_IOC_ADDFB, &fb, sizeof(fb));
    if (r) {
        gpu_drm_gem_close_uapi gc = { .handle = gem, .pad = 0 };
        kms_ioctl(p->dev_idx, GPU_DRM_IOW(0x09, gpu_drm_gem_close_uapi), &gc, sizeof(gc));
        LOG_WARNING("present: ADDFB %ux%u pitch %u of the renderer's buffer failed (%ld)", p->fb_w, p->fb_h,
                    pitch, r);
        return GPU_ERR_DRM;
    }
    p->zc_fb[p->zc_count] = fb.fb_id;
    p->zc_gem[p->zc_count] = gem;
    return p->zc_count++;
}

int gpu_present_zc_show(gpu_present *p, int slot) {
    if (!p->kms || slot < 0 || slot >= p->zc_count) return GPU_ERR_PARAM;
    if (p->can_flip) {
        gpu_drm_page_flip_uapi flip;
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = p->crtc_id;
        flip.fb_id = p->zc_fb[slot];
        for (uint32_t busy = 0;; busy++) {
            long r = kms_ioctl(p->dev_idx, GPU_DRM_IOC_PAGE_FLIP, &flip, sizeof(flip));
            if (r == 0) return GPU_OK;
            if (r != GPU_DRM_ERR_BUSY || busy >= 1500) {
                LOG_WARNING("present: page flip to the renderer's buffer failed (%ld) - using SETCRTC", r);
                p->can_flip = false;
                break;
            }
            scheduler_sleep_ms(1);
        }
    }
    gpu_drm_crtc_uapi set;
    memset(&set, 0, sizeof(set));
    set.crtc_id = p->crtc_id;
    set.fb_id = p->zc_fb[slot];
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&p->connector_id;
    set.count_connectors = 1;
    gpu_drm_crtc_uapi cur;
    memset(&cur, 0, sizeof(cur));
    cur.crtc_id = p->crtc_id;
    if (kms_ioctl(p->dev_idx, GPU_DRM_IOC_GETCRTC, &cur, sizeof(cur)) || !cur.mode_valid) return GPU_ERR_DRM;
    set.mode_valid = 1;
    set.mode = cur.mode;
    return kms_ioctl(p->dev_idx, GPU_DRM_IOC_SETCRTC, &set, sizeof(set)) ? GPU_ERR_DRM : GPU_OK;
}

static void kms_zc_release(gpu_present *p) {
    for (int i = 0; i < p->zc_count; i++) {
        uint32_t fb_id = p->zc_fb[i];
        if (fb_id) kms_ioctl(p->dev_idx, GPU_DRM_IOC_RMFB, &fb_id, sizeof(fb_id));
        gpu_drm_gem_close_uapi gc = { .handle = p->zc_gem[i], .pad = 0 };
        if (gc.handle) kms_ioctl(p->dev_idx, GPU_DRM_IOW(0x09, gpu_drm_gem_close_uapi), &gc, sizeof(gc));
        p->zc_fb[i] = 0;
        p->zc_gem[i] = 0;
    }
    p->zc_count = 0;
}

void gpu_present_zc_reset(gpu_present *p) {
    if (p->kms) kms_zc_release(p);
}

void gpu_present_close(gpu_present *p) {
    if (!p->kms) {
        if (p->snapshot && p->pixels) {
            uint8_t *fbp;
            uint32_t w, h, pitch;
            if (limine_video_fb_info(&fbp, &w, &h, &pitch))
                memcpy(fbp, p->snapshot, (size_t)h * pitch);
            limine_video_announce_damage();
            kfree(p->snapshot);
        }
        return;
    }
    int dev = p->dev_idx;
    gpu_monitor *m = NULL;
    if (p->restore_mon && p->mon_idx >= 0 && p->mon_idx < gpu_mon_count) {
        m = &gpu_mons[p->mon_idx];
        if (!m->enabled || !m->fb_id || !m->crtc_id || m->mode_current < 0 || m->dev_idx != dev) m = NULL;
    }
    if (m) {
        gpu_drm_crtc_uapi set;
        memset(&set, 0, sizeof(set));
        set.crtc_id = m->crtc_id;
        set.fb_id = m->fb_id;
        set.set_connectors_ptr = (uint64_t)(uintptr_t)&m->connector_id;
        set.count_connectors = 1;
        set.mode_valid = 1;
        set.mode = m->modes[m->mode_current];
        kms_set_master(dev);
        long r = kms_ioctl(dev, GPU_DRM_IOC_SETCRTC, &set, sizeof(set));
        if (r) LOG_WARNING("present: restoring %s %ux%u failed (%ld)", m->name, set.mode.hdisplay,
                           set.mode.vdisplay, r);
    } else {
        lkpi_console_hold_connector(gpu_devices[dev].primary_devt, p->connector_id, 0);
    }
    kms_zc_release(p);
    for (int i = 0; i < 2; i++) {
        if (!p->fb_id[i]) continue;
        kms_fb f = {.fb_id = p->fb_id[i],
                    .gem_handle = p->gem_handle[i],
                    .pitch = p->pitch,
                    .map_token = p->map_token[i],
                    .pixels = NULL,
                    .map_size = p->map_size[i]};
        kms_fb_destroy(dev, &f);
    }
    kms_drop_master(dev);
    if (!kms_dev_has_enabled(dev)) gpu_drm_close(dev);
    lkpi_console_restore(gpu_devices[dev].primary_devt);
}

int gpu_kmscube_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out) {
    int m = mesa_start();
    if (m != GPU_OK) return m;
    int r = gpu_gl_cube_run(opts, frames_out, fps_out);
    if (r != GPU_OK) LOG_WARNING("kmscube(GL): real-GL path failed (%d) - check dmesg", r);
    return r;
}

int gpu_vkkmv_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out) {
    int m = mesa_start();
    if (m != GPU_OK) return m;
    int r = gpu_vk_run(opts, frames_out, fps_out);
    if (r != GPU_OK) LOG_WARNING("vkkmv(Vulkan): real-VK path failed (%d) - check dmesg", r);
    return r;
}

static uint8_t *rec_at(struct gpu_client *c, uint16_t i) {
    return &c->batch[GPU_BATCH_REC_OFF + (size_t)i * sizeof(gpu_cmd_rec)];
}

static gpu_cmd_rec *rec_of(struct gpu_client *c, uint16_t i) {
    return (gpu_cmd_rec *)rec_at(c, i);
}

static int result_write(uint8_t *area, uint16_t off, int status, const void *data, uint16_t len) {
    uint8_t *p = area + off;
    p[0] = (uint8_t)(len & 0xff);
    p[1] = (uint8_t)(len >> 8);
    p[2] = (int8_t)status;
    p[3] = 0;
    if (len) memcpy(p + GPU_RESULT_HDR, data, len);
    return GPU_OK;
}


static int gpu_exec_drm_record(struct gpu_client *c, gpu_cmd_rec *rec) {
    struct gpu_device *d = core_dev(c->dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (!(d->flags & GPU_DEV_FLAG_BOUND)) return GPU_ERR_NO_DRIVER;
    if (gpu_drm_open(c->dev_idx, false)) return GPU_ERR_DRM;
    gpu_devctx *ctx = gpu_dev_ctx(c->dev_idx);
    uint8_t *results = &c->batch[GPU_BATCH_RESULT_OFF];
    int r = GPU_OK;

    switch (rec->opcode) {
    case GPU_CMD_DRM_VERSION: {
        if (!d->drm_name[0]) gpu_query_drm_identity(d);
        char text[128];
        int n = snprintf(text, sizeof(text), "%d.%d.%d %s%s%s", d->drm_major, d->drm_minor, d->drm_patch,
                         d->drm_name, d->drm_desc[0] ? " - " : "", d->drm_desc);
        if (rec->result_off)
            result_write(results, rec->result_off, 0, text, (uint16_t)(n > 0 ? (n < (int)sizeof(text) ? n : (int)sizeof(text) - 1) : 0));
        break;
    }
    case GPU_CMD_DRM_GET_CAP: {
        gpu_drm_get_cap_uapi cap = { .capability = rec->args[0], .value = 0 };
        long ret = lkpi_gpu_ioctl(ctx->lx_card, GPU_DRM_IOWR(0x0c, gpu_drm_get_cap_uapi), &cap);
        if (rec->result_off) {
            uint64_t v = ret ? (uint64_t)(int64_t)ret : cap.value;
            result_write(results, rec->result_off, ret ? GPU_ERR_DRM : GPU_OK, &v, sizeof(v));
        }
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_SET_CLIENT_CAP: {
        gpu_drm_get_cap_uapi cap = { .capability = rec->args[0], .value = rec->args[1] };
        long ret = lkpi_gpu_ioctl(ctx->lx_card, GPU_DRM_IOWR(0x0d, gpu_drm_get_cap_uapi), &cap);
        if (rec->result_off) {
            uint64_t v = (uint64_t)(int64_t)ret;
            result_write(results, rec->result_off, ret ? GPU_ERR_DRM : GPU_OK, &v, sizeof(v));
        }
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_GEM_CLOSE: {
        gpu_drm_gem_close_uapi gc = { .handle = (uint32_t)rec->args[0], .pad = 0 };
        long ret = lkpi_gpu_ioctl(ctx->lx_card, GPU_DRM_IOW(0x09, gpu_drm_gem_close_uapi), &gc);
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_GET_MAGIC: {
        gpu_drm_magic_uapi m = { .magic = 0, .pad = 0 };
        long ret = lkpi_gpu_ioctl(ctx->lx_card, GPU_DRM_IOR(0x02, gpu_drm_magic_uapi), &m);
        if (rec->result_off) {
            uint32_t magic = m.magic;
            result_write(results, rec->result_off, ret ? GPU_ERR_DRM : GPU_OK, &magic, sizeof(magic));
        }
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_AUTH_MAGIC: {
        gpu_drm_auth_uapi a = { .magic = (uint32_t)rec->args[0], .pad = 0 };
        long ret = lkpi_gpu_ioctl(ctx->lx_card, GPU_DRM_IOW(0x03, gpu_drm_auth_uapi), &a);
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_SET_MASTER:
    case GPU_CMD_DRM_DROP_MASTER: {
        long ret = lkpi_gpu_ioctl(ctx->lx_card,
                                  GPU_DRM_IO(rec->opcode == GPU_CMD_DRM_SET_MASTER ? 0x1e : 0x1f), NULL);
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_MODE_GETRESOURCES: {
        gpu_drm_card_res_uapi res;
        memset(&res, 0, sizeof(res));
        long ret = lkpi_gpu_ioctl(ctx->lx_card, GPU_DRM_IOWR(0xA0, gpu_drm_card_res_uapi), &res);
        if (rec->result_off && !ret) {
            uint32_t counts[4] = { res.count_crtcs, res.count_connectors, res.count_encoders, res.count_fbs };
            result_write(results, rec->result_off, 0, counts, sizeof(counts));
        }
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    case GPU_CMD_DRM_IOCTL: {
        uint32_t cmd = (uint32_t)rec->args[0];
        uint32_t size = (uint32_t)rec->args[1];
        uint8_t *payload = &c->batch[GPU_BATCH_PAYLOAD_OFF + rec->payload_off];
        if (!size || size > rec->payload_len || !cmd) return GPU_ERR_PARAM;
        long ret = lkpi_gpu_ioctl(ctx->lx_card, cmd, payload);
        if (rec->result_off) {
            uint16_t room = (uint16_t)(rec->result_cap > GPU_RESULT_HDR ? rec->result_cap - GPU_RESULT_HDR : 0);
            uint16_t len = (uint16_t)(size < room ? size : room);
            result_write(results, rec->result_off, ret ? GPU_ERR_DRM : GPU_OK, payload, len);
        }
        r = ret ? GPU_ERR_DRM : GPU_OK;
        break;
    }
    default:
        return GPU_ERR_PARAM;
    }
    return r;
}


static struct gpu_client *client_get(int client_id) {
    if (client_id < 0 || client_id >= GPU_MAX_CLIENTS) return NULL;
    struct gpu_client *c = &gpu_clients[client_id];
    return c->used ? c : NULL;
}

struct gpu_ring_slot *gpu_slot_by_seq(int dev_idx, uint32_t seq) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return NULL;
    for (int i = 0; i < GPU_RING_SLOTS; i++) {
        struct gpu_ring_slot *s = &ctx->ring[i];
        if (s->used && s->seq == seq) return s;
    }
    return NULL;
}

int gpu_slot_publish(int dev_idx, uint32_t seq, uint32_t status) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return GPU_ERR_NODEV;
    uint64_t flags = spin_lock_irqsave(&gpu_registry_lock);
    struct gpu_ring_slot *slot = NULL;
    for (int i = 0; i < GPU_RING_SLOTS; i++) {
        struct gpu_ring_slot *s = &ctx->ring[i];
        if (s->used && s->seq == seq && !s->complete_ns) { slot = s; break; }
    }
    if (!slot) {
        spin_unlock_irqrestore(&gpu_registry_lock, flags);
        return GPU_ERR_PARAM;
    }
    slot->status = status;
    slot->complete_ns = gpu_now_ms();
    slot->done_flag = 1;
    ctx->total_completed++;
    spin_unlock_irqrestore(&gpu_registry_lock, flags);
    scheduler_wake_all((void *)&slot->done_flag);
    return GPU_OK;
}

int gpu_client_open(int dev_idx, uint8_t api) {
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    if (api >= GPU_API_COUNT) return GPU_ERR_PARAM;

    uint64_t flags = spin_lock_irqsave(&gpu_client_lock);
    int id = -1;
    for (int i = 0; i < GPU_MAX_CLIENTS; i++) {
        if (!gpu_clients[i].used) { id = i; break; }
    }
    if (id < 0) {
        spin_unlock_irqrestore(&gpu_client_lock, flags);
        return GPU_ERR_BUSY;
    }
    memset(&gpu_clients[id], 0, sizeof(gpu_clients[id]));
    gpu_clients[id].used = 1;
    gpu_clients[id].dev_idx = dev_idx;
    gpu_clients[id].api = api;
    spin_unlock_irqrestore(&gpu_client_lock, flags);

    if (api != GPU_API_NONE && !mesa_host_pid_get())
        mesa_start_async();

    LOG_INFO("gpu core: %s client on %s (%s)",
             api == GPU_API_VULKAN ? "Vulkan" : (api == GPU_API_NONE ? "DRM-direct" : "OpenGL"),
             d->card[0] ? d->card : d->pci_name, d->driver);
    return id;
}

int gpu_client_close(int client_id) {
    struct gpu_client *c = client_get(client_id);
    if (!c) return GPU_ERR_PARAM;
    uint64_t flags = spin_lock_irqsave(&gpu_client_lock);
    memset(c, 0, sizeof(*c));
    spin_unlock_irqrestore(&gpu_client_lock, flags);
    return GPU_OK;
}

int gpu_cmd_begin(int client_id) {
    struct gpu_client *c = client_get(client_id);
    if (!c) return GPU_ERR_PARAM;
    if (c->batch_open) return GPU_ERR_STATE;
    memset(&c->batch, 0, GPU_BATCH_TOTAL_OFF);
    gpu_batch_hdr *hdr = (gpu_batch_hdr *)&c->batch[GPU_BATCH_HDR_OFF];
    hdr->magic = GPU_CMD_MAGIC;
    hdr->api = c->api;
    hdr->device = (uint32_t)c->dev_idx;
    hdr->client_id = (uint32_t)client_id;
    c->batch_open = 1;
    c->cmd_count = 0;
    c->payload_used = 0;
    c->result_used = 0;
    return GPU_OK;
}

static int cmd_encode(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
                      const void *payload, uint16_t payload_len, uint16_t result_cap) {
    struct gpu_client *c = client_get(client_id);
    if (!c) return GPU_ERR_PARAM;
    if (!c->batch_open) return GPU_ERR_STATE;
    if (c->cmd_count >= GPU_BATCH_MAX_CMDS) return GPU_ERR_OVERFLOW;
    if (argc > GPU_CMD_MAX_ARGS) return GPU_ERR_PARAM;

    uint16_t payload_off = 0;
    if (payload_len) {
        if (!payload) return GPU_ERR_PARAM;
        if (c->payload_used + payload_len > GPU_BATCH_PAYLOAD) return GPU_ERR_OVERFLOW;
        payload_off = (uint16_t)((c->payload_used + 7) & ~7);
        if (payload_off + payload_len > GPU_BATCH_PAYLOAD) return GPU_ERR_OVERFLOW;
        memcpy(&c->batch[GPU_BATCH_PAYLOAD_OFF + payload_off], payload, payload_len);
        c->payload_used = payload_off + payload_len;
    }

    uint16_t result_off = 0;
    if (result_cap) {
        if (c->result_used + GPU_RESULT_HDR + result_cap > GPU_BATCH_RESULT) return GPU_ERR_OVERFLOW;
        result_off = (uint16_t)(c->result_used + GPU_RESULT_HDR);
        c->result_used = result_off + result_cap;
    }

    gpu_cmd_rec *rec = rec_of(c, c->cmd_count);
    memset(rec, 0, sizeof(*rec));
    rec->opcode = opcode;
    rec->argc = argc;
    if (args && argc) memcpy(rec->args, args, sizeof(uint64_t) * argc);
    rec->payload_off = payload_off;
    rec->payload_len = payload_len;
    rec->result_off = result_off;
    rec->result_cap = result_cap;
    if (result_cap) rec->flags |= GPU_REC_FLAG_RESULT;
    c->cmd_count++;
    return GPU_OK;
}

int gpu_cmd_gl(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
               const void *payload, uint16_t payload_len, uint16_t result_cap) {
    int i = (int)opcode - GPU_CMD_GL_FIRST;
    if (i < 0 || opcode > GPU_CMD_GL_LAST) return GPU_ERR_PARAM;
    if (i >= GPU_CMD_GL_COUNT && opcode < GPU_CMD_GL_EGL_FIRST) return GPU_ERR_PARAM;
    struct gpu_client *c = client_get(client_id);
    if (c && c->api == GPU_API_NONE) return GPU_ERR_STATE;
    return cmd_encode(client_id, opcode, argc, args, payload, payload_len, result_cap);
}

int gpu_cmd_vk(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
               const void *payload, uint16_t payload_len, uint16_t result_cap) {
    int i = (int)opcode - GPU_CMD_VK_FIRST;
    if (i < 0 || i >= GPU_CMD_VK_COUNT) return GPU_ERR_PARAM;
    struct gpu_client *c = client_get(client_id);
    if (c && c->api == GPU_API_NONE) return GPU_ERR_STATE;
    return cmd_encode(client_id, opcode, argc, args, payload, payload_len, result_cap);
}

int gpu_cmd_drm(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
                const void *payload, uint16_t payload_len, uint16_t result_cap) {
    if (opcode < GPU_CMD_DRM_FIRST || opcode >= GPU_CMD_DRM_LAST) return GPU_ERR_PARAM;
    if (argc > 2 && opcode != GPU_CMD_DRM_IOCTL) return GPU_ERR_PARAM;
    return cmd_encode(client_id, opcode, argc, args, payload, payload_len, result_cap);
}


int gpu_flush(int client_id, uint32_t timeout_ms) {
    struct gpu_client *c = client_get(client_id);
    if (!c) return GPU_ERR_PARAM;
    if (!c->batch_open) return GPU_ERR_STATE;

    gpu_batch_hdr *hdr = (gpu_batch_hdr *)&c->batch[GPU_BATCH_HDR_OFF];
    c->seq++;
    hdr->seq = c->seq;
    hdr->cmd_count = c->cmd_count;
    hdr->payload_used = c->payload_used;
    hdr->result_used = c->result_used;
    hdr->submit_ns = gpu_now_ms();

    bool has_api_cmds = false;
    int status = GPU_OK;
    uint16_t fail_opcode = 0;
    for (uint16_t i = 0; i < c->cmd_count; i++) {
        gpu_cmd_rec *rec = rec_of(c, i);
        if (rec->opcode >= GPU_CMD_DRM_FIRST && rec->opcode < GPU_CMD_DRM_LAST) {
            int r = gpu_exec_drm_record(c, rec);
            if (r && !status) { status = r; fail_opcode = rec->opcode; }
        } else {
            has_api_cmds = true;
        }
    }
    c->flushed_cmds += c->cmd_count;
    c->flushed_batches++;
    c->batch_open = 0;

    if (!has_api_cmds) {
        c->last_slot = NULL;
        return status;
    }

    gpu_devctx *ctx = gpu_dev_ctx(c->dev_idx);
    struct gpu_device *d = core_dev(c->dev_idx);
    if (!ctx || !d) return GPU_ERR_NODEV;

    uint64_t flags = spin_lock_irqsave(&gpu_registry_lock);
    struct gpu_ring_slot *slot = NULL;
    for (int i = 0; i < GPU_RING_SLOTS; i++) {
        struct gpu_ring_slot *s = &ctx->ring[i];
        if (!s->used || s->complete_ns) { slot = s; break; }
    }
    if (!slot) {
        ctx->dropped++;
        spin_unlock_irqrestore(&gpu_registry_lock, flags);
        return GPU_ERR_OVERFLOW;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->api = c->api;
    slot->seq = c->seq;
    slot->device = (uint32_t)c->dev_idx;
    slot->payload_used = c->payload_used;
    slot->status = 0;
    memcpy(slot->data, c->batch, GPU_BATCH_TOTAL_OFF);
    uint16_t bytes = (uint16_t)(GPU_BATCH_REC_OFF + (size_t)c->cmd_count * sizeof(gpu_cmd_rec) + c->payload_used);
    slot->bytes = bytes;
    ctx->total_flushed++;
    ctx->last_seq = c->seq;
    if (bytes > ctx->largest_batch) ctx->largest_batch = bytes;
    c->last_slot = slot;
    bool attached = ctx->attached != 0;
    spin_unlock_irqrestore(&gpu_registry_lock, flags);

    if (has_api_cmds && c->tr_epoch && c->tr_epoch != mesa_epoch_get()) {
        gpu_slot_publish(c->dev_idx, c->seq, GPU_ERR_NO_TRANSLATOR);
        return GPU_ERR_NO_TRANSLATOR;
    }

    mesa_on_flush(c->dev_idx, slot);

    if (!attached && !mesa_ready() && !mesa_failed() && timeout_ms) {
        int ms = mesa_start();
        if (ms && !mesa_ready()) return GPU_ERR_NO_TRANSLATOR;
    }
    if (!timeout_ms) return GPU_OK;

    int w = scheduler_wait((void *)&slot->done_flag, (volatile int *)&slot->done_flag, timeout_ms);
    if (w == 0) {
        if (!slot->status) c->tr_epoch = mesa_epoch_get();
        return slot->status ? (int)slot->status : GPU_OK;
    }
    (void)fail_opcode;
    return GPU_ERR_TIMEOUT;
}

int gpu_result(int client_id, uint16_t cmd_index, void *out, uint16_t cap, uint16_t *out_len) {
    struct gpu_client *c = client_get(client_id);
    if (!c) return GPU_ERR_PARAM;
    if (cmd_index >= c->cmd_count) return GPU_ERR_PARAM;

    gpu_cmd_rec *rec = rec_of(c, cmd_index);
    if (!rec->result_off) return GPU_ERR_PARAM;

    const uint8_t *area = NULL;
    if (c->last_slot && c->last_slot->seq == c->seq && c->last_slot->done_flag != 0) {
        area = &c->last_slot->data[GPU_BATCH_RESULT_OFF];
    } else if (!c->batch_open) {
        area = &c->batch[GPU_BATCH_RESULT_OFF];
    } else {
        return GPU_ERR_STATE;
    }

    uint16_t len = (uint16_t)(area[rec->result_off] | (area[rec->result_off + 1] << 8));
    int8_t status = (int8_t)area[rec->result_off + 2];
    uint16_t n = len < cap ? len : cap;
    if (n) memcpy(out, area + rec->result_off + GPU_RESULT_HDR, n);
    if (out_len) *out_len = len;
    return status ? status : GPU_OK;
}


int gpu_translator_attach(int dev_idx) {
    struct gpu_device *d = core_dev(dev_idx);
    if (!d) return GPU_ERR_NODEV;
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return GPU_ERR_NODEV;
    ctx->attached = 1;
    d->flags |= GPU_DEV_FLAG_TRANSLATOR;
    LOG_INFO("gpu core: Mesa translator attached to %s (%s)", d->card[0] ? d->card : d->pci_name, d->driver);
    return GPU_OK;
}

int gpu_translator_detach(int dev_idx) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx) return GPU_ERR_NODEV;
    if (!ctx->attached) return GPU_ERR_PARAM;
    ctx->attached = 0;
    if (dev_idx < gpu_device_count) gpu_devices[dev_idx].flags &= (uint8_t)~GPU_DEV_FLAG_TRANSLATOR;
    LOG_INFO("gpu core: Mesa translator detached from device %d", dev_idx);
    return GPU_OK;
}

bool gpu_translator_attached(int dev_idx) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    return ctx && ctx->attached != 0;
}

int gpu_translator_ring_info(int dev_idx, struct gpu_ring_info *out) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx || !out) return GPU_ERR_NODEV;
    uint64_t flags = spin_lock_irqsave(&gpu_registry_lock);
    uint32_t pending = 0;
    for (int i = 0; i < GPU_RING_SLOTS; i++)
        if (ctx->ring[i].used && !ctx->ring[i].complete_ns) pending++;
    memset(out, 0, sizeof(*out));
    out->device = (uint32_t)dev_idx;
    out->slots = GPU_RING_SLOTS;
    out->pending = pending;
    out->total_flushed = ctx->total_flushed;
    out->total_completed = ctx->total_completed;
    out->dropped = ctx->dropped;
    out->largest_batch = ctx->largest_batch;
    out->last_seq = ctx->last_seq;
    spin_unlock_irqrestore(&gpu_registry_lock, flags);
    return GPU_OK;
}

int gpu_translator_drain(int dev_idx, void *buf, uint32_t cap, uint32_t *out_len) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx || !buf || !out_len) return GPU_ERR_PARAM;
    if (cap < GPU_BATCH_TOTAL_OFF) return GPU_ERR_PARAM;

    uint64_t flags = spin_lock_irqsave(&gpu_registry_lock);
    struct gpu_ring_slot *slot = NULL;
    for (int i = 0; i < GPU_RING_SLOTS; i++) {
        struct gpu_ring_slot *s = &ctx->ring[i];
        if (s->used && !s->complete_ns) { slot = s; break; }
    }
    if (!slot) {
        spin_unlock_irqrestore(&gpu_registry_lock, flags);
        *out_len = 0;
        return GPU_ERR_BUSY;
    }
    memcpy(buf, slot->data, GPU_BATCH_TOTAL_OFF);
    *out_len = GPU_BATCH_TOTAL_OFF;
    spin_unlock_irqrestore(&gpu_registry_lock, flags);
    return GPU_OK;
}

int gpu_translator_complete(int dev_idx, const struct gpu_done *done, const void *result_data,
                            uint32_t result_len) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    if (!ctx || !done) return GPU_ERR_PARAM;
    if (result_len > GPU_BATCH_RESULT) return GPU_ERR_PARAM;

    uint64_t flags = spin_lock_irqsave(&gpu_registry_lock);
    struct gpu_ring_slot *slot = NULL;
    for (int i = 0; i < GPU_RING_SLOTS; i++) {
        struct gpu_ring_slot *s = &ctx->ring[i];
        if (s->used && s->seq == done->seq && !s->complete_ns) { slot = s; break; }
    }
    if (!slot) {
        spin_unlock_irqrestore(&gpu_registry_lock, flags);
        return GPU_ERR_PARAM;
    }
    if (result_len && result_data)
        memcpy(&slot->data[GPU_BATCH_RESULT_OFF], result_data, result_len);
    slot->result_used = done->result_used;
    slot->status = done->status;
    slot->fail_opcode = (uint16_t)done->fail_opcode;
    slot->complete_ns = gpu_now_ms();
    slot->done_flag = 1;
    ctx->total_completed++;
    spin_unlock_irqrestore(&gpu_registry_lock, flags);

    scheduler_wake_all((void *)&slot->done_flag);
    return GPU_OK;
}

uint32_t gpu_translator_total_flushed(int dev_idx) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    return ctx ? ctx->total_flushed : 0;
}

uint32_t gpu_translator_dropped(int dev_idx) {
    gpu_devctx *ctx = gpu_dev_ctx(dev_idx);
    return ctx ? ctx->dropped : 0;
}


int gpu_core_api_info(struct gpu_api_info *info) {
    if (!info) return GPU_ERR_PARAM;
    memset(info, 0, sizeof(*info));
    info->gl_commands = (uint32_t)GPU_CMD_GL_COUNT;
    info->vk_commands = (uint32_t)GPU_CMD_VK_COUNT;
    info->drm_commands = (uint32_t)(GPU_CMD_DRM_LAST - GPU_CMD_DRM_FIRST);
    info->batch_bytes = GPU_BATCH_BYTES;
    info->ring_slots = GPU_RING_SLOTS;
    info->protocol = GPU_PROTOCOL_VERSION;
    for (int i = 0; i < gpu_device_count; i++) {
        if (!gpu_devices[i].valid || !(gpu_devices[i].flags & GPU_DEV_FLAG_BOUND)) continue;
        strncpy(info->drv_name, gpu_devices[i].drm_name, sizeof(info->drv_name) - 1);
        strncpy(info->drv_desc, gpu_devices[i].drm_desc, sizeof(info->drv_desc) - 1);
        info->drm_version[0] = gpu_devices[i].drm_major;
        info->drm_version[1] = gpu_devices[i].drm_minor;
        info->drm_version[2] = gpu_devices[i].drm_patch;
        break;
    }
    return GPU_OK;
}


static void core_scan_all(void) { gpu_core_refresh(); }

static int core_refresh(void) {
    gpu_core_refresh();
    return gpu_device_count;
}

static int core_get_device_count(void) { return gpu_device_count; }

static struct gpu_device *core_get_device(int idx) { return core_dev(idx); }

static int core_client_gl(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
                          const void *payload, uint16_t payload_len, uint16_t result_cap) {
    return gpu_cmd_gl(client_id, opcode, argc, args, payload, payload_len, result_cap);
}

static int core_client_vk(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
                          const void *payload, uint16_t payload_len, uint16_t result_cap) {
    return gpu_cmd_vk(client_id, opcode, argc, args, payload, payload_len, result_cap);
}

static int core_client_drm(int client_id, uint16_t opcode, uint8_t argc, const uint64_t *args,
                           const void *payload, uint16_t payload_len, uint16_t result_cap) {
    return gpu_cmd_drm(client_id, opcode, argc, args, payload, payload_len, result_cap);
}

static const char *core_cmd_name(uint8_t api, uint16_t opcode) { return gpu_cmd_name(api, opcode); }

static struct gpu_core_driver gpu_core = {
    .scan_all = core_scan_all,
    .refresh = core_refresh,
    .get_device_count = core_get_device_count,
    .get_device = core_get_device,
    .start_stack = gpu_start_stack,
    .restart_stack = gpu_restart_stack,
    .bind = gpu_bind_device,
    .unbind = gpu_unbind_device,
    .set_debug = gpu_set_debug_mask,
    .driver_list = gpu_driver_list,
    .drm_open = gpu_drm_open,
    .drm_ioctl = gpu_drm_ioctl,
    .drm_close = gpu_drm_close,
    .client_open = gpu_client_open,
    .client_close = gpu_client_close,
    .cmd_begin = gpu_cmd_begin,
    .cmd_gl = core_client_gl,
    .cmd_vk = core_client_vk,
    .cmd_drm = core_client_drm,
    .flush = gpu_flush,
    .result = gpu_result,
    .api_info = gpu_core_api_info,
    .cmd_name = core_cmd_name,
};

struct gpu_core_driver *return_gpu_core_driver(void) {
    if (!gpu_core_ready) mesa_init();
    if (gpu_core_ready) return &gpu_core;
    gpu_core_ready = true;
    LOG_INFO("gpu core ready (first enumeration deferred to the first gpu command): "
             "%d GL + %d Vulkan + %d DRM commands",
             GPU_CMD_GL_COUNT, GPU_CMD_VK_COUNT,
             (int)(GPU_CMD_DRM_LAST - GPU_CMD_DRM_FIRST));
    return &gpu_core;
}

struct driver *return_meta_gpu_core_driver(void) {
    static struct dependency deps[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "GPU Core Driver",
        .type = GPU_DRIVER,
        .sub_type = GPU_CORE_SLOT,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &deps[0] },
        .dependency_count = 1,
        .self = &gpu_core,
        .init = (void *)return_gpu_core_driver,
    };
    return &meta;
}


