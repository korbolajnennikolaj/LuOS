// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <linux/idr.h>


static struct kobject lkpi_kernel_kobj = { .name = "kernel" };
struct kobject *kernel_kobj = &lkpi_kernel_kobj;

static int kobject_set_name_vargs(struct kobject *kobj, const char *fmt, va_list vargs)
{
	char *name = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!name) return -ENOMEM;
	kfree(kobj->name);
	kobj->name = name;
	return 0;
}

int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int ret = kobject_set_name_vargs(kobj, fmt, args);
	va_end(args);
	return ret;
}

static void kobject_init_internal(struct kobject *kobj)
{
	kref_init(&kobj->kref);
	INIT_LIST_HEAD(&kobj->entry);
	kobj->state_in_sysfs = 0;
	kobj->state_initialized = 1;
}

void kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
	kobject_init_internal(kobj);
	kobj->ktype = ktype;
}

static void kobj_kset_join(struct kobject *kobj)
{
	if (!kobj->kset) return;
	kobject_get(&kobj->kset->kobj);
	spin_lock(&kobj->kset->list_lock);
	list_add_tail(&kobj->entry, &kobj->kset->list);
	spin_unlock(&kobj->kset->list_lock);
}

static void kobj_kset_leave(struct kobject *kobj)
{
	if (!kobj->kset) return;
	spin_lock(&kobj->kset->list_lock);
	list_del_init(&kobj->entry);
	spin_unlock(&kobj->kset->list_lock);
	kobject_put(&kobj->kset->kobj);
}

static int kobject_add_internal(struct kobject *kobj, struct kobject *parent)
{
	if (!kobj->name || !kobj->name[0]) return -EINVAL;
	if (kobj->kset) {
		if (!parent) parent = &kobj->kset->kobj;
		kobj_kset_join(kobj);
	}
	kobj->parent = parent;
	kobj->state_in_sysfs = 1;
	return 0;
}

int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
	if (fmt) {
		va_list args;
		va_start(args, fmt);
		int ret = kobject_set_name_vargs(kobj, fmt, args);
		va_end(args);
		if (ret) return ret;
	}
	return kobject_add_internal(kobj, parent);
}

int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype, struct kobject *parent, const char *fmt, ...)
{
	kobject_init(kobj, ktype);
	if (fmt) {
		va_list args;
		va_start(args, fmt);
		int ret = kobject_set_name_vargs(kobj, fmt, args);
		va_end(args);
		if (ret) return ret;
	}
	return kobject_add_internal(kobj, parent);
}

struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj) kref_get(&kobj->kref);
	return kobj;
}

static void kobject_release(struct kref *kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);
	const char *name = kobj->name;
	if (kobj->state_in_sysfs) kobject_del(kobj);
	if (kobj->ktype && kobj->ktype->release) kobj->ktype->release(kobj);
	kfree(name);
}

void kobject_put(struct kobject *kobj)
{
	if (kobj) kref_put(&kobj->kref, kobject_release);
}

void kobject_del(struct kobject *kobj)
{
	if (!kobj) return;
	kobj_kset_leave(kobj);
	kobj->parent = NULL;
	kobj->state_in_sysfs = 0;
}


static void kset_init(struct kset *kset)
{
	kobject_init_internal(&kset->kobj);
	INIT_LIST_HEAD(&kset->list);
	spin_lock_init(&kset->list_lock);
}

int kset_register(struct kset *kset)
{
	if (!kset) return -EINVAL;
	kset_init(kset);
	return kobject_add_internal(&kset->kobj, kset->kobj.parent);
}

void kset_unregister(struct kset *kset)
{
	if (!kset) return;
	kobject_del(&kset->kobj);
	kobject_put(&kset->kobj);
}

static void kset_release(struct kobject *kobj) { kfree(to_kset(kobj)); }
static const struct kobj_type kset_ktype = { .release = kset_release };

struct kset *kset_create_and_add(const char *name, const struct kset_uevent_ops *uevent_ops, struct kobject *parent_kobj)
{
	struct kset *kset = kzalloc(sizeof(*kset), GFP_KERNEL);
	if (!kset) return NULL;
	if (kobject_set_name(&kset->kobj, "%s", name)) {
		kfree(kset);
		return NULL;
	}
	kset->uevent_ops = uevent_ops;
	kset->kobj.parent = parent_kobj;
	kset->kobj.ktype = &kset_ktype;
	kset->kobj.kset = NULL;
	if (kset_register(kset)) {
		kobject_put(&kset->kobj);
		return NULL;
	}
	return kset;
}

static void dynamic_kobj_release(struct kobject *kobj) { kfree(kobj); }
static const struct kobj_type dynamic_kobj_ktype = { .release = dynamic_kobj_release };

struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *kobj = kzalloc(sizeof(*kobj), GFP_KERNEL);
	if (!kobj) return NULL;
	kobject_init(kobj, &dynamic_kobj_ktype);
	if (kobject_add(kobj, parent, "%s", name)) {
		kobject_put(kobj);
		return NULL;
	}
	return kobj;
}


static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);

struct devnode_entry {
	struct list_head node;
	struct device *dev;
	char path[64];
};
static LIST_HEAD(devnode_list);
unsigned long lkpi_device_generation;

static void device_release(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	devres_release_all(dev);
	if (dev->release) dev->release(dev);
	else if (dev->type && dev->type->release) dev->type->release(dev);
	else if (dev->class && dev->class->dev_release) dev->class->dev_release(dev);
}

static const struct kobj_type device_ktype = { .release = device_release };

void device_initialize(struct device *dev)
{
	kobject_init(&dev->kobj, &device_ktype);
	mutex_init(&dev->mutex);
	INIT_LIST_HEAD(&dev->dma_pools);
	INIT_LIST_HEAD(&dev->devres_head);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->lkpi_node);
	dev->numa_node = NUMA_NO_NODE;
}

int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int ret = kobject_set_name_vargs(&dev->kobj, fmt, args);
	va_end(args);
	return ret;
}

static void device_add_devnode(struct device *dev)
{
	umode_t mode = 0;
	char *name = NULL;
	if (dev->type && dev->type->devnode) name = dev->type->devnode(dev, &mode, NULL, NULL);
	if (!name && dev->class && dev->class->devnode) name = dev->class->devnode(dev, &mode);
	struct devnode_entry *e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		kfree(name);
		return;
	}
	e->dev = dev;
	snprintf(e->path, sizeof(e->path), "/dev/%s", name ? name : dev_name(dev));
	kfree(name);
	if (luos_devnode_register(e->path, MAJOR(dev->devt), MINOR(dev->devt))) {
		kfree(e);
		return;
	}
	list_add(&e->node, &devnode_list);
}

static void device_del_devnode(struct device *dev)
{
	struct devnode_entry *e, *n;
	list_for_each_entry_safe(e, n, &devnode_list, node) {
		if (e->dev != dev) continue;
		luos_devnode_unregister(e->path);
		list_del(&e->node);
		kfree(e);
	}
}

static bool bus_is_generic(const struct bus_type *bus);
static void bus_attach_device(struct device *dev);
static void driver_detach_device(struct device *dev);

int device_add(struct device *dev)
{
	if (dev->init_name) {
		int ret = dev_set_name(dev, "%s", dev->init_name);
		dev->init_name = NULL;
		if (ret) return ret;
	}
	if (!dev->kobj.name) return -EINVAL;
	if (dev->parent) get_device(dev->parent);
	dev->kobj.parent = dev->parent ? &dev->parent->kobj : NULL;
	dev->kobj.state_in_sysfs = 1;
	mutex_lock(&device_list_lock);
	list_add_tail(&dev->lkpi_node, &device_list);
	if (dev->devt) device_add_devnode(dev);
	lkpi_device_generation++;
	mutex_unlock(&device_list_lock);
	dev->lkpi_added = true;
	if (bus_is_generic(dev->bus)) bus_attach_device(dev);
	return 0;
}

void lkpi_for_each_device(void (*fn)(struct device *dev, void *arg), void *arg)
{
	struct device *dev;
	mutex_lock(&device_list_lock);
	list_for_each_entry(dev, &device_list, lkpi_node) fn(dev, arg);
	mutex_unlock(&device_list_lock);
}

const char *lkpi_device_devname(struct device *dev)
{
	struct devnode_entry *e;
	list_for_each_entry(e, &devnode_list, node)
		if (e->dev == dev) return e->path + 5;
	return NULL;
}

int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}

void device_del(struct device *dev)
{
	if (!dev->lkpi_added) return;
	if (bus_is_generic(dev->bus)) driver_detach_device(dev);
	mutex_lock(&device_list_lock);
	device_del_devnode(dev);
	list_del_init(&dev->lkpi_node);
	lkpi_device_generation++;
	mutex_unlock(&device_list_lock);
	dev->lkpi_added = false;
	dev->kobj.state_in_sysfs = 0;
	if (dev->parent) put_device(dev->parent);
}

void device_unregister(struct device *dev)
{
	device_del(dev);
	put_device(dev);
}

struct device *get_device(struct device *dev)
{
	if (dev) kobject_get(&dev->kobj);
	return dev;
}

void put_device(struct device *dev)
{
	if (dev) kobject_put(&dev->kobj);
}

static void device_create_release(struct device *dev) { kfree(dev); }

struct device *device_create(const struct class *cls, struct device *parent, dev_t devt, void *drvdata, const char *fmt, ...)
{
	va_list args;
	struct device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) return ERR_PTR(-ENOMEM);
	device_initialize(dev);
	dev->devt = devt;
	dev->class = cls;
	dev->parent = parent;
	dev->release = device_create_release;
	dev_set_drvdata(dev, drvdata);
	va_start(args, fmt);
	int ret = kobject_set_name_vargs(&dev->kobj, fmt, args);
	va_end(args);
	if (!ret) ret = device_add(dev);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}
	return dev;
}

void device_destroy(const struct class *cls, dev_t devt)
{
	struct device *dev, *found = NULL;
	mutex_lock(&device_list_lock);
	list_for_each_entry(dev, &device_list, lkpi_node)
		if (dev->class == cls && dev->devt == devt) found = dev;
	mutex_unlock(&device_list_lock);
	if (found) device_unregister(found);
}

int device_rename(struct device *dev, const char *new_name) { return dev_set_name(dev, "%s", new_name); }

struct class *class_create(const char *name)
{
	struct class *cls = kzalloc(sizeof(*cls), GFP_KERNEL);
	if (!cls) return ERR_PTR(-ENOMEM);
	cls->name = name;
	return cls;
}

void class_destroy(const struct class *cls)
{
	if (!IS_ERR_OR_NULL(cls)) kfree(cls);
}

int class_register(const struct class *class) { return 0; }
void class_unregister(const struct class *class) { }

int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{
	va_list args;
	int len;
	if (env->envp_idx >= ARRAY_SIZE(env->envp)) return -ENOMEM;
	va_start(args, format);
	len = vsnprintf(&env->buf[env->buflen], sizeof(env->buf) - env->buflen, format, args);
	va_end(args);
	if (len >= (int)sizeof(env->buf) - env->buflen) return -ENOMEM;
	env->envp[env->envp_idx++] = &env->buf[env->buflen];
	env->buflen += len + 1;
	return 0;
}

struct bus_driver_entry {
	struct list_head node;
	struct device_driver *drv;
};
static LIST_HEAD(bus_drivers);
static DEFINE_MUTEX(bus_drivers_lock);

static bool bus_is_generic(const struct bus_type *bus)
{
	return bus && bus != &platform_bus_type && bus != &pci_bus_type && (bus->match || bus->probe);
}

int bus_register(const struct bus_type *bus) { return 0; }
void bus_unregister(const struct bus_type *bus) { }

static int driver_probe_device(struct device_driver *drv, struct device *dev)
{
	int ret;
	if (dev->driver) return -EBUSY;
	if (drv->bus->match && drv->bus->match(dev, drv) <= 0) return -ENODEV;
	device_lock(dev);
	dev->driver = drv;
	if (drv->bus->probe) ret = drv->bus->probe(dev);
	else if (drv->probe) ret = drv->probe(dev);
	else ret = 0;
	if (ret) {
		dev->driver = NULL;
		devres_release_all(dev);
		if (ret != -ENODEV && ret != -ENXIO)
			dev_warn(dev, "probe with driver %s failed with error %d\n", drv->name, ret);
	} else {
		lkpi_device_generation++;
		dev_info(dev, "bound to driver %s\n", drv->name);
	}
	device_unlock(dev);
	return ret;
}

static void driver_detach_device(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	if (!drv) return;
	device_lock(dev);
	if (drv->bus && drv->bus->remove) drv->bus->remove(dev);
	else if (drv->remove) drv->remove(dev);
	dev->driver = NULL;
	devres_release_all(dev);
	lkpi_device_generation++;
	device_unlock(dev);
}

static void bus_attach_device(struct device *dev)
{
	struct bus_driver_entry *e;
	int idx = 0;
	for (;;) {
		struct device_driver *drv = NULL;
		int i = 0;
		mutex_lock(&bus_drivers_lock);
		list_for_each_entry(e, &bus_drivers, node) {
			if (i++ < idx) continue;
			if (e->drv->bus == dev->bus) {
				drv = e->drv;
				break;
			}
		}
		mutex_unlock(&bus_drivers_lock);
		if (!drv || dev->driver) return;
		idx = i;
		if (!driver_probe_device(drv, dev)) return;
	}
}

static struct device **bus_devices_snapshot(const struct bus_type *bus, int *count)
{
	struct device *dev, **list = NULL;
	int n = 0, cap = 0;
	mutex_lock(&device_list_lock);
	list_for_each_entry(dev, &device_list, lkpi_node) {
		if (dev->bus != bus) continue;
		if (n == cap) {
			int ncap = cap ? cap * 2 : 16;
			struct device **nl = krealloc(list, ncap * sizeof(*nl), GFP_KERNEL);
			if (!nl) break;
			list = nl;
			cap = ncap;
		}
		list[n++] = get_device(dev);
	}
	mutex_unlock(&device_list_lock);
	*count = n;
	return list;
}

static void put_devices(struct device **list, int n)
{
	for (int i = 0; i < n; i++) put_device(list[i]);
	kfree(list);
}

int driver_register(struct device_driver *drv)
{
	struct bus_driver_entry *e;
	int n;
	if (!bus_is_generic(drv->bus)) return 0;
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) return -ENOMEM;
	e->drv = drv;
	mutex_lock(&bus_drivers_lock);
	list_add_tail(&e->node, &bus_drivers);
	mutex_unlock(&bus_drivers_lock);
	struct device **devs = bus_devices_snapshot(drv->bus, &n);
	for (int i = 0; i < n; i++)
		if (!devs[i]->driver) driver_probe_device(drv, devs[i]);
	put_devices(devs, n);
	return 0;
}

void driver_unregister(struct device_driver *drv)
{
	struct bus_driver_entry *e, *t;
	int n;
	if (!bus_is_generic(drv->bus)) return;
	mutex_lock(&bus_drivers_lock);
	list_for_each_entry_safe(e, t, &bus_drivers, node) {
		if (e->drv != drv) continue;
		list_del(&e->node);
		kfree(e);
	}
	mutex_unlock(&bus_drivers_lock);
	struct device **devs = bus_devices_snapshot(drv->bus, &n);
	for (int i = 0; i < n; i++)
		if (devs[i]->driver == drv) driver_detach_device(devs[i]);
	put_devices(devs, n);
}

int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data, int (*fn)(struct device *dev, void *data))
{
	int n, ret = 0, i = 0;
	struct device **devs = bus_devices_snapshot(bus, &n);
	if (start)
		while (i < n && devs[i++] != start) { }
	for (; i < n && !ret; i++) ret = fn(devs[i], data);
	put_devices(devs, n);
	return ret;
}

int bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data,
		     int (*fn)(struct device_driver *drv, void *data))
{
	struct bus_driver_entry *e;
	bool started = !start;
	int ret = 0;
	mutex_lock(&bus_drivers_lock);
	list_for_each_entry(e, &bus_drivers, node) {
		if (e->drv->bus != bus) continue;
		if (!started) {
			started = e->drv == start;
			continue;
		}
		ret = fn(e->drv, data);
		if (ret) break;
	}
	mutex_unlock(&bus_drivers_lock);
	return ret;
}

int device_attach(struct device *dev)
{
	if (!bus_is_generic(dev->bus)) return 0;
	bus_attach_device(dev);
	return dev->driver ? 1 : 0;
}

int device_driver_attach(const struct device_driver *drv, struct device *dev)
{
	if (!bus_is_generic(drv->bus)) return 0;
	return driver_probe_device((struct device_driver *)drv, dev);
}

void device_release_driver(struct device *dev)
{
	if (bus_is_generic(dev->bus)) {
		driver_detach_device(dev);
		return;
	}
	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);
		if (pdev->driver && pdev->driver->remove) pdev->driver->remove(pdev);
		pdev->driver = NULL;
	}
	dev->driver = NULL;
	devres_release_all(dev);
	lkpi_device_generation++;
}

struct device *bus_find_device_by_name(const struct bus_type *bus, struct device *start, const char *name)
{
	struct device *dev, *found = NULL;
	mutex_lock(&device_list_lock);
	list_for_each_entry(dev, &device_list, lkpi_node) {
		if (dev->bus == bus && !strcmp(dev_name(dev), name)) {
			found = get_device(dev);
			break;
		}
	}
	mutex_unlock(&device_list_lock);
	return found;
}


struct devres {
	struct list_head entry;
	dr_release_t release;
	const char *name;
	size_t size;
	u8 __aligned(ARCH_DMA_MINALIGN) data[];
};

static void devres_group_marker(struct device *dev, void *res) { }

struct devres_group {
	void *id;
	bool closed;
};

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name)
{
	struct devres *dr = kmalloc(sizeof(*dr) + size, gfp);
	if (!dr) return NULL;
	INIT_LIST_HEAD(&dr->entry);
	dr->release = release;
	dr->name = name;
	dr->size = size;
	return dr->data;
}

static struct devres *to_devres(void *res) { return container_of(res, struct devres, data); }

void devres_free(void *res)
{
	if (res) kfree(to_devres(res));
}

void devres_add(struct device *dev, void *res)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->devres_lock, flags);
	list_add_tail(&to_devres(res)->entry, &dev->devres_head);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
}

static struct devres *devres_find_locked(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
	struct devres *dr;
	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release != release) continue;
		if (match && !match(dev, dr->data, match_data)) continue;
		return dr;
	}
	return NULL;
}

void *devres_find(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->devres_lock, flags);
	struct devres *dr = devres_find_locked(dev, release, match, match_data);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	return dr ? dr->data : NULL;
}

void *devres_remove(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->devres_lock, flags);
	struct devres *dr = devres_find_locked(dev, release, match, match_data);
	if (dr) list_del_init(&dr->entry);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	return dr ? dr->data : NULL;
}

int devres_destroy(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
	void *res = devres_remove(dev, release, match, match_data);
	if (!res) return -ENOENT;
	devres_free(res);
	return 0;
}

int devres_release(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
	void *res = devres_remove(dev, release, match, match_data);
	if (!res) return -ENOENT;
	release(dev, res);
	devres_free(res);
	return 0;
}

static int release_from(struct device *dev, struct devres *first)
{
	int n = 0;
	for (;;) {
		unsigned long flags;
		struct devres *dr = NULL;
		spin_lock_irqsave(&dev->devres_lock, flags);
		if (!list_empty(&dev->devres_head) && (first == NULL || !list_empty(&first->entry))) {
			dr = list_last_entry(&dev->devres_head, struct devres, entry);
			list_del_init(&dr->entry);
		}
		spin_unlock_irqrestore(&dev->devres_lock, flags);
		if (!dr) break;
		if (dr->release != devres_group_marker) {
			dr->release(dev, dr->data);
			n++;
		}
		bool last = dr == first;
		kfree(dr);
		if (last) break;
	}
	return n;
}

void devres_release_all(struct device *dev)
{
	if (!dev->devres_head.next) return;
	release_from(dev, NULL);
}

void *devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	struct devres_group *g = __devres_alloc_node(devres_group_marker, sizeof(*g), gfp, NUMA_NO_NODE, "group");
	if (!g) return NULL;
	g->id = id ? id : g;
	g->closed = false;
	devres_add(dev, g);
	return g->id;
}

static struct devres *find_group(struct device *dev, void *id)
{
	struct devres *dr;
	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release != devres_group_marker) continue;
		struct devres_group *g = (void *)dr->data;
		if (!id || g->id == id) return dr;
	}
	return NULL;
}

void devres_close_group(struct device *dev, void *id)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->devres_lock, flags);
	struct devres *dr = find_group(dev, id);
	if (dr) ((struct devres_group *)dr->data)->closed = true;
	spin_unlock_irqrestore(&dev->devres_lock, flags);
}

void devres_remove_group(struct device *dev, void *id)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->devres_lock, flags);
	struct devres *dr = find_group(dev, id);
	if (dr) list_del_init(&dr->entry);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	if (dr) kfree(dr);
}

int devres_release_group(struct device *dev, void *id)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->devres_lock, flags);
	struct devres *dr = find_group(dev, id);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	return dr ? release_from(dev, dr) : -ENOENT;
}

static void devm_kmalloc_release(struct device *dev, void *res) { }
static int devm_kmalloc_match(struct device *dev, void *res, void *data) { return res == data; }

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	void *p = __devres_alloc_node(devm_kmalloc_release, size, gfp, NUMA_NO_NODE, "devm_kzalloc");
	if (!p) return NULL;
	devres_add(dev, p);
	return p;
}

void devm_kfree(struct device *dev, const void *p)
{
	if (ZERO_OR_NULL_PTR(p)) return;
	devres_destroy(dev, devm_kmalloc_release, devm_kmalloc_match, (void *)p);
}

char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp)
{
	if (!s) return NULL;
	size_t n = strlen(s) + 1;
	char *p = devm_kmalloc(dev, n, gfp);
	if (p) memcpy(p, s, n);
	return p;
}

const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp) { return devm_kstrdup(dev, s, gfp); }

char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char *tmp = kvasprintf(gfp, fmt, args);
	va_end(args);
	if (!tmp) return NULL;
	char *p = devm_kstrdup(dev, tmp, gfp);
	kfree(tmp);
	return p;
}

void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
	void *p = devm_kmalloc(dev, len, gfp);
	if (p) memcpy(p, src, len);
	return p;
}

struct action_devres {
	void *data;
	void (*action)(void *);
};

static void devm_action_release(struct device *dev, void *res)
{
	struct action_devres *a = res;
	a->action(a->data);
}

static int devm_action_match(struct device *dev, void *res, void *p)
{
	struct action_devres *a = res, *t = p;
	return a->action == t->action && a->data == t->data;
}

int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name)
{
	struct action_devres *a = __devres_alloc_node(devm_action_release, sizeof(*a), GFP_KERNEL, NUMA_NO_NODE, name);
	if (!a) return -ENOMEM;
	a->data = data;
	a->action = action;
	devres_add(dev, a);
	return 0;
}

void devm_remove_action(struct device *dev, void (*action)(void *), void *data)
{
	struct action_devres t = { data, action };
	devres_destroy(dev, devm_action_release, devm_action_match, &t);
}

void devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	struct action_devres t = { data, action };
	devres_release(dev, devm_action_release, devm_action_match, &t);
}

static void devm_iounmap_release(struct device *dev, void *res) { iounmap(*(void __iomem **)res); }
static int devm_iounmap_match(struct device *dev, void *res, void *data) { return *(void **)res == data; }

static void __iomem *devm_ioremap_common(struct device *dev, void __iomem *addr)
{
	if (!addr) return NULL;
	void __iomem **ptr = __devres_alloc_node(devm_iounmap_release, sizeof(*ptr), GFP_KERNEL, NUMA_NO_NODE, "ioremap");
	if (!ptr) {
		iounmap(addr);
		return NULL;
	}
	*ptr = addr;
	devres_add(dev, ptr);
	return addr;
}

void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size)
{
	return devm_ioremap_common(dev, ioremap(offset, size));
}

void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset, resource_size_t size)
{
	return devm_ioremap_common(dev, ioremap_wc(offset, size));
}

void devm_iounmap(struct device *dev, void __iomem *addr)
{
	devres_destroy(dev, devm_iounmap_release, devm_iounmap_match, (void __force *)addr);
	iounmap(addr);
}

void __iomem *devm_ioremap_resource(struct device *dev, const struct resource *res)
{
	if (!res || resource_type(res) != IORESOURCE_MEM) return IOMEM_ERR_PTR(-EINVAL);
	void __iomem *p = devm_ioremap(dev, res->start, resource_size(res));
	return p ? p : IOMEM_ERR_PTR(-ENOMEM);
}

struct resource *devm_request_mem_region(struct device *dev, resource_size_t start, resource_size_t n, const char *name)
{
	return request_mem_region(start, n, name);
}

int devm_request_resource(struct device *dev, struct resource *root, struct resource *new)
{
	return request_resource(root, new);
}

int devm_arch_phys_wc_add(struct device *dev, unsigned long base, unsigned long size) { return 0; }
int devm_arch_io_reserve_memtype_wc(struct device *dev, resource_size_t start, resource_size_t size) { return 0; }

static void devm_memremap_release(struct device *dev, void *res) { memunmap(*(void **)res); }

void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags)
{
	void *addr = memremap(offset, size, flags);
	if (!addr) return ERR_PTR(-ENXIO);
	void **ptr = __devres_alloc_node(devm_memremap_release, sizeof(*ptr), GFP_KERNEL, NUMA_NO_NODE, "memremap");
	if (ptr) {
		*ptr = addr;
		devres_add(dev, ptr);
	}
	return addr;
}


void _dev_printk(const char *level, const struct device *dev, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (dev) printk("%s%s %s: %pV", level, dev_driver_string(dev), dev_name(dev), &vaf);
	else printk("%s(NULL device *): %pV", level, &vaf);
	va_end(args);
}

int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (err != -EPROBE_DEFER) _dev_printk(KERN_ERR, dev, "error %pe: %pV", ERR_PTR(err), &vaf);
	else _dev_printk(KERN_DEBUG, dev, "deferred probe: %pV", &vaf);
	va_end(args);
	return err;
}


struct irq_action {
	struct list_head node;
	unsigned int irq;
	irq_handler_t handler;
	irq_handler_t thread_fn;
	void *dev_id;
	const char *name;
	unsigned long flags;
	struct task_struct *thread;
	atomic_t thread_pending;
	atomic_t thread_running;
};

struct irq_desc_lkpi {
	struct list_head actions;
	raw_spinlock_t lock;
	atomic_t running;
	int depth;
	bool attached;
	struct task_struct *poller;
};

static struct irq_desc_lkpi irq_descs[256];
static DEFINE_MUTEX(irq_mutex);

static int irq_thread_fn(void *data)
{
	struct irq_action *a = data;
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) break;
		if (!atomic_xchg(&a->thread_pending, 0)) {
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		atomic_set(&a->thread_running, 1);
		a->thread_fn(a->irq, a->dev_id);
		atomic_set(&a->thread_running, 0);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void irq_dispatch(void *ctx)
{
	struct irq_desc_lkpi *desc = ctx;
	struct irq_action *a;
	atomic_inc(&desc->running);
	raw_spin_lock(&desc->lock);
	if (!desc->depth) {
		list_for_each_entry(a, &desc->actions, node) {
			irqreturn_t r = a->handler ? a->handler(a->irq, a->dev_id) : IRQ_WAKE_THREAD;
			if ((r & IRQ_WAKE_THREAD) && a->thread) {
				atomic_set(&a->thread_pending, 1);
				wake_up_process(a->thread);
			}
		}
	}
	raw_spin_unlock(&desc->lock);
	atomic_dec(&desc->running);
}

static int irq_poll_fn(void *data)
{
	struct irq_desc_lkpi *desc = data;
	while (!kthread_should_stop()) {
		unsigned long f;
		msleep(4);
		if (kthread_should_stop()) break;
		local_irq_save(f);
		irq_dispatch(desc);
		local_irq_restore(f);
	}
	return 0;
}

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
			 const char *name, void *dev)
{
	if (irq < 32 || irq > 255) return -EINVAL;
	if (!handler && !thread_fn) return -EINVAL;
	struct irq_desc_lkpi *desc = &irq_descs[irq];
	struct irq_action *a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a) return -ENOMEM;
	a->irq = irq;
	a->handler = handler;
	a->thread_fn = thread_fn;
	a->dev_id = dev;
	a->name = name;
	a->flags = flags;
	if (thread_fn) {
		a->thread = kthread_run(irq_thread_fn, a, "irq/%u-%s", irq, name ? name : "");
		if (IS_ERR(a->thread)) {
			int ret = PTR_ERR(a->thread);
			kfree(a);
			return ret;
		}
	}
	mutex_lock(&irq_mutex);
	unsigned long f;
	raw_spin_lock_irqsave(&desc->lock, f);
	list_add_tail(&a->node, &desc->actions);
	if (flags & IRQF_NO_AUTOEN) desc->depth++;
	raw_spin_unlock_irqrestore(&desc->lock, f);
	if (!desc->attached) {
		if (luos_irq_attach(irq, irq_dispatch, desc) < 0) {
			raw_spin_lock_irqsave(&desc->lock, f);
			list_del(&a->node);
			raw_spin_unlock_irqrestore(&desc->lock, f);
			mutex_unlock(&irq_mutex);
			if (a->thread) kthread_stop(a->thread);
			kfree(a);
			return -EBUSY;
		}
		desc->attached = true;
		if (luos_intx_route(irq, 1) == 0 && !desc->poller) {
			struct task_struct *p = kthread_run(irq_poll_fn, desc, "irq/%u-poll", irq);
			desc->poller = IS_ERR(p) ? NULL : p;
		}
	}
	mutex_unlock(&irq_mutex);
	return 0;
}

const void *free_irq(unsigned int irq, void *dev_id)
{
	if (irq > 255) return NULL;
	struct irq_desc_lkpi *desc = &irq_descs[irq];
	struct irq_action *a, *found = NULL;
	unsigned long f;
	mutex_lock(&irq_mutex);
	raw_spin_lock_irqsave(&desc->lock, f);
	list_for_each_entry(a, &desc->actions, node) {
		if (a->dev_id == dev_id) {
			found = a;
			list_del(&a->node);
			break;
		}
	}
	bool empty = list_empty(&desc->actions);
	raw_spin_unlock_irqrestore(&desc->lock, f);
	struct task_struct *poller = NULL;
	if (empty && desc->attached) {
		luos_intx_route(irq, 0);
		luos_irq_detach(irq);
		desc->attached = false;
		desc->depth = 0;
		poller = desc->poller;
		desc->poller = NULL;
	}
	mutex_unlock(&irq_mutex);
	if (poller) kthread_stop(poller);
	if (!found) return NULL;
	while (atomic_read(&desc->running)) cpu_relax();
	if (found->thread) kthread_stop(found->thread);
	const char *name = found->name;
	kfree(found);
	return name;
}

struct irq_devres {
	unsigned int irq;
	void *dev_id;
};

static void devm_irq_release(struct device *dev, void *res)
{
	struct irq_devres *d = res;
	free_irq(d->irq, d->dev_id);
}

static int devm_irq_match(struct device *dev, void *res, void *data)
{
	struct irq_devres *a = res, *b = data;
	return a->irq == b->irq && a->dev_id == b->dev_id;
}

int devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long irqflags, const char *devname, void *dev_id)
{
	struct irq_devres *d = __devres_alloc_node(devm_irq_release, sizeof(*d), GFP_KERNEL, NUMA_NO_NODE, "irq");
	if (!d) return -ENOMEM;
	int ret = request_threaded_irq(irq, handler, thread_fn, irqflags, devname ? devname : dev_name(dev), dev_id);
	if (ret) {
		devres_free(d);
		return ret;
	}
	d->irq = irq;
	d->dev_id = dev_id;
	devres_add(dev, d);
	return 0;
}

void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
	struct irq_devres d = { irq, dev_id };
	devres_release(dev, devm_irq_release, devm_irq_match, &d);
}

void synchronize_irq(unsigned int irq)
{
	if (irq > 255) return;
	struct irq_desc_lkpi *desc = &irq_descs[irq];
	struct irq_action *a;
	while (atomic_read(&desc->running)) cpu_relax();
	mutex_lock(&irq_mutex);
	list_for_each_entry(a, &desc->actions, node)
		while (atomic_read(&a->thread_pending) || atomic_read(&a->thread_running)) schedule_timeout_uninterruptible(1);
	mutex_unlock(&irq_mutex);
}

void disable_irq_nosync(unsigned int irq)
{
	unsigned long f;
	if (irq > 255) return;
	raw_spin_lock_irqsave(&irq_descs[irq].lock, f);
	irq_descs[irq].depth++;
	raw_spin_unlock_irqrestore(&irq_descs[irq].lock, f);
}

void disable_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	synchronize_irq(irq);
}

void enable_irq(unsigned int irq)
{
	unsigned long f;
	if (irq > 255) return;
	raw_spin_lock_irqsave(&irq_descs[irq].lock, f);
	if (irq_descs[irq].depth > 0) irq_descs[irq].depth--;
	raw_spin_unlock_irqrestore(&irq_descs[irq].lock, f);
}


static void tasklet_work_fn(struct work_struct *work)
{
	struct tasklet_struct *t = container_of(work, struct tasklet_struct, lkpi_work);
	if (tasklet_trylock(t)) {
		if (!atomic_read(&t->count)) {
			if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state)) WARN_ON_ONCE(1);
			if (t->use_callback) t->callback(t);
			else t->func(t->data);
			tasklet_unlock(t);
			return;
		}
		tasklet_unlock(t);
	}
	cond_resched();
	queue_work(lkpi_irq_wq, &t->lkpi_work);
}

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data)
{
	memset(t, 0, sizeof(*t));
	t->func = func;
	t->data = data;
	INIT_WORK(&t->lkpi_work, tasklet_work_fn);
}

void tasklet_setup(struct tasklet_struct *t, void (*callback)(struct tasklet_struct *))
{
	memset(t, 0, sizeof(*t));
	t->use_callback = true;
	t->callback = callback;
	INIT_WORK(&t->lkpi_work, tasklet_work_fn);
}

void tasklet_schedule(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) queue_work(lkpi_irq_wq, &t->lkpi_work);
}

void tasklet_unlock(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	clear_bit(TASKLET_STATE_RUN, &t->state);
	smp_mb__after_atomic();
}

void tasklet_unlock_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &t->state)) cond_resched();
}

void tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &t->state)) cpu_relax();
}

void tasklet_kill(struct tasklet_struct *t)
{
	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) cond_resched();
	tasklet_unlock_wait(t);
	cancel_work_sync(&t->lkpi_work);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}

static void irq_work_fn(struct work_struct *w)
{
	struct irq_work *work = container_of(w, struct irq_work, lkpi_work);
	int flags = atomic_read(&work->node.a_flags) & ~IRQ_WORK_PENDING;
	atomic_set(&work->node.a_flags, flags);
	smp_mb();
	work->func(work);
	(void)atomic_cmpxchg(&work->node.a_flags, flags, flags & ~IRQ_WORK_BUSY);
}

bool irq_work_queue(struct irq_work *work)
{
	int oflags = atomic_fetch_or(IRQ_WORK_CLAIMED, &work->node.a_flags);
	if (oflags & IRQ_WORK_PENDING) return false;
	if (!work->lkpi_work.func) INIT_WORK(&work->lkpi_work, irq_work_fn);
	queue_work(lkpi_irq_wq, &work->lkpi_work);
	return true;
}

void irq_work_sync(struct irq_work *work)
{
	while (irq_work_is_busy(work)) cond_resched();
}


static int firmware_get(const struct firmware **fw, const char *name, bool warn)
{
	const void *data;
	unsigned long size;
	void *handle;
	*fw = NULL;
	struct firmware *f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) return -ENOMEM;
	int ret = luos_firmware_get(name, &data, &size, &handle);
	if (ret) {
		if (warn) pr_warn("Direct firmware load for %s failed with error %d\n", name, ret);
		kfree(f);
		return ret;
	}
	f->data = data;
	f->size = size;
	f->priv = handle;
	*fw = f;
	return 0;
}

int request_firmware(const struct firmware **fw, const char *name, struct device *device)
{
	return firmware_get(fw, name, true);
}

int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *device)
{
	return firmware_get(fw, name, false);
}

int request_firmware_direct(const struct firmware **fw, const char *name, struct device *device)
{
	return firmware_get(fw, name, false);
}

struct fw_async {
	struct work_struct work;
	const char *name;
	struct device *dev;
	void *context;
	void (*cont)(const struct firmware *fw, void *context);
};

static void fw_async_fn(struct work_struct *work)
{
	struct fw_async *a = container_of(work, struct fw_async, work);
	const struct firmware *fw = NULL;
	request_firmware(&fw, a->name, a->dev);
	a->cont(fw, a->context);
	put_device(a->dev);
	kfree_const(a->name);
	kfree(a);
}

int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *device, gfp_t gfp,
			    void *context, void (*cont)(const struct firmware *fw, void *context))
{
	struct fw_async *a = kzalloc(sizeof(*a), gfp);
	if (!a) return -ENOMEM;
	a->name = kstrdup_const(name, gfp);
	a->dev = get_device(device);
	a->context = context;
	a->cont = cont;
	INIT_WORK(&a->work, fw_async_fn);
	queue_work(system_long_wq, &a->work);
	return 0;
}

int request_firmware_into_buf(const struct firmware **firmware_p, const char *name, struct device *device, void *buf,
			      size_t size)
{
	const struct firmware *fw;
	int ret = firmware_get(&fw, name, true);
	if (ret) return ret;
	struct firmware *f = (struct firmware *)fw;
	if (f->size > size) {
		release_firmware(f);
		*firmware_p = NULL;
		return -EFBIG;
	}
	memcpy(buf, f->data, f->size);
	luos_firmware_put(f->priv);
	f->priv = NULL;
	f->data = buf;
	*firmware_p = f;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
	if (!fw) return;
	if (fw->priv) luos_firmware_put(fw->priv);
	kfree(fw);
}


static int platform_match(struct device *dev, const struct device_driver *drv);
const struct bus_type platform_bus_type = { .name = "platform", .match = platform_match };
static LIST_HEAD(platform_drivers);
static LIST_HEAD(platform_devices);
static DEFINE_MUTEX(platform_lock);

struct platform_entry {
	struct list_head node;
	void *obj;
};

static const struct platform_device_id *platform_match_id(const struct platform_device_id *id, struct platform_device *pdev)
{
	for (; id && id->name[0]; id++)
		if (!strcmp(pdev->name, id->name)) return id;
	return NULL;
}

static int platform_match(struct device *dev, const struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct platform_driver *pdrv = container_of(drv, struct platform_driver, driver);
	if (pdrv->id_table) return platform_match_id(pdrv->id_table, pdev) != NULL;
	return drv->name && !strcmp(pdev->name, drv->name);
}

static void platform_try_probe(struct platform_device *pdev, struct platform_driver *drv)
{
	if (pdev->dev.driver || !platform_match(&pdev->dev, &drv->driver) || !drv->probe) return;
	pdev->id_entry = platform_match_id(drv->id_table, pdev);
	pdev->dev.driver = &drv->driver;
	int ret = drv->probe(pdev);
	if (ret) {
		pdev->dev.driver = NULL;
		devres_release_all(&pdev->dev);
		dev_warn(&pdev->dev, "probe with driver %s failed with error %d\n", drv->driver.name, ret);
	}
}

static void platform_device_release(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	kfree(pdev->dev.platform_data);
	kfree(pdev->resource);
	kfree(pdev->name);
	kfree(pdev);
}

struct platform_device *platform_device_alloc(const char *name, int id)
{
	struct platform_device *pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
	if (!pdev) return NULL;
	pdev->name = kstrdup(name, GFP_KERNEL);
	pdev->id = id;
	device_initialize(&pdev->dev);
	pdev->dev.release = platform_device_release;
	pdev->platform_dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &pdev->platform_dma_mask;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	return pdev;
}

int platform_device_add_data(struct platform_device *pdev, const void *data, size_t size)
{
	void *d = NULL;
	if (data) {
		d = kmemdup(data, size, GFP_KERNEL);
		if (!d) return -ENOMEM;
	}
	kfree(pdev->dev.platform_data);
	pdev->dev.platform_data = d;
	return 0;
}

int platform_device_add_resources(struct platform_device *pdev, const struct resource *res, unsigned int num)
{
	struct resource *r = NULL;
	if (res && num) {
		r = kmemdup(res, sizeof(*res) * num, GFP_KERNEL);
		if (!r) return -ENOMEM;
	}
	kfree(pdev->resource);
	pdev->resource = r;
	pdev->num_resources = num;
	return 0;
}

int platform_device_add(struct platform_device *pdev)
{
	static atomic_t auto_id = ATOMIC_INIT(0);
	struct platform_driver *drv;
	struct platform_entry *e;
	if (pdev->id == PLATFORM_DEVID_AUTO) {
		pdev->id = atomic_inc_return(&auto_id);
		pdev->id_auto = true;
	}
	if (pdev->id == PLATFORM_DEVID_NONE) dev_set_name(&pdev->dev, "%s", pdev->name);
	else dev_set_name(&pdev->dev, "%s.%d", pdev->name, pdev->id);
	pdev->dev.bus = &platform_bus_type;
	int ret = device_add(&pdev->dev);
	if (ret) return ret;
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) return -ENOMEM;
	e->obj = pdev;
	mutex_lock(&platform_lock);
	list_add_tail(&e->node, &platform_devices);
	mutex_unlock(&platform_lock);
	for (int idx = 0;; idx++) {
		int i = 0;
		drv = NULL;
		mutex_lock(&platform_lock);
		list_for_each_entry(e, &platform_drivers, node)
			if (i++ == idx) {
				drv = e->obj;
				break;
			}
		mutex_unlock(&platform_lock);
		if (!drv || pdev->dev.driver) break;
		platform_try_probe(pdev, drv);
	}
	return 0;
}

void platform_device_del(struct platform_device *pdev)
{
	struct platform_entry *e, *n;
	if (!pdev) return;
	mutex_lock(&platform_lock);
	list_for_each_entry_safe(e, n, &platform_devices, node) {
		if (e->obj != pdev) continue;
		list_del(&e->node);
		kfree(e);
	}
	mutex_unlock(&platform_lock);
	if (pdev->dev.driver) {
		struct platform_driver *drv = container_of(pdev->dev.driver, struct platform_driver, driver);
		if (drv->remove) drv->remove(pdev);
		pdev->dev.driver = NULL;
		devres_release_all(&pdev->dev);
	}
	device_del(&pdev->dev);
}

void platform_device_put(struct platform_device *pdev)
{
	if (pdev) put_device(&pdev->dev);
}

int platform_device_register(struct platform_device *pdev)
{
	device_initialize(&pdev->dev);
	return platform_device_add(pdev);
}

void platform_device_unregister(struct platform_device *pdev)
{
	platform_device_del(pdev);
	platform_device_put(pdev);
}

struct platform_device *platform_device_register_resndata(struct device *parent, const char *name, int id,
							   const struct resource *res, unsigned int num,
							   const void *data, size_t size)
{
	struct platform_device *pdev = platform_device_alloc(name, id);
	int ret;
	if (!pdev) return ERR_PTR(-ENOMEM);
	pdev->dev.parent = parent;
	ret = platform_device_add_resources(pdev, res, num);
	if (!ret) ret = platform_device_add_data(pdev, data, size);
	if (!ret) ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		return ERR_PTR(ret);
	}
	return pdev;
}

int __platform_driver_register(struct platform_driver *drv, struct module *owner)
{
	struct platform_entry *e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) return -ENOMEM;
	drv->driver.bus = &platform_bus_type;
	e->obj = drv;
	mutex_lock(&platform_lock);
	list_add_tail(&e->node, &platform_drivers);
	mutex_unlock(&platform_lock);
	for (int idx = 0;; idx++) {
		struct platform_device *pdev = NULL;
		int i = 0;
		mutex_lock(&platform_lock);
		list_for_each_entry(e, &platform_devices, node)
			if (i++ == idx) {
				pdev = e->obj;
				get_device(&pdev->dev);
				break;
			}
		mutex_unlock(&platform_lock);
		if (!pdev) break;
		platform_try_probe(pdev, drv);
		put_device(&pdev->dev);
	}
	return 0;
}

void platform_driver_unregister(struct platform_driver *drv)
{
	struct platform_entry *e, *n;
	mutex_lock(&platform_lock);
	list_for_each_entry(e, &platform_devices, node) {
		struct platform_device *pdev = e->obj;
		if (pdev->dev.driver != &drv->driver) continue;
		if (drv->remove) drv->remove(pdev);
		pdev->dev.driver = NULL;
		devres_release_all(&pdev->dev);
	}
	list_for_each_entry_safe(e, n, &platform_drivers, node) {
		if (e->obj != drv) continue;
		list_del(&e->node);
		kfree(e);
	}
	mutex_unlock(&platform_lock);
}

struct resource *platform_get_resource(struct platform_device *dev, unsigned int type, unsigned int num)
{
	for (u32 i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i];
		if (type == resource_type(r) && num-- == 0) return r;
	}
	return NULL;
}

int platform_get_irq(struct platform_device *dev, unsigned int num) { return -ENXIO; }


const struct bus_type i2c_bus_type = { .name = "i2c" };
static DEFINE_IDR(i2c_adapter_idr);
static DEFINE_MUTEX(i2c_lock);

int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret = -EAGAIN;
	if (!adap->algo || !adap->algo->xfer) return -EOPNOTSUPP;
	for (int tries = 0; tries <= adap->retries; tries++) {
		ret = adap->algo->xfer(adap, msgs, num);
		if (ret != -EAGAIN) break;
	}
	return ret;
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;
	if (adap->lock_ops) adap->lock_ops->lock_bus(adap, I2C_LOCK_SEGMENT);
	else i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	ret = __i2c_transfer(adap, msgs, num);
	if (adap->lock_ops) adap->lock_ops->unlock_bus(adap, I2C_LOCK_SEGMENT);
	else i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}

int i2c_add_adapter(struct i2c_adapter *adap)
{
	mutex_init(&adap->bus_lock.m);
	mutex_init(&adap->userspace_clients_lock);
	if (!adap->timeout) adap->timeout = HZ;
	mutex_lock(&i2c_lock);
	int id = idr_alloc(&i2c_adapter_idr, adap, 0, 0, GFP_KERNEL);
	mutex_unlock(&i2c_lock);
	if (id < 0) return id;
	adap->nr = id;
	return 0;
}

static void devm_i2c_del_adapter(void *adap) { i2c_del_adapter(adap); }

int devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adapter)
{
	int ret = i2c_add_adapter(adapter);
	if (ret) return ret;
	return devm_add_action_or_reset(dev, devm_i2c_del_adapter, adapter);
}

void i2c_del_adapter(struct i2c_adapter *adap)
{
	mutex_lock(&i2c_lock);
	if (idr_find(&i2c_adapter_idr, adap->nr) == adap) idr_remove(&i2c_adapter_idr, adap->nr);
	mutex_unlock(&i2c_lock);
}


struct i2c_adapter *i2c_get_adapter(int nr)
{
	mutex_lock(&i2c_lock);
	struct i2c_adapter *a = idr_find(&i2c_adapter_idr, nr);
	mutex_unlock(&i2c_lock);
	return a;
}

void i2c_put_adapter(struct i2c_adapter *adap) { }
struct i2c_adapter *of_find_i2c_adapter_by_node(struct device_node *node) { return NULL; }
struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info) { return ERR_PTR(-ENODEV); }
void i2c_unregister_device(struct i2c_client *client) { }

s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command)
{
	u8 val;
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &command },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = &val },
	};
	int ret = i2c_transfer(client->adapter, msgs, 2);
	return ret == 2 ? val : (ret < 0 ? ret : -EIO);
}

s32 i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value)
{
	u8 buf[2] = { command, value };
	struct i2c_msg msg = { .addr = client->addr, .flags = 0, .len = 2, .buf = buf };
	int ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}


static void notifier_insert(struct notifier_block **nl, struct notifier_block *n)
{
	while (*nl && (*nl)->priority >= n->priority) nl = &(*nl)->next;
	n->next = *nl;
	*nl = n;
}

static void notifier_remove(struct notifier_block **nl, struct notifier_block *n)
{
	for (; *nl; nl = &(*nl)->next) {
		if (*nl == n) {
			*nl = n->next;
			return;
		}
	}
}

static int notifier_call(struct notifier_block *nb, unsigned long val, void *v)
{
	int ret = NOTIFY_DONE;
	while (nb) {
		struct notifier_block *next = nb->next;
		ret = nb->notifier_call(nb, val, v);
		if (ret & NOTIFY_STOP_MASK) break;
		nb = next;
	}
	return ret;
}

int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *nb)
{
	down_write(&nh->rwsem);
	notifier_insert(&nh->head, nb);
	up_write(&nh->rwsem);
	return 0;
}

int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *nb)
{
	down_write(&nh->rwsem);
	notifier_remove(&nh->head, nb);
	up_write(&nh->rwsem);
	return 0;
}

int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v)
{
	int ret = NOTIFY_DONE;
	if (!READ_ONCE(nh->head)) return ret;
	down_read(&nh->rwsem);
	ret = notifier_call(nh->head, val, v);
	up_read(&nh->rwsem);
	return ret;
}

int atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *nb)
{
	unsigned long flags;
	spin_lock_irqsave(&nh->lock, flags);
	notifier_insert(&nh->head, nb);
	spin_unlock_irqrestore(&nh->lock, flags);
	return 0;
}

int atomic_notifier_chain_unregister(struct atomic_notifier_head *nh, struct notifier_block *nb)
{
	unsigned long flags;
	spin_lock_irqsave(&nh->lock, flags);
	notifier_remove(&nh->head, nb);
	spin_unlock_irqrestore(&nh->lock, flags);
	synchronize_rcu();
	return 0;
}

int atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v)
{
	rcu_read_lock();
	int ret = notifier_call(nh->head, val, v);
	rcu_read_unlock();
	return ret;
}



int param_set_int(const char *val, const struct kernel_param *kp) { return kstrtoint(val, 0, kp->arg); }
int param_get_int(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%i\n", *(int *)kp->arg); }
int param_set_uint(const char *val, const struct kernel_param *kp) { return kstrtouint(val, 0, kp->arg); }
int param_get_uint(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%u\n", *(unsigned int *)kp->arg); }
static int param_set_long(const char *val, const struct kernel_param *kp) { return kstrtol(val, 0, kp->arg); }
static int param_get_long(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%li\n", *(long *)kp->arg); }
static int param_set_ulong(const char *val, const struct kernel_param *kp) { return kstrtoul(val, 0, kp->arg); }
static int param_get_ulong(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%lu\n", *(unsigned long *)kp->arg); }
static int param_set_ullong(const char *val, const struct kernel_param *kp) { return kstrtoull(val, 0, kp->arg); }
static int param_get_ullong(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%llu\n", *(unsigned long long *)kp->arg);
}
static int param_set_short(const char *val, const struct kernel_param *kp) { return kstrtos16(val, 0, kp->arg); }
static int param_get_short(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%hi\n", *(short *)kp->arg); }
static int param_set_ushort(const char *val, const struct kernel_param *kp) { return kstrtou16(val, 0, kp->arg); }
static int param_get_ushort(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%hu\n", *(unsigned short *)kp->arg);
}
static int param_set_byte(const char *val, const struct kernel_param *kp) { return kstrtou8(val, 0, kp->arg); }
static int param_get_byte(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%hhu\n", *(u8 *)kp->arg); }
int param_set_bool(const char *val, const struct kernel_param *kp)
{
	if (!val) val = "1";
	return kstrtobool(val, kp->arg);
}
int param_get_bool(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%c\n", *(bool *)kp->arg ? 'Y' : 'N'); }
int param_set_bint(const char *val, const struct kernel_param *kp)
{
	bool v;
	int ret = kstrtobool(val ? val : "1", &v);
	if (!ret) *(int *)kp->arg = v;
	return ret;
}
int param_get_bint(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%c\n", *(int *)kp->arg ? 'Y' : 'N'); }
static int param_set_invbool(const char *val, const struct kernel_param *kp)
{
	bool b;
	int ret = kstrtobool(val ? val : "1", &b);
	if (!ret) *(bool *)kp->arg = !b;
	return ret;
}
static int param_get_invbool(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%c\n", *(bool *)kp->arg ? 'N' : 'Y');
}
int param_set_charp(const char *val, const struct kernel_param *kp)
{
	char *copy = kstrdup(val ? val : "", GFP_KERNEL);
	if (!copy) return -ENOMEM;
	*(char **)kp->arg = copy;
	return 0;
}
int param_get_charp(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s\n", *(char **)kp->arg ? *(char **)kp->arg : "(null)");
}
static int param_set_copystring(const char *val, const struct kernel_param *kp)
{
	if (!val || strlen(val) + 1 > kp->lkpi_maxlen) return -ENOSPC;
	strcpy(kp->arg, val);
	return 0;
}
static int param_get_string(char *buffer, const struct kernel_param *kp) { return sysfs_emit(buffer, "%s\n", (char *)kp->arg); }
static int param_set_bool_enable_only(const char *val, const struct kernel_param *kp)
{
	bool b;
	int ret = kstrtobool(val ? val : "1", &b);
	if (ret) return ret;
	if (!b && *(bool *)kp->arg) return -EROFS;
	*(bool *)kp->arg = b;
	return 0;
}
#define LKPI_PARAM_OPS(type, setf, getf) const struct kernel_param_ops param_ops_##type = { .set = setf, .get = getf }
LKPI_PARAM_OPS(int, param_set_int, param_get_int);
LKPI_PARAM_OPS(hexint, param_set_uint, param_get_uint);
LKPI_PARAM_OPS(uint, param_set_uint, param_get_uint);
LKPI_PARAM_OPS(long, param_set_long, param_get_long);
LKPI_PARAM_OPS(ulong, param_set_ulong, param_get_ulong);
LKPI_PARAM_OPS(ullong, param_set_ullong, param_get_ullong);
LKPI_PARAM_OPS(short, param_set_short, param_get_short);
LKPI_PARAM_OPS(ushort, param_set_ushort, param_get_ushort);
LKPI_PARAM_OPS(byte, param_set_byte, param_get_byte);
LKPI_PARAM_OPS(bool, param_set_bool, param_get_bool);
LKPI_PARAM_OPS(bint, param_set_bint, param_get_bint);
LKPI_PARAM_OPS(invbool, param_set_invbool, param_get_invbool);
LKPI_PARAM_OPS(bool_enable_only, param_set_bool_enable_only, param_get_bool);
LKPI_PARAM_OPS(charp, param_set_charp, param_get_charp);
LKPI_PARAM_OPS(string, param_set_copystring, param_get_string);

extern const struct kernel_param __start_lkpi_params[], __stop_lkpi_params[];
extern const struct lkpi_setup __start_lkpi_setup[], __stop_lkpi_setup[];

static bool lkpi_param_name_eq(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char ca = a[i] == '-' ? '_' : a[i], cb = b[i] == '-' ? '_' : b[i];
		if (ca != cb) return false;
	}
	return b[n] == 0;
}

static void lkpi_param_word(char *word)
{
	char *eq = strchr(word, '=');
	size_t klen = eq ? (size_t)(eq - word) : strlen(word);
	const char *val = eq ? eq + 1 : NULL;
	if (memchr(word, '.', klen)) {
		for (const struct kernel_param *kp = __start_lkpi_params; kp < __stop_lkpi_params; kp++) {
			if (!lkpi_param_name_eq(word, kp->name, klen)) continue;
			int ret = kp->ops && kp->ops->set ? kp->ops->set(val, kp) : -EINVAL;
			if (ret) pr_warn("LinuxKPI: parameter %.*s: invalid value '%s' (%d)\n", (int)klen, word, val ? val : "", ret);
			else pr_info("LinuxKPI: parameter %.*s=%s\n", (int)klen, word, val ? val : "1");
			return;
		}
		return;
	}
	for (const struct lkpi_setup *st = __start_lkpi_setup; st < __stop_lkpi_setup; st++) {
		size_t n = strlen(st->str);
		bool with_value = n && st->str[n - 1] == '=';
		if (with_value ? !strncmp(word, st->str, n) : (klen == n && !strncmp(word, st->str, n))) {
			st->fn(word + (with_value ? n : klen));
			pr_info("LinuxKPI: %s\n", word);
			return;
		}
	}
}

void lkpi_params_apply(const char *cmdline)
{
	char *buf, *p, *word;
	if (!cmdline || !*cmdline) return;
	buf = kstrdup(cmdline, GFP_KERNEL);
	if (!buf) return;
	for (p = buf; (word = strsep(&p, " \t\n")) != NULL;)
		if (*word) lkpi_param_word(word);
	kfree(buf);
}

int fb_get_options(const char *name, char **option)
{
	if (option) *option = NULL;
	return 0;
}

bool video_is_primary_device(struct device *dev)
{
	if (!dev || !dev_is_pci(dev)) return false;
	return luos_pci_is_boot_vga(to_pci_dev(dev)->luos_dev) != 0;
}

void lkpi_device_init(void)
{
	for (int i = 0; i < 256; i++) {
		INIT_LIST_HEAD(&irq_descs[i].actions);
		raw_spin_lock_init(&irq_descs[i].lock);
	}
	kobject_init(&lkpi_kernel_kobj, NULL);
}
