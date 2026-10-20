// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <linux/maple_tree.h>
#include <linux/radix-tree.h>

unsigned long __per_cpu_offset[NR_CPUS];
struct cpumask lkpi_cpu_online_mask;
struct cpuinfo_x86 boot_cpu_data;

extern char __start_lkpi_percpu[], __stop_lkpi_percpu[];
extern const struct lkpi_initcall __start_lkpi_initcall[], __stop_lkpi_initcall[];

bool lkpi_boot_cpu_has(int bit)
{
	if (bit < 0 || bit >= (int)(ARRAY_SIZE(boot_cpu_data.x86_capability) * 32)) return false;
	return (boot_cpu_data.x86_capability[bit / 32] >> (bit % 32)) & 1;
}

static void cpu_data_init(void)
{
	unsigned int r[4];
	struct cpuinfo_x86 *c = &boot_cpu_data;

	luos_cpuid(0, 0, r);
	unsigned int max_leaf = r[0];
	memcpy(c->x86_vendor_id + 0, &r[1], 4);
	memcpy(c->x86_vendor_id + 4, &r[3], 4);
	memcpy(c->x86_vendor_id + 8, &r[2], 4);
	if (!strcmp(c->x86_vendor_id, "AuthenticAMD")) c->x86_vendor = X86_VENDOR_AMD;
	else if (!strcmp(c->x86_vendor_id, "HygonGenuine")) c->x86_vendor = X86_VENDOR_HYGON;
	else c->x86_vendor = X86_VENDOR_INTEL;

	luos_cpuid(1, 0, r);
	unsigned int family = (r[0] >> 8) & 0xf, model = (r[0] >> 4) & 0xf;
	if (family == 0xf) family += (r[0] >> 20) & 0xff;
	if (family >= 6) model += ((r[0] >> 16) & 0xf) << 4;
	c->x86 = family;
	c->x86_model = model;
	c->x86_stepping = r[0] & 0xf;
	c->x86_clflush_size = ((r[1] >> 8) & 0xff) * 8;
	if (!c->x86_clflush_size) c->x86_clflush_size = 64;
	c->x86_cache_alignment = c->x86_clflush_size;
	c->x86_capability[0] = r[3];
	c->x86_capability[4] = r[2];
	if (max_leaf >= 7) {
		luos_cpuid(7, 0, r);
		c->x86_capability[9] = r[1];
		c->x86_capability[16] = r[2];
		c->x86_capability[18] = r[3];
	}
	luos_cpuid(0x80000000, 0, r);
	unsigned int max_ext = r[0];
	if (max_ext >= 0x80000001) {
		luos_cpuid(0x80000001, 0, r);
		c->x86_capability[1] = r[3];
		c->x86_capability[6] = r[2];
	}
	if (max_ext >= 0x80000004) {
		for (unsigned int i = 0; i < 3; i++) {
			luos_cpuid(0x80000002 + i, 0, r);
			memcpy(c->x86_model_id + i * 16, r, 16);
		}
		c->x86_model_id[48] = 0;
	}
}

static void percpu_init(void)
{
	size_t size = __stop_lkpi_percpu - __start_lkpi_percpu;
	int n = luos_cpu_count();
	if (n > NR_CPUS) n = NR_CPUS;
	cpumask_clear(&lkpi_cpu_online_mask);
	for (int cpu = 0; cpu < n; cpu++) {
		cpumask_set_cpu(cpu, &lkpi_cpu_online_mask);
		if (cpu == 0 || !size) continue;
		char *area = luos_heap_alloc(size, 64);
		if (!area) luos_panic("LinuxKPI: out of memory for per-CPU data");
		memcpy(area, __start_lkpi_percpu, size);
		__per_cpu_offset[cpu] = (unsigned long)area - (unsigned long)__start_lkpi_percpu;
	}
}

static char lkpi_stage[96] = "starting";
static bool lkpi_started;

void lkpi_set_stage(const char *fmt, ...)
{
	va_list ap;
	if (!fmt) {
		WRITE_ONCE(lkpi_started, true);
		return;
	}
	va_start(ap, fmt);
	vscnprintf(lkpi_stage, sizeof(lkpi_stage), fmt, ap);
	va_end(ap);
}

bool lkpi_startup_stage(char *buf, size_t len)
{
	if (READ_ONCE(lkpi_started)) return false;
	strscpy(buf, lkpi_stage, len);
	return true;
}

static void run_initcalls(void)
{
	size_t n = __stop_lkpi_initcall - __start_lkpi_initcall;
	int ok = 0, failed = 0;
	for (int level = 0; level <= 7; level++) {
		for (size_t i = 0; i < n; i++) {
			const struct lkpi_initcall *c = &__start_lkpi_initcall[i];
			if (c->level != level) continue;
			lkpi_set_stage("initcall %s", c->name);
			int ret = c->fn();
			if (ret) {
				pr_warn("LinuxKPI: initcall %s returned %d\n", c->name, ret);
				failed++;
			} else {
				ok++;
			}
		}
	}
	pr_info("LinuxKPI: %d initcalls done, %d failed\n", ok, failed);
	lkpi_set_stage("initcalls done");
}

void lkpi_set_drm_debug(unsigned long mask)
{
	extern unsigned long __drm_debug;
	__drm_debug = mask;
}

void lkpi_init(void)
{
	lkpi_mm_init();
	cpu_data_init();
	percpu_init();
	pr_info("LinuxKPI: Linux 6.18 driver environment, %d CPUs, %s\n", luos_cpu_count(),
		boot_cpu_data.x86_model_id[0] ? strim(boot_cpu_data.x86_model_id) : boot_cpu_data.x86_vendor_id);
	radix_tree_init();
	maple_tree_init();
	lkpi_sched_init();
	lkpi_sync_init();
	lkpi_device_init();
	lkpi_fs_init();
	lkpi_pci_init();
	tsc_khz = luos_tsc_khz();
	lkpi_intel_stolen_init();
	lkpi_params_apply(luos_cmdline());
	run_initcalls();
	lkpi_console_setup_unclaimed();
	lkpi_sysfb_init();
	lkpi_set_stage(NULL);
}
