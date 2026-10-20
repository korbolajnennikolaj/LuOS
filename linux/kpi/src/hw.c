// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <drm/intel/i915_drm.h>
#include <drm/intel/pciids.h>
#include <drm/intel/intel-gtt.h>
#include <linux/auxiliary_bus.h>
#include <linux/relay.h>


struct resource intel_graphics_stolen_res = DEFINE_RES_MEM(0, 0);

struct intel_stolen_ops {
	resource_size_t (*size)(struct pci_dev *gpu);
	resource_size_t (*base)(struct pci_dev *gpu, resource_size_t size);
};

static u16 host_bridge_read16(int where)
{
	struct pci_dev *bridge = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	u16 v = 0;
	if (bridge) {
		pci_read_config_word(bridge, where, &v);
		pci_dev_put(bridge);
	}
	return v;
}

static resource_size_t gen3_stolen_base(struct pci_dev *gpu, resource_size_t size)
{
	u32 bsm = 0;
	pci_read_config_dword(gpu, INTEL_BSM, &bsm);
	return bsm & INTEL_BSM_MASK;
}

static resource_size_t gen11_stolen_base(struct pci_dev *gpu, resource_size_t size)
{
	u32 lo = 0, hi = 0;
	pci_read_config_dword(gpu, INTEL_GEN11_BSM_DW0, &lo);
	pci_read_config_dword(gpu, INTEL_GEN11_BSM_DW1, &hi);
	return ((u64)hi << 32) | (lo & INTEL_BSM_MASK);
}

static resource_size_t gen3_stolen_size(struct pci_dev *gpu)
{
	u16 gmch_ctrl = host_bridge_read16(I830_GMCH_CTRL);
	switch (gmch_ctrl & I855_GMCH_GMS_MASK) {
	case I855_GMCH_GMS_STOLEN_1M: return SZ_1M;
	case I855_GMCH_GMS_STOLEN_4M: return SZ_4M;
	case I855_GMCH_GMS_STOLEN_8M: return SZ_8M;
	case I855_GMCH_GMS_STOLEN_16M: return SZ_16M;
	case I855_GMCH_GMS_STOLEN_32M: return SZ_32M;
	case I915_GMCH_GMS_STOLEN_48M: return 48 * SZ_1M;
	case I915_GMCH_GMS_STOLEN_64M: return SZ_64M;
	case G33_GMCH_GMS_STOLEN_128M: return SZ_128M;
	case G33_GMCH_GMS_STOLEN_256M: return SZ_256M;
	case INTEL_GMCH_GMS_STOLEN_96M: return 96 * SZ_1M;
	case INTEL_GMCH_GMS_STOLEN_160M: return 160 * SZ_1M;
	case INTEL_GMCH_GMS_STOLEN_224M: return 224 * SZ_1M;
	case INTEL_GMCH_GMS_STOLEN_352M: return 352 * SZ_1M;
	default:
		pr_warn("Intel graphics: unknown GMCH_CTRL value %x\n", gmch_ctrl);
		return 0;
	}
}

static u16 gpu_gmch_ctrl(struct pci_dev *gpu)
{
	u16 v = 0;
	pci_read_config_word(gpu, SNB_GMCH_CTRL, &v);
	return v;
}

static resource_size_t gen6_stolen_size(struct pci_dev *gpu)
{
	return ((gpu_gmch_ctrl(gpu) >> SNB_GMCH_GMS_SHIFT) & SNB_GMCH_GMS_MASK) * SZ_32M;
}

static resource_size_t gen8_stolen_size(struct pci_dev *gpu)
{
	return ((gpu_gmch_ctrl(gpu) >> BDW_GMCH_GMS_SHIFT) & BDW_GMCH_GMS_MASK) * SZ_32M;
}

static resource_size_t chv_stolen_size(struct pci_dev *gpu)
{
	u16 gms = (gpu_gmch_ctrl(gpu) >> SNB_GMCH_GMS_SHIFT) & SNB_GMCH_GMS_MASK;
	if (gms < 0x11) return gms * SZ_32M;
	if (gms < 0x17) return (gms - 0x11) * SZ_4M + SZ_8M;
	return (gms - 0x17) * SZ_4M + 36 * SZ_1M;
}

static resource_size_t gen9_stolen_size(struct pci_dev *gpu)
{
	u16 gms = (gpu_gmch_ctrl(gpu) >> BDW_GMCH_GMS_SHIFT) & BDW_GMCH_GMS_MASK;
	if (gms < 0xf0) return gms * SZ_32M;
	return (gms - 0xf0) * SZ_4M + SZ_4M;
}

static const struct intel_stolen_ops gen3_ops = { gen3_stolen_size, gen3_stolen_base };
static const struct intel_stolen_ops gen6_ops = { gen6_stolen_size, gen3_stolen_base };
static const struct intel_stolen_ops gen8_ops = { gen8_stolen_size, gen3_stolen_base };
static const struct intel_stolen_ops chv_ops = { chv_stolen_size, gen3_stolen_base };
static const struct intel_stolen_ops gen9_ops = { gen9_stolen_size, gen3_stolen_base };
static const struct intel_stolen_ops gen11_ops = { gen9_stolen_size, gen11_stolen_base };

#define LKPI_STOLEN_ID(_id, _ops) { .device = (_id), .ops = (_ops) }
static const struct { u16 device; const struct intel_stolen_ops *ops; } intel_stolen_ids[] = {
	INTEL_I915G_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_I915GM_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_I945G_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_I945GM_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_VLV_IDS(LKPI_STOLEN_ID, &gen6_ops),
	INTEL_PNV_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_I965G_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_G33_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_I965GM_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_GM45_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_G45_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_ILK_IDS(LKPI_STOLEN_ID, &gen3_ops),
	INTEL_SNB_IDS(LKPI_STOLEN_ID, &gen6_ops),
	INTEL_IVB_IDS(LKPI_STOLEN_ID, &gen6_ops),
	INTEL_HSW_IDS(LKPI_STOLEN_ID, &gen6_ops),
	INTEL_BDW_IDS(LKPI_STOLEN_ID, &gen8_ops),
	INTEL_CHV_IDS(LKPI_STOLEN_ID, &chv_ops),
	INTEL_SKL_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_BXT_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_KBL_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_CFL_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_WHL_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_CML_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_GLK_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_CNL_IDS(LKPI_STOLEN_ID, &gen9_ops),
	INTEL_ICL_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_EHL_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_JSL_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_TGL_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_RKL_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_ADLS_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_ADLP_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_ADLN_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_RPLS_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_RPLU_IDS(LKPI_STOLEN_ID, &gen11_ops),
	INTEL_RPLP_IDS(LKPI_STOLEN_ID, &gen11_ops),
};

void lkpi_intel_stolen_init(void)
{
	struct pci_dev *gpu = NULL;
	while ((gpu = pci_get_device(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, gpu))) {
		if ((gpu->class >> 16) != PCI_BASE_CLASS_DISPLAY || gpu->bus->number != 0) continue;
		for (size_t i = 0; i < ARRAY_SIZE(intel_stolen_ids); i++) {
			if (intel_stolen_ids[i].device != gpu->device) continue;
			resource_size_t size = intel_stolen_ids[i].ops->size(gpu);
			resource_size_t base = intel_stolen_ids[i].ops->base(gpu, size);
			if (size && base) {
				intel_graphics_stolen_res.start = base;
				intel_graphics_stolen_res.end = base + size - 1;
				luos_pmm_reserve_range(base, size);
				pr_info("Intel graphics stolen memory at %pR\n", &intel_graphics_stolen_res);
			}
			pci_dev_put(gpu);
			return;
		}
	}
}




void ips_link_to_i915_driver(void) { }


unsigned int tsc_khz;

const struct x86_cpu_id *x86_match_cpu(const struct x86_cpu_id *match)
{
	const struct cpuinfo_x86 *c = &boot_cpu_data;
	for (const struct x86_cpu_id *m = match; m->flags & X86_CPU_ID_FLAG_ENTRY_VALID; m++) {
		if (m->vendor != X86_VENDOR_ANY && c->x86_vendor != m->vendor) continue;
		if (m->family != X86_FAMILY_ANY && c->x86 != m->family) continue;
		if (m->model != X86_MODEL_ANY && c->x86_model != m->model) continue;
		if (m->steppings != X86_STEPPING_ANY && !(BIT(c->x86_stepping) & m->steppings)) continue;
		if (m->feature != X86_FEATURE_ANY && !lkpi_boot_cpu_has(m->feature)) continue;
		return m;
	}
	return NULL;
}


int _atomic_dec_and_lock_irqsave(atomic_t *atomic, spinlock_t *lock, unsigned long *flags)
{
	if (atomic_add_unless(atomic, -1, 1)) return 0;
	spin_lock_irqsave(lock, *flags);
	if (atomic_dec_and_test(atomic)) return 1;
	spin_unlock_irqrestore(lock, *flags);
	return 0;
}

int on_each_cpu(void (*func)(void *info), void *info, int wait)
{
	unsigned long flags;
	local_irq_save(flags);
	func(info);
	local_irq_restore(flags);
	return 0;
}

int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	unsigned long flags;
	int ret;
	local_irq_save(flags);
	ret = fn(data);
	local_irq_restore(flags);
	return ret;
}

bool synchronize_hardirq(unsigned int irq)
{
	synchronize_irq(irq);
	return true;
}

int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen,
		       bool ascii)
{
	const u8 *ptr = buf;
	size_t lx = 0;
	if (rowsize != 16 && rowsize != 32) rowsize = 16;
	if (len > (size_t)rowsize) len = rowsize;
	if (!is_power_of_2(groupsize) || groupsize > 8 || len % groupsize) groupsize = 1;
	if (!linebuflen) return 0;
	linebuf[0] = 0;
	for (size_t j = 0; j < len; j += groupsize) {
		u64 v = 0;
		memcpy(&v, ptr + j, groupsize);
		int n = scnprintf(linebuf + lx, linebuflen - lx, "%s%0*llx", j ? " " : "", groupsize * 2,
				  (unsigned long long)v);
		lx += n;
	}
	if (ascii) {
		lx += scnprintf(linebuf + lx, linebuflen - lx, "  ");
		for (size_t j = 0; j < len && lx + 1 < linebuflen; j++) {
			u8 ch = ptr[j];
			linebuf[lx++] = (isascii(ch) && isprint(ch)) ? ch : '.';
		}
		linebuf[lx] = 0;
	}
	return lx;
}


ssize_t kernel_write(struct file *file, const void *buf, size_t count, loff_t *pos) { return -EINVAL; }

int vfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!file->f_op || !file->f_op->mmap) return -ENODEV;
	return file->f_op->mmap(file, vma);
}

struct file *get_file_active(struct file **f)
{
	struct file *file;
	rcu_read_lock();
	file = READ_ONCE(*f);
	if (file && !atomic_long_inc_not_zero(&file->f_ref)) file = NULL;
	rcu_read_unlock();
	return file;
}

const int sysctl_vals[] = { -1, 0, 1, 2, 4, 100, 200, 1000, 3000, INT_MAX, 65535, -1 };
int proc_dointvec_minmax(const struct ctl_table *table, int write, void *buffer, size_t *lenp, loff_t *ppos) { return -EINVAL; }
int proc_dointvec(const struct ctl_table *table, int write, void *buffer, size_t *lenp, loff_t *ppos) { return -EINVAL; }


static ssize_t kobj_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct kobj_attribute *kattr = container_of(attr, struct kobj_attribute, attr);
	return kattr->show ? kattr->show(kobj, kattr, buf) : -EIO;
}

static ssize_t kobj_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
	struct kobj_attribute *kattr = container_of(attr, struct kobj_attribute, attr);
	return kattr->store ? kattr->store(kobj, kattr, buf, count) : -EIO;
}

const struct sysfs_ops kobj_sysfs_ops = { .show = kobj_attr_show, .store = kobj_attr_store };


const char power_group_name[] = "power";
int pm_generic_suspend(struct device *dev) { return 0; }
int pm_generic_resume(struct device *dev) { return 0; }
int pm_generic_freeze(struct device *dev) { return 0; }
int pm_generic_thaw(struct device *dev) { return 0; }
int pm_generic_poweroff(struct device *dev) { return 0; }
int pm_generic_restore(struct device *dev) { return 0; }
int pm_generic_runtime_suspend(struct device *dev) { return 0; }
int pm_generic_runtime_resume(struct device *dev) { return 0; }


resource_size_t pcibios_align_resource(void *data, const struct resource *res, resource_size_t size,
				       resource_size_t align)
{
	return res->start;
}

int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res, resource_size_t size, resource_size_t align,
			   resource_size_t min, unsigned long type_mask,
			   resource_size_t (*alignf)(void *, const struct resource *, resource_size_t, resource_size_t),
			   void *alignf_data)
{
	return -ENOMEM;
}

int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val)
{
	u32 v;
	int ret = pci_bus_read_config_dword(bus, devfn, where & ~3, &v);
	*val = ret ? 0xff : (v >> ((where & 3) * 8)) & 0xff;
	return ret;
}

int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val)
{
	u32 v;
	int ret = pci_bus_read_config_dword(bus, devfn, where & ~3, &v);
	if (ret) return ret;
	v &= ~(0xffu << ((where & 3) * 8));
	v |= (u32)val << ((where & 3) * 8);
	return pci_bus_write_config_dword(bus, devfn, where & ~3, v);
}


struct platform_device *platform_device_register_full(const struct platform_device_info *pdevinfo)
{
	return platform_device_register_resndata(pdevinfo->parent, pdevinfo->name, pdevinfo->id, pdevinfo->res,
						 pdevinfo->num_res, pdevinfo->data, pdevinfo->size_data);
}

void handle_simple_irq(struct irq_desc *desc) { }


int auxiliary_device_init(struct auxiliary_device *auxdev)
{
	struct device *dev = &auxdev->dev;
	if (!dev->parent || !auxdev->name) return -EINVAL;
	device_initialize(dev);
	mutex_init(&auxdev->sysfs.lock);
	xa_init(&auxdev->sysfs.irqs);
	return 0;
}

int __auxiliary_device_add(struct auxiliary_device *auxdev, const char *modname) { return -ENODEV; }


struct rchan *relay_open(const char *base_filename, struct dentry *parent, size_t subbuf_size, size_t n_subbufs,
			 const struct rchan_callbacks *cb, void *private_data)
{
	return NULL;
}
void relay_close(struct rchan *chan) { }
void relay_flush(struct rchan *chan) { }
size_t relay_stats(struct rchan *chan, int flags) { return 0; }
void relay_subbufs_consumed(struct rchan *chan, unsigned int cpu, size_t consumed) { }
void relay_reset(struct rchan *chan) { }
int relay_buf_full(struct rchan_buf *buf) { return 1; }
size_t relay_switch_subbuf(struct rchan_buf *buf, size_t length) { return 0; }
const struct file_operations relay_file_operations = { };


struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	struct shrinker *s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (s) s->flags = flags;
	return s;
}

void shrinker_register(struct shrinker *shrinker) { shrinker->flags |= SHRINKER_REGISTERED; }
void shrinker_free(struct shrinker *shrinker) { kfree(shrinker); }


void copy_highpage(struct page *to, struct page *from) { memcpy(page_address(to), page_address(from), PAGE_SIZE); }
