// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <linux/acpi.h>
#include <linux/backlight.h>
#include <linux/hmm.h>
#include <linux/hwmon.h>
#include <linux/crc32.h>


bool lkpi_gpu_autoinit;
module_param_named(autoinit, lkpi_gpu_autoinit, bool, 0444);
MODULE_PARM_DESC(autoinit, "probe display controllers as the drivers register (default off: gpu-init starts the stack)");

void emergency_restart(void)
{
	pr_emerg("emergency restart\n");
	luos_reboot();
	for (;;) cpu_relax();
}


int mmu_interval_notifier_insert(struct mmu_interval_notifier *interval_sub, struct mm_struct *mm,
				 unsigned long start, unsigned long length,
				 const struct mmu_interval_notifier_ops *ops)
{
	return -EOPNOTSUPP;
}

void mmu_interval_notifier_remove(struct mmu_interval_notifier *interval_sub) { }


void orderly_poweroff(bool force)
{
	pr_emerg("orderly_poweroff(%d): powering off\n", force);
	luos_poweroff();
	for (;;) cpu_relax();
}

unsigned int topology_num_cores_per_package(void)
{
	unsigned int r[4], vendor_ebx, max_ext;

	luos_cpuid(0, 0, r);
	vendor_ebx = r[1];
	if (vendor_ebx == 0x68747541) {
		luos_cpuid(0x80000000, 0, r);
		max_ext = r[0];
		if (max_ext >= 0x80000008) {
			unsigned int threads, tpc = 1;
			luos_cpuid(0x80000008, 0, r);
			threads = (r[2] & 0xff) + 1;
			if (max_ext >= 0x8000001e) {
				luos_cpuid(0x8000001e, 0, r);
				tpc = ((r[1] >> 8) & 0xff) + 1;
			}
			if (threads >= tpc) return threads / tpc;
		}
	} else if (vendor_ebx == 0x756e6547) {
		luos_cpuid(4, 0, r);
		if (r[0] & 0x1f) return ((r[0] >> 26) & 0x3f) + 1;
	}
	return num_possible_cpus();
}


struct irq_domain *irq_domain_create_linear(struct fwnode_handle *fwnode, unsigned int size,
					    const struct irq_domain_ops *ops, void *host_data)
{
	struct irq_domain *domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain) return NULL;
	domain->ops = ops;
	domain->host_data = host_data;
	domain->size = size;
	return domain;
}

void irq_domain_remove(struct irq_domain *domain) { kfree(domain); }

unsigned int irq_create_mapping(struct irq_domain *domain, irq_hw_number_t hwirq) { return 0; }

int generic_handle_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq) { return -EINVAL; }


acpi_status acpi_get_table(const char *signature, u32 instance, struct acpi_table_header **out_table)
{
	const void *table;
	if (!signature || !out_table) return AE_ERROR;
	table = luos_acpi_find_table(signature, instance ? (int)instance - 1 : 0, NULL);
	if (!table) return AE_NOT_FOUND;
	*out_table = (struct acpi_table_header *)table;
	return AE_OK;
}

const char *acpi_format_exception(acpi_status status)
{
	switch (status) {
	case AE_OK: return "AE_OK";
	case AE_ERROR: return "AE_ERROR";
	case AE_NOT_FOUND: return "AE_NOT_FOUND";
	case AE_NOT_EXIST: return "AE_NOT_EXIST";
	case AE_SUPPORT: return "AE_SUPPORT";
	default: return "UNKNOWN_STATUS_CODE";
	}
}

bool page_is_ram(unsigned long pfn) { return luos_pfn_is_ram(pfn); }


u32 crc32_le(u32 crc, const void *p, size_t len)
{
	static u32 table[256];
	static bool built;
	const u8 *buf = p;

	if (!built) {
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xedb88320 : c >> 1;
			table[i] = c;
		}
		smp_wmb();
		built = true;
	}
	while (len--) crc = table[(crc ^ *buf++) & 0xff] ^ (crc >> 8);
	return crc;
}


int hmm_range_fault(struct hmm_range *range) { return -EOPNOTSUPP; }


static void hwmon_release(struct device *dev) { kfree(dev); }

struct device *hwmon_device_register_with_groups(struct device *dev, const char *name, void *drvdata,
						 const struct attribute_group **groups)
{
	static atomic_t index = ATOMIC_INIT(0);
	struct device *hwdev = kzalloc(sizeof(*hwdev), GFP_KERNEL);
	int ret;

	if (!hwdev) return ERR_PTR(-ENOMEM);
	device_initialize(hwdev);
	hwdev->parent = dev;
	hwdev->release = hwmon_release;
	dev_set_drvdata(hwdev, drvdata);
	dev_set_name(hwdev, "hwmon%d", atomic_fetch_inc(&index));
	ret = device_add(hwdev);
	if (ret) {
		put_device(hwdev);
		return ERR_PTR(ret);
	}
	return hwdev;
}

void hwmon_device_unregister(struct device *dev)
{
	if (dev) device_unregister(dev);
}


static void backlight_release(struct device *dev) { kfree(container_of(dev, struct backlight_device, dev)); }

#define LKPI_BACKLIGHT_MAX 8
static struct backlight_device *lkpi_backlights[LKPI_BACKLIGHT_MAX];
static char lkpi_backlight_names[LKPI_BACKLIGHT_MAX][32];
static int lkpi_backlight_count_v;
static DEFINE_MUTEX(lkpi_backlight_lock);

struct backlight_device *backlight_device_register(const char *name, struct device *parent, void *devdata,
						   const struct backlight_ops *ops, const struct backlight_properties *props)
{
	struct backlight_device *bd = kzalloc(sizeof(*bd), GFP_KERNEL);
	int ret;

	if (!bd) return ERR_PTR(-ENOMEM);
	mutex_init(&bd->update_lock);
	mutex_init(&bd->ops_lock);
	device_initialize(&bd->dev);
	bd->dev.parent = parent;
	bd->dev.release = backlight_release;
	dev_set_name(&bd->dev, "%s", name);
	bd->ops = ops;
	if (props) {
		memcpy(&bd->props, props, sizeof(bd->props));
		if (props->max_brightness < props->brightness) bd->props.brightness = props->max_brightness;
	}
	bd->dev.driver_data = devdata;
        ret = device_add(&bd->dev);
        if (ret) {
                put_device(&bd->dev);
                return ERR_PTR(ret);
        }
        mutex_lock(&lkpi_backlight_lock);
        if (lkpi_backlight_count_v < LKPI_BACKLIGHT_MAX) {
                lkpi_backlights[lkpi_backlight_count_v] = bd;
                strscpy(lkpi_backlight_names[lkpi_backlight_count_v], name ? name : "backlight",
                        sizeof(lkpi_backlight_names[0]));
                lkpi_backlight_count_v++;
        }
        mutex_unlock(&lkpi_backlight_lock);
        return bd;
}

void backlight_device_unregister(struct backlight_device *bd)
{
	int i;
	if (!bd) return;
	mutex_lock(&lkpi_backlight_lock);
	for (i = 0; i < lkpi_backlight_count_v; i++) {
		if (lkpi_backlights[i] != bd) continue;
		memmove(&lkpi_backlights[i], &lkpi_backlights[i + 1],
			(lkpi_backlight_count_v - i - 1) * sizeof(lkpi_backlights[0]));
		memmove(&lkpi_backlight_names[i], &lkpi_backlight_names[i + 1],
			(lkpi_backlight_count_v - i - 1) * sizeof(lkpi_backlight_names[0]));
		lkpi_backlight_count_v--;
		break;
	}
	mutex_unlock(&lkpi_backlight_lock);
	mutex_lock(&bd->ops_lock);
	bd->ops = NULL;
	mutex_unlock(&bd->ops_lock);
	device_unregister(&bd->dev);
}

int lkpi_backlight_count(void)
{
	return lkpi_backlight_count_v;
}

static struct backlight_device *lkpi_backlight_ref(int idx, char *name, int name_cap)
{
	struct backlight_device *bd = NULL;
	mutex_lock(&lkpi_backlight_lock);
	if (idx >= 0 && idx < lkpi_backlight_count_v && lkpi_backlights[idx]) {
		bd = lkpi_backlights[idx];
		get_device(&bd->dev);
		if (name && name_cap > 0) strscpy(name, lkpi_backlight_names[idx], name_cap);
	}
	mutex_unlock(&lkpi_backlight_lock);
	return bd;
}

int lkpi_backlight_get(int idx, char *name, int name_cap, unsigned int *cur, unsigned int *max)
{
	struct backlight_device *bd = lkpi_backlight_ref(idx, name, name_cap);
	int now;
	if (!bd) return -ENOENT;
	mutex_lock(&bd->ops_lock);
	now = bd->props.brightness;
	if (bd->ops && bd->ops->get_brightness) {
		int hw = bd->ops->get_brightness(bd);
		if (hw >= 0 && hw <= bd->props.max_brightness) now = hw;
	}
	mutex_unlock(&bd->ops_lock);
	if (cur) *cur = (unsigned int)now;
	if (max) *max = (unsigned int)bd->props.max_brightness;
	put_device(&bd->dev);
	return 0;
}

int lkpi_backlight_set(int idx, unsigned int value)
{
	struct backlight_device *bd = lkpi_backlight_ref(idx, NULL, 0);
	const struct backlight_ops *ops;
	int ret = 0;
	if (!bd) return -ENOENT;
	if (!bd->props.max_brightness) {
		put_device(&bd->dev);
		return -EIO;
	}
	if (value > (unsigned int)bd->props.max_brightness) value = (unsigned int)bd->props.max_brightness;
	mutex_lock(&bd->update_lock);
	mutex_lock(&bd->ops_lock);
	ops = bd->ops;
	if (ops && ops->update_status) {
		bd->props.brightness = (int)value;
		bd->props.power = BACKLIGHT_POWER_ON;
		ret = ops->update_status(bd);
	} else {
		ret = -EIO;
	}
	mutex_unlock(&bd->ops_lock);
	mutex_unlock(&bd->update_lock);
	put_device(&bd->dev);
	return ret;
}
