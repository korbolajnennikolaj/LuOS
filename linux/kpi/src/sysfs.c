// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"

enum { SYSFS_NONE, SYSFS_DIR, SYSFS_FILE, SYSFS_LINK };

enum sysfs_attr {
	A_NONE,
	A_PCI_VENDOR, A_PCI_DEVICE, A_PCI_SUBVENDOR, A_PCI_SUBDEVICE, A_PCI_REVISION, A_PCI_CLASS, A_PCI_IRQ,
	A_PCI_BOOT_VGA, A_PCI_ENABLE, A_PCI_UEVENT, A_PCI_CONFIG, A_PCI_RESOURCE, A_PCI_LOCAL_CPULIST,
	A_NUMA_NODE, A_DEV, A_DEV_UEVENT, A_CPU_ONLINE, A_CPU_POSSIBLE, A_CPU_ONE_ONLINE,
};

struct sysfs_node {
	char name[64];
	int type;
	struct sysfs_node *parent, *children, *next;
	char *target;
	struct device *dev;
	int attr;
	char devname[64];
};

static DEFINE_MUTEX(sysfs_lock);
static struct sysfs_node *sysfs_root;
static unsigned long sysfs_generation = ~0UL;
static unsigned long sysfs_ino_seed = 0x5f5f0000;

static void node_free(struct sysfs_node *n)
{
	while (n) {
		struct sysfs_node *next = n->next;
		node_free(n->children);
		kfree(n->target);
		kfree(n);
		n = next;
	}
}

static struct sysfs_node *node_find(struct sysfs_node *dir, const char *name, size_t len)
{
	for (struct sysfs_node *c = dir->children; c; c = c->next)
		if (strlen(c->name) == len && !strncmp(c->name, name, len)) return c;
	return NULL;
}

static struct sysfs_node *node_add(struct sysfs_node *dir, const char *name, int type)
{
	struct sysfs_node *n = node_find(dir, name, strlen(name));
	if (n) return n;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n) return NULL;
	strscpy(n->name, name, sizeof(n->name));
	n->type = type;
	n->parent = dir;
	n->next = dir->children;
	dir->children = n;
	return n;
}

static struct sysfs_node *mkdir_p(const char *path)
{
	struct sysfs_node *dir = sysfs_root;
	const char *p = path + 4;
	while (dir && *p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *e = strchr(p, '/');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		char name[64];
		if (len >= sizeof(name)) return NULL;
		memcpy(name, p, len);
		name[len] = 0;
		dir = node_add(dir, name, SYSFS_DIR);
		p += len;
	}
	return dir;
}

static void node_path(struct sysfs_node *n, char *buf, size_t cap)
{
	const char *parts[32];
	int depth = 0;
	for (; n && n != sysfs_root && depth < 32; n = n->parent) parts[depth++] = n->name;
	size_t len = scnprintf(buf, cap, "/sys");
	while (depth-- > 0) len += scnprintf(buf + len, cap - len, "/%s", parts[depth]);
}

static void rel_path(const char *from, const char *to, char *out, size_t cap)
{
	size_t common = 0, i = 0;
	while (from[i] && to[i] && from[i] == to[i]) {
		if (from[i] == '/') common = i;
		i++;
	}
	if (!from[i] && (to[i] == '/' || !to[i])) common = i;
	size_t len = 0;
	out[0] = 0;
	for (const char *p = from + common; *p; p++)
		if (*p == '/') len += scnprintf(out + len, cap - len, "../");
	scnprintf(out + len, cap - len, "%s", to[common] == '/' ? to + common + 1 : to + common);
}

static void add_link(const char *dir_path, const char *name, const char *target)
{
	struct sysfs_node *dir = mkdir_p(dir_path);
	char rel[256];
	if (!dir) return;
	struct sysfs_node *n = node_add(dir, name, SYSFS_LINK);
	if (!n || n->target) return;
	rel_path(dir_path, target, rel, sizeof(rel));
	n->target = kstrdup(rel, GFP_KERNEL);
}

static void add_attr(struct sysfs_node *dir, const char *name, struct device *dev, int attr)
{
	struct sysfs_node *n = node_add(dir, name, SYSFS_FILE);
	if (!n) return;
	n->dev = dev;
	n->attr = attr;
}

static bool device_dir(struct device *dev, char *buf, size_t cap, int depth)
{
	char base[256];
	if (depth > 16) return false;
	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);
		if (pdev->bus->self && device_dir(&pdev->bus->self->dev, base, sizeof(base), depth + 1))
			scnprintf(buf, cap, "%s/%s", base, pci_name(pdev));
		else
			scnprintf(buf, cap, "/sys/devices/pci%04x:%02x/%s", pdev->bus->domain_nr, pdev->bus->number, pci_name(pdev));
		return true;
	}
	if (dev->class) {
		if (!dev->parent) {
			scnprintf(buf, cap, "/sys/devices/virtual/%s/%s", dev->class->name, dev_name(dev));
			return true;
		}
		if (!device_dir(dev->parent, base, sizeof(base), depth + 1)) return false;
		if (dev->parent->class == dev->class) scnprintf(buf, cap, "%s/%s", base, dev_name(dev));
		else scnprintf(buf, cap, "%s/%s/%s", base, dev->class->name, dev_name(dev));
		return true;
	}
	if (dev->bus == &platform_bus_type && !dev->parent) {
		scnprintf(buf, cap, "/sys/devices/platform/%s", dev_name(dev));
		return true;
	}
	if (dev->parent && device_dir(dev->parent, base, sizeof(base), depth + 1)) {
		scnprintf(buf, cap, "%s/%s", base, dev_name(dev));
		return true;
	}
	return false;
}

static void build_device(struct device *dev, void *arg)
{
	char path[256], target[256];
	struct sysfs_node *dir;
	if (!device_dir(dev, path, sizeof(path), 0)) return;
	dir = mkdir_p(path);
	if (!dir) return;
	dir->dev = dev;
	add_attr(dir, "uevent", dev, dev_is_pci(dev) ? A_PCI_UEVENT : A_DEV_UEVENT);

	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);
		add_attr(dir, "vendor", dev, A_PCI_VENDOR);
		add_attr(dir, "device", dev, A_PCI_DEVICE);
		add_attr(dir, "subsystem_vendor", dev, A_PCI_SUBVENDOR);
		add_attr(dir, "subsystem_device", dev, A_PCI_SUBDEVICE);
		add_attr(dir, "revision", dev, A_PCI_REVISION);
		add_attr(dir, "class", dev, A_PCI_CLASS);
		add_attr(dir, "irq", dev, A_PCI_IRQ);
		add_attr(dir, "enable", dev, A_PCI_ENABLE);
		add_attr(dir, "config", dev, A_PCI_CONFIG);
		add_attr(dir, "resource", dev, A_PCI_RESOURCE);
		add_attr(dir, "numa_node", dev, A_NUMA_NODE);
		add_attr(dir, "local_cpulist", dev, A_PCI_LOCAL_CPULIST);
		if ((pdev->class >> 16) == PCI_BASE_CLASS_DISPLAY) add_attr(dir, "boot_vga", dev, A_PCI_BOOT_VGA);
		add_link(path, "subsystem", "/sys/bus/pci");
		add_link("/sys/bus/pci/devices", pci_name(pdev), path);
		if (pdev->driver) {
			scnprintf(target, sizeof(target), "/sys/bus/pci/drivers/%s", pdev->driver->name);
			add_link(path, "driver", target);
			add_link(target, pci_name(pdev), path);
		}
		return;
	}

	if (dev->devt) {
		add_attr(dir, "dev", dev, A_DEV);
		const char *dn = lkpi_device_devname(dev);
		if (dn) strscpy(dir->devname, dn, sizeof(dir->devname));
		scnprintf(target, sizeof(target), "/sys/dev/char/%u:%u", MAJOR(dev->devt), MINOR(dev->devt));
		add_link("/sys/dev/char", target + strlen("/sys/dev/char/"), path);
	}
	if (dev->class) {
		scnprintf(target, sizeof(target), "/sys/class/%s", dev->class->name);
		add_link(path, "subsystem", target);
		add_link(target, dev_name(dev), path);
	} else if (dev->bus == &platform_bus_type) {
		add_link(path, "subsystem", "/sys/bus/platform");
		add_link("/sys/bus/platform/devices", dev_name(dev), path);
	}
	if (dev->parent && device_dir(dev->parent, target, sizeof(target), 0)) add_link(path, "device", target);
}

static void sysfs_rebuild(void)
{
	if (sysfs_generation == READ_ONCE(lkpi_device_generation) && sysfs_root) return;
	node_free(sysfs_root);
	sysfs_root = kzalloc(sizeof(*sysfs_root), GFP_KERNEL);
	if (!sysfs_root) return;
	sysfs_root->type = SYSFS_DIR;
	strscpy(sysfs_root->name, "sys", sizeof(sysfs_root->name));
	mkdir_p("/sys/dev/char");
	mkdir_p("/sys/dev/block");
	mkdir_p("/sys/class/drm");
	mkdir_p("/sys/bus/pci/devices");
	mkdir_p("/sys/bus/pci/drivers");
	struct sysfs_node *cpus = mkdir_p("/sys/devices/system/cpu");
	if (cpus) {
		unsigned int cpu;
		add_attr(cpus, "online", NULL, A_CPU_ONLINE);
		add_attr(cpus, "present", NULL, A_CPU_POSSIBLE);
		add_attr(cpus, "possible", NULL, A_CPU_POSSIBLE);
		for_each_possible_cpu(cpu) {
			char name[16];
			scnprintf(name, sizeof(name), "cpu%u", cpu);
			struct sysfs_node *c = node_add(cpus, name, SYSFS_DIR);
			if (c) {
				add_attr(c, "online", NULL, A_CPU_ONE_ONLINE);
				c->attr = cpu;
			}
		}
	}
	mkdir_p("/sys/kernel");
	mkdir_p("/sys/module");
	mkdir_p("/sys/firmware");
	sysfs_generation = READ_ONCE(lkpi_device_generation);
	lkpi_for_each_device(build_device, NULL);
}

static struct sysfs_node *sysfs_walk(const char *path, bool follow, int depth)
{
	struct sysfs_node *n = sysfs_root;
	const char *p;
	if (!n || depth > 16) return NULL;
	if (strncmp(path, "/sys", 4) || (path[4] && path[4] != '/')) return NULL;
	p = path + 4;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *e = strchr(p, '/');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		bool last = !e || !e[strspn(e, "/")];
		if (n->type != SYSFS_DIR) return NULL;
		if (len == 1 && p[0] == '.') {
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			if (n->parent) n = n->parent;
		} else {
			struct sysfs_node *c = node_find(n, p, len);
			if (!c) return NULL;
			if (c->type == SYSFS_LINK && (!last || follow)) {
				char dir[256], target[512];
				node_path(n, dir, sizeof(dir));
				if (c->target[0] == '/') strscpy(target, c->target, sizeof(target));
				else scnprintf(target, sizeof(target), "%s/%s", dir, c->target);
				c = sysfs_walk(target, true, depth + 1);
				if (!c) return NULL;
			}
			n = c;
		}
		p += len;
	}
	return n;
}

static size_t attr_render(struct sysfs_node *n, char *buf, size_t cap)
{
	struct device *dev = n->dev;
	struct pci_dev *pdev = dev && dev_is_pci(dev) ? to_pci_dev(dev) : NULL;
	switch (n->attr) {
	case A_PCI_VENDOR: return scnprintf(buf, cap, "0x%04x\n", pdev->vendor);
	case A_PCI_DEVICE: return scnprintf(buf, cap, "0x%04x\n", pdev->device);
	case A_PCI_SUBVENDOR: return scnprintf(buf, cap, "0x%04x\n", pdev->subsystem_vendor);
	case A_PCI_SUBDEVICE: return scnprintf(buf, cap, "0x%04x\n", pdev->subsystem_device);
	case A_PCI_REVISION: return scnprintf(buf, cap, "0x%02x\n", pdev->revision);
	case A_PCI_CLASS: return scnprintf(buf, cap, "0x%06x\n", pdev->class);
	case A_PCI_IRQ: return scnprintf(buf, cap, "%u\n", pdev->irq);
	case A_PCI_ENABLE: return scnprintf(buf, cap, "%d\n", atomic_read(&pdev->enable_cnt) > 0);
	case A_PCI_BOOT_VGA: return scnprintf(buf, cap, "%d\n", luos_pci_is_boot_vga(pdev->luos_dev) ? 1 : 0);
	case A_NUMA_NODE: return scnprintf(buf, cap, "-1\n");
	case A_PCI_LOCAL_CPULIST: return scnprintf(buf, cap, "0-%u\n", num_online_cpus() - 1);
	case A_PCI_UEVENT:
		return scnprintf(buf, cap,
				 "DRIVER=%s\nPCI_CLASS=%X\nPCI_ID=%04X:%04X\nPCI_SUBSYS_ID=%04X:%04X\nPCI_SLOT_NAME=%s\n"
				 "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X\n",
				 pdev->driver ? pdev->driver->name : "", pdev->class, pdev->vendor, pdev->device,
				 pdev->subsystem_vendor, pdev->subsystem_device, pci_name(pdev), pdev->vendor, pdev->device,
				 pdev->subsystem_vendor, pdev->subsystem_device, pdev->class >> 16, (pdev->class >> 8) & 0xff,
				 pdev->class & 0xff);
	case A_PCI_CONFIG: {
		size_t len = min_t(size_t, cap, 256) & ~3UL;
		for (size_t off = 0; off < len; off += 4) {
			u32 v;
			pci_read_config_dword(pdev, (int)off, &v);
			memcpy(buf + off, &v, 4);
		}
		return len;
	}
	case A_PCI_RESOURCE: {
		size_t len = 0;
		for (int i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
			struct resource *r = &pdev->resource[i];
			len += scnprintf(buf + len, cap - len, "0x%016llx 0x%016llx 0x%016llx\n",
					 (unsigned long long)(r->flags ? r->start : 0), (unsigned long long)(r->flags ? r->end : 0),
					 (unsigned long long)r->flags);
		}
		return len;
	}
	case A_CPU_ONLINE:
		return num_online_cpus() > 1 ? scnprintf(buf, cap, "0-%u\n", num_online_cpus() - 1) : scnprintf(buf, cap, "0\n");
	case A_CPU_POSSIBLE:
		return num_possible_cpus() > 1 ? scnprintf(buf, cap, "0-%u\n", num_possible_cpus() - 1) : scnprintf(buf, cap, "0\n");
	case A_CPU_ONE_ONLINE: return scnprintf(buf, cap, "%d\n", cpu_online(n->parent->attr) ? 1 : 0);
	case A_DEV: return scnprintf(buf, cap, "%u:%u\n", MAJOR(dev->devt), MINOR(dev->devt));
	case A_DEV_UEVENT: {
		size_t len = 0;
		struct sysfs_node *dir = n->parent;
		if (dev->devt) len += scnprintf(buf + len, cap - len, "MAJOR=%u\nMINOR=%u\n", MAJOR(dev->devt), MINOR(dev->devt));
		if (dir && dir->devname[0]) len += scnprintf(buf + len, cap - len, "DEVNAME=%s\n", dir->devname);
		if (dev->type && dev->type->name) len += scnprintf(buf + len, cap - len, "DEVTYPE=%s\n", dev->type->name);
		if (dev->driver && dev->driver->name) len += scnprintf(buf + len, cap - len, "DRIVER=%s\n", dev->driver->name);
		if (dev->bus == &platform_bus_type)
			len += scnprintf(buf + len, cap - len, "MODALIAS=platform:%s\n", to_platform_device(dev)->name);
		return len;
	}
	}
	return 0;
}


int lkpi_sysfs_lookup(const char *path, int follow, int *type, unsigned long *size, unsigned long *ino)
{
	static char tmp[4096];
	int ret = -ENOENT;
	mutex_lock(&sysfs_lock);
	sysfs_rebuild();
	struct sysfs_node *n = sysfs_walk(path, follow != 0, 0);
	if (n) {
		*type = n->type;
		*size = n->type == SYSFS_FILE ? attr_render(n, tmp, sizeof(tmp)) : n->type == SYSFS_LINK ? strlen(n->target) : 4096;
		*ino = sysfs_ino_seed ^ (unsigned long)n;
		ret = 0;
	}
	mutex_unlock(&sysfs_lock);
	return ret;
}

long lkpi_sysfs_read(const char *path, char *buf, unsigned long cap)
{
	long ret = -ENOENT;
	mutex_lock(&sysfs_lock);
	sysfs_rebuild();
	struct sysfs_node *n = sysfs_walk(path, true, 0);
	if (n) ret = n->type == SYSFS_FILE ? (long)attr_render(n, buf, cap) : -EISDIR;
	mutex_unlock(&sysfs_lock);
	return ret;
}

long lkpi_sysfs_readlink(const char *path, char *buf, unsigned long cap)
{
	long ret = -ENOENT;
	mutex_lock(&sysfs_lock);
	sysfs_rebuild();
	struct sysfs_node *n = sysfs_walk(path, false, 0);
	if (n) {
		if (n->type != SYSFS_LINK) {
			ret = -EINVAL;
		} else {
			size_t len = min_t(size_t, strlen(n->target), cap);
			memcpy(buf, n->target, len);
			ret = (long)len;
		}
	}
	mutex_unlock(&sysfs_lock);
	return ret;
}

int lkpi_sysfs_readdir(const char *path, unsigned long index, char *name, unsigned long cap, int *type)
{
	int ret = -ENOENT;
	mutex_lock(&sysfs_lock);
	sysfs_rebuild();
	struct sysfs_node *n = sysfs_walk(path, true, 0);
	if (n && n->type == SYSFS_DIR) {
		struct sysfs_node *c = n->children;
		while (c && index--) c = c->next;
		ret = 0;
		if (c) {
			strscpy(name, c->name, cap);
			*type = c->type;
			ret = 1;
		}
	} else if (n) {
		ret = -ENOTDIR;
	}
	mutex_unlock(&sysfs_lock);
	return ret;
}
