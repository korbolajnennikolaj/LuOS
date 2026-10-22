// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include "../../../src/kernel/lkpi/lkpi.h"

#include <drm/drm.h>
#include <drm/drm_auth.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode_config.h>

int lkpi_chrdev_open(unsigned int major, unsigned int minor, unsigned int flags, void **lx_file);
void lkpi_file_put(void *lx);
long lkpi_file_ioctl(void *lx, unsigned int cmd, unsigned long arg);

extern struct list_head lkpi_pci_devices;
extern struct list_head lkpi_pci_drivers;
void lkpi_pci_list_lock(void);
void lkpi_pci_list_unlock(void);

static void row_init(struct lkpi_gpu_devinfo *row)
{
        memset(row, 0, sizeof(*row));
        row->primary_minor = -1;
        row->render_minor = -1;
}


struct enum_ctx {
        struct lkpi_gpu_devinfo *rows;
        int max;
        int count;
        struct pci_dev **pci;
        struct lkpi_gpu_devinfo extra[8];
        int extra_count;
};

static void minor_fill(struct lkpi_gpu_devinfo *row, struct drm_minor *minor,
                       struct device *kdev)
{
        struct drm_device *ddev = minor->dev;

        if (minor->type == DRM_MINOR_PRIMARY) {
                if (row->primary_minor >= 0) return;
                row->primary_minor = minor->index;
                row->primary_devt = kdev->devt;
                snprintf(row->card, sizeof(row->card), "card%d", minor->index);
        } else {
                if (row->render_minor >= 0) return;
                row->render_minor = minor->index;
                row->render_devt = kdev->devt;
                snprintf(row->render, sizeof(row->render), "renderD%d", minor->index);
        }
        if (ddev->driver && ddev->driver->name[0])
                strscpy(row->driver, ddev->driver->name, sizeof(row->driver));
        row->num_crtcs = ddev->mode_config.num_crtc;
        row->num_connectors = ddev->mode_config.num_connector;
        row->modeset = drm_core_check_feature(ddev, DRIVER_MODESET);
        row->render_avail = drm_core_check_feature(ddev, DRIVER_RENDER);
}

static void minor_visit(struct device *kdev, void *arg)
{
        struct enum_ctx *ctx = arg;
        struct drm_minor *minor;
        struct device *parent;

        if (!kdev->type || !kdev->type->name || strcmp(kdev->type->name, "drm_minor")) return;
        minor = dev_get_drvdata(kdev);
        if (!minor || !minor->dev) return;
        if (minor->type != DRM_MINOR_PRIMARY && minor->type != DRM_MINOR_RENDER) return;
        parent = minor->dev->dev;

        if (dev_is_pci(parent)) {
                for (int i = 0; i < ctx->count; i++) {
                        if (&ctx->pci[i]->dev != parent) continue;
                        minor_fill(&ctx->rows[i], minor, kdev);
                        return;
                }
                return;
        }

        if (minor->type != DRM_MINOR_PRIMARY) return;
        for (int i = 0; i < ctx->extra_count; i++) {
                if (ctx->extra[i].primary_devt == kdev->devt) return;
        }
        if (ctx->extra_count >= (int)ARRAY_SIZE(ctx->extra)) return;
        struct lkpi_gpu_devinfo *row = &ctx->extra[ctx->extra_count++];
        row_init(row);
        minor_fill(row, minor, kdev);
}

static unsigned long vram_of(const struct pci_dev *pdev)
{
        unsigned long best = 0;
        for (int b = 0; b < 6; b++) {
                unsigned int flags = 0;
                unsigned long size = luos_pci_bar_info(pdev->luos_dev, b, &flags);
                if (!size || (flags & LUOS_BAR_IO) || !(flags & LUOS_BAR_PREFETCH)) continue;
                if (size > best) best = size;
        }
        return best;
}

int lkpi_gpu_enumerate(struct lkpi_gpu_devinfo *out, int max)
{
        struct enum_ctx ctx;
        struct pci_dev *d;
        struct pci_dev *refs[32];

        if (!out || max <= 0) return -EINVAL;
        memset(&ctx, 0, sizeof(ctx));
        ctx.rows = out;
        ctx.max = max;
        ctx.pci = refs;

        lkpi_pci_list_lock();
        list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                if ((d->class >> 16) != PCI_BASE_CLASS_DISPLAY) continue;
                if (ctx.count >= max || ctx.count >= (int)ARRAY_SIZE(refs)) break;
                refs[ctx.count] = pci_dev_get(d);
                struct lkpi_gpu_devinfo *row = &out[ctx.count];
                row_init(row);
                strscpy(row->pci_name, pci_name(d), sizeof(row->pci_name));
                row->vendor = d->vendor;
                row->device = d->device;
                row->class_code = d->class;
                row->revision = d->revision;
                row->boot_vga = luos_pci_is_boot_vga(d->luos_dev);
                row->vram_bytes = vram_of(d);
                if (d->driver && d->driver->name[0])
                        strscpy(row->driver, d->driver->name, sizeof(row->driver));
                ctx.count++;
        }
        lkpi_pci_list_unlock();

        lkpi_for_each_device(minor_visit, &ctx);

        int total = ctx.count;
        for (int i = 0; i < ctx.extra_count && total < max; i++)
                out[total++] = ctx.extra[i];

        for (int i = 0; i < ctx.count; i++) pci_dev_put(refs[i]);
        return total;
}

int lkpi_gpu_driver_list(const char *names, int rows, int row_len)
{
        struct pci_dev *d;
        struct pci_driver *drv;
        int count = 0;

        if (!names || rows <= 0 || row_len < 8) return -EINVAL;
        memset((void *)names, 0, (size_t)rows * (size_t)row_len);

        lkpi_pci_list_lock();
        list_for_each_entry(drv, &lkpi_pci_drivers, lkpi_node) {
                if (count >= rows) break;
                bool wants = false;
                list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                        if ((d->class >> 16) != PCI_BASE_CLASS_DISPLAY) continue;
                        if (d->driver == drv || pci_match_id(drv->id_table, d)) { wants = true; break; }
                }
                if (!wants) continue;
                strscpy((char *)names + (size_t)count * (size_t)row_len, drv->name, row_len);
                count++;
        }
        lkpi_pci_list_unlock();
        return count;
}


int lkpi_gpu_open_dev(unsigned int devt, unsigned int flags, void **handle)
{
        if (!handle) return -EINVAL;
        if (!devt) return -ENODEV;
        return lkpi_chrdev_open(MAJOR(devt), MINOR(devt), flags, handle);
}

void lkpi_gpu_close_dev(void *handle)
{
        if (handle) lkpi_file_put(handle);
}

long lkpi_gpu_ioctl(void *handle, unsigned int cmd, void *karg)
{
        struct task_struct *t = current;
        long ret;

        if (!handle) return -EBADF;
        t->uaccess_kds++;
        ret = lkpi_file_ioctl(handle, cmd, (unsigned long)karg);
        t->uaccess_kds--;
        return ret;
}

long lkpi_gpu_drm_version(void *handle, int *major, int *minor, int *patch,
                          char *name, int name_cap, char *date, int date_cap,
                          char *desc, int desc_cap)
{
        struct drm_version v;
        char kname[64], kdate[32], kdesc[128];
        long ret;

        if (!handle) return -EBADF;
        memset(&v, 0, sizeof(v));
        memset(kname, 0, sizeof(kname));
        memset(kdate, 0, sizeof(kdate));
        memset(kdesc, 0, sizeof(kdesc));
        v.name_len = sizeof(kname) - 1;
        v.date_len = sizeof(kdate) - 1;
        v.desc_len = sizeof(kdesc) - 1;
        v.name = kname;
        v.date = kdate;
        v.desc = kdesc;

        ret = lkpi_gpu_ioctl(handle, DRM_IOCTL_VERSION, &v);
        if (ret) return ret;
        if (major) *major = v.version_major;
        if (minor) *minor = v.version_minor;
        if (patch) *patch = v.version_patchlevel;
        if (name && name_cap > 0) strscpy(name, kname, name_cap);
        if (date && date_cap > 0) strscpy(date, kdate, date_cap);
        if (desc && desc_cap > 0) strscpy(desc, kdesc, desc_cap);
        return 0;
}

struct lkpi_gem_map {
        struct drm_gem_object *obj;
        struct iosys_map map;
};

long lkpi_gpu_gem_vmap(void *handle, unsigned int gem_handle, void **map_token,
                       unsigned long *kaddr, unsigned long long *size)
{
        struct file *f = handle;
        struct drm_file *priv;
        struct drm_gem_object *obj;
        struct lkpi_gem_map *m;
        long ret;

        if (!handle || !map_token || !kaddr) return -EINVAL;
        priv = f->private_data;
        if (!priv) return -ENODEV;
        obj = drm_gem_object_lookup(priv, gem_handle);
        if (!obj) return -ENOENT;
        m = kzalloc(sizeof(*m), GFP_KERNEL);
        if (!m) {
                drm_gem_object_put(obj);
                return -ENOMEM;
        }
        ret = drm_gem_vmap(obj, &m->map);
        if (ret) {
                kfree(m);
                drm_gem_object_put(obj);
                return ret;
        }
        m->obj = obj;
        *map_token = m;
        *kaddr = (unsigned long)(m->map.is_iomem ? (void __force *)m->map.vaddr_iomem
                                                  : m->map.vaddr);
        if (size) *size = obj->size;
        return 0;
}

long lkpi_gpu_gem_share(void *dst_handle, void *src_file, unsigned int src_gem, unsigned int *dst_gem)
{
        struct file *dst = dst_handle, *src = src_file;
        struct drm_file *pd, *ps;
        struct drm_gem_object *obj;
        long ret;

        if (!dst || !src || !dst_gem) return -EINVAL;
        if (dst->f_op != src->f_op) return -EXDEV;
        pd = dst->private_data;
        ps = src->private_data;
        if (!pd || !ps || !pd->minor || !ps->minor || pd->minor->dev != ps->minor->dev) return -EXDEV;
        obj = drm_gem_object_lookup(ps, src_gem);
        if (!obj) return -ENOENT;
        ret = drm_gem_handle_create(pd, obj, dst_gem);
        drm_gem_object_put(obj);
        return ret;
}

long lkpi_gpu_take_master(void *handle)
{
        struct file *f = handle;
        struct drm_file *priv, *it, *holder = NULL;
        struct drm_device *dev;

        if (!f || !f->private_data) return -EINVAL;
        priv = f->private_data;
        if (!priv->minor) return -ENODEV;
        dev = priv->minor->dev;
        mutex_lock(&dev->master_mutex);
        if (!dev->master || drm_is_current_master(priv)) {
                mutex_unlock(&dev->master_mutex);
                return 0;
        }
        mutex_lock(&dev->filelist_mutex);
        list_for_each_entry(it, &dev->filelist, lhead) {
                if (it->is_master && it->master == dev->master) {
                        holder = it;
                        break;
                }
        }
        mutex_unlock(&dev->filelist_mutex);
        if (holder && dev->driver->master_drop) dev->driver->master_drop(dev, holder);
        drm_master_put(&dev->master);
        mutex_unlock(&dev->master_mutex);
        return 1;
}

long lkpi_gpu_gem_vunmap(void *map_token)
{
        struct lkpi_gem_map *m = map_token;
        if (!m) return -EINVAL;
        drm_gem_vunmap(m->obj, &m->map);
        drm_gem_object_put(m->obj);
        kfree(m);
        return 0;
}
