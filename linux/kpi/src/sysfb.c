// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <linux/platform_data/simplefb.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include "../../upstream/drivers/gpu/drm/drm_internal.h"

static DEFINE_MUTEX(sysfb_lock);
static struct platform_device *sysfb_pd;
static bool sysfb_disabled;

static struct device *sysfb_parent(void)
{
        struct pci_dev *pdev = lkpi_vga_default_device();
        if (pdev && luos_pci_is_boot_vga(pdev->luos_dev)) return &pdev->dev;
        return NULL;
}

struct sysfb_minor_scan {
        struct device *parent;
        bool found;
};

static void sysfb_minor_scan_fn(struct device *kdev, void *arg)
{
        struct sysfb_minor_scan *scan = arg;
        struct drm_minor *minor;
        if (scan->found || !kdev->type || !kdev->type->name || strcmp(kdev->type->name, "drm_minor")) return;
        minor = dev_get_drvdata(kdev);
        if (!minor || minor->type != DRM_MINOR_PRIMARY || minor->index != 0) return;
        if (minor->dev && minor->dev->dev == scan->parent) scan->found = true;
}

static bool sysfb_minor_alive(struct device *parent)
{
        struct sysfb_minor_scan scan = { .parent = parent };
        lkpi_for_each_device(sysfb_minor_scan_fn, &scan);
        return scan.found;
}

static void sysfb_wait_card_released(struct device *parent)
{
        u64 deadline = ktime_get() + 3 * NSEC_PER_SEC;
        while (sysfb_minor_alive(parent)) {
                if (ktime_get() > deadline) {
                        pr_warn("sysfb: the card of the removed firmware framebuffer is not released after 3 s\n");
                        return;
                }
                luos_yield();
        }
}

bool sysfb_disable(struct device *dev)
{
        struct device *parent;
        bool removed = false;
        mutex_lock(&sysfb_lock);
        parent = sysfb_parent();
        if (!dev || !parent || dev == parent) {
                if (sysfb_pd) {
                        struct device *fbdev = &sysfb_pd->dev;
                        platform_device_unregister(sysfb_pd);
                        sysfb_pd = NULL;
                        removed = true;
                        sysfb_wait_card_released(fbdev);
                }
                sysfb_disabled = true;
        }
        mutex_unlock(&sysfb_lock);
        return removed;
}

static const char *sysfb_format(const struct luos_boot_fb *fb)
{
        static const struct simplefb_format formats[] = SIMPLEFB_FORMATS;
        const char *with_alpha = NULL;
        for (size_t i = 0; i < ARRAY_SIZE(formats); i++) {
                const struct simplefb_format *f = &formats[i];
                if (f->bits_per_pixel != fb->bpp || f->red.offset != fb->red_shift || f->red.length != fb->red_size ||
                    f->green.offset != fb->green_shift || f->green.length != fb->green_size ||
                    f->blue.offset != fb->blue_shift || f->blue.length != fb->blue_size)
                        continue;
                if (!f->transp.length) return f->name;
                if (!with_alpha) with_alpha = f->name;
        }
        return with_alpha;
}

void lkpi_sysfb_init(void)
{
        struct luos_boot_fb fb;
        struct simplefb_platform_data pd;
        struct resource res;
        struct platform_device *pdev;
        const char *format;

        if (luos_boot_framebuffer(&fb)) return;
        mutex_lock(&sysfb_lock);
        if (sysfb_pd) {
                mutex_unlock(&sysfb_lock);
                return;
        }
        if (sysfb_disabled) {
                struct device *parent = sysfb_parent();
                if (!parent || parent->driver) {
                        mutex_unlock(&sysfb_lock);
                        pr_info("sysfb: the boot display has a native driver, no simple-framebuffer\n");
                        return;
                }
                pr_warn("sysfb: the native driver of the boot display did not bind, using the firmware framebuffer\n");
        }
        format = sysfb_format(&fb);
        if (!format) {
                mutex_unlock(&sysfb_lock);
                pr_warn("sysfb: boot framebuffer %ux%u, %u bpp, r%u@%u g%u@%u b%u@%u has no simplefb format\n", fb.width,
                        fb.height, fb.bpp, fb.red_size, fb.red_shift, fb.green_size, fb.green_shift, fb.blue_size,
                        fb.blue_shift);
                return;
        }
        pd = (struct simplefb_platform_data){
                .width = fb.width,
                .height = fb.height,
                .stride = fb.pitch,
                .format = format,
        };
        res = (struct resource)DEFINE_RES_MEM_NAMED(fb.phys, (resource_size_t)fb.pitch * fb.height, "BOOTFB");
        pr_info("sysfb: boot framebuffer %ux%u %s at %pa as simple-framebuffer.0\n", fb.width, fb.height, format,
                &res.start);
        pdev = platform_device_register_resndata(sysfb_parent(), "simple-framebuffer", 0, &res, 1, &pd, sizeof(pd));
        if (IS_ERR(pdev)) pr_warn("sysfb: cannot register simple-framebuffer: %ld\n", PTR_ERR(pdev));
        else sysfb_pd = pdev;
        mutex_unlock(&sysfb_lock);
}
