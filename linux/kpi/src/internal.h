// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_INTERNAL_H
#define _LKPI_INTERNAL_H

#include <lkpi/all.h>
#include "luos_glue.h"

struct lkpi_waiter {
        struct list_head node;
        struct task_struct *task;
        int granted;
        int type;
};

void lkpi_sched_init(void);
void lkpi_sync_init(void);
void lkpi_mm_init(void);
void lkpi_fs_init(void);
void lkpi_device_init(void);
void lkpi_pci_init(void);
void lkpi_sysfb_init(void);
void lkpi_scan_flash_vbios(struct pci_dev *pdev);
void lkpi_console_setup_unclaimed(void);
__printf(1, 2) void lkpi_set_stage(const char *fmt, ...);
bool lkpi_startup_stage(char *buf, size_t len);
int lkpi_pci_probe_all(void);
int lkpi_gpu_reinit(void);
int lkpi_gpu_restart(int *unbound, int *bound);
extern bool lkpi_gpu_autoinit;

extern struct workqueue_struct *lkpi_irq_wq;
bool lkpi_addr_is_direct(const void *p);
extern unsigned long lkpi_device_generation;
void lkpi_for_each_device(void (*fn)(struct device *dev, void *arg), void *arg);
const char *lkpi_device_devname(struct device *dev);
void lkpi_file_release_deferred(struct file *file);
int lkpi_vma_fault(void *ctx, void *proc, luos_u64 addr, int write);
void *lkpi_vma_split(void *ctx, luos_u64 old_start, luos_u64 new_start, luos_u64 new_end);
void lkpi_vma_close(void *ctx);

#endif
