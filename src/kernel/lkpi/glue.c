// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "components/ACPI/power.h"
#include "components/ACPI/tables.h"
#include "components/firmware.h"
#include "components/logger.h"
#include "components/pci.h"
#include "drivers/Video/limine_video_driver.h"
#include "components/Interruptions/ioapic.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "drivers/Timer/apic_driver.h"
#include "drivers/Timer/tsc_driver.h"
#include "kernel/proc/process.h"
#include "kernel/proc/syscall.h"
#include "kernel/proc/vfs.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/smp/tlb.h"

#include <cpuid.h>
#include <msr.h>
#include <string.h>

typedef uint64_t luos_u64;


void luos_log(int level, const char *msg)
{
    switch (level) {
        case 0: LOG_DEBUG("%s", msg); break;
        case 1: LOG_INFO("%s", msg); break;
        case 2: LOG_WARNING("%s", msg); break;
        default: LOG_ERROR("%s", msg); break;
    }
}

void luos_panic(const char *msg)
{
    LOG_ERROR("Linux code panicked: %s", msg);
    for (;;) asm volatile("cli; hlt");
}


void *luos_heap_alloc(unsigned long size, unsigned long align)
{
    if (!size) size = 1;
    return align > 16 ? kmalloc_aligned(size, align) : kmalloc(size);
}

void luos_heap_free(void *p) { kfree(p); }
unsigned long luos_heap_block_size(const void *p) { return heap_block_size(p); }

luos_u64 luos_phys_alloc(luos_u64 pages, luos_u64 align_pages, luos_u64 limit)
{
    return pmm_alloc_pages_aligned(pages, align_pages ? align_pages : 1, limit);
}

void luos_phys_free(luos_u64 phys, luos_u64 pages) { pmm_free_pages(phys, pages); }
void luos_pmm_reserve_range(luos_u64 base, luos_u64 length) { pmm_reserve_range(base, length); }
luos_u64 luos_hhdm_offset(void) { return mm_phys_to_virt(0); }
luos_u64 luos_virt_to_phys(const void *virt) { return mm_virt_to_phys((uint64_t)virt); }
luos_u64 luos_highest_pfn(void) { return pmm_highest_frame(); }
int luos_pfn_is_ram(luos_u64 pfn) { return pmm_frame_is_ram(pfn); }
luos_u64 luos_total_pages(void) { return pmm_total_pages(); }
luos_u64 luos_free_pages(void) { return pmm_free_page_count(); }

luos_u64 luos_vmap(const luos_u64 *phys, luos_u64 count, int cache)
{
    return vmm_vmap(phys, count, VMM_FLAG_WRITE | VMM_FLAG_GLOBAL | VMM_FLAG_NX, (enum vmm_cache_type)cache);
}

void luos_vunmap(luos_u64 virt) { vmm_vunmap(virt); }
luos_u64 luos_ioremap(luos_u64 phys, luos_u64 size, int cache) { return vmm_ioremap(phys, size, (enum vmm_cache_type)cache); }
void luos_iounmap(luos_u64 virt) { vmm_iounmap(virt); }
int luos_set_cache(luos_u64 virt, luos_u64 size, int cache) { return vmm_set_cache_type(virt, size, (enum vmm_cache_type)cache); }
void luos_flush_cache(luos_u64 virt, luos_u64 size) { vmm_flush_cache_range(virt, size); }


void *luos_current_task(void) { return current_task(); }

void **luos_task_lx_slot(void *task)
{
    return task ? &((struct task *)task)->lx_task : NULL;
}

int luos_task_tid(void *task) { return task ? (int)((struct task *)task)->tid : 0; }
const char *luos_task_name(void *task) { return task ? ((struct task *)task)->name : "?"; }
int luos_task_is_user(void *task) { return task && ((struct task *)task)->proc != NULL; }

int luos_task_kill_pending(void *task)
{
    struct task *t = task;
    if (!t) return 0;
    if (t->kill_pending) return 1;
    struct process *p = t->proc;
    return p && p->exiting;
}

void *luos_task_spawn(const char *name, void (*fn)(void *), void *arg)
{
    return task_create(name, fn, arg, PRIO_DEFAULT);
}

void luos_task_exit(void) { task_exit(); }

int luos_wait(void *channel, volatile int *flag, luos_u64 timeout_ms)
{
    return scheduler_wait(channel, flag, timeout_ms);
}

void luos_wake(void *channel) { scheduler_wake(channel); }
void luos_wake_all(void *channel) { scheduler_wake_all(channel); }
void luos_yield(void) { scheduler_yield(); }
void luos_sleep_ms(luos_u64 ms) { scheduler_sleep_ms(ms); }

static int luos_early_preempt;

int *luos_preempt_counter(void)
{
    struct task *t = current_task();
    return t ? &t->preempt_count : &luos_early_preempt;
}

void luos_preempt_resume(void) { scheduler_preempt_resume(); }
int luos_in_interrupt(void) { return isr_in_interrupt(); }
int luos_cpu_id(void) { return current_core(); }
int luos_cpu_count(void) { return scheduler_core_count(); }

#define CPU_WORK_QUEUE 64

struct cpu_work {
    void (*fn)(void *);
    void *arg;
};

struct cpu_worker {
    spinlock_t lock;
    struct cpu_work queue[CPU_WORK_QUEUE];
    unsigned int head, tail;
    volatile int pending;
    volatile int started;
};

static struct cpu_worker cpu_workers[MAX_CORES];

static void cpu_worker_main(void *arg)
{
    struct cpu_worker *w = arg;
    for (;;) {
        while (!__atomic_load_n(&w->pending, __ATOMIC_ACQUIRE)) scheduler_wait(w, &w->pending, 0);
        __atomic_store_n(&w->pending, 0, __ATOMIC_RELEASE);
        for (;;) {
            uint64_t f = spin_lock_irqsave(&w->lock);
            if (w->head == w->tail) {
                spin_unlock_irqrestore(&w->lock, f);
                break;
            }
            struct cpu_work work = w->queue[w->head % CPU_WORK_QUEUE];
            w->head++;
            spin_unlock_irqrestore(&w->lock, f);
            work.fn(work.arg);
        }
    }
}

void luos_run_on_cpu(int cpu, void (*fn)(void *), void *arg)
{
    if (cpu < 0 || cpu >= MAX_CORES) return;
    struct cpu_worker *w = &cpu_workers[cpu];
    if (!__atomic_exchange_n(&w->started, 1, __ATOMIC_ACQ_REL)) {
        if (!task_create_on_core("lkpi-cpu", cpu_worker_main, w, PRIO_DEFAULT, cpu)) {
            __atomic_store_n(&w->started, 0, __ATOMIC_RELEASE);
            task_create_on_core("lkpi-cpu", fn, arg, PRIO_DEFAULT, cpu);
            return;
        }
    }
    uint64_t f = spin_lock_irqsave(&w->lock);
    int full = w->tail - w->head >= CPU_WORK_QUEUE;
    if (!full) {
        w->queue[w->tail % CPU_WORK_QUEUE] = (struct cpu_work){ fn, arg };
        w->tail++;
    }
    spin_unlock_irqrestore(&w->lock, f);
    if (full) {
        task_create_on_core("lkpi-cpu", fn, arg, PRIO_DEFAULT, cpu);
        return;
    }
    __atomic_store_n(&w->pending, 1, __ATOMIC_RELEASE);
    scheduler_wake(w);
}


luos_u64 luos_monotonic_ns(void) { return sys_monotonic_ns(); }
luos_u64 luos_realtime_ns(void) { return sys_realtime_ns(); }

void luos_udelay(luos_u64 us)
{
    luos_u64 end = sys_monotonic_ns() + us * 1000ULL;
    while (sys_monotonic_ns() < end) asm volatile("pause");
}


int luos_copy_to_user(luos_u64 dst, const void *src, unsigned long n) { return copy_to_user(dst, src, n); }
int luos_copy_from_user(void *dst, luos_u64 src, unsigned long n) { return copy_from_user(dst, src, n); }
int luos_clear_user(luos_u64 dst, unsigned long n) { return clear_user(dst, n); }
void *luos_current_process(void) { return proc_current(); }

int luos_user_map_pfn(void *proc, luos_u64 addr, luos_u64 pfn, int writable, int cache)
{
    struct process *p = proc;
    if (!p || addr >= USER_LIMIT) return -1;
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX;
    if (writable) flags |= VMM_FLAG_WRITE;
    flags |= pat_pte_bits((enum vmm_cache_type)cache, false);
    return vmm_map_page(p->mm.pml4, addr & ~0xFFFULL, pfn << 12, flags);
}

int luos_user_is_mapped(void *proc, luos_u64 addr)
{
    struct process *p = proc;
    if (!p || addr >= USER_LIMIT) return 0;
    return vmm_is_mapped(p->mm.pml4, addr & ~0xFFFULL);
}

static struct mm_device_hooks luos_device_hooks;

void luos_mm_set_device_hooks(int (*fault)(void *ctx, void *proc, luos_u64 addr, int write),
                              void *(*split)(void *ctx, luos_u64 old_start, luos_u64 new_start, luos_u64 new_end),
                              void (*close)(void *ctx))
{
    luos_device_hooks.fault = fault;
    luos_device_hooks.split = split;
    luos_device_hooks.close = close;
    mm_device_hooks = &luos_device_hooks;
}

void luos_user_unmap(void *proc, luos_u64 addr, luos_u64 len)
{
    struct process *p = proc;
    if (!p) return;
    uint64_t phys[64];
    uint64_t va = addr & ~0xFFFULL;
    uint64_t end = addr + len;
    while (va < end) {
        uint64_t next = end;
        vmm_unmap_collect(p->mm.pml4, va, end, phys, 64, &next);
        tlb_shootdown(va, next - va);
        va = next;
    }
}


int luos_fd_reserve(int cloexec)
{
    struct process *p = proc_current();
    if (!p) return -LX_EBADF;
    return fd_install(p, &vfs_reserved_fd, 0, cloexec != 0);
}

void luos_fd_unreserve(int fd)
{
    struct process *p = proc_current();
    if (p && fd >= 0 && fd < PROC_MAX_FDS && p->fds[fd] == &vfs_reserved_fd) {
        uint64_t fl = spin_lock_irqsave(&p->fd_lock);
        p->fds[fd] = NULL;
        p->fd_cloexec[fd] = 0;
        spin_unlock_irqrestore(&p->fd_lock, fl);
    }
}

void luos_fd_install_lx(int fd, void *lx_file)
{
    struct process *p = proc_current();
    struct ufile *f = vfs_wrap_lx_file(lx_file, LX_O_RDWR);
    if (!p || !f || fd < 0 || fd >= PROC_MAX_FDS) {
        if (f) ufile_put(f);
        return;
    }
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct ufile *old = p->fds[fd];
    p->fds[fd] = f;
    spin_unlock_irqrestore(&p->fd_lock, fl);
    if (old && old != &vfs_reserved_fd) ufile_put(old);
}

void *luos_fd_get_lx(int fd)
{
    struct process *p = proc_current();
    if (!p) return NULL;
    struct ufile *f = fd_get(p, fd);
    if (!f) return NULL;
    void *lx = vfs_ufile_lx(f);
    ufile_put(f);
    return lx;
}

int luos_devnode_register(const char *path, unsigned int major, unsigned int minor)
{
    return vfs_devnode_register(path, major, minor);
}

void luos_devnode_unregister(const char *path) { vfs_devnode_unregister(path); }


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

int luos_pci_count(void) { return pci_get_device_count(); }

int luos_pci_info(int index, struct luos_pci_info *o)
{
    struct pci_device *d = pci_get_device(index);
    if (!d) return -1;
    memset(o, 0, sizeof(*o));
    o->segment = d->segment;
    o->bus = d->bus;
    o->slot = d->slot;
    o->func = d->func;
    o->vendor = d->vendor_id;
    o->device = d->device_id;
    o->subsys_vendor = d->subsys_vendor;
    o->subsys_device = d->subsys_id;
    o->class_code = ((unsigned)d->class_code << 16) | ((unsigned)d->subclass << 8) | d->prog_if;
    o->revision = d->revision;
    o->header_type = d->header_type & 0x7F;
    o->irq_line = d->irq_line;
    o->irq_pin = d->irq_pin;
    o->msi_cap = d->msi_cap;
    o->msix_cap = d->msix_cap;
    o->pcie_cap = d->pcie_cap;
    o->pm_cap = d->pm_cap;
    o->cfg_size = d->cfg_size;
    o->parent = d->parent;
    for (int b = 0; b < 6; b++) {
        o->bar_base[b] = d->bars[b].base;
        o->bar_size[b] = d->bars[b].size;
        o->bar_flags[b] = d->bars[b].flags;
    }
    o->rom_base = d->rom_base;
    o->rom_size = d->rom_size;
    o->handle = d;
    return 0;
}

uint64_t luos_pci_bar_info(void *handle, int bar, unsigned int *flags)
{
    struct pci_device *d = handle;
    if (!d || bar < 0 || bar >= 6) return 0;
    if (!(d->bars[bar].flags & PCI_BAR_PRESENT)) return 0;
    if (flags) *flags = d->bars[bar].flags;
    return d->bars[bar].size;
}

unsigned int luos_pci_cfg_read(void *handle, int offset, int width)
{
    struct pci_device *d = handle;
    return pci_cfg_read(d->segment, d->bus, d->slot, d->func, (uint16_t)offset, width);
}

void luos_pci_cfg_write(void *handle, int offset, int width, unsigned int value)
{
    struct pci_device *d = handle;
    pci_cfg_write(d->segment, d->bus, d->slot, d->func, (uint16_t)offset, width, value);
}

int luos_pci_msi_enable(void *handle, int vector)
{
    return pci_msi_enable_dev(handle, (uint8_t)vector, apic_get_lapic_id());
}

int luos_pci_msix_enable(void *handle, const int *vectors, int count)
{
    struct pci_device *d = handle;
    if (pci_msix_map(d) != 0) return -1;
    for (int i = 0; i < count; i++) {
        pci_msix_set_entry(d, i, (uint8_t)vectors[i], apic_get_lapic_id());
        pci_msix_mask_entry(d, i, false);
    }
    return pci_msix_enable_dev(d);
}

void luos_pci_msi_disable(void *handle)
{
    struct pci_device *d = handle;
    if (d->msix_enabled) pci_msix_disable_dev(d);
    if (d->msi_enabled) pci_msi_disable_dev(d);
}

int luos_pci_msi_count(void *handle) { return pci_msi_vector_count(handle); }
int luos_pci_msix_count(void *handle) { return pci_msix_table_size(handle); }

extern volatile struct limine_framebuffer_request framebuffer_request;

int luos_pci_is_boot_vga(void *handle)
{
    struct pci_device *d = handle;
    if (d->class_code != PCI_CLASS_DISPLAY) return 0;
    uint64_t fb_phys = 0;
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count)
        fb_phys = mm_virt_to_phys((uint64_t)framebuffer_request.response->framebuffers[0]->address);
    for (int b = 0; b < 6 && fb_phys; b++) {
        if (!(d->bars[b].flags & PCI_BAR_PRESENT) || (d->bars[b].flags & PCI_BAR_IO)) continue;
        if (fb_phys >= d->bars[b].base && fb_phys < d->bars[b].base + d->bars[b].size) return 1;
    }
    return 0;
}

struct luos_boot_fb {
    luos_u64 phys;
    unsigned int width, height, pitch, bpp;
    unsigned char red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};

int luos_boot_framebuffer(struct luos_boot_fb *o)
{
    if (!framebuffer_request.response || !framebuffer_request.response->framebuffer_count) return -1;
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    if (!fb->address || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) return -1;
    o->phys = mm_virt_to_phys((uint64_t)fb->address);
    o->width = (unsigned int)fb->width;
    o->height = (unsigned int)fb->height;
    o->pitch = (unsigned int)fb->pitch;
    o->bpp = fb->bpp;
    o->red_size = fb->red_mask_size;
    o->red_shift = fb->red_mask_shift;
    o->green_size = fb->green_mask_size;
    o->green_shift = fb->green_mask_shift;
    o->blue_size = fb->blue_mask_size;
    o->blue_shift = fb->blue_mask_shift;
    return 0;
}


int luos_irq_alloc(void) { return irq_alloc_vector(); }
void luos_irq_free(int vector) { irq_free_vector((uint8_t)vector); }

struct luos_irq_slot {
    void (*fn)(void *ctx);
    void *ctx;
};
static struct luos_irq_slot luos_irq_slots[256];

static void luos_irq_trampoline(void *ctx, struct registers *regs)
{
    (void)regs;
    struct luos_irq_slot *s = ctx;
    if (s->fn) s->fn(s->ctx);
}

int luos_irq_attach(int vector, void (*fn)(void *ctx), void *ctx)
{
    if (vector < 0 || vector > 255) return -1;
    luos_irq_slots[vector].fn = fn;
    luos_irq_slots[vector].ctx = ctx;
    return irq_register_ctx_handler((uint8_t)vector, luos_irq_trampoline, &luos_irq_slots[vector]);
}

void luos_irq_detach(int vector)
{
    if (vector < 0 || vector > 255) return;
    irq_unregister_ctx_handler((uint8_t)vector);
    luos_irq_slots[vector].fn = NULL;
}

static int luos_intx_vec[24];
static uint8_t luos_intx_line[256];
static uint8_t luos_intx_owned[256];
static spinlock_t luos_intx_lock = SPINLOCK_INIT;

int luos_pci_intx_vector(void *handle)
{
    struct pci_device *d = handle;
    if (!d || !d->irq_pin) return -1;
    unsigned int line = d->irq_line;
    if (line == 0 || line == 1 || line == 2 || line == 4 || line == 8 || line == 12 || line >= 24) return -1;
    uint64_t flags = spin_lock_irqsave(&luos_intx_lock);
    int v = luos_intx_vec[line];
    if (!v) {
        int cur = ioapic_pin_vector((uint8_t)line);
        bool owned = cur < 32;
        v = owned ? irq_alloc_vector() : cur;
        if (v >= 32) {
            luos_intx_vec[line] = v;
            luos_intx_line[v] = (uint8_t)(line + 1);
            luos_intx_owned[v] = owned;
        }
    }
    spin_unlock_irqrestore(&luos_intx_lock, flags);
    if (v >= 32) LOG_DEBUG("lkpi: %02x:%02x.%x INT%c# on IRQ line %u, vector 0x%02x", d->bus, d->slot, d->func,
                           'A' + d->irq_pin - 1, line, (unsigned)v);
    return v >= 32 ? v : -1;
}

int luos_intx_route(int vector, int enable)
{
    if (vector < 32 || vector > 255 || !luos_intx_line[vector]) return -1;
    uint8_t line = (uint8_t)(luos_intx_line[vector] - 1);
    if (enable) {
        if (ioapic_pin_vector(line) != vector) ioapic_map_pci_irq(line, (uint8_t)vector, apic_get_lapic_id());
    } else if (luos_intx_owned[vector] && ioapic_pin_vector(line) == vector) {
        ioapic_mask_irq(line);
    }
    return 0;
}

int luos_irq_route_gsi(int gsi, int vector)
{
    (void)gsi;
    (void)vector;
    return -1;
}


int luos_firmware_get(const char *name, const void **data, unsigned long *size, void **handle)
{
    const struct firmware_blob *fw = NULL;
    int r = firmware_request(name, &fw);
    if (r != FIRMWARE_OK) return r == FIRMWARE_ERR_NOMEM ? -12 : -2;
    *data = fw->data;
    *size = fw->size;
    *handle = (void *)fw;
    return 0;
}

void luos_firmware_put(void *handle) { firmware_release(handle); }


int luos_cpu_has(int bit)
{
    int word = bit / 32;
    int b = bit % 32;
    if (word == 0) return (cpuid(1, 0).edx >> b) & 1;
    if (word == 4) return (cpuid(1, 0).ecx >> b) & 1;
    if (word == 9) return (cpuid(7, 0).ebx >> b) & 1;
    return 0;
}

void luos_cpuid(unsigned int leaf, unsigned int subleaf, unsigned int regs[4])
{
    struct cpuid_result r = cpuid(leaf, subleaf);
    regs[0] = r.eax;
    regs[1] = r.ebx;
    regs[2] = r.ecx;
    regs[3] = r.edx;
}

void luos_wbinvd_all(void)
{
    wbinvd();
}

void luos_reboot(void)
{
    acpi_reboot();
}

void luos_poweroff(void)
{
    acpi_shutdown();
}

const void *luos_acpi_find_table(const char *signature, int index, unsigned int *len)
{
    const ACPI_SDT_HEADER *h = acpi_find_table(signature, index);
    if (!h) return NULL;
    if (len) *len = h->Length;
    return h;
}


int luos_console_attach(void *pixels, unsigned int width, unsigned int height, unsigned int pitch)
{
    return limine_video_attach(pixels, width, height, pitch) ? 0 : -1;
}

void luos_console_detach(void) { limine_video_detach(); }

int luos_console_take_damage(unsigned int rect[4]) { return limine_video_take_damage(rect) ? 1 : 0; }

unsigned int luos_tsc_khz(void)
{
    struct tsc_driver *t = return_tsc_driver();
    return t && t->get_tsc_ticks_per_ms ? (unsigned int)t->get_tsc_ticks_per_ms() : 0;
}

const char *limine_kernel_cmdline(void);

const char *luos_cmdline(void) { return limine_kernel_cmdline(); }


void lkpi_init(void);

static void lkpi_task(void *arg)
{
    (void)arg;
    lkpi_init();
}

void lkpi_boot(void)
{
    task_create("linux", lkpi_task, NULL, PRIO_DEFAULT);
}
