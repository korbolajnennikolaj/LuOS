// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/gpu_demo_math.h"
#include "drivers/GPU/gpu_demo_vk_shaders.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/mesa_vk_defs.h"
#include "drivers/GPU/mesa_gl_defs.h"
#include "drivers/GPU/mesa_vk_pack.h"

#include "components/Memory/heap.h"
#include "components/logger.h"
#include "drivers/Input/keyboard_driver.h"
#include "kernel/lkpi/lkpi.h"
#include "kernel/proc/syscall.h"
#include "kernel/scheduler/scheduler.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef VK_API_VERSION_1_1
#define VK_API_VERSION_1_1 0x00401000u
#endif

struct vks {
    int cl;
    char err[160];
    uint8_t rbuf[720];
    uint16_t rlen;
};

static uint64_t vk_msnow(void) { return sys_monotonic_ns() / 1000000ULL; }

static int vks_flush(struct vks *v, uint32_t timeout) {
    int r = gpu_flush(v->cl, timeout);
    if (r) {
        if (!v->err[0]) snprintf(v->err, sizeof(v->err), "gpu_flush %d", r);
        return 1;
    }
    return 0;
}

static int vks_one(struct vks *v, uint16_t op, uint8_t argc, const uint64_t *args,
                   const void *payload, uint32_t payload_len, uint16_t cap) {
    if (gpu_cmd_begin(v->cl)) return 1;
    int enc;
    if (op >= GPU_CMD_VK_FIRST && op <= GPU_CMD_VK_LAST)
        enc = gpu_cmd_vk(v->cl, op, argc, args, payload, (uint16_t)payload_len, cap ? cap + 8 : 0);
    else
        enc = gpu_cmd_gl(v->cl, op, argc, args, payload, (uint16_t)payload_len, cap ? cap + 8 : 0);
    if (enc) {
        if (!v->err[0]) snprintf(v->err, sizeof(v->err), "encode op 0x%x: %d", op, enc);
        return 1;
    }
    if (vks_flush(v, 180000)) {
        size_t n = strlen(v->err);
        if (n + 16 < sizeof(v->err)) snprintf(v->err + n, sizeof(v->err) - n, " (op 0x%x)", op);
        return 1;
    }
    if (cap) {
        uint16_t len = 0;
        memset(v->rbuf, 0, sizeof(v->rbuf));
        if (gpu_result(v->cl, 0, v->rbuf, cap, &len)) {
            if (!v->err[0]) snprintf(v->err, sizeof(v->err), "result of op 0x%x", op);
            return 1;
        }
        v->rlen = len;
    }
    return 0;
}

static uint64_t vks_r64(struct vks *v) {
    uint64_t r = 0;
    memcpy(&r, v->rbuf, 8);
    return r;
}

static bool vks_abort(void) {
    struct keyboard_driver *kbd = get_self_driver(KEYBOARD_DRIVER, VIRTUAL_KEYBOARD);
    if (!kbd) return false;
    struct key_event ev;
    bool stop = false;
    while (kbd->get_key_event(&ev))
        if (ev.code == KEY_ESC) stop = true;
    return stop;
}

struct vkres {
    uint64_t instance, pdev, dev, queue, pool, cmdbuf;
    uint64_t vbuf, vmem, image, imem, iview, pass, fb;
    uint64_t dimage, dmem, dview;
    uint64_t vsmod, fsmod, layout, pipeline, cbuf, cmem;
};

#define VKKMV_DEPTH_FORMAT VK_FORMAT_D16_UNORM

static uint32_t vk_pick_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t type_bits,
                            uint32_t want, uint32_t fallback_want) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t flags = pass ? fallback_want : want;
        for (uint32_t i = 0; i < mp->memoryTypeCount && i < 32; i++) {
            if (!(type_bits & (1u << i))) continue;
            if ((mp->memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
    }
    return 0xFFFFFFFFu;
}

static void vk_destroy(struct vks *v, struct vkres *r) {
    uint64_t z[6] = {0};
    if (r->pipeline) vks_one(v, GPU_CMD_VK_DESTROY_PIPELINE, 2, (uint64_t[]){r->dev, r->pipeline}, NULL, 0, 0);
    if (r->layout) vks_one(v, GPU_CMD_VK_DESTROY_PIPELINE_LAYOUT, 2, (uint64_t[]){r->dev, r->layout}, NULL, 0, 0);
    if (r->vsmod) vks_one(v, GPU_CMD_VK_DESTROY_SHADER_MODULE, 2, (uint64_t[]){r->dev, r->vsmod}, NULL, 0, 0);
    if (r->fsmod) vks_one(v, GPU_CMD_VK_DESTROY_SHADER_MODULE, 2, (uint64_t[]){r->dev, r->fsmod}, NULL, 0, 0);
    if (r->fb) vks_one(v, GPU_CMD_VK_DESTROY_FRAMEBUFFER, 2, (uint64_t[]){r->dev, r->fb}, NULL, 0, 0);
    if (r->pass) vks_one(v, GPU_CMD_VK_DESTROY_RENDER_PASS, 2, (uint64_t[]){r->dev, r->pass}, NULL, 0, 0);
    if (r->dview) vks_one(v, GPU_CMD_VK_DESTROY_IMAGE_VIEW, 2, (uint64_t[]){r->dev, r->dview}, NULL, 0, 0);
    if (r->dimage) vks_one(v, GPU_CMD_VK_DESTROY_IMAGE, 2, (uint64_t[]){r->dev, r->dimage}, NULL, 0, 0);
    if (r->dmem) vks_one(v, GPU_CMD_VK_FREE_MEMORY, 2, (uint64_t[]){r->dev, r->dmem}, NULL, 0, 0);
    if (r->iview) vks_one(v, GPU_CMD_VK_DESTROY_IMAGE_VIEW, 2, (uint64_t[]){r->dev, r->iview}, NULL, 0, 0);
    if (r->image) vks_one(v, GPU_CMD_VK_DESTROY_IMAGE, 2, (uint64_t[]){r->dev, r->image}, NULL, 0, 0);
    if (r->imem) vks_one(v, GPU_CMD_VK_FREE_MEMORY, 2, (uint64_t[]){r->dev, r->imem}, NULL, 0, 0);
    if (r->vbuf) vks_one(v, GPU_CMD_VK_DESTROY_BUFFER, 2, (uint64_t[]){r->dev, r->vbuf}, NULL, 0, 0);
    if (r->vmem) vks_one(v, GPU_CMD_VK_FREE_MEMORY, 2, (uint64_t[]){r->dev, r->vmem}, NULL, 0, 0);
    if (r->cmem) vks_one(v, GPU_CMD_VK_UNMAP_MEMORY, 2, (uint64_t[]){r->dev, r->cmem}, NULL, 0, 0);
    if (r->cmem) vks_one(v, GPU_CMD_VK_FREE_MEMORY, 2, (uint64_t[]){r->dev, r->cmem}, NULL, 0, 0);
    if (r->cbuf) vks_one(v, GPU_CMD_VK_DESTROY_BUFFER, 2, (uint64_t[]){r->dev, r->cbuf}, NULL, 0, 0);
    if (r->pool) vks_one(v, GPU_CMD_VK_DESTROY_COMMAND_POOL, 2, (uint64_t[]){r->dev, r->pool}, NULL, 0, 0);
    if (r->dev) vks_one(v, GPU_CMD_VK_DESTROY_DEVICE, 2, (uint64_t[]){r->dev, 0xFFFE}, NULL, 0, 0);
    if (r->instance) vks_one(v, GPU_CMD_VK_DESTROY_INSTANCE, 2, (uint64_t[]){r->instance, 0xFFFE}, NULL, 0, 0);
    (void)z;
}

static uint32_t vk_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ 0xFFFFFFFFu;
}

static int vk_make_image(struct vks *v, uint8_t *pkbuf, uint32_t pkcap, uint64_t dev,
                         const VkPhysicalDeviceMemoryProperties *mp, uint32_t w, uint32_t h,
                         uint32_t format, uint32_t usage, uint32_t aspect, uint64_t *image,
                         uint64_t *mem, uint64_t *view) {
    struct vkp pk;
    vkp_init(&pk, pkbuf, pkcap);
    uint32_t imci_off;
    VkImageCreateInfo *imci = vkp_alloc(&pk, VK_S_VKIMAGECREATEINFO, sizeof(VkImageCreateInfo), &imci_off);
    imci->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imci->imageType = VK_IMAGE_TYPE_2D;
    imci->format = format;
    imci->extent.width = w;
    imci->extent.height = h;
    imci->extent.depth = 1;
    imci->mipLevels = 1;
    imci->arrayLayers = 1;
    imci->samples = VK_SAMPLE_COUNT_1_BIT;
    imci->tiling = VK_IMAGE_TILING_OPTIMAL;
    imci->usage = usage;
    imci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t len = vkp_finish(&pk);
    if (vks_one(v, GPU_CMD_VK_CREATE_IMAGE, 3, (uint64_t[]){dev, imci_off, 0xFFFE}, pkbuf, len, 8))
        return 1;
    *image = vks_r64(v);
    if (!*image) {
        snprintf(v->err, sizeof(v->err), "vkCreateImage(format %u) failed", format);
        return 1;
    }

    VkMemoryRequirements reqs;
    if (vks_one(v, GPU_CMD_VK_GET_IMAGE_MEMORY_REQUIREMENTS, 3, (uint64_t[]){dev, *image, 0}, NULL, 0,
                sizeof(reqs)))
        return 1;
    memcpy(&reqs, v->rbuf, sizeof(reqs));
    uint32_t type = vk_pick_mem(mp, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == 0xFFFFFFFFu) {
        snprintf(v->err, sizeof(v->err), "no memory type for image (bits 0x%x)", reqs.memoryTypeBits);
        return 1;
    }
    vkp_init(&pk, pkbuf, pkcap);
    uint32_t mai_off;
    VkMemoryAllocateInfo *mai = vkp_alloc(&pk, VK_S_VKMEMORYALLOCATEINFO, sizeof(VkMemoryAllocateInfo), &mai_off);
    mai->sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai->allocationSize = reqs.size;
    mai->memoryTypeIndex = type;
    len = vkp_finish(&pk);
    if (vks_one(v, GPU_CMD_VK_ALLOCATE_MEMORY, 3, (uint64_t[]){dev, mai_off, 0xFFFE}, pkbuf, len, 8))
        return 1;
    *mem = vks_r64(v);
    if (!*mem) {
        snprintf(v->err, sizeof(v->err), "vkAllocateMemory(image) failed");
        return 1;
    }
    if (vks_one(v, GPU_CMD_VK_BIND_IMAGE_MEMORY, 4, (uint64_t[]){dev, *image, *mem, 0}, NULL, 0, 0))
        return 1;

    vkp_init(&pk, pkbuf, pkcap);
    uint32_t ivci_off;
    VkImageViewCreateInfo *ivci = vkp_alloc(&pk, VK_S_VKIMAGEVIEWCREATEINFO, sizeof(VkImageViewCreateInfo), &ivci_off);
    ivci->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci->image = *image;
    ivci->viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci->format = format;
    ivci->subresourceRange.aspectMask = aspect;
    ivci->subresourceRange.levelCount = 1;
    ivci->subresourceRange.layerCount = 1;
    len = vkp_finish(&pk);
    if (vks_one(v, GPU_CMD_VK_CREATE_IMAGE_VIEW, 3, (uint64_t[]){dev, ivci_off, 0xFFFE}, pkbuf, len, 8))
        return 1;
    *view = vks_r64(v);
    if (!*view) {
        snprintf(v->err, sizeof(v->err), "vkCreateImageView(format %u) failed", format);
        return 1;
    }
    return 0;
}

#define VKZ_BUFS 3
#define VKZ_TIMEOUT_MS 20000

#define VKZ_FAIL(what)                                                        \
    do {                                                                      \
        if (!v->err[0]) snprintf(v->err, sizeof(v->err), "%s", what);         \
        goto fail;                                                            \
    } while (0)

struct vkzc {
    int n;
    int cur;
    uint32_t w, h;
    uint64_t pass;
    uint64_t image[VKZ_BUFS], mem[VKZ_BUFS], view[VKZ_BUFS], fb[VKZ_BUFS];
    int slot[VKZ_BUFS];
};

static void vkz_destroy(struct vks *v, uint64_t dev, struct vkzc *z) {
    for (int i = 0; i < VKZ_BUFS; i++) {
        if (z->fb[i]) vks_one(v, GPU_CMD_VK_DESTROY_FRAMEBUFFER, 2, (uint64_t[]){dev, z->fb[i]}, NULL, 0, 0);
        if (z->view[i]) vks_one(v, GPU_CMD_VK_DESTROY_IMAGE_VIEW, 2, (uint64_t[]){dev, z->view[i]}, NULL, 0, 0);
        if (z->image[i]) vks_one(v, GPU_CMD_VK_DESTROY_IMAGE, 2, (uint64_t[]){dev, z->image[i]}, NULL, 0, 0);
        if (z->mem[i]) vks_one(v, GPU_CMD_VK_FREE_MEMORY, 2, (uint64_t[]){dev, z->mem[i]}, NULL, 0, 0);
    }
    if (z->pass) vks_one(v, GPU_CMD_VK_DESTROY_RENDER_PASS, 2, (uint64_t[]){dev, z->pass}, NULL, 0, 0);
    memset(z, 0, sizeof(*z));
}

static int vkz_open_node(int dev_idx) {
    const struct gpu_device *pd = gpu_get_device(dev_idx);
    char node[24] = "";
    if (pd && pd->render[0])
        snprintf(node, sizeof(node), "/dev/dri/%s", pd->render);
    else if (pd && pd->card[0])
        snprintf(node, sizeof(node), "/dev/dri/%s", pd->card);
    if (!node[0]) return -1;
    struct mesa_dc *d = mesa_dc_new();
    if (!d) return -1;
    uint32_t path_off = mesa_dc_in(d, node, (uint32_t)strlen(node) + 1);
    mesa_dc_call(d, "open", 2, (uint64_t[]){path_off, 0x80002}, 1u << 0, 0);
    uint64_t rets[1] = {(uint64_t)-1};
    int r = mesa_dc_run(d, rets, 1, NULL, 0, VKZ_TIMEOUT_MS);
    mesa_dc_free(d);
    return r || (int64_t)rets[0] < 0 ? -1 : (int)rets[0];
}

static void vkz_close_node(int fd) {
    if (fd < 0) return;
    struct mesa_dc *d = mesa_dc_new();
    if (!d) return;
    mesa_dc_call(d, "close", 1, (uint64_t[]){(uint64_t)fd}, 0, 0);
    mesa_dc_run(d, NULL, 0, NULL, 0, VKZ_TIMEOUT_MS);
    mesa_dc_free(d);
}

static int vkz_export(struct vks *v, int drm_fd, uint64_t dev, uint64_t mem, uint64_t *getfd, uint32_t *gem) {
    uint64_t rets[2];
    struct mesa_dc *d;
    if (!*getfd) {
        d = mesa_dc_new();
        if (!d) return 1;
        uint32_t name = mesa_dc_in(d, "vkGetMemoryFdKHR", 17);
        mesa_dc_call(d, "vkGetDeviceProcAddr", 2, (uint64_t[]){dev, name}, 1u << 1, 0);
        int r = mesa_dc_run(d, rets, 1, NULL, 0, VKZ_TIMEOUT_MS);
        mesa_dc_free(d);
        if (r || !rets[0]) {
            snprintf(v->err, sizeof(v->err), "no vkGetMemoryFdKHR");
            return 1;
        }
        *getfd = rets[0];
    }
    struct VkMemoryGetFdInfoKHR info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = mem;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    d = mesa_dc_new();
    if (!d) return 1;
    uint32_t info_off = mesa_dc_in(d, &info, sizeof(info));
    uint32_t fd_off = mesa_dc_out(d, 8);
    mesa_dc_fnptr(d, 4, (uint64_t[]){*getfd, dev, info_off, fd_off}, (1u << 2) | (1u << 3), 0);
    int32_t fd_out[2] = {-1, 0};
    int r = mesa_dc_run(d, rets, 1, fd_out, sizeof(fd_out), VKZ_TIMEOUT_MS);
    mesa_dc_free(d);
    if (r || rets[0] || fd_out[0] < 0) {
        snprintf(v->err, sizeof(v->err), "vkGetMemoryFdKHR failed (%d)", (int)rets[0]);
        return 1;
    }
    d = mesa_dc_new();
    if (!d) return 1;
    uint32_t h_off = mesa_dc_out(d, 8);
    mesa_dc_call(d, "drmPrimeFDToHandle", 3, (uint64_t[]){(uint64_t)drm_fd, (uint64_t)fd_out[0], h_off},
                 1u << 2, 0);
    mesa_dc_call(d, "close", 1, (uint64_t[]){(uint64_t)fd_out[0]}, 0, 0);
    uint32_t h_out[2] = {0, 0};
    r = mesa_dc_run(d, rets, 2, h_out, sizeof(h_out), VKZ_TIMEOUT_MS);
    mesa_dc_free(d);
    if (r || rets[0] || !h_out[0]) {
        snprintf(v->err, sizeof(v->err), "drmPrimeFDToHandle failed (%d)", (int)rets[0]);
        return 1;
    }
    *gem = h_out[0];
    return 0;
}

static int vkz_images(struct vks *v, uint8_t *pkbuf, uint32_t pkcap, const struct mesa_card_ctx *mc, uint64_t dev,
                      const VkPhysicalDeviceMemoryProperties *mp, gpu_present *pt, struct vkzc *z) {
    struct vkp pk;
    uint64_t getfd = 0;
    void *host = NULL;
    int node = vkz_open_node(mc->dev_idx);
    if (node < 0) {
        snprintf(v->err, sizeof(v->err), "private DRM node");
        return 1;
    }
    z->w = pt->fb_w;
    z->h = pt->fb_h;
    for (int i = 0; i < VKZ_BUFS; i++) {
        z->slot[i] = -1;
        vkp_init(&pk, pkbuf, pkcap);
        uint32_t ext_off, imci_off;
        struct VkExternalMemoryImageCreateInfo *ext = vkp_alloc(&pk, VK_S_VKEXTERNALMEMORYIMAGECREATEINFO,
                                                                sizeof(*ext), &ext_off);
        ext->sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext->handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageCreateInfo *imci = vkp_alloc(&pk, VK_S_VKIMAGECREATEINFO, sizeof(VkImageCreateInfo), &imci_off);
        imci->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imci->pNext = (const void *)(uint64_t)ext_off;
        imci->imageType = VK_IMAGE_TYPE_2D;
        imci->format = VK_FORMAT_B8G8R8A8_UNORM;
        imci->extent.width = z->w;
        imci->extent.height = z->h;
        imci->extent.depth = 1;
        imci->mipLevels = 1;
        imci->arrayLayers = 1;
        imci->samples = VK_SAMPLE_COUNT_1_BIT;
        imci->tiling = VK_IMAGE_TILING_LINEAR;
        imci->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t len = vkp_finish(&pk);
        if (vks_one(v, GPU_CMD_VK_CREATE_IMAGE, 3, (uint64_t[]){dev, imci_off, 0xFFFE}, pkbuf, len, 8)) VKZ_FAIL("vkCreateImage");
        z->image[i] = vks_r64(v);
        if (!z->image[i]) VKZ_FAIL("linear external image");

        VkMemoryRequirements reqs;
        if (vks_one(v, GPU_CMD_VK_GET_IMAGE_MEMORY_REQUIREMENTS, 3, (uint64_t[]){dev, z->image[i], 0}, NULL, 0,
                    sizeof(reqs)))
            VKZ_FAIL("image memory requirements");
        memcpy(&reqs, v->rbuf, sizeof(reqs));
        uint32_t type = vk_pick_mem(mp, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (type == 0xFFFFFFFFu) VKZ_FAIL("device-local memory type");
        vkp_init(&pk, pkbuf, pkcap);
        uint32_t ded_off, exp_off, mai_off;
        struct VkMemoryDedicatedAllocateInfo *ded = vkp_alloc(&pk, VK_S_VKMEMORYDEDICATEDALLOCATEINFO,
                                                              sizeof(*ded), &ded_off);
        ded->sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded->image = z->image[i];
        struct VkExportMemoryAllocateInfo *exp = vkp_alloc(&pk, VK_S_VKEXPORTMEMORYALLOCATEINFO, sizeof(*exp),
                                                           &exp_off);
        exp->sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exp->pNext = (const void *)(uint64_t)ded_off;
        exp->handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryAllocateInfo *mai = vkp_alloc(&pk, VK_S_VKMEMORYALLOCATEINFO, sizeof(VkMemoryAllocateInfo),
                                              &mai_off);
        mai->sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai->pNext = (const void *)(uint64_t)exp_off;
        mai->allocationSize = reqs.size;
        mai->memoryTypeIndex = type;
        len = vkp_finish(&pk);
        if (vks_one(v, GPU_CMD_VK_ALLOCATE_MEMORY, 3, (uint64_t[]){dev, mai_off, 0xFFFE}, pkbuf, len, 16))
            VKZ_FAIL("vkAllocateMemory");
        z->mem[i] = vks_r64(v);
        if (!z->mem[i]) {
            int32_t res = 0;
            memcpy(&res, v->rbuf + 8, 4);
            snprintf(v->err, sizeof(v->err), "exportable memory (VkResult %d)", res);
            goto fail;
        }
        if (vks_one(v, GPU_CMD_VK_BIND_IMAGE_MEMORY, 4, (uint64_t[]){dev, z->image[i], z->mem[i], 0}, NULL, 0, 0))
            VKZ_FAIL("vkBindImageMemory");

        vkp_init(&pk, pkbuf, pkcap);
        uint32_t sub_off;
        struct VkImageSubresource *sub = vkp_alloc(&pk, VK_S_VKIMAGESUBRESOURCE, sizeof(*sub), &sub_off);
        sub->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        len = vkp_finish(&pk);
        struct VkSubresourceLayout lay;
        if (vks_one(v, GPU_CMD_VK_GET_IMAGE_SUBRESOURCE_LAYOUT, 4, (uint64_t[]){dev, z->image[i], sub_off, 0}, pkbuf,
                    len, sizeof(lay)))
            VKZ_FAIL("subresource layout");
        memcpy(&lay, v->rbuf, sizeof(lay));
        if (!lay.rowPitch || lay.offset) VKZ_FAIL("row pitch");

        vkp_init(&pk, pkbuf, pkcap);
        uint32_t ivci_off;
        VkImageViewCreateInfo *ivci = vkp_alloc(&pk, VK_S_VKIMAGEVIEWCREATEINFO, sizeof(VkImageViewCreateInfo),
                                                &ivci_off);
        ivci->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci->image = z->image[i];
        ivci->viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci->format = VK_FORMAT_B8G8R8A8_UNORM;
        ivci->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci->subresourceRange.levelCount = 1;
        ivci->subresourceRange.layerCount = 1;
        len = vkp_finish(&pk);
        if (vks_one(v, GPU_CMD_VK_CREATE_IMAGE_VIEW, 3, (uint64_t[]){dev, ivci_off, 0xFFFE}, pkbuf, len, 8))
            VKZ_FAIL("vkCreateImageView");
        z->view[i] = vks_r64(v);
        if (!z->view[i]) VKZ_FAIL("image view");

        uint32_t gem = 0;
        if (vkz_export(v, node, dev, z->mem[i], &getfd, &gem)) goto fail;
        if (!host) host = mesa_host_drm_file(node);
        if (!host) VKZ_FAIL("renderer DRM file");
        z->slot[i] = gpu_present_zc_add(pt, host, gem, (uint32_t)lay.rowPitch);
        if (z->slot[i] < 0) VKZ_FAIL("KMS framebuffer");
    }
    if (host) lkpi_file_put(host);
    vkz_close_node(node);
    return 0;

fail:
    if (host) lkpi_file_put(host);
    vkz_close_node(node);
    return 1;
}

static int vkz_pass_fbs(struct vks *v, uint8_t *pkbuf, uint32_t pkcap, uint64_t dev, uint64_t dview,
                        struct vkzc *z) {
    struct vkp pk;
    vkp_init(&pk, pkbuf, pkcap);
    uint32_t att_off;
    VkAttachmentDescription *att = vkp_alloc(&pk, VK_S_VKATTACHMENTDESCRIPTION,
                                             2 * sizeof(VkAttachmentDescription), &att_off);
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    att[1].format = VKKMV_DEPTH_FORMAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    uint32_t aref_off, dref_off;
    VkAttachmentReference *aref = vkp_alloc(&pk, VK_S_VKATTACHMENTREFERENCE, sizeof(VkAttachmentReference),
                                            &aref_off);
    aref->attachment = 0;
    aref->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference *dref = vkp_alloc(&pk, VK_S_VKATTACHMENTREFERENCE, sizeof(VkAttachmentReference),
                                            &dref_off);
    dref->attachment = 1;
    dref->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    uint32_t sub_off;
    VkSubpassDescription *sub = vkp_alloc(&pk, VK_S_VKSUBPASSDESCRIPTION, sizeof(VkSubpassDescription), &sub_off);
    sub->pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub->colorAttachmentCount = 1;
    sub->pColorAttachments = (const void *)(uint64_t)aref_off;
    sub->pDepthStencilAttachment = (const void *)(uint64_t)dref_off;
    uint32_t dep_off;
    VkSubpassDependency *dep = vkp_alloc(&pk, VK_S_VKSUBPASSDEPENDENCY, 2 * sizeof(VkSubpassDependency), &dep_off);
    dep[0].srcSubpass = 0xFFFFFFFFu;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = 0xFFFFFFFFu;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    uint32_t rpci_off;
    VkRenderPassCreateInfo *rpci = vkp_alloc(&pk, VK_S_VKRENDERPASSCREATEINFO, sizeof(VkRenderPassCreateInfo),
                                             &rpci_off);
    rpci->sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci->attachmentCount = 2;
    rpci->pAttachments = (const void *)(uint64_t)att_off;
    rpci->subpassCount = 1;
    rpci->pSubpasses = (const void *)(uint64_t)sub_off;
    rpci->dependencyCount = 2;
    rpci->pDependencies = (const void *)(uint64_t)dep_off;
    uint32_t len = vkp_finish(&pk);
    if (vks_one(v, GPU_CMD_VK_CREATE_RENDER_PASS, 3, (uint64_t[]){dev, rpci_off, 0xFFFE}, pkbuf, len, 8)) return 1;
    z->pass = vks_r64(v);
    if (!z->pass) return 1;
    for (int i = 0; i < VKZ_BUFS; i++) {
        vkp_init(&pk, pkbuf, pkcap);
        uint32_t fci_off;
        VkFramebufferCreateInfo *fci = vkp_alloc(&pk, VK_S_VKFRAMEBUFFERCREATEINFO, sizeof(VkFramebufferCreateInfo),
                                                 &fci_off);
        fci->sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci->renderPass = z->pass;
        fci->attachmentCount = 2;
        uint64_t views[2] = {z->view[i], dview};
        fci->pAttachments = (const void *)(uint64_t)vkp_put(&pk, views, sizeof(views));
        fci->width = z->w;
        fci->height = z->h;
        fci->layers = 1;
        len = vkp_finish(&pk);
        if (vks_one(v, GPU_CMD_VK_CREATE_FRAMEBUFFER, 3, (uint64_t[]){dev, fci_off, 0xFFFE}, pkbuf, len, 8)) return 1;
        z->fb[i] = vks_r64(v);
        if (!z->fb[i]) return 1;
    }
    z->n = VKZ_BUFS;
    return 0;
}

int gpu_vk_run_crc(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out,
                   uint32_t *crc_out) {
    int ms = mesa_start();
    if (ms) return ms;
    struct vks v;
    memset(&v, 0, sizeof(v));
    v.cl = gpu_client_open(opts->dev_idx >= 0 ? opts->dev_idx : 0, GPU_API_VULKAN);
    if (v.cl < 0) return GPU_ERR_BUSY;

    gpu_present pt;
    memset(&pt, 0, sizeof(pt));
    struct mesa_card_ctx mc;
    bool zc_try = !crc_out && mesa_card_context(&mc) && (opts->dev_idx < 0 || opts->dev_idx == mc.dev_idx);
    struct vkzc z;
    memset(&z, 0, sizeof(z));

    struct vkres r;
    memset(&r, 0, sizeof(r));
    uint32_t frames = 0;
    int rc = 0;
    uint8_t pkbuf[GPU_BATCH_PAYLOAD];
    uint8_t spkbuf[1024];
    struct vkp pk;
    struct vkp spk;

    float verts[DEMO_CUBE_FLOATS];
    dm_cube_build(verts);

    vkp_init(&pk, pkbuf, sizeof(pkbuf));
    uint32_t ai_off;
    VkApplicationInfo *ai = vkp_alloc(&pk, VK_S_VKAPPLICATIONINFO,
                                      sizeof(VkApplicationInfo), &ai_off);
    ai->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai->apiVersion = VK_API_VERSION_1_1;
    ai->pApplicationName = (const char *)(uint64_t)vkp_str(&pk, "LuOS vkkmv");
    uint32_t ici_off;
    VkInstanceCreateInfo *ici = vkp_alloc(&pk, VK_S_VKINSTANCECREATEINFO,
                                          sizeof(VkInstanceCreateInfo), &ici_off);
    ici->sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici->pApplicationInfo = (const void *)(uint64_t)ai_off;
    uint32_t ici_len = vkp_finish(&pk);

    if (vks_one(&v, GPU_CMD_VK_CREATE_INSTANCE, 3, (uint64_t[]){ici_off, 0xFFFE, 0}, pkbuf, ici_len, 8)) rc = 1;
    if (!rc) r.instance = vks_r64(&v);
    if (!rc && !r.instance) { snprintf(v.err, sizeof(v.err), "vkCreateInstance null"); rc = 1; }

    if (!rc) {
        if (vks_one(&v, GPU_CMD_VK_ENUMERATE_PHYSICAL_DEVICES, 3,
                    (uint64_t[]){r.instance, 0, 0xFFFE}, NULL, 0, 8))
            rc = 1;
        else {
            uint32_t count = (uint32_t)vks_r64(&v);
            if (!count) {
                snprintf(v.err, sizeof(v.err), "no Vulkan ICD devices");
                rc = 1;
            } else {
                uint8_t pl[16];
                memset(pl, 0, sizeof(pl));
                memcpy(pl + 8, &count, 4);
                if (vks_one(&v, GPU_CMD_VK_ENUMERATE_PHYSICAL_DEVICES, 3,
                            (uint64_t[]){r.instance, 0, 0}, pl, 12, 16))
                    rc = 1;
                else {
                    memcpy(&r.pdev, v.rbuf + 4, 8);
                }
            }
        }
    }

    VkPhysicalDeviceMemoryProperties mprops;
    if (!rc) {
        if (vks_one(&v, GPU_CMD_VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES, 2,
                    (uint64_t[]){r.pdev, 0}, NULL, 0, sizeof(mprops)))
            rc = 1;
        else
            memcpy(&mprops, v.rbuf, sizeof(mprops));
    }
    const uint32_t host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!rc) {
        if (vk_pick_mem(&mprops, 0xFFFFFFFFu, host_flags, host_flags) == 0xFFFFFFFFu) {
            snprintf(v.err, sizeof(v.err), "no host memory type");
            LOG_WARNING("vkkmv: mprops dump typeCount=%u heapCount=%u first16=%02x%02x%02x%02x %02x%02x%02x%02x",
                        mprops.memoryTypeCount, mprops.memoryHeapCount,
                        ((uint8_t *)&mprops)[0], ((uint8_t *)&mprops)[1],
                        ((uint8_t *)&mprops)[2], ((uint8_t *)&mprops)[3],
                        ((uint8_t *)&mprops)[4], ((uint8_t *)&mprops)[5],
                        ((uint8_t *)&mprops)[6], ((uint8_t *)&mprops)[7]);
            rc = 1;
        }
    }

    for (int attempt = 0; !rc && attempt < 2 && !r.dev; attempt++) {
        if (attempt) zc_try = false;
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        float prio = 1.0f;
        uint32_t prio_off = vkp_put(&pk, &prio, 4);
        uint32_t qi_off;
        VkDeviceQueueCreateInfo *qi = vkp_alloc(&pk, VK_S_VKDEVICEQUEUECREATEINFO,
                                                sizeof(VkDeviceQueueCreateInfo), &qi_off);
        qi->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi->queueFamilyIndex = 0;
        qi->queueCount = 1;
        qi->pQueuePriorities = (const float *)(uint64_t)prio_off;
        uint32_t dci_off;
        VkDeviceCreateInfo *dci = vkp_alloc(&pk, VK_S_VKDEVICECREATEINFO,
                                            sizeof(VkDeviceCreateInfo), &dci_off);
        dci->sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci->queueCreateInfoCount = 1;
        dci->pQueueCreateInfos = (const void *)(uint64_t)qi_off;
        if (zc_try) {
            static const char *const exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
            dci->enabledExtensionCount = 2;
            dci->ppEnabledExtensionNames = (const void *)(uint64_t)vkp_strv(&pk, exts, 2);
        }
        uint32_t dlen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_DEVICE, 3, (uint64_t[]){r.pdev, dci_off, 0xFFFE}, pkbuf, dlen, 8))
            rc = 1;
        else
            r.dev = vks_r64(&v);
        if (!rc && !r.dev && zc_try) LOG_INFO("vkkmv: no dma-buf export on this Vulkan device, using the CPU copy path");
    }

    if (!rc) {
        if (vks_one(&v, GPU_CMD_VK_GET_DEVICE_QUEUE, 4, (uint64_t[]){r.dev, 0, 0, 0}, NULL, 0, 8))
            rc = 1;
        else
            r.queue = vks_r64(&v);
    }
    if (!rc && !r.queue) { snprintf(v.err, sizeof(v.err), "no queue"); rc = 1; }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t cpi_off;
        VkCommandPoolCreateInfo *cpi = vkp_alloc(&pk, VK_S_VKCOMMANDPOOLCREATEINFO,
                                                    sizeof(VkCommandPoolCreateInfo), &cpi_off);
        cpi->sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi->queueFamilyIndex = 0;
        cpi->flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        uint32_t clen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_COMMAND_POOL, 3, (uint64_t[]){r.dev, cpi_off, 0xFFFE}, pkbuf, clen, 8))
            rc = 1;
        else
            r.pool = vks_r64(&v);
    }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t cai_off;
        VkCommandBufferAllocateInfo *cai = vkp_alloc(&pk, VK_S_VKCOMMANDBUFFERALLOCATEINFO,
                                                      sizeof(VkCommandBufferAllocateInfo), &cai_off);
        cai->sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai->commandPool = r.pool;
        cai->level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai->commandBufferCount = 1;
        uint32_t clen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_ALLOCATE_COMMAND_BUFFERS, 3, (uint64_t[]){r.dev, cai_off, 0}, pkbuf,
                    clen, 8))
            rc = 1;
        else
            r.cmdbuf = vks_r64(&v);
    }
    if (!rc && !r.cmdbuf) { snprintf(v.err, sizeof(v.err), "no command buffer"); rc = 1; }

    VkMemoryRequirements reqs;
    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t bci_off;
        VkBufferCreateInfo *bci = vkp_alloc(&pk, VK_S_VKBUFFERCREATEINFO,
                                                sizeof(VkBufferCreateInfo), &bci_off);
        bci->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci->size = sizeof(verts);
        bci->usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        uint32_t blen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_BUFFER, 3, (uint64_t[]){r.dev, bci_off, 0xFFFE}, pkbuf, blen, 8))
            rc = 1;
        else
            r.vbuf = vks_r64(&v);
    }
    if (!rc && !r.vbuf) { snprintf(v.err, sizeof(v.err), "no vertex buffer"); rc = 1; }

    if (!rc) {
        if (vks_one(&v, GPU_CMD_VK_GET_BUFFER_MEMORY_REQUIREMENTS, 3,
                    (uint64_t[]){r.dev, r.vbuf, 0}, NULL, 0, sizeof(reqs)))
            rc = 1;
        else
            memcpy(&reqs, v.rbuf, sizeof(reqs));
    }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t mai_off;
        VkMemoryAllocateInfo *mai = vkp_alloc(&pk, VK_S_VKMEMORYALLOCATEINFO,
                                                sizeof(VkMemoryAllocateInfo), &mai_off);
        mai->sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai->allocationSize = reqs.size;
        mai->memoryTypeIndex = vk_pick_mem(&mprops, reqs.memoryTypeBits, host_flags, host_flags);
        uint32_t mlen = vkp_finish(&pk);
        if (mai->memoryTypeIndex == 0xFFFFFFFFu) {
            snprintf(v.err, sizeof(v.err), "no host memory for the vertex buffer");
            rc = 1;
        } else if (vks_one(&v, GPU_CMD_VK_ALLOCATE_MEMORY, 3, (uint64_t[]){r.dev, mai_off, 0xFFFE}, pkbuf, mlen, 8))
            rc = 1;
        else
            r.vmem = vks_r64(&v);
    }
    if (!rc) {
        if (vks_one(&v, GPU_CMD_VK_BIND_BUFFER_MEMORY, 4,
                    (uint64_t[]){r.dev, r.vbuf, r.vmem, 0}, NULL, 0, 0))
            rc = 1;
    }
    if (!rc) {
        if (vks_one(&v, GPU_CMD_VK_MAP_MEMORY, 6,
                    (uint64_t[]){r.dev, r.vmem, 0, reqs.size, 0, 0}, NULL, 0, 8))
            rc = 1;
    }
    if (!rc) {
        if (vks_one(&v, GPU_CMD_MESA_TRACK_POKE, 3,
                    (uint64_t[]){0, 0, sizeof(verts)}, verts, sizeof(verts), 0))
            rc = 1;
    }

    if (!rc) {
        if (gpu_present_open(&pt, opts, opts->width, opts->height)) {
            vk_destroy(&v, &r);
            gpu_client_close(v.cl);
            return GPU_ERR_NOT_STARTED;
        }
        pt.src_bgra = true;
        zc_try = zc_try && pt.kms && mc.dev_idx == pt.dev_idx;
    }

    uint32_t saved_w = pt.width, saved_h = pt.height;
    if (!rc && zc_try) {
        if (vkz_images(&v, pkbuf, sizeof(pkbuf), &mc, r.dev, &mprops, &pt, &z)) {
            LOG_WARNING("vkkmv: scanout images unavailable (%s), using the CPU copy path", v.err);
            vkz_destroy(&v, r.dev, &z);
            gpu_present_zc_reset(&pt);
            v.err[0] = '\0';
        } else {
            pt.width = z.w;
            pt.height = z.h;
        }
    }
    if (!rc && vk_make_image(&v, pkbuf, sizeof(pkbuf), r.dev, &mprops, pt.width, pt.height,
                             VK_FORMAT_B8G8R8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &r.image, &r.imem, &r.iview))
        rc = 1;
    if (!rc && vk_make_image(&v, pkbuf, sizeof(pkbuf), r.dev, &mprops, pt.width, pt.height,
                             VKKMV_DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, &r.dimage, &r.dmem, &r.dview))
        rc = 1;
    if (!rc && z.w) {
        if (vkz_pass_fbs(&v, pkbuf, sizeof(pkbuf), r.dev, r.dview, &z)) {
            LOG_WARNING("vkkmv: scanout framebuffers unavailable (%s), using the CPU copy path", v.err);
            vkz_destroy(&v, r.dev, &z);
            gpu_present_zc_reset(&pt);
            v.err[0] = '\0';
            pt.width = saved_w;
            pt.height = saved_h;
        } else {
            LOG_INFO("present: Vulkan renders straight into %d scanout images %ux%u, no CPU copy", z.n, z.w, z.h);
        }
    }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t att_off;
        VkAttachmentDescription *att = vkp_alloc(&pk, VK_S_VKATTACHMENTDESCRIPTION,
                                                 2 * sizeof(VkAttachmentDescription), &att_off);
        att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        att[1].format = VKKMV_DEPTH_FORMAT;
        att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        uint32_t aref_off, dref_off;
        VkAttachmentReference *aref = vkp_alloc(&pk, VK_S_VKATTACHMENTREFERENCE,
                                                sizeof(VkAttachmentReference), &aref_off);
        aref->attachment = 0;
        aref->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference *dref = vkp_alloc(&pk, VK_S_VKATTACHMENTREFERENCE,
                                                sizeof(VkAttachmentReference), &dref_off);
        dref->attachment = 1;
        dref->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        uint32_t sub_off;
        VkSubpassDescription *sub = vkp_alloc(&pk, VK_S_VKSUBPASSDESCRIPTION,
                                              sizeof(VkSubpassDescription), &sub_off);
        sub->pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub->colorAttachmentCount = 1;
        sub->pColorAttachments = (const void *)(uint64_t)aref_off;
        sub->pDepthStencilAttachment = (const void *)(uint64_t)dref_off;
        uint32_t dep_off;
        VkSubpassDependency *dep = vkp_alloc(&pk, VK_S_VKSUBPASSDEPENDENCY,
                                             2 * sizeof(VkSubpassDependency), &dep_off);
        dep[0].srcSubpass = 0xFFFFFFFFu;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = 0xFFFFFFFFu;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        uint32_t rpci_off;
        VkRenderPassCreateInfo *rpci = vkp_alloc(&pk, VK_S_VKRENDERPASSCREATEINFO,
                                                  sizeof(VkRenderPassCreateInfo), &rpci_off);
        rpci->sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci->attachmentCount = 2;
        rpci->pAttachments = (const void *)(uint64_t)att_off;
        rpci->subpassCount = 1;
        rpci->pSubpasses = (const void *)(uint64_t)sub_off;
        rpci->dependencyCount = 2;
        rpci->pDependencies = (const void *)(uint64_t)dep_off;
        uint32_t rlen2 = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_RENDER_PASS, 3, (uint64_t[]){r.dev, rpci_off, 0xFFFE}, pkbuf, rlen2, 8))
            rc = 1;
        else
            r.pass = vks_r64(&v);
    }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t fci_off;
        VkFramebufferCreateInfo *fci = vkp_alloc(&pk, VK_S_VKFRAMEBUFFERCREATEINFO,
                                                      sizeof(VkFramebufferCreateInfo), &fci_off);
        fci->sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci->renderPass = r.pass;
        fci->attachmentCount = 2;
        uint64_t views[2] = {r.iview, r.dview};
        uint32_t views_off = vkp_put(&pk, views, sizeof(views));
        fci->pAttachments = (const void *)(uint64_t)views_off;
        fci->width = pt.width;
        fci->height = pt.height;
        fci->layers = 1;
        uint32_t flen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_FRAMEBUFFER, 3, (uint64_t[]){r.dev, fci_off, 0xFFFE}, pkbuf, flen, 8))
            rc = 1;
        else
            r.fb = vks_r64(&v);
    }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t smci_off;
        VkShaderModuleCreateInfo *smci = vkp_alloc(&pk, VK_S_VKSHADERMODULECREATEINFO,
                                                      sizeof(VkShaderModuleCreateInfo), &smci_off);
        smci->sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci->codeSize = sizeof(vk_vert_spv);
        smci->pCode = (const void *)(uint64_t)vkp_put(&pk, vk_vert_spv, sizeof(vk_vert_spv));
        uint32_t slen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_SHADER_MODULE, 3, (uint64_t[]){r.dev, smci_off, 0xFFFE}, pkbuf, slen, 8))
            rc = 1;
        else
            r.vsmod = vks_r64(&v);
    }
    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t smci_off;
        VkShaderModuleCreateInfo *smci = vkp_alloc(&pk, VK_S_VKSHADERMODULECREATEINFO,
                                                      sizeof(VkShaderModuleCreateInfo), &smci_off);
        smci->sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci->codeSize = sizeof(vk_frag_spv);
        smci->pCode = (const void *)(uint64_t)vkp_put(&pk, vk_frag_spv, sizeof(vk_frag_spv));
        uint32_t slen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_SHADER_MODULE, 3, (uint64_t[]){r.dev, smci_off, 0xFFFE}, pkbuf, slen, 8))
            rc = 1;
        else
            r.fsmod = vks_r64(&v);
    }
    if ((!rc) && (!r.vsmod || !r.fsmod)) { snprintf(v.err, sizeof(v.err), "shader modules failed"); rc = 1; }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t pcr_off;
        VkPushConstantRange *pcr = vkp_alloc(&pk, VK_S_VKPUSHCONSTANTRANGE,
                                             sizeof(VkPushConstantRange), &pcr_off);
        pcr->stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr->offset = 0;
        pcr->size = 64;
        uint32_t plci_off;
        VkPipelineLayoutCreateInfo *plci = vkp_alloc(&pk, VK_S_VKPIPELINELAYOUTCREATEINFO,
                                                      sizeof(VkPipelineLayoutCreateInfo), &plci_off);
        plci->sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci->pushConstantRangeCount = 1;
        plci->pPushConstantRanges = (const void *)(uint64_t)pcr_off;
        uint32_t plen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_PIPELINE_LAYOUT, 3, (uint64_t[]){r.dev, plci_off, 0xFFFE}, pkbuf,
                    plen, 8))
            rc = 1;
        else
            r.layout = vks_r64(&v);
    }

    if (!rc) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t vs_off, fs_off;
        VkPipelineShaderStageCreateInfo *vss = vkp_alloc(
            &pk, VK_S_VKPIPELINESHADERSTAGECREATEINFO, sizeof(VkPipelineShaderStageCreateInfo), &vs_off);
        vss->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vss->stage = VK_SHADER_STAGE_VERTEX_BIT;
        vss->module = r.vsmod;
        VkPipelineShaderStageCreateInfo *fss = vkp_alloc(
            &pk, VK_S_VKPIPELINESHADERSTAGECREATEINFO, sizeof(VkPipelineShaderStageCreateInfo), &fs_off);
        fss->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fss->stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fss->module = r.fsmod;
        uint32_t name_off = vkp_str(&pk, "main");
        vss->pName = (const void *)(uint64_t)name_off;
        fss->pName = (const void *)(uint64_t)name_off;

        uint32_t vbind_off, vattr0_off, vattr1_off;
        VkVertexInputBindingDescription *vbind = vkp_alloc(
            &pk, VK_S_VKVERTEXINPUTBINDINGDESCRIPTION, sizeof(VkVertexInputBindingDescription),
            &vbind_off);
        vbind->binding = 0;
        vbind->stride = 24;
        vbind->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription *va0 = vkp_alloc(
            &pk, VK_S_VKVERTEXINPUTATTRIBUTEDESCRIPTION, sizeof(VkVertexInputAttributeDescription),
            &vattr0_off);
        va0->location = 0;
        va0->binding = 0;
        va0->format = VK_FORMAT_R32G32B32_SFLOAT;
        va0->offset = 0;
        VkVertexInputAttributeDescription *va1 = vkp_alloc(
            &pk, VK_S_VKVERTEXINPUTATTRIBUTEDESCRIPTION, sizeof(VkVertexInputAttributeDescription),
            &vattr1_off);
        va1->location = 1;
        va1->binding = 0;
        va1->format = VK_FORMAT_R32G32B32_SFLOAT;
        va1->offset = 12;

        uint32_t vi_off;
        VkPipelineVertexInputStateCreateInfo *vis = vkp_alloc(
            &pk, VK_S_VKPIPELINEVERTEXINPUTSTATECREATEINFO,
            sizeof(VkPipelineVertexInputStateCreateInfo), &vi_off);
        vis->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis->vertexBindingDescriptionCount = 1;
        vis->pVertexBindingDescriptions = (const void *)(uint64_t)vbind_off;
        vis->vertexAttributeDescriptionCount = 2;
        vis->pVertexAttributeDescriptions = (const void *)(uint64_t)vattr0_off;

        uint32_t ia_off;
        VkPipelineInputAssemblyStateCreateInfo *ias = vkp_alloc(
            &pk, VK_S_VKPIPELINEINPUTASSEMBLYSTATECREATEINFO,
            sizeof(VkPipelineInputAssemblyStateCreateInfo), &ia_off);
        ias->sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        uint32_t vp_off, sc_off, vs_off2;
        VkViewport *vport = vkp_alloc(&pk, VK_S_VKVIEWPORT, sizeof(VkViewport), &vp_off);
        vport->x = 0;
        vport->y = 0;
        vport->width = (float)pt.width;
        vport->height = (float)pt.height;
        vport->minDepth = 0.0f;
        vport->maxDepth = 1.0f;
        VkRect2D *scis = vkp_alloc(&pk, VK_S_VKRECT2D, sizeof(VkRect2D), &sc_off);
        scis->offset.x = 0;
        scis->offset.y = 0;
        scis->extent.width = pt.width;
        scis->extent.height = pt.height;
        VkPipelineViewportStateCreateInfo *vps = vkp_alloc(
            &pk, VK_S_VKPIPELINEVIEWPORTSTATECREATEINFO,
            sizeof(VkPipelineViewportStateCreateInfo), &vs_off2);
        vps->sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps->viewportCount = 1;
        vps->pViewports = (const void *)(uint64_t)vp_off;
        vps->scissorCount = 1;
        vps->pScissors = (const void *)(uint64_t)sc_off;

        uint32_t rs_off;
        VkPipelineRasterizationStateCreateInfo *rss = vkp_alloc(
            &pk, VK_S_VKPIPELINERASTERIZATIONSTATECREATEINFO,
            sizeof(VkPipelineRasterizationStateCreateInfo), &rs_off);
        rss->sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rss->polygonMode = VK_POLYGON_MODE_FILL;
        rss->cullMode = VK_CULL_MODE_NONE;
        rss->frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rss->lineWidth = 1.0f;

        uint32_t ms_off;
        VkPipelineMultisampleStateCreateInfo *mss = vkp_alloc(
            &pk, VK_S_VKPIPELINEMULTISAMPLESTATECREATEINFO,
            sizeof(VkPipelineMultisampleStateCreateInfo), &ms_off);
        mss->sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        mss->rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        mss->minSampleShading = 1.0f;

        uint32_t cb_off, cba_off;
        VkPipelineColorBlendAttachmentState *cba = vkp_alloc(
            &pk, VK_S_VKPIPELINECOLORBLENDATTACHMENTSTATE,
            sizeof(VkPipelineColorBlendAttachmentState), &cba_off);
        cba->blendEnable = 0;
        cba->colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo *cbs = vkp_alloc(
            &pk, VK_S_VKPIPELINECOLORBLENDSTATECREATEINFO,
            sizeof(VkPipelineColorBlendStateCreateInfo), &cb_off);
        cbs->sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs->attachmentCount = 1;
        cbs->pAttachments = (const void *)(uint64_t)cba_off;

        uint32_t ds_off;
        VkPipelineDepthStencilStateCreateInfo *dss = vkp_alloc(
            &pk, VK_S_VKPIPELINEDEPTHSTENCILSTATECREATEINFO,
            sizeof(VkPipelineDepthStencilStateCreateInfo), &ds_off);
        dss->sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dss->depthTestEnable = 1;
        dss->depthWriteEnable = 1;
        dss->depthCompareOp = VK_COMPARE_OP_LESS;
        dss->maxDepthBounds = 1.0f;

        uint32_t stages_off = vs_off;
        uint32_t gpci_off;
        VkGraphicsPipelineCreateInfo *gpci = vkp_alloc(&pk, VK_S_VKGRAPHICSPIPELINECREATEINFO,
                                                       sizeof(VkGraphicsPipelineCreateInfo), &gpci_off);
        gpci->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci->stageCount = 2;
        gpci->pStages = (const void *)(uint64_t)stages_off;
        gpci->pVertexInputState = (const void *)(uint64_t)vi_off;
        gpci->pInputAssemblyState = (const void *)(uint64_t)ia_off;
        gpci->pViewportState = (const void *)(uint64_t)vs_off2;
        gpci->pRasterizationState = (const void *)(uint64_t)rs_off;
        gpci->pMultisampleState = (const void *)(uint64_t)ms_off;
        gpci->pDepthStencilState = (const void *)(uint64_t)ds_off;
        gpci->pColorBlendState = (const void *)(uint64_t)cb_off;
        gpci->layout = r.layout;
        gpci->renderPass = r.pass;
        gpci->subpass = 0;
        uint32_t glen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_GRAPHICS_PIPELINES, 6,
                    (uint64_t[]){r.dev, 0, 1, gpci_off, 0xFFFE, 0}, pkbuf, glen, 8))
            rc = 1;
        else
            r.pipeline = vks_r64(&v);
    }
    if (!rc && !r.pipeline) { snprintf(v.err, sizeof(v.err), "pipeline creation failed"); rc = 1; }

    VkMemoryRequirements creqs;
    bool cmem_coherent = true;
    if (!rc && !z.n) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t bci_off;
        VkBufferCreateInfo *bci = vkp_alloc(&pk, VK_S_VKBUFFERCREATEINFO,
                                                sizeof(VkBufferCreateInfo), &bci_off);
        bci->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci->size = (uint64_t)pt.width * pt.height * 4;
        bci->usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        uint32_t blen = vkp_finish(&pk);
        if (vks_one(&v, GPU_CMD_VK_CREATE_BUFFER, 3, (uint64_t[]){r.dev, bci_off, 0xFFFE}, pkbuf, blen, 8))
            rc = 1;
        else
            r.cbuf = vks_r64(&v);
    }
    if (!rc && !z.n) {
        if (vks_one(&v, GPU_CMD_VK_GET_BUFFER_MEMORY_REQUIREMENTS, 3,
                    (uint64_t[]){r.dev, r.cbuf, 0}, NULL, 0, sizeof(creqs)))
            rc = 1;
        else
            memcpy(&creqs, v.rbuf, sizeof(creqs));
    }
    if (!rc && !z.n) {
        vkp_init(&pk, pkbuf, sizeof(pkbuf));
        uint32_t mai_off;
        VkMemoryAllocateInfo *mai = vkp_alloc(&pk, VK_S_VKMEMORYALLOCATEINFO,
                                                sizeof(VkMemoryAllocateInfo), &mai_off);
        mai->sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai->allocationSize = creqs.size;
        mai->memoryTypeIndex = vk_pick_mem(&mprops, creqs.memoryTypeBits,
                                           host_flags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mai->memoryTypeIndex == 0xFFFFFFFFu)
            mai->memoryTypeIndex = vk_pick_mem(&mprops, creqs.memoryTypeBits, host_flags, host_flags);
        if (mai->memoryTypeIndex != 0xFFFFFFFFu)
            cmem_coherent = (mprops.memoryTypes[mai->memoryTypeIndex].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        uint32_t mlen = vkp_finish(&pk);
        if (mai->memoryTypeIndex == 0xFFFFFFFFu) {
            snprintf(v.err, sizeof(v.err), "no host memory for the readback buffer");
            rc = 1;
        } else if (vks_one(&v, GPU_CMD_VK_ALLOCATE_MEMORY, 3, (uint64_t[]){r.dev, mai_off, 0xFFFE}, pkbuf, mlen, 8))
            rc = 1;
        else
            r.cmem = vks_r64(&v);
    }
    if (!rc && !z.n) {
        if (vks_one(&v, GPU_CMD_VK_BIND_BUFFER_MEMORY, 4,
                    (uint64_t[]){r.dev, r.cbuf, r.cmem, 0}, NULL, 0, 0))
            rc = 1;
    }
    if (!rc && !z.n) {
        if (vks_one(&v, GPU_CMD_VK_MAP_MEMORY, 6,
                    (uint64_t[]){r.dev, r.cmem, 0, creqs.size, 0, 0}, NULL, 0, 8))
            rc = 1;
    }

    uint32_t run_ms = opts->seconds ? opts->seconds * 1000 : GPU_CUBE_DEFAULT_SECONDS * 1000;
    uint64_t t0 = vk_msnow();
    if (!rc) {
        for (;;) {
            uint64_t now = vk_msnow();
            if (now - t0 >= run_ms) break;
            if (vks_abort()) break;
            float t = (float)((now - t0) / 1000.0);

            float mvp[16];
            dm_cube_mvp(mvp, t, pt.width, pt.height, 1);


            vkp_init(&pk, pkbuf, sizeof(pkbuf));
            uint32_t binfo_off;
            vkp_alloc(&pk, VK_S_VKCOMMANDBUFFERBEGININFO, sizeof(VkCommandBufferBeginInfo),
                      &binfo_off);
            uint32_t cb_off = vkp_put(&pk, &r.cmdbuf, 8);
            uint64_t vbinfo[2] = {r.vbuf, 0};
            uint32_t vb_off = vkp_put(&pk, vbinfo, 16);
            uint32_t vo_off = vkp_put(&pk, &(uint64_t){0}, 8);
            float clearv[8] = {0.05f, 0.05f, 0.08f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
            uint32_t cv_off = vkp_put(&pk, clearv, sizeof(clearv));
            uint32_t mvp_off = vkp_put(&pk, mvp, sizeof(mvp));
            VkBufferImageCopy copy;
            memset(&copy, 0, sizeof(copy));
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent.width = pt.width;
            copy.imageExtent.height = pt.height;
            copy.imageExtent.depth = 1;
            uint32_t copy_off = vkp_put(&pk, &copy, sizeof(copy));
            uint32_t rp_off;
            VkRenderPassBeginInfo *rpbi = vkp_alloc(&pk, VK_S_VKRENDERPASSBEGININFO,
                                                    sizeof(VkRenderPassBeginInfo), &rp_off);
            rpbi->sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbi->renderPass = z.n ? z.pass : r.pass;
            rpbi->framebuffer = z.n ? z.fb[z.cur] : r.fb;
            rpbi->renderArea.extent.width = pt.width;
            rpbi->renderArea.extent.height = pt.height;
            rpbi->clearValueCount = 2;
            rpbi->pClearValues = (const void *)(uint64_t)cv_off;
            uint32_t plen = vkp_finish(&pk);

            vkp_init(&spk, spkbuf, sizeof(spkbuf));
            uint32_t cbarr = vkp_put(&spk, &r.cmdbuf, 8);
            uint32_t si_off;
            VkSubmitInfo *si = vkp_alloc(&spk, VK_S_VKSUBMITINFO, sizeof(VkSubmitInfo), &si_off);
            si->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si->commandBufferCount = 1;
            si->pCommandBuffers = (const void *)(uint64_t)cbarr;
            uint32_t inv_off = 0;
            if (!cmem_coherent) {
                VkMappedMemoryRange *mr = vkp_alloc(&spk, VK_S_VKMAPPEDMEMORYRANGE, sizeof(VkMappedMemoryRange),
                                                    &inv_off);
                mr->sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                mr->memory = r.cmem;
                mr->offset = 0;
                mr->size = ~0ULL;
            }
            uint32_t slen = vkp_finish(&spk);

            if (gpu_cmd_begin(v.cl)) break;
            gpu_cmd_vk(v.cl, GPU_CMD_VK_RESET_COMMAND_BUFFER, 3,
                       (uint64_t[]){r.cmdbuf, 0, 0}, NULL, 0, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_BEGIN_COMMAND_BUFFER, 2,
                       (uint64_t[]){r.cmdbuf, binfo_off}, pkbuf, plen, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_BEGIN_RENDER_PASS, 3,
                       (uint64_t[]){r.cmdbuf, rp_off, 0}, pkbuf, plen, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_BIND_PIPELINE, 3,
                       (uint64_t[]){r.cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline}, NULL, 0, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_BIND_VERTEX_BUFFERS, 5,
                       (uint64_t[]){r.cmdbuf, 0, 1, vb_off, vo_off}, pkbuf, plen, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_PUSH_CONSTANTS, 6,
                       (uint64_t[]){r.cmdbuf, r.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp_off},
                       pkbuf, plen, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_DRAW, 5,
                       (uint64_t[]){r.cmdbuf, DEMO_CUBE_VERTS, 1, 0, 0}, NULL, 0, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_END_RENDER_PASS, 1,
                       (uint64_t[]){r.cmdbuf}, NULL, 0, 0);

            if (!z.n)
                gpu_cmd_vk(v.cl, GPU_CMD_VK_CMD_COPY_IMAGE_TO_BUFFER, 6,
                       (uint64_t[]){r.cmdbuf, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.cbuf, 1, copy_off}, pkbuf, plen, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_END_COMMAND_BUFFER, 1,
                       (uint64_t[]){r.cmdbuf}, NULL, 0, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_QUEUE_SUBMIT, 4,
                       (uint64_t[]){r.queue, 1, si_off, 0}, spkbuf, slen, 0);
            gpu_cmd_vk(v.cl, GPU_CMD_VK_QUEUE_WAIT_IDLE, 1, (uint64_t[]){r.queue}, NULL, 0, 0);
            if (!cmem_coherent && !z.n)
                gpu_cmd_vk(v.cl, GPU_CMD_VK_INVALIDATE_MAPPED_MEMORY_RANGES, 3,
                           (uint64_t[]){r.dev, 1, inv_off}, spkbuf, slen, 0);
            if (!z.n && gpu_cmd_gl(v.cl, GPU_CMD_MESA_TRACK_PIX, 3,
                                   (uint64_t[]){0, 0, (uint64_t)pt.width * pt.height * 4}, NULL, 0, 0)) {
                snprintf(v.err, sizeof(v.err), "frame batch overflow");
                break;
            }
            if (vks_flush(&v, 180000)) break;
            if (z.n) {
                if (gpu_present_zc_show(&pt, z.slot[z.cur])) {
                    snprintf(v.err, sizeof(v.err), "scanout flip");
                    break;
                }
                z.cur = (z.cur + 1) % z.n;
                frames++;
                continue;
            }

            if (crc_out) {
                *crc_out = vk_crc32(mesa_pix_data(), pt.width * pt.height * 4);
                crc_out = NULL;
            }
            gpu_present_frame(&pt, (const uint32_t *)mesa_pix_data(), pt.width, pt.height);
            frames++;
        }
    }

    uint64_t elapsed = vk_msnow() - t0;
    if (!rc && v.err[0]) {
        LOG_WARNING("vkkmv(Vulkan): frame %u failed: %s", frames, v.err);
        if (!frames) rc = 1;
    }
    vkz_destroy(&v, r.dev, &z);
    vk_destroy(&v, &r);
    gpu_present_close(&pt);
    if (frames_out) *frames_out = frames;
    if (fps_out) *fps_out = elapsed > 500 ? (uint32_t)(frames * 1000ULL / elapsed) : frames;
    if (rc && opts->progress) {
        char line[200];
        snprintf(line, sizeof(line), "  Vulkan path failed (%s) - see dmesg",
                 v.err);
        opts->progress(opts->user, line);
    }
    gpu_client_close(v.cl);
    if (rc) LOG_WARNING("vkkmv(Vulkan): %s", v.err);
    return rc ? 1 : GPU_OK;
}

int gpu_vk_run(const gpu_cube_opts *opts, uint32_t *frames_out, uint32_t *fps_out) {
    return gpu_vk_run_crc(opts, frames_out, fps_out, NULL);
}

int gpu_shader_test_vk(uint32_t *crc_out, char *note, int note_cap) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.seconds = 1;
    opts.width = 128;
    opts.height = 128;
    opts.progress = NULL;
    uint32_t frames = 0, fps = 0, crc = 0;
    int r = gpu_vk_run_crc(&opts, &frames, &fps, &crc);
    if (r == GPU_OK && frames) {
        if (crc_out) *crc_out = crc;
        if (note) snprintf(note, note_cap, "SPIR-V vertex+fragment pipeline, 128x128");
        return GPU_OK;
    }
    return r ? r : GPU_ERR_STATE;
}
