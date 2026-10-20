/* i810_drv.c -- I810 driver -*- linux-c -*-
 * Created: Mon Dec 13 01:56:22 1999 by jhartmann@precisioninsight.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Rickard E. (Rik) Faith <faith@valinux.com>
 *    Jeff Hartmann <jhartmann@valinux.com>
 *    Gareth Hughes <gareth@valinux.com>
 */

#include "i810_drv.h"

#include <linux/module.h>
#include <linux/pci.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/i810_drm.h>

static struct pci_device_id pciidlist[] = {
        { 0x8086, 0x7121, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0x8086, 0x7123, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0x8086, 0x7125, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0x8086, 0x1132, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0 },
};

static const struct file_operations i810_driver_fops = {
        .owner = THIS_MODULE,
        .open = drm_legacy_open,
        .release = drm_legacy_release,
        .unlocked_ioctl = drm_ioctl,
        .mmap = drm_legacy_mmap,
        .poll = drm_poll,
        .llseek = noop_llseek,
};

static struct drm_legacy_driver driver = {
        .lastclose = i810_driver_lastclose,
        .preclose = i810_driver_preclose,
        .dma_quiescent = i810_driver_dma_quiescent,
        .dev_priv_size = sizeof(drm_i810_buf_priv_t),
        .base = {
                .driver_features = 0,
                .load = i810_driver_load,
                .ioctls = i810_ioctls,
                .num_ioctls = 0,
                .fops = &i810_driver_fops,
                .name = DRIVER_NAME,
                .desc = DRIVER_DESC,
                .major = DRIVER_MAJOR,
                .minor = DRIVER_MINOR,
                .patchlevel = DRIVER_PATCHLEVEL,
        },
};

static struct pci_driver i810_pci_driver = {
        .name = DRIVER_NAME,
        .id_table = pciidlist,
};

static int __init i810_init(void)
{
        driver.base.num_ioctls = i810_max_ioctl;
        return drm_legacy_pci_init(&driver, &i810_pci_driver);
}

static void __exit i810_exit(void)
{
        drm_legacy_pci_exit(&driver, &i810_pci_driver);
}

module_init(i810_init);
module_exit(i810_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
