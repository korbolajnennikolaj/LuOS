// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_LUOS_GLUE_H
#define _LKPI_LUOS_GLUE_H

typedef unsigned long luos_u64;

void luos_log(int level, const char *msg);
void luos_panic(const char *msg) __attribute__((noreturn));

void *luos_heap_alloc(unsigned long size, unsigned long align);
void luos_heap_free(void *p);
unsigned long luos_heap_block_size(const void *p);

luos_u64 luos_phys_alloc(luos_u64 pages, luos_u64 align_pages, luos_u64 limit);
void luos_phys_free(luos_u64 phys, luos_u64 pages);
void luos_pmm_reserve_range(luos_u64 base, luos_u64 length);
luos_u64 luos_hhdm_offset(void);
luos_u64 luos_virt_to_phys(const void *virt);
luos_u64 luos_highest_pfn(void);
int luos_pfn_is_ram(luos_u64 pfn);
luos_u64 luos_total_pages(void);
luos_u64 luos_free_pages(void);
luos_u64 luos_vmap(const luos_u64 *phys, luos_u64 count, int cache);
void luos_vunmap(luos_u64 virt);
luos_u64 luos_ioremap(luos_u64 phys, luos_u64 size, int cache);
void luos_iounmap(luos_u64 virt);
int luos_set_cache(luos_u64 virt, luos_u64 size, int cache);
void luos_flush_cache(luos_u64 virt, luos_u64 size);

void *luos_current_task(void);
void **luos_task_lx_slot(void *task);
int luos_task_tid(void *task);
const char *luos_task_name(void *task);
int luos_task_is_user(void *task);
int luos_task_kill_pending(void *task);
void *luos_task_spawn(const char *name, void (*fn)(void *), void *arg);
void luos_task_exit(void) __attribute__((noreturn));
int luos_wait(void *channel, volatile int *flag, luos_u64 timeout_ms);
void luos_wake(void *channel);
void luos_wake_all(void *channel);
void luos_yield(void);
void luos_sleep_ms(luos_u64 ms);
int *luos_preempt_counter(void);
void luos_preempt_resume(void);
int luos_in_interrupt(void);
int luos_cpu_id(void);
int luos_cpu_count(void);
void luos_run_on_cpu(int cpu, void (*fn)(void *), void *arg);

luos_u64 luos_monotonic_ns(void);
luos_u64 luos_realtime_ns(void);
void luos_udelay(luos_u64 us);

int luos_copy_to_user(luos_u64 dst, const void *src, unsigned long n);
int luos_copy_from_user(void *dst, luos_u64 src, unsigned long n);
int luos_clear_user(luos_u64 dst, unsigned long n);
void *luos_current_process(void);
int luos_user_map_pfn(void *proc, luos_u64 addr, luos_u64 pfn, int writable, int cache);
void luos_user_unmap(void *proc, luos_u64 addr, luos_u64 len);
int luos_user_is_mapped(void *proc, luos_u64 addr);
void luos_mm_set_device_hooks(int (*fault)(void *ctx, void *proc, luos_u64 addr, int write),
                              void *(*split)(void *ctx, luos_u64 old_start, luos_u64 new_start, luos_u64 new_end),
                              void (*close)(void *ctx));

int luos_fd_reserve(int cloexec);
void luos_fd_unreserve(int fd);
void luos_fd_install_lx(int fd, void *lx_file);
void *luos_fd_get_lx(int fd);
void luos_lx_file_release(void *lx_file);
int luos_devnode_register(const char *path, unsigned int major, unsigned int minor);
void luos_devnode_unregister(const char *path);

struct luos_pci_info {
        unsigned short segment;
        unsigned char bus, slot, func;
        unsigned short vendor, device, subsys_vendor, subsys_device;
        unsigned int class_code;
        unsigned char revision, header_type, irq_line, irq_pin;
        unsigned char msi_cap, msix_cap, pcie_cap, pm_cap;
        unsigned short cfg_size;
        int parent;
        luos_u64 bar_base[6];
        luos_u64 bar_size[6];
        unsigned int bar_flags[6];
        luos_u64 rom_base;
        unsigned int rom_size;
        void *handle;
};
#define LUOS_BAR_PRESENT 1u
#define LUOS_BAR_IO 2u
#define LUOS_BAR_MEM64 4u
#define LUOS_BAR_PREFETCH 8u
#define LUOS_BAR_UPPER 16u
int luos_pci_count(void);
int luos_pci_info(int index, struct luos_pci_info *out);
luos_u64 luos_pci_bar_info(void *handle, int bar, unsigned int *flags);
unsigned int luos_pci_cfg_read(void *handle, int offset, int width);
void luos_pci_cfg_write(void *handle, int offset, int width, unsigned int value);
int luos_pci_msi_enable(void *handle, int vector);
int luos_pci_msix_enable(void *handle, const int *vectors, int count);
void luos_pci_msi_disable(void *handle);
int luos_pci_msi_count(void *handle);
int luos_pci_msix_count(void *handle);
int luos_pci_is_boot_vga(void *handle);

int luos_irq_alloc(void);
void luos_irq_free(int vector);
int luos_irq_attach(int vector, void (*fn)(void *ctx), void *ctx);
void luos_irq_detach(int vector);
int luos_irq_route_gsi(int gsi, int vector);

int luos_firmware_get(const char *name, const void **data, unsigned long *size, void **handle);
void luos_firmware_put(void *handle);

struct luos_boot_fb {
        luos_u64 phys;
        unsigned int width, height, pitch, bpp;
        unsigned char red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};
int luos_boot_framebuffer(struct luos_boot_fb *out);

int luos_console_attach(void *pixels, unsigned int width, unsigned int height, unsigned int pitch);
void luos_console_detach(void);
int luos_console_take_damage(unsigned int rect[4]);

unsigned int luos_tsc_khz(void);
const char *luos_cmdline(void);
int luos_cpu_has(int leaf_reg_bit);
void luos_cpuid(unsigned int leaf, unsigned int subleaf, unsigned int regs[4]);
void luos_wbinvd_all(void);
void luos_reboot(void);
void luos_poweroff(void);
const void *luos_acpi_find_table(const char *signature, int index, unsigned int *len);

#endif
