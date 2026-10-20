// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_GPU_H
#define _LKPI_GPU_H

#include <lkpi/misc.h>

#define PM_HIBERNATION_PREPARE 0x0001
#define PM_POST_HIBERNATION 0x0002
#define PM_SUSPEND_PREPARE 0x0003
#define PM_POST_SUSPEND 0x0004
#define PM_RESTORE_PREPARE 0x0005
#define PM_POST_RESTORE 0x0006

static inline bool pm_hibernate_is_recovering(void) { return false; }
static inline bool pm_hibernation_mode_is_suspend(void) { return false; }
static inline int ksys_sync_helper(void) { return 0; }
static inline u64 pm_runtime_autosuspend_expiration(struct device *dev) { return 0; }

void emergency_restart(void);

#include <linux/interval_tree.h>
#include <linux/range.h>

enum mmu_notifier_event {
	MMU_NOTIFY_UNMAP = 0, MMU_NOTIFY_CLEAR, MMU_NOTIFY_PROTECTION_VMA, MMU_NOTIFY_PROTECTION_PAGE,
	MMU_NOTIFY_SOFT_DIRTY, MMU_NOTIFY_RELEASE, MMU_NOTIFY_MIGRATE, MMU_NOTIFY_EXCLUSIVE,
};
#define MMU_NOTIFIER_RANGE_BLOCKABLE (1 << 0)
struct mmu_notifier_range {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	unsigned flags;
	enum mmu_notifier_event event;
	void *owner;
};
static inline bool mmu_notifier_range_blockable(const struct mmu_notifier_range *range)
{
	return (range->flags & MMU_NOTIFIER_RANGE_BLOCKABLE);
}
struct mmu_interval_notifier;
struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *interval_sub, const struct mmu_notifier_range *range,
			   unsigned long cur_seq);
};
struct mmu_interval_notifier {
	struct interval_tree_node interval_tree;
	const struct mmu_interval_notifier_ops *ops;
	struct mm_struct *mm;
	struct hlist_node deferred_item;
	unsigned long invalidate_seq;
};
int mmu_interval_notifier_insert(struct mmu_interval_notifier *interval_sub, struct mm_struct *mm,
				 unsigned long start, unsigned long length,
				 const struct mmu_interval_notifier_ops *ops);
void mmu_interval_notifier_remove(struct mmu_interval_notifier *interval_sub);
static inline void mmu_interval_set_seq(struct mmu_interval_notifier *interval_sub, unsigned long cur_seq)
{
	WRITE_ONCE(interval_sub->invalidate_seq, cur_seq);
}
static inline unsigned long mmu_interval_read_begin(struct mmu_interval_notifier *interval_sub)
{
	return READ_ONCE(interval_sub->invalidate_seq);
}
static inline bool mmu_interval_read_retry(struct mmu_interval_notifier *interval_sub, unsigned long seq)
{
	return READ_ONCE(interval_sub->invalidate_seq) != seq;
}
static inline void mmu_notifier_synchronize(void) { }
struct mmu_notifier_ops;
struct mmu_notifier {
	struct hlist_node hlist;
	const struct mmu_notifier_ops *ops;
	struct mm_struct *mm;
	struct rcu_head rcu;
	unsigned int users;
};

enum memory_type {
	MEMORY_DEVICE_PRIVATE = 1, MEMORY_DEVICE_COHERENT, MEMORY_DEVICE_FS_DAX, MEMORY_DEVICE_GENERIC,
	MEMORY_DEVICE_PCI_P2PDMA,
};
struct dev_pagemap;
struct dev_pagemap_ops {
	void (*page_free)(struct page *page);
	vm_fault_t (*migrate_to_ram)(struct vm_fault *vmf);
	int (*memory_failure)(struct dev_pagemap *pgmap, unsigned long pfn, unsigned long nr_pages, int mf_flags);
};
struct dev_pagemap {
	enum memory_type type;
	unsigned int flags;
	const struct dev_pagemap_ops *ops;
	void *owner;
	int nr_range;
	struct range range;
};

#ifndef __pfn_to_phys
#define __pfn_to_phys(pfn) ((phys_addr_t)(pfn) << PAGE_SHIFT)
#endif

void orderly_poweroff(bool force);

struct clk;
struct regulator;
static inline unsigned long clk_get_rate(struct clk *clk) { return 0; }
static inline int regulator_get_voltage(struct regulator *regulator) { return -ENODEV; }
static inline int regulator_set_voltage(struct regulator *regulator, int min_uV, int max_uV) { return -ENODEV; }

#define pci_p2pdma_distance(provider, client, verbose) (-1)

#define kernel_fpu_available() (!in_interrupt())

bool page_is_ram(unsigned long pfn);
#define num_possible_nodes() 1
#define num_online_nodes() 1
#define pfn_to_nid(pfn) 0
unsigned int topology_num_cores_per_package(void);

struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	long tm_year;
	int tm_wday;
	int tm_yday;
};
void time64_to_tm(time64_t totalsecs, int offset, struct tm *result);

#ifndef I2C_CLASS_HWMON
#define I2C_CLASS_HWMON (1 << 0)
#endif

typedef unsigned long irq_hw_number_t;
struct irq_domain;
struct irq_domain_ops {
	int (*match)(struct irq_domain *d, struct device_node *node, int bus_token);
	int (*map)(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw);
	void (*unmap)(struct irq_domain *d, unsigned int virq);
	int (*xlate)(struct irq_domain *d, struct device_node *node, const u32 *intspec, unsigned int intsize,
		     unsigned long *out_hwirq, unsigned int *out_type);
};
struct irq_domain {
	const struct irq_domain_ops *ops;
	void *host_data;
	unsigned int size;
};
struct irq_domain *irq_domain_create_linear(struct fwnode_handle *fwnode, unsigned int size,
					    const struct irq_domain_ops *ops, void *host_data);
void irq_domain_remove(struct irq_domain *domain);
unsigned int irq_create_mapping(struct irq_domain *domain, irq_hw_number_t hwirq);
int generic_handle_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq);
#define irq_set_chip_and_handler(irq, chip, handle) do { } while (0)

#endif
