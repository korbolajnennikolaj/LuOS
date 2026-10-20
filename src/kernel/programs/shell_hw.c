// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "shell_hw.h"

#include "components/drivers.h"
#include "components/firmware.h"
#include "components/logger.h"
#include "components/pci.h"
#include "components/Interruptions/isr.h"
#include "components/Interruptions/msi.h"
#include "components/Memory/dma.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pat.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "drivers/Timer/apic_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/GPU/gpu_core.h"
#include "drivers/GPU/mesa.h"
#include "drivers/GPU/intel_legacy.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/limine.h"
#include "kernel/lkpi/lkpi.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/smp/tlb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct limine_framebuffer_request *get_framebuffer_request();

#define C_HEAD LIMINE_COLOR_LIGHT_BLUE
#define C_OK LIMINE_COLOR_LIGHT_GREEN
#define C_BAD LIMINE_COLOR_LIGHT_RED
#define C_TXT LIMINE_COLOR_LIGHT_GRAY
#define C_VAL LIMINE_COLOR_AMBER

static const char *hw_skip(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static int hw_hex(const char **ps, unsigned *out) {
    const char *s = *ps;
    unsigned v = 0;
    int n = 0;
    for (;; s++, n++) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned)(c - 'A' + 10);
        else break;
    }
    *ps = s;
    *out = v;
    return n;
}

static struct pci_device *hw_parse_bdf(const char *s) {
    unsigned a, b, c, d;
    s = hw_skip(s);
    if (!hw_hex(&s, &a) || *s++ != ':') return NULL;
    if (!hw_hex(&s, &b)) return NULL;
    if (*s == ':') {
        s++;
        if (!hw_hex(&s, &c) || *s++ != '.' || !hw_hex(&s, &d)) return NULL;
        return pci_find_bdf((uint16_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
    }
    if (*s++ != '.' || !hw_hex(&s, &c)) return NULL;
    return pci_find_bdf(0, (uint8_t)a, (uint8_t)b, (uint8_t)c);
}

static inline uint64_t hw_rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t hw_tsc_per_ms(void) {
    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return (tsc && tsc->get_tsc_ticks_per_ms) ? tsc->get_tsc_ticks_per_ms() : 0;
}

static const char *hw_vendor_name(uint16_t v) {
    switch (v) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x1234: return "QEMU/Bochs";
        case 0x1AF4: return "virtio";
        case 0x1B36: return "Red Hat";
        case 0x15AD: return "VMware";
        default: return "";
    }
}

static void hw_print_link(struct pci_device *d) {
    if (!d->pcie_cap) return;
    uint16_t cap_reg = pci_dev_read16(d, d->pcie_cap + 0x02);
    uint32_t lnkcap = pci_dev_read32(d, d->pcie_cap + 0x0C);
    uint16_t lnksta = pci_dev_read16(d, d->pcie_cap + 0x12);
    static const char *const types[] = { "endpoint", "legacy-ep", "?", "?", "root-port", "upstream", "downstream",
                                         "pcie-pci", "pci-pcie", "rc-ep", "rc-ec" };
    unsigned t = (cap_reg >> 4) & 0xF;
    printf_color(C_TXT, "    PCIe %s, link %s x%u (max gen%u x%u)\n", t < 11 ? types[t] : "?",
                 (lnksta & 0xF) == 1 ? "2.5GT/s" : (lnksta & 0xF) == 2 ? "5GT/s" : (lnksta & 0xF) == 3 ? "8GT/s"
                 : (lnksta & 0xF) == 4 ? "16GT/s" : (lnksta & 0xF) == 5 ? "32GT/s" : "down",
                 (unsigned)((lnksta >> 4) & 0x3F), (unsigned)(lnkcap & 0xF), (unsigned)((lnkcap >> 4) & 0x3F));
}

static void hw_print_bars(struct pci_device *d) {
    for (int b = 0; b < 6; b++) {
        struct pci_bar *bar = &d->bars[b];
        if (!(bar->flags & PCI_BAR_PRESENT)) continue;
        uint64_t sz = bar->size;
        const char *unit = "B";
        if (sz >= (1ULL << 30) && !(sz & ((1ULL << 30) - 1))) { sz >>= 30; unit = "GiB"; }
        else if (sz >= (1ULL << 20) && !(sz & ((1ULL << 20) - 1))) { sz >>= 20; unit = "MiB"; }
        else if (sz >= 1024 && !(sz & 1023)) { sz >>= 10; unit = "KiB"; }
        printf_color(C_TXT, "    BAR%d %s 0x%llx  %llu %s%s%s\n", b, (bar->flags & PCI_BAR_IO) ? "io " : "mem",
                     (unsigned long long)bar->base, (unsigned long long)sz, unit,
                     (bar->flags & PCI_BAR_MEM64) ? " 64-bit" : "", (bar->flags & PCI_BAR_PREFETCH) ? " prefetch" : "");
    }
    if (d->rom_size)
        printf_color(C_TXT, "    ROM 0x%llx  %u KiB\n", (unsigned long long)d->rom_base, (unsigned)(d->rom_size / 1024));
}

static void hw_print_ext_caps(struct pci_device *d) {
    if (d->cfg_size <= 256) return;
    printf_color(C_TXT, "    ext caps:");
    uint16_t pos = 0x100;
    for (int ttl = 0; ttl < 64; ttl++) {
        uint32_t h = pci_dev_read32(d, pos);
        if (h == 0 || h == 0xFFFFFFFFu) break;
        printf_color(C_VAL, " %s@%03x", pci_ext_cap_name((uint16_t)(h & 0xFFFF)), (unsigned)pos);
        uint16_t next = (uint16_t)((h >> 20) & 0xFFC);
        if (next < 0x100) break;
        pos = next;
    }
    printf_color(C_TXT, "\n");
}

static void hw_print_device(struct pci_device *d, bool verbose) {
    printf_color(C_OK, "%04x:%02x:%02x.%x ", (unsigned)d->segment, (unsigned)d->bus, (unsigned)d->slot,
                 (unsigned)d->func);
    printf_color(C_VAL, "%04x:%04x ", (unsigned)d->vendor_id, (unsigned)d->device_id);
    printf_color(C_TXT, "rev %02x class %02x%02x%02x %s cfg %u%s%s%s", (unsigned)d->revision,
                 (unsigned)d->class_code, (unsigned)d->subclass, (unsigned)d->prog_if, hw_vendor_name(d->vendor_id),
                 (unsigned)d->cfg_size, d->msi_cap ? " MSI" : "", d->msix_cap ? " MSI-X" : "",
                 d->is_bridge ? " bridge" : "");
    struct pci_device *up = pci_upstream_bridge(d);
    if (up) printf_color(C_TXT, " <- %02x:%02x.%x", (unsigned)up->bus, (unsigned)up->slot, (unsigned)up->func);
    printf_color(C_TXT, "\n");
    if (!verbose) return;
    if (d->is_bridge)
        printf_color(C_TXT, "    buses %02x-%02x\n", (unsigned)d->secondary_bus, (unsigned)d->subordinate_bus);
    hw_print_bars(d);
    hw_print_link(d);
    if (d->msix_cap) printf_color(C_TXT, "    MSI-X table size %d\n", pci_msix_table_size(d));
    if (d->msi_cap) printf_color(C_TXT, "    MSI up to %d vector(s)\n", pci_msi_vector_count(d));
    hw_print_ext_caps(d);
}

static void cmd_pci_ext(const char *arg) {
    arg = hw_skip(arg);
    if (*arg) {
        struct pci_device *d = hw_parse_bdf(arg);
        if (!d) { printf_color(C_BAD, "no such device (use bus:dev.fn or seg:bus:dev.fn, hex)\n"); return; }
        hw_print_device(d, true);
        return;
    }
    printf_color(C_HEAD, "PCI: %d device(s), ECAM %s (%d segment window(s))\n", pci_get_device_count(),
                 pci_ecam_available() ? "on" : "off", pci_ecam_segment_count());
    for (int i = 0; i < pci_get_device_count(); i++) hw_print_device(pci_get_device(i), false);
}

static void cmd_gpu_scan(void) {
    int found = 0;
    for (struct pci_device *d = pci_find_class(PCI_CLASS_DISPLAY, 0xFF, NULL); d;
         d = pci_find_class(PCI_CLASS_DISPLAY, 0xFF, d)) {
        found++;
        printf_color(C_HEAD, "GPU %d: %s\n", found, hw_vendor_name(d->vendor_id)[0] ? hw_vendor_name(d->vendor_id) : "unknown vendor");
        hw_print_device(d, true);
        uint16_t rebar = pci_find_ext_capability(d, PCI_EXT_CAP_ID_REBAR);
        if (rebar) {
            uint32_t cap = pci_dev_read32(d, rebar + 4);
            uint32_t ctl = pci_dev_read32(d, rebar + 8);
            printf_color(C_TXT, "    Resizable BAR%u: current %u MiB, supported mask 0x%x\n", (unsigned)(ctl & 7),
                         1u << (((ctl >> 8) & 0x3F) + 0), (unsigned)(cap >> 4));
        }
        for (struct pci_device *up = pci_upstream_bridge(d); up; up = pci_upstream_bridge(up)) {
            printf_color(C_TXT, "  upstream ");
            hw_print_device(up, false);
            hw_print_link(up);
        }
    }
    if (!found) printf_color(C_BAD, "no display controllers (class 03) found\n");
}

static void cmd_mem_info(void) {
    struct vmm_stats st;
    vmm_get_stats(&st);
    printf_color(C_HEAD, "Memory\n");
    printf_color(C_TXT, "  physical free %llu MiB of %llu MiB\n",
                 (unsigned long long)(pmm_free_page_count() * PAGE_SIZE >> 20),
                 (unsigned long long)(pmm_total_pages() * PAGE_SIZE >> 20));
    printf_color(C_TXT, "  heap mapped %llu MiB, used %llu KiB, grown %llu time(s)\n",
                 (unsigned long long)(heap_get_mapped() >> 20), (unsigned long long)(heap_get_used() >> 10),
                 (unsigned long long)heap_get_grow_count());
    printf_color(C_TXT, "  MMIO window %llu KiB in %llu mapping(s), vmap %llu KiB in %llu mapping(s)\n",
                 (unsigned long long)(st.mmio_used >> 10), (unsigned long long)st.mmio_maps,
                 (unsigned long long)(st.vmap_used >> 10), (unsigned long long)st.vmap_maps);
    printf_color(C_TXT, "  DMA buffers %llu KiB, large-page splits %llu, TLB shootdowns %llu on %d CPU(s)\n",
                 (unsigned long long)(dma_bytes_in_use() >> 10), (unsigned long long)st.huge_splits,
                 (unsigned long long)tlb_shootdown_count(), tlb_online_cpus());
    printf_color(C_TXT, "  PAT 0x%016llx (boot 0x%016llx), dynamic IRQ vectors in use %d\n",
                 (unsigned long long)pat_current_value(), (unsigned long long)pat_boot_value(), irq_dyn_vectors_used());
}

static bool hw_check(bool ok, const char *what) {
    printf_color(ok ? C_OK : C_BAD, "  [%s] ", ok ? " OK " : "FAIL");
    printf_color(C_TXT, "%s\n", what);
    return ok;
}

static void cmd_mmio_test(void) {
    printf_color(C_HEAD, "MMIO / vmap / cache-type self test\n");
    struct vmm_stats before, after;
    vmm_get_stats(&before);
    uint64_t sd_before = tlb_shootdown_count();
    int fails = 0;

    struct limine_framebuffer_request *fbr = get_framebuffer_request();
    uint64_t fb_phys = 0, fb_size = 0;
    if (fbr && fbr->response && fbr->response->framebuffer_count) {
        struct limine_framebuffer *fb = fbr->response->framebuffers[0];
        fb_phys = mm_virt_to_phys((uint64_t)fb->address);
        fb_size = fb->pitch * fb->height;
    }

    if (fb_phys) {
        uint64_t uc = vmm_ioremap(fb_phys, fb_size, VMM_CACHE_UC);
        uint64_t wc = vmm_ioremap(fb_phys, fb_size, VMM_CACHE_WC);
        enum vmm_cache_type tu = VMM_CACHE_COUNT, tw = VMM_CACHE_COUNT;
        vmm_get_cache_type(vmm_kernel_pml4(), uc, &tu);
        vmm_get_cache_type(vmm_kernel_pml4(), wc, &tw);
        fails += !hw_check(uc && wc && tu == VMM_CACHE_UC && tw == VMM_CACHE_WC,
                           "framebuffer mapped twice, as UC and as WC (PTE types read back)");
        fails += !hw_check(uc && vmm_get_phys(vmm_kernel_pml4(), wc + fb_size / 2) == fb_phys + fb_size / 2,
                           "WC alias translates to the framebuffer");
        vmm_iounmap(uc);
        vmm_iounmap(wc);
        fails += !hw_check(!vmm_is_mapped(vmm_kernel_pml4(), wc), "unmapped WC alias is gone");
    }

    uint64_t big = vmm_ioremap(0xFD00000000ULL, 64ULL << 20, VMM_CACHE_WC);
    enum vmm_cache_type tb = VMM_CACHE_COUNT;
    vmm_get_cache_type(vmm_kernel_pml4(), big + (32ULL << 20), &tb);
    fails += !hw_check(big && (big & (VMM_PAGE_2M - 1)) == 0 && tb == VMM_CACHE_WC,
                       "64 MiB WC window uses 2 MiB pages (VRAM-aperture sized mapping)");
    vmm_iounmap(big);

    uint64_t pages[16];
    int got = 0;
    for (; got < 16; got++) {
        pages[got] = pmm_alloc_page();
        if (!pages[got]) break;
    }
    uint64_t v = vmm_vmap(pages, (uint64_t)got, VMM_FLAG_WRITE | VMM_FLAG_GLOBAL | VMM_FLAG_NX, VMM_CACHE_WB);
    bool vm_ok = v != 0;
    for (int i = 0; vm_ok && i < got; i++) {
        *(volatile uint64_t *)(v + (uint64_t)i * PAGE_SIZE) = 0x10C0FFEE00ULL + (uint64_t)i;
        vm_ok = *(volatile uint64_t *)mm_phys_to_virt(pages[i]) == 0x10C0FFEE00ULL + (uint64_t)i;
    }
    fails += !hw_check(vm_ok, "vmap of 16 scattered pages is coherent with the HHDM");
    vmm_vunmap(v);
    for (int i = 0; i < got; i++) pmm_free_page(pages[i]);

    for (int i = 0; i < 200; i++) {
        uint64_t m = vmm_ioremap(0xFE000000ULL + (uint64_t)(i % 7) * PAGE_SIZE, PAGE_SIZE * (1 + i % 5), VMM_CACHE_UC);
        vmm_iounmap(m);
    }
    vmm_get_stats(&after);
    fails += !hw_check(after.mmio_used == before.mmio_used && after.mmio_maps == before.mmio_maps &&
                       after.vmap_used == before.vmap_used,
                       "200 map/unmap cycles leave the windows exactly as before");
    fails += !hw_check(tlb_online_cpus() <= 1 || tlb_shootdown_count() > sd_before,
                       "unmaps were shot down on the other CPUs");

    printf_color(fails ? C_BAD : C_OK, "%d check(s) failed\n", fails);
}

static void cmd_heap_test(void) {
    printf_color(C_HEAD, "Heap growth self test\n");
    int fails = 0;
    size_t mapped_before = heap_get_mapped();
    uint64_t grows_before = heap_get_grow_count();
    size_t big = mapped_before + (32u << 20);

    uint8_t *p = kmalloc(big);
    bool ok = p != NULL;
    if (ok) {
        p[0] = 0x11;
        p[big / 2] = 0x22;
        p[big - 1] = 0x33;
        ok = p[0] == 0x11 && p[big / 2] == 0x22 && p[big - 1] == 0x33;
    }
    char what[96];
    snprintf(what, sizeof(what), "kmalloc of %llu MiB (more than the %llu MiB mapped) succeeds and is writable",
             (unsigned long long)(big >> 20), (unsigned long long)(mapped_before >> 20));
    fails += !hw_check(ok, what);
    fails += !hw_check(heap_get_grow_count() > grows_before && heap_get_mapped() > mapped_before,
                       "heap grew on demand");
    kfree(p);

    void *small[64];
    for (int i = 0; i < 64; i++) small[i] = kmalloc(4096 + (size_t)i * 64);
    bool all = true;
    for (int i = 0; i < 64; i++) all &= small[i] != NULL;
    for (int i = 0; i < 64; i++) kfree(small[i]);
    fails += !hw_check(all, "small allocations still work after the big one is freed");
    printf_color(fails ? C_BAD : C_OK, "%d check(s) failed\n", fails);
}

static volatile uint64_t hw_irq_hits[256];

static void hw_irq_test_handler(void *ctx, struct registers *regs) {
    (void)regs;
    (void)ctx;
    __atomic_fetch_add(&hw_irq_hits[apic_get_lapic_id()], 1, __ATOMIC_RELAXED);
}

static void cmd_irq_test(void) {
    printf_color(C_HEAD, "Dynamic vector + IPI self test\n");
    int vec = irq_alloc_vector();
    if (vec < 0) { printf_color(C_BAD, "no free vector\n"); return; }
    irq_register_ctx_handler((uint8_t)vec, hw_irq_test_handler, NULL);

    uint8_t ids[64];
    int n = tlb_cpu_lapic_ids(ids, 64);
    for (int i = 0; i < 256; i++) hw_irq_hits[i] = 0;
    uint64_t tpm = hw_tsc_per_ms();
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < n; i++) {
            uint64_t want = hw_irq_hits[ids[i]] + 1;
            apic_send_ipi(ids[i], (uint8_t)vec);
            uint64_t t0 = hw_rdtsc();
            while (__atomic_load_n(&hw_irq_hits[ids[i]], __ATOMIC_ACQUIRE) < want) {
                asm volatile("pause");
                if (tpm && hw_rdtsc() - t0 > 100 * tpm) break;
            }
        }
    }

    int fails = 0;
    for (int i = 0; i < n; i++) {
        char what[64];
        snprintf(what, sizeof(what), "LAPIC %u got %llu of 10 interrupts on vector 0x%02x", (unsigned)ids[i],
                 (unsigned long long)hw_irq_hits[ids[i]], (unsigned)vec);
        fails += !hw_check(hw_irq_hits[ids[i]] == 10, what);
    }
    irq_unregister_ctx_handler((uint8_t)vec);
    irq_free_vector((uint8_t)vec);
    printf_color(fails ? C_BAD : C_OK, "%d check(s) failed\n", fails);
}

static void cmd_wc_test(void) {
    struct limine_framebuffer_request *fbr = get_framebuffer_request();
    if (!fbr || !fbr->response || !fbr->response->framebuffer_count) {
        printf_color(C_BAD, "no framebuffer\n");
        return;
    }
    struct limine_framebuffer *fb = fbr->response->framebuffers[0];
    uint64_t phys = mm_virt_to_phys((uint64_t)fb->address);
    uint64_t size = fb->pitch * fb->height;
    uint64_t tpm = hw_tsc_per_ms();
    if (!tpm) { printf_color(C_BAD, "TSC not calibrated\n"); return; }

    static const enum vmm_cache_type types[] = { VMM_CACHE_UC, VMM_CACHE_WC, VMM_CACHE_WT };
    uint64_t mbps[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        uint64_t v = vmm_ioremap(phys, size, types[k]);
        if (!v) continue;
        uint64_t t0 = hw_rdtsc();
        for (int pass = 0; pass < 4; pass++) {
            volatile uint64_t *p = (volatile uint64_t *)v;
            for (uint64_t i = 0; i < size / 8; i++) p[i] = 0xFF101820FF101820ULL + (uint64_t)pass;
        }
        asm volatile("sfence" ::: "memory");
        uint64_t ms = (hw_rdtsc() - t0) / tpm;
        if (!ms) ms = 1;
        mbps[k] = (size * 4 / 1000) / ms;
        vmm_iounmap(v);
    }
    struct limine_video_driver *video = get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    if (video) video->clear(LIMINE_COLOR_BLACK);

    printf_color(C_HEAD, "Framebuffer write speed, %llu KiB x 4 passes\n", (unsigned long long)(size >> 10));
    for (int k = 0; k < 3; k++)
        printf_color(C_VAL, "  %-3s %6llu MB/s\n", pat_type_name(types[k]), (unsigned long long)mbps[k]);
    LOG_INFO("wc-test: UC %llu MB/s, WC %llu MB/s, WT %llu MB/s", (unsigned long long)mbps[0],
             (unsigned long long)mbps[1], (unsigned long long)mbps[2]);
    printf_color(C_TXT, "WC should be many times faster than UC on real hardware (QEMU/TCG shows little difference).\n");
}

static const char *gpu_err_name(int r);

static const char *fw_err_name(int r) {
    switch (r) {
        case FIRMWARE_ERR_NOENT: return "no such path on any mounted volume";
        case FIRMWARE_ERR_SOURCE: return "not a firmware directory or zip archive";
        case FIRMWARE_ERR_NOMEM: return "out of memory";
        default: return "read error";
    }
}

static void cmd_fw_load(const char *name) {
    name = hw_skip(name);
    const struct firmware_blob *fw = NULL;
    int r = firmware_request(name, &fw);
    if (r != FIRMWARE_OK) {
        printf_color(C_BAD, "firmware '%s': %s\n", name,
                     r == FIRMWARE_ERR_NOENT ? "not found" : r == FIRMWARE_ERR_NOMEM ? "out of memory"
                     : r == FIRMWARE_ERR_PARAM ? "bad name" : fw_err_name(r));
        return;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < fw->size; i++) {
        crc ^= fw->data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    printf_color(C_OK, "%s: %llu bytes from %s, crc32 %08x\n", fw->name, (unsigned long long)fw->size, fw->path,
                 ~crc);
    firmware_release(fw);
}

static void cmd_fw_path(const char *arg) {
    arg = hw_skip(arg);
    if (!*arg) {
        switch (firmware_source_kind()) {
            case FW_SOURCE_DIR:
                printf_color(C_TXT, "firmware source: directory '%s'\n", firmware_source_path());
                break;
            case FW_SOURCE_ZIP:
                printf_color(C_TXT, "firmware source: zip '%s', %u entr%s, read on the fly, never unpacked\n",
                             firmware_source_path(), firmware_source_entries(),
                             firmware_source_entries() == 1 ? "y" : "ies");
                break;
            default: {
                char vol[192];
                firmware_volume_zip_note(vol, sizeof(vol));
                if (firmware_image_zip_present())
                    printf_color(C_TXT, "no firmware source; image archive %s, %u entries%s%s\n",
                                 firmware_image_zip_path(), firmware_image_zip_entries(), vol[0] ? "; " : "", vol);
                else
                    printf_color(C_TXT, "no firmware source; %s%s/lib/firmware, /firmware, /boot/firmware of every mount\n",
                                 vol, vol[0] ? "; " : "");
                break;
            }
        }
        printf_color(C_TXT, "fw-path PATH sets one (a lib directory or a .zip), fw-path off clears it\n");
        return;
    }
    if (strcmp(arg, "off") == 0 || strcmp(arg, "none") == 0) {
        firmware_set_source(NULL);
        printf_color(C_OK, "firmware source cleared\n");
        return;
    }
    int r = firmware_set_source(arg);
    if (r != FIRMWARE_OK) {
        printf_color(C_BAD, "fw-path %s: %s\n", arg, fw_err_name(r));
        return;
    }
    if (firmware_source_kind() == FW_SOURCE_ZIP)
        printf_color(C_OK, "firmware source: zip '%s', %u entries, read on the fly\n", firmware_source_path(),
                     firmware_source_entries());
    else
        printf_color(C_OK, "firmware source: directory '%s'\n", firmware_source_path());
}

static void gpu_emit_line(const char *line) {
    printf_color(C_TXT, "  %s\n", line);
}

static void cmd_gpu_stack(const char *arg, bool restart) {
    arg = hw_skip(arg);
    if (*arg) {
        int r = firmware_set_source(arg);
        if (r != FIRMWARE_OK) {
            printf_color(C_BAD, "fw-path %s: %s\n", arg, fw_err_name(r));
            return;
        }
        if (firmware_source_kind() == FW_SOURCE_ZIP)
            printf_color(C_OK, "firmware source: zip '%s', %u entries, read on the fly\n", firmware_source_path(),
                         firmware_source_entries());
        else
            printf_color(C_OK, "firmware source: directory '%s'\n", firmware_source_path());
        printf_color(C_TXT, "the path allows the Linux DRM driver to load its firmware and probe the card\n");
    } else {
        printf_color(C_TXT, "external cards are not initialized by default; a PATH (firmware dir or .zip) allows initialization\n");
    }

    if (restart) {
        printf_color(C_HEAD, "GPU stack: restarting (unbinding every display controller, then probing)\n");
        int unbound = 0, bound = 0;
        int r = gpu_restart_stack(&unbound, &bound);
        if (r == GPU_ERR_TIMEOUT || r == GPU_ERR_BUSY) {
            printf_color(C_BAD, "gpu-reinit: %s (the shell stays alive, check dmesg)\n", gpu_err_name(r));
            return;
        }
        if (r < 0) {
            printf_color(C_BAD, "gpu-reinit failed (%d)\n", r);
            return;
        }
        if (unbound > 0)
            printf_color(C_OK, "%d display controller(s) unbound\n", unbound);
        else
            printf_color(C_TXT, "nothing was bound (starting the stack fresh)\n");
        if (bound > 0)
            printf_color(C_OK, "%d display controller(s) got a driver\n", bound);
        else
            printf_color(C_TXT, "no driver bound (probe failed again — dmesg says why)\n");
        gpu_core_refresh();
        if (gpu_stack_started()) {
            printf_color(C_OK, "GPU stack started: kmscube, vkkmv and the gpu-mon-* commands are available\n");
            mesa_prime_refresh();
        }
    } else {
        printf_color(C_HEAD, "GPU stack: initializing (probing display controllers without a driver)\n");
        int bound = gpu_start_stack();
        if (bound == GPU_ERR_TIMEOUT || bound == GPU_ERR_BUSY) {
            printf_color(C_BAD, "gpu-init: %s (the shell stays alive, check dmesg)\n", gpu_err_name(bound));
            return;
        }
        if (bound < 0) {
            printf_color(C_BAD, "gpu-init failed (%d)\n", bound);
            return;
        }
        if (bound > 0)
            printf_color(C_OK, "%d display controller(s) got a driver\n", bound);
        else
            printf_color(C_TXT, "no new driver bound (already bound, or probe failed again — dmesg says why)\n");
        gpu_core_refresh();
        if (gpu_stack_started()) {
            printf_color(C_OK, "GPU stack started: kmscube, vkkmv and the gpu-mon-* commands are available\n");
            mesa_prime_refresh();
        }
    }

    lkpi_status(gpu_emit_line);
}


static const char *gpu_err_name(int r) {
    switch (r) {
        case GPU_ERR_NODEV: return "no such device";
        case GPU_ERR_PARAM: return "bad parameter";
        case GPU_ERR_BUSY: return "busy";
        case GPU_ERR_NOMEM: return "out of memory";
        case GPU_ERR_NO_DRIVER: return "no Linux driver bound";
        case GPU_ERR_NO_TRANSLATOR: return "no Mesa translator attached (GL/VK batches queue up)";
        case GPU_ERR_TIMEOUT: return "timeout";
        case GPU_ERR_OVERFLOW: return "batch or ring full";
        case GPU_ERR_DRM: return "DRM ioctl failed (dmesg says why)";
        case GPU_ERR_STATE: return "wrong client/batch state";
        case GPU_ERR_NOT_STARTED: return "GPU stack not started (external cards are off by default: run gpu-init PATH)";
        default: return "error";
    }
}

static bool gpu_need_stack(void) {
    if (gpu_stack_started()) return true;
    printf_color(C_BAD, "gpu: no external card initialized - run gpu-init PATH first (PATH = firmware dir or .zip)\n");
    return false;
}

static void cmd_gpu_devs(void) {
    gpu_core_refresh();
    int count = gpu_get_device_count();
    printf_color(C_HEAD, "GPU devices (%d)\n", count);
    if (!count) {
        printf_color(C_TXT, "  none: no display controller and no DRM card (gpu-scan shows PCI, gpu-init starts the stack)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (!d) continue;
        printf_color(C_VAL, "  [%d] ", i);
        if (d->pci_name[0]) printf_color(C_TXT, "%s %04x:%04x rev %02x", d->pci_name, d->vendor, d->device, d->revision);
        else printf_color(C_TXT, "(no PCI device)");
        if (d->flags & GPU_DEV_FLAG_BOOT_VGA) printf_color(C_OK, "  boot display");
        printf_color(C_TXT, "\n");
        printf_color(C_TXT, "      driver %-12s %s%s", d->driver[0] ? d->driver : "none",
                     d->card[0] ? "/dev/dri/" : "", d->card[0] ? d->card : "");
        if (d->render[0]) printf_color(C_TXT, ", /dev/dri/%s", d->render);
        printf_color(C_TXT, "\n");
        if (d->vram_bytes)
            printf_color(C_TXT, "      VRAM/aperture %llu MiB, %d CRTC(s), %d connector(s)\n",
                         (unsigned long long)(d->vram_bytes >> 20), d->num_crtcs, d->num_connectors);
        if (d->vendor == 0x8086) {
            const intel_legacy_id *lid = intel_legacy_lookup(d->device);
            if (lid) printf_color(C_VAL, "      legacy intel: %s (%s, gen%d)\n", lid->marketing, lid->chip, lid->gen);
        }
        if (d->drm_name[0])
            printf_color(C_TXT, "      DRM %d.%d.%d %s\n", d->drm_major, d->drm_minor, d->drm_patch, d->drm_name);
        if (gpu_translator_attached(i)) printf_color(C_OK, "      Mesa translator attached (/dev/gpu)\n");
    }
}

static void cmd_gpu_info(const char *arg) {
    arg = hw_skip(arg);
    int idx = 0;
    if (*arg) idx = (int)strtol(arg, NULL, 0);
    struct gpu_device *d = gpu_get_device(idx);
    if (!d) { gpu_core_refresh(); d = gpu_get_device(idx); }
    if (!d) { printf_color(C_BAD, "no such GPU device (gpu-devs lists them)\n"); return; }
    printf_color(C_HEAD, "GPU device %d\n", idx);
    printf_color(C_TXT, "  PCI      %s  %04x:%04x class %06x rev %02x\n",
                 d->pci_name[0] ? d->pci_name : "(none)", d->vendor, d->device, d->class_code, d->revision);
    printf_color(C_TXT, "  driver   %s%s\n", d->driver[0] ? d->driver : "none (gpu-init / gpu-bind)",
                 (d->flags & GPU_DEV_FLAG_BOOT_VGA) ? ", boot display" : "");
    printf_color(C_TXT, "  nodes    %s%s%s\n", d->card[0] ? "/dev/dri/" : "", d->card[0] ? d->card : "-",
                 d->render[0] ? ", /dev/dri/" : "");
    if (d->render[0]) printf_color(C_TXT, "%s", d->render);
    printf_color(C_TXT, "\n");
    printf_color(C_TXT, "  VRAM     %llu MiB, %d CRTC(s), %d connector(s)\n",
                 (unsigned long long)(d->vram_bytes >> 20), d->num_crtcs, d->num_connectors);
    if (d->drm_name[0]) {
        printf_color(C_TXT, "  DRM      %d.%d.%d %s\n", d->drm_major, d->drm_minor, d->drm_patch, d->drm_name);
        if (d->drm_desc[0]) printf_color(C_TXT, "           %s\n", d->drm_desc);
    }
    printf_color(C_TXT, "  flags    %s%s%s%s%s\n",
                 (d->flags & GPU_DEV_FLAG_BOUND) ? "bound " : "unbound ",
                 (d->flags & GPU_DEV_FLAG_MODESET) ? "modeset " : "",
                 (d->flags & GPU_DEV_FLAG_RENDER) ? "render " : "",
                 (d->flags & GPU_DEV_FLAG_NO_PCI) ? "no-pci " : "",
                 gpu_translator_attached(idx) ? "translator" : "");
    printf_color(C_TXT, "  ring     %u flushed, %u completed, %u dropped (translator %s)\n",
                 gpu_translator_total_flushed(idx), (unsigned)0, gpu_translator_dropped(idx),
                 gpu_translator_attached(idx) ? "attached" : "not attached");
}

static void cmd_gpu_drivers(void) {
    char names[8 * 32];
    int n = gpu_driver_list(names, 8, 32);
    printf_color(C_HEAD, "Linux DRM drivers registered in the KPI (%d)\n", n);
    for (int i = 0; i < n; i++)
        printf_color(C_TXT, "  %s\n", &names[i * 32]);
    if (!n) printf_color(C_TXT, "  none yet (the Linux drivers are still starting, or none registered)\n");
}

static void cmd_gpu_bind(const char *arg, bool bind) {
    arg = hw_skip(arg);
    if (!*arg) { printf_color(C_BAD, "usage: gpu-%s <BDF|index>\n", bind ? "bind" : "unbind"); return; }
    int idx = -1;
    struct gpu_device *d = NULL;
    if (arg[0] >= '0' && arg[0] <= '9' && !strchr(arg, ':')) {
        idx = (int)strtol(arg, NULL, 0);
        d = gpu_get_device(idx);
    } else {
        gpu_core_refresh();
        for (int i = 0; i < gpu_get_device_count(); i++) {
            struct gpu_device *g = gpu_get_device(i);
            if (g && g->pci_name[0] && strcmp(g->pci_name, arg) == 0) { d = g; idx = i; break; }
        }
    }
    if (!d) { printf_color(C_BAD, "no such device (%s)\n", arg); return; }
    if (!d->pci_name[0]) { printf_color(C_BAD, "the non-PCI card has nothing to %s\n", bind ? "bind" : "unbind"); return; }
    printf_color(C_HEAD, "%s %s (%s)\n", bind ? "binding" : "unbinding", d->pci_name,
                 d->driver[0] ? d->driver : "no driver");
    int r = bind ? gpu_bind_device(idx) : gpu_unbind_device(idx);
    if (r < 0) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
    else printf_color(C_OK, "done\n");
}

static void cmd_gpu_api(void) {
    struct gpu_api_info info;
    if (gpu_core_api_info(&info)) { printf_color(C_BAD, "gpu core not ready\n"); return; }
    gpu_core_refresh();
    printf_color(C_HEAD, "gpu_core command surface (protocol %u)\n", info.protocol);
    printf_color(C_TXT, "  OpenGL 4.6 (core+compat): %u commands\n", info.gl_commands);
    printf_color(C_TXT, "  Vulkan 1.3 (core+swapchain): %u commands\n", info.vk_commands);
    printf_color(C_TXT, "  DRM-direct: %u commands (run in the kernel, no userspace involved)\n", info.drm_commands);
    printf_color(C_TXT, "  batch %u bytes (%u commands max), ring %u slots per device\n",
                 info.batch_bytes, 96, info.ring_slots);
    if (info.drv_name[0])
        printf_color(C_TXT, "  card 0: DRM %d.%d.%d %s\n", info.drm_version[0], info.drm_version[1],
                     info.drm_version[2], info.drv_name);
    int dev = gpu_get_device_count() ? 0 : -1;
    for (int i = 0; i < gpu_get_device_count(); i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (d && (d->flags & GPU_DEV_FLAG_BOUND)) { dev = i; break; }
    }
    if (dev >= 0) {
        struct gpu_ring_info ring;
        if (!gpu_translator_ring_info(dev, &ring))
            printf_color(C_TXT, "  device %d ring: %u/%u pending, %u flushed, %u completed, %u dropped\n",
                         dev, ring.pending, ring.slots, ring.total_flushed, ring.total_completed, ring.dropped);
    }
    printf_color(C_TXT, "  /dev/gpu: the Mesa translator node (GPU_IOC_ATTACH/RING/DONE/API)\n");
}

static void cmd_gpu_test(void) {
    printf_color(C_HEAD, "gpu_core self test\n");
    if (!gpu_stack_started()) {
        printf_color(C_BAD, "  [SKIP] GPU stack not started: external cards are not initialized by default;\n");
        printf_color(C_TXT, "        run gpu-init PATH (PATH = a firmware directory or .zip), then try again.\n");
        printf_color(C_TXT, "        kmscube / vkkmv / glxgears render through the Mesa translator (llvmpipe).\n");
        return;
    }
    gpu_core_refresh();
    int count = gpu_get_device_count();
    int fails = 0;
    printf_color(C_TXT, "  devices: %d\n", count);

    int dev = -1;
    for (int i = 0; i < count; i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (d && (d->flags & GPU_DEV_FLAG_BOUND)) { dev = i; break; }
    }
    if (dev < 0) {
        printf_color(C_BAD, "  [SKIP] no bound card: run gpu-init first (the DRM-direct half needs one)\n");
    } else {
        struct gpu_device *d = gpu_get_device(dev);
        int cl = gpu_client_open(dev, GPU_API_NONE);
        printf_color(cl >= 0 ? C_OK : C_BAD, "  [%s] client open on %s (%s)\n",
                     cl >= 0 ? " OK " : "FAIL", d->card[0] ? d->card : d->pci_name, d->driver);

        uint64_t args[2];
        int r = gpu_cmd_begin(cl);
        r |= gpu_cmd_drm(cl, GPU_CMD_DRM_VERSION, 0, NULL, NULL, 0, 96);
        args[0] = GPU_DRM_CAP_DUMB_BUFFER;
        r |= gpu_cmd_drm(cl, GPU_CMD_DRM_GET_CAP, 1, args, NULL, 0, 8);
        args[0] = GPU_DRM_CAP_CURSOR_WIDTH;
        r |= gpu_cmd_drm(cl, GPU_CMD_DRM_GET_CAP, 1, args, NULL, 0, 8);
        r |= gpu_cmd_drm(cl, GPU_CMD_DRM_MODE_GETRESOURCES, 0, NULL, NULL, 0, 16);
        int fl = r ? r : gpu_flush(cl, 0);
        printf_color(fl == GPU_OK ? C_OK : C_BAD, "  [%s] DRM-direct batch: version, 2 caps, mode resources\n",
                     fl == GPU_OK ? " OK " : "FAIL");

        char ver[96];
        uint16_t len = 0;
        int q = gpu_result(cl, 0, ver, sizeof(ver) - 1, &len);
        if (q == GPU_OK && len) {
            ver[len < sizeof(ver) - 1 ? len : sizeof(ver) - 1] = 0;
            printf_color(C_OK, "  [ OK ] DRM version: %s\n", ver);
        } else {
            printf_color(C_BAD, "  [FAIL] DRM version result (%d)\n", q);
            fails++;
        }
        uint64_t cap = 0;
        q = gpu_result(cl, 1, &cap, sizeof(cap), &len);
        printf_color(q == GPU_OK ? C_OK : C_BAD, "  [%s] DRM_CAP_DUMB_BUFFER = %llu\n",
                     q == GPU_OK ? " OK " : "FAIL", (unsigned long long)cap);
        uint32_t counts[4] = {0, 0, 0, 0};
        q = gpu_result(cl, 3, counts, sizeof(counts), &len);
        printf_color(q == GPU_OK ? C_OK : C_BAD, "  [%s] KMS: %u CRTC, %u connector(s), %u encoder(s)\n",
                     q == GPU_OK ? " OK " : "FAIL", counts[0], counts[1], counts[2]);
        gpu_client_close(cl);
    }

    struct gpu_api_info info;
    gpu_core_api_info(&info);
    printf_color(C_TXT, "  vocabulary: %u GL, %u VK, %u DRM commands\n", info.gl_commands, info.vk_commands,
                 info.drm_commands);
    const char *n1 = gpu_cmd_name(GPU_API_OPENGL, GPU_CMD_GL_DRAW_ARRAYS);
    const char *n2 = gpu_cmd_name(GPU_API_VULKAN, GPU_CMD_VK_QUEUE_PRESENT_KHR);
    printf_color((n1 && n2) ? C_OK : C_BAD, "  [%s] name table: %s / %s\n",
                 (n1 && n2) ? " OK " : "FAIL", n1 ? n1 : "?", n2 ? n2 : "?");

    if (count > 0) {
        int cl = gpu_client_open(dev >= 0 ? dev : 0, GPU_API_OPENGL);
        if (cl >= 0) {
            uint64_t args[6];
            int r = gpu_cmd_begin(cl);
            args[0] = 0x0400;
            r |= gpu_cmd_gl(cl, GPU_CMD_GL_CLEAR, 1, args, NULL, 0, 0);
            args[0] = 4; args[1] = 0; args[2] = 6;
            r |= gpu_cmd_gl(cl, GPU_CMD_GL_DRAW_ARRAYS, 3, args, NULL, 0, 0);
            args[0] = 0;
            r |= gpu_cmd_gl(cl, GPU_CMD_GL_FLUSH, 0, NULL, NULL, 0, 0);
            int fl = r ? r : gpu_flush(cl, 0);
            printf_color(fl == GPU_OK ? C_OK : C_BAD, "  [%s] OpenGL batch: glClear, glDrawArrays, glFlush -> ring\n",
                         fl == GPU_OK ? " OK " : "FAIL");
            gpu_client_close(cl);
        }
        int gd = dev >= 0 ? dev : 0;
        struct gpu_ring_info ring;
        if (!gpu_translator_ring_info(gd, &ring))
            printf_color(C_TXT, "  ring: %u pending, %u flushed, %u completed, translator %s\n",
                         ring.pending, ring.total_flushed, ring.total_completed,
                         gpu_translator_attached(gd) ? "attached" : "not attached (batches queue up)");
    }
    printf_color(fails ? C_BAD : C_OK, "  gpu core %s\n", fails ? "FAILED" : "ready");
}

static void cube_progress(void *user, const char *line) {
    (void)user;
    printf_color(C_TXT, "%s\n", line);
}

static void cmd_shader_test(const char *arg) {
    arg = hw_skip(arg);
    bool do_gl = true, do_vk = true;
    if (*arg) {
        do_gl = strncmp(arg, "gl", 2) == 0;
        do_vk = strncmp(arg, "vk", 2) == 0 || strncmp(arg, "vul", 3) == 0;
    }
    printf_color(C_HEAD, "shader self test (real Mesa, kernel-space API)\n");
    if (do_gl) {
        uint32_t crc = 0;
        char note[96] = "";
        int r = gpu_shader_test_gl(&crc, note, sizeof(note));
        if (r == GPU_OK)
            printf_color(C_OK, "  [ OK ] OpenGL: %s, frame CRC32 %08x\n", note, crc);
        else
            printf_color(C_BAD, "  [FAIL] OpenGL shader pipeline (%d, dmesg says why)\n", r);
    }
    if (do_vk) {
        uint32_t crc = 0;
        char note[96] = "";
        int r = gpu_shader_test_vk(&crc, note, sizeof(note));
        if (r == GPU_OK)
            printf_color(C_OK, "  [ OK ] Vulkan: %s, frame CRC32 %08x\n", note, crc);
        else
            printf_color(C_BAD, "  [FAIL] Vulkan shader pipeline (%d, dmesg says why)\n", r);
    }
}

static void cmd_prime(const char *arg) {
    arg = hw_skip(arg);
    if (!*arg) {
        printf_color(C_HEAD, "Mesa PRIME render device (current: %s)\n", mesa_prime_name(mesa_prime_get()));
        for (int i = 0; i < mesa_prime_count(); i++)
            printf_color(C_TXT, "  [%d] %s%s\n", i, mesa_prime_name(i),
                         i == mesa_prime_get() ? "  <- selected" : "");
        printf_color(C_TXT, "  gpu-prime N selects one; the Mesa host restarts on the next use\n");
        return;
    }
    if (strcmp(arg, "auto") == 0 || strcmp(arg, "llvm") == 0 || strcmp(arg, "llvmpipe") == 0) {
        int sel = arg[0] == 'a' ? MESA_PRIME_AUTO : MESA_PRIME_LLVM;
        int r = mesa_prime_set(sel);
        if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
        else printf_color(C_OK, "PRIME: %s\n", mesa_prime_name(sel));
        return;
    }
    int idx = (int)strtol(arg, NULL, 0);
    int r = mesa_prime_set(idx);
    if (r) printf_color(C_BAD, "failed: %s (%d) - gpu-prime lists them\n", gpu_err_name(r), r);
    else printf_color(C_OK, "PRIME: %s\n", mesa_prime_name(idx));
}

static void cmd_mesa_info(void) {
    struct mesa_host_info info;
    if (mesa_host_info(&info) || !mesa_host_pid_get()) {
        printf_color(C_TXT, "Mesa host not running (starts on the first GL/Vulkan use; llvmpipe by default)\n");
        return;
    }
    printf_color(C_HEAD, "Mesa translator (kernel-space API -> built-in Mesa)\n");
    printf_color(C_TXT, "  host pid %u, context %s\n", info.pid, info.gl_ok ? "ready" : "starting");
    if (info.renderer[0]) printf_color(C_VAL, "  renderer: %s\n", info.renderer);
    if (info.env[0]) printf_color(C_TXT, "  engine: %s\n", info.env);
    printf_color(C_TXT, "  PRIME: %s\n", mesa_prime_name(mesa_prime_get()));
}

static void cmd_glxgears(const char *arg) {
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.progress = cube_progress;
    arg = hw_skip(arg);
    if (*arg >= '0' && *arg <= '9') opts.seconds = (uint32_t)strtoul(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    if (*arg >= '0' && *arg <= '9') opts.dev_idx = (int)strtol(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    if (*arg == '-' || (*arg >= '0' && *arg <= '9')) opts.connector = (int)strtol(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    unsigned w = (unsigned)strtoul(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    unsigned h = (*arg == 'x') ? (unsigned)strtoul(arg + 1, NULL, 0) : 0;
    if (w && h) {
        opts.width = (uint16_t)w;
        opts.height = (uint16_t)h;
    }
    uint32_t frames = 0, fps = 0;
    int r = gpu_glxgears_run(&opts, &frames, &fps);
    if (r) printf_color(C_BAD, "  failed: %s (%d) - glxgears needs the Mesa translator\n", gpu_err_name(r), r);
    else printf_color(C_OK, "  glxgears: OK (%u frames, %u fps, real OpenGL via Mesa)\n", frames, fps);
}

static void cmd_cube(const char *arg, bool vkk) {
    if (!gpu_stack_started())
        printf_color(C_TXT, "%s: no external card - rendering through Mesa llvmpipe (gpu-init PATH for a card)\n",
                     vkk ? "vkkmv" : "kmscube");
    gpu_cube_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.dev_idx = -1;
    opts.connector = -1;
    opts.progress = cube_progress;
    arg = hw_skip(arg);
    if (*arg >= '0' && *arg <= '9') opts.seconds = (uint32_t)strtoul(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    if (*arg >= '0' && *arg <= '9') opts.dev_idx = (int)strtol(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    if (*arg == '-' || (*arg >= '0' && *arg <= '9'))
        opts.connector = (int)strtol(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    unsigned w = (unsigned)strtoul(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    unsigned h = (*arg == 'x') ? (unsigned)strtoul(arg + 1, NULL, 0) : 0;
    if (w && h) {
        opts.width = (uint16_t)w;
        opts.height = (uint16_t)h;
    }
    uint32_t frames = 0, fps = 0;
    int r = vkk ? gpu_vkkmv_run(&opts, &frames, &fps) : gpu_kmscube_run(&opts, &frames, &fps);
    if (r) printf_color(C_BAD, "  failed: %s (%d)\n", gpu_err_name(r), r);
    else printf_color(C_OK, "  %s: OK (%u frames, %u fps)\n", vkk ? "vkkmv" : "kmscube", frames, fps);
}

static void cmd_gpu_mons(void) {
    if (!gpu_need_stack()) return;
    gpu_core_refresh();
    int probed = 0;
    for (int i = 0; i < gpu_get_device_count(); i++) {
        struct gpu_device *d = gpu_get_device(i);
        if (d && (d->flags & GPU_DEV_FLAG_MODESET)) {
            if (gpu_monitor_probe(i) == GPU_OK) probed++;
        }
    }
    printf_color(C_HEAD, "Monitors (%d connector(s) on %d card(s))\n", gpu_monitor_count(), probed);
    for (int i = 0; i < gpu_monitor_count(); i++) {
        const gpu_monitor *m = gpu_monitor_get(i);
        if (!m) continue;
        printf_color(C_VAL, "  [%d] ", i);
        printf_color(C_TXT, "%s%s", m->name, m->connected ? "" : " (disconnected)");
        if (m->enabled && m->mode_current >= 0) {
            const gpu_drm_modeinfo_uapi *mode = &m->modes[m->mode_current];
            printf_color(C_OK, " on %ux%u@%u at +%u+%u", mode->hdisplay, mode->vdisplay,
                         mode->vrefresh, m->x, m->y);
            if (m->cloned) printf_color(C_TXT, " (clone of %d)", m->cloned - 1);
        }
        printf_color(C_TXT, ", %d mode(s)", m->mode_count);
        if (m->brightness != 100) printf_color(C_TXT, ", brightness %u%%", m->brightness);
        printf_color(C_TXT, "\n");
    }
    printf_color(C_TXT, "  desktop %ux%u; gpu-mon-on N [MODE] / gpu-mon-off N / gpu-mon-move N X Y\n",
                 gpu_monitor_desktop_width(), gpu_monitor_desktop_height());
    printf_color(C_TXT, "  gpu-mon-mode N M|WxH[@HZ] / gpu-mon-modes N / gpu-mon-bright N 0-100\n");
}

static int mon_index(const char **parg) {
    const char *arg = hw_skip(*parg);
    if (*arg < '0' || *arg > '9') return -1;
    int idx = (int)strtol(arg, (char **)&arg, 0);
    *parg = arg;
    return idx;
}

static void cmd_gpu_mon_on(const char *arg) {
    int idx = mon_index(&arg);
    if (idx < 0) { printf_color(C_BAD, "usage: gpu-mon-on N [MODE]\n"); return; }
    arg = hw_skip(arg);
    int mode = -1;
    if (*arg >= '0' && *arg <= '9') mode = (int)strtol(arg, NULL, 0);
    int r = gpu_monitor_enable(idx, mode);
    if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
    else {
        const gpu_monitor *m = gpu_monitor_get(idx);
        if (m && m->mode_current >= 0) {
            const gpu_drm_modeinfo_uapi *mode2 = &m->modes[m->mode_current];
            printf_color(C_OK, "%s: %ux%u@%u at +%u+%u\n", m->name, mode2->hdisplay, mode2->vdisplay,
                         mode2->vrefresh, m->x, m->y);
        }
    }
}

static void cmd_gpu_mon_move(const char *arg) {
    int idx = mon_index(&arg);
    if (idx < 0) { printf_color(C_BAD, "usage: gpu-mon-move N X Y\n"); return; }
    arg = hw_skip(arg);
    unsigned x = (unsigned)strtoul(arg, (char **)&arg, 0);
    arg = hw_skip(arg);
    unsigned y = (unsigned)strtoul(arg, NULL, 0);
    int r = gpu_monitor_move(idx, x, y);
    if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
    else printf_color(C_OK, "monitor %d at +%u+%u\n", idx, x, y);
}

static void cmd_gpu_mon_clone(const char *arg) {
    int dst = mon_index(&arg);
    arg = hw_skip(arg);
    int src = mon_index(&arg);
    if (dst < 0 || src < 0) { printf_color(C_BAD, "usage: gpu-mon-clone DST SRC\n"); return; }
    int r = gpu_monitor_clone(dst, src);
    if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
    else printf_color(C_OK, "monitor %d clones %d\n", dst, src);
}

static void cmd_gpu_mon_modes(const char *arg) {
    int idx = mon_index(&arg);
    if (idx < 0) { printf_color(C_BAD, "usage: gpu-mon-modes N\n"); return; }
    const gpu_monitor *m = gpu_monitor_get(idx);
    if (!m) { printf_color(C_BAD, "no such monitor\n"); return; }
    printf_color(C_HEAD, "Monitor %d (%s) modes\n", idx, m->name);
    for (int i = 0; i < m->mode_count; i++) {
        const gpu_drm_modeinfo_uapi *mode = &m->modes[i];
        printf_color(m->mode_current == i ? C_OK : C_TXT, "  [%2d] %4ux%-4u @ %2u Hz%s\n", i,
                     mode->hdisplay, mode->vdisplay, mode->vrefresh,
                     (mode->type & GPU_DRM_MODE_TYPE_PREFERRED) ? "  (preferred)" : "");
    }
}

static void cmd_gpu_mon_bright(const char *arg) {
    int idx = mon_index(&arg);
    if (idx < 0) { printf_color(C_BAD, "usage: gpu-mon-bright N 0-100\n"); return; }
    arg = hw_skip(arg);
    if (*arg < '0' || *arg > '9') {
        const gpu_monitor *m = gpu_monitor_get(idx);
        if (!m) { printf_color(C_BAD, "no such monitor\n"); return; }
        printf_color(C_TXT, "monitor %d (%s): brightness %u%%\n", idx, m->name, m->brightness);
        return;
    }
    unsigned pct = (unsigned)strtoul(arg, NULL, 0);
    int r = gpu_monitor_set_brightness(idx, pct);
    if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
    else printf_color(C_OK, "monitor %d brightness %u%%\n", idx, pct);
}

static void cmd_gpu_bright(const char *arg) {
    arg = hw_skip(arg);
    int n = gpu_backlight_count();
    if (!*arg) {
        printf_color(C_HEAD, "Panel backlights (%d)\n", n);
        if (!n) {
            printf_color(C_TXT, "  none (a bound GPU driver registers panel backlights; external monitors have none -\n"
                                "  use gpu-mon-bright N 0-100 for per-monitor software brightness)\n");
            return;
        }
        for (int i = 0; i < n; i++) {
            char name[32];
            uint32_t cur = 0, max = 0;
            if (gpu_backlight_get(i, name, sizeof(name), &cur, &max)) continue;
            printf_color(C_VAL, "  [%d] ", i);
            printf_color(C_TXT, "%-16s %u/%u\n", name, cur, max);
        }
        printf_color(C_TXT, "  gpu-bright N VALUE sets one (hardware backlight)\n");
        return;
    }
    char *end = NULL;
    const char *rest = NULL;
    long idx = strtol(arg, &end, 0);
    rest = hw_skip(end);
    if (idx < 0 || idx >= n || !*rest) { printf_color(C_BAD, "usage: gpu-bright N VALUE (0..max)\n"); return; }
    long val = strtol(rest, NULL, 0);
    int r = gpu_backlight_set((int)idx, (uint32_t)val);
    if (r) printf_color(C_BAD, "failed: backlight %d not set (%d)\n", (int)idx, r);
    else {
        char name[32];
        uint32_t cur = 0, max = 0;
        gpu_backlight_get((int)idx, name, sizeof(name), &cur, &max);
        printf_color(C_OK, "%s: %u/%u\n", name, cur, max);
    }
}

static void cmd_gpu_intel(void) {
    printf_color(C_HEAD, "Old Intel integrated graphics (gen1 via i810 DRM + intel-gtt AGP, gen2+ via i915)\n");
    int found = 0;
    for (int i = 0; i < pci_get_device_count(); i++) {
        struct pci_device *pdev = pci_get_device(i);
        if (!pdev || pdev->vendor_id != 0x8086 || pdev->class_code != 0x03) continue;
        const intel_legacy_id *id = intel_legacy_lookup(pdev->device_id);
        if (!id) continue;
        printf_color(C_VAL, "  %02x:%02x.%x ", pdev->bus, pdev->slot, pdev->func);
        printf_color(C_TXT, "%04x %s (%s, gen%d)\n", pdev->device_id, id->marketing, id->chip, id->gen);
        found++;
    }
    if (!found) {
        printf_color(C_TXT, "  none (the table covers gen1-gen8: i810/i810e/i815, i830M..i965, GMA 900/950/3000/3100/3150, X3000/X3500/X4500, Ironlake, Sandy Bridge..Broadwell)\n");
        return;
    }
    int bound = intel_legacy_bind_all();
    printf_color(bound ? C_OK : C_TXT, "  %d newly bound through the Linux stack (%d already had a driver)\n",
                 bound, found - bound);
}
bool shell_hw_command(const char *cmd) {
    if (strcmp(cmd, "gpu-scan") == 0) { cmd_gpu_scan(); return true; }
    if (strcmp(cmd, "kmscube") == 0) { cmd_cube("", false); return true; }
    if (strncmp(cmd, "kmscube ", 8) == 0) { cmd_cube(cmd + 8, false); return true; }
    if (strcmp(cmd, "vkkmv") == 0) { cmd_cube("", true); return true; }
    if (strncmp(cmd, "vkkmv ", 6) == 0) { cmd_cube(cmd + 6, true); return true; }
    if (strcmp(cmd, "gpu-mons") == 0) { cmd_gpu_mons(); return true; }
    if (strncmp(cmd, "gpu-mon-on ", 11) == 0) { cmd_gpu_mon_on(cmd + 11); return true; }
    if (strncmp(cmd, "gpu-mon-off ", 12) == 0) {
        const char *p = cmd + 12;
        int idx = mon_index(&p);
        if (idx < 0) { printf_color(C_BAD, "usage: gpu-mon-off N\n"); return true; }
        int r = gpu_monitor_disable(idx);
        if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
        else printf_color(C_OK, "monitor %d off\n", idx);
        return true;
    }
    if (strncmp(cmd, "gpu-mon-mode ", 13) == 0) {
        const char *p = cmd + 13;
        char *end2 = NULL;
        int idx = mon_index(&p);
        if (idx < 0) { printf_color(C_BAD, "usage: gpu-mon-mode N M|WxH[@HZ]\n"); return true; }
        p = hw_skip(p);
        if (!*p) { printf_color(C_BAD, "usage: gpu-mon-mode N M|WxH[@HZ]\n"); return true; }
        int r;
        if (strchr(p, 'x')) {
            unsigned w = (unsigned)strtoul(p, &end2, 0);
            unsigned h = 0, hz = 0;
            if (w && *end2 == 'x') {
                h = (unsigned)strtoul(end2 + 1, &end2, 0);
                if (*end2 == '@') hz = (unsigned)strtoul(end2 + 1, NULL, 0);
            }
            if (!w || !h) { printf_color(C_BAD, "usage: gpu-mon-mode N WxH[@HZ]\n"); return true; }
            r = gpu_monitor_set_mode_wh(idx, w, h, hz);
            if (r) printf_color(C_BAD, "failed: %s (%d) - gpu-mon-modes N lists them\n", gpu_err_name(r), r);
            else {
                const gpu_monitor *m = gpu_monitor_get(idx);
                if (m && m->mode_current >= 0) {
                    const gpu_drm_modeinfo_uapi *mode = &m->modes[m->mode_current];
                    printf_color(C_OK, "monitor %d: %ux%u@%u\n", idx, mode->hdisplay, mode->vdisplay, mode->vrefresh);
                }
            }
        } else {
            int mode = (*p >= '0' && *p <= '9') ? (int)strtol(p, NULL, 0) : -1;
            if (mode < 0) { printf_color(C_BAD, "usage: gpu-mon-mode N M|WxH[@HZ]\n"); return true; }
            r = gpu_monitor_set_mode(idx, mode);
            if (r) printf_color(C_BAD, "failed: %s (%d)\n", gpu_err_name(r), r);
            else printf_color(C_OK, "monitor %d switched to mode %d\n", idx, mode);
        }
        return true;
    }
    if (strncmp(cmd, "gpu-mon-modes ", 14) == 0) { cmd_gpu_mon_modes(cmd + 14); return true; }
    if (strncmp(cmd, "gpu-mon-bright ", 15) == 0) { cmd_gpu_mon_bright(cmd + 15); return true; }
    if (strcmp(cmd, "gpu-bright") == 0) { cmd_gpu_bright(""); return true; }
    if (strncmp(cmd, "gpu-bright ", 11) == 0) { cmd_gpu_bright(cmd + 11); return true; }
    if (strncmp(cmd, "gpu-mon-move ", 13) == 0) { cmd_gpu_mon_move(cmd + 13); return true; }
    if (strncmp(cmd, "gpu-mon-clone ", 14) == 0) { cmd_gpu_mon_clone(cmd + 14); return true; }
    if (strcmp(cmd, "gpu-mon-layout") == 0) {
        int r = gpu_monitor_layout_auto();
        if (r) printf_color(C_BAD, "no enabled monitor\n");
        else printf_color(C_OK, "side by side, desktop %ux%u\n",
                          gpu_monitor_desktop_width(), gpu_monitor_desktop_height());
        return true;
    }
    if (strcmp(cmd, "gpu-intel") == 0) { cmd_gpu_intel(); return true; }
    if (strcmp(cmd, "glxgears") == 0) { cmd_glxgears(""); return true; }
    if (strncmp(cmd, "glxgears ", 9) == 0) { cmd_glxgears(cmd + 9); return true; }
    if (strcmp(cmd, "gpu-prime") == 0) { cmd_prime(""); return true; }
    if (strncmp(cmd, "gpu-prime ", 10) == 0) { cmd_prime(cmd + 10); return true; }
    if (strcmp(cmd, "gpu-mesa") == 0) { cmd_mesa_info(); return true; }
    if (strcmp(cmd, "gpu-shader") == 0) { cmd_shader_test(""); return true; }
    if (strncmp(cmd, "gpu-shader ", 11) == 0) { cmd_shader_test(cmd + 11); return true; }
    if (strcmp(cmd, "pci-ext") == 0) { cmd_pci_ext(""); return true; }
    if (strncmp(cmd, "pci-ext ", 8) == 0) { cmd_pci_ext(cmd + 8); return true; }
    if (strcmp(cmd, "mem-info") == 0) { cmd_mem_info(); return true; }
    if (strcmp(cmd, "mmio-test") == 0) { cmd_mmio_test(); return true; }
    if (strcmp(cmd, "heap-test") == 0) { cmd_heap_test(); return true; }
    if (strcmp(cmd, "irq-test") == 0) { cmd_irq_test(); return true; }
    if (strcmp(cmd, "wc-test") == 0) { cmd_wc_test(); return true; }
    if (strcmp(cmd, "fw-path") == 0) { cmd_fw_path(""); return true; }
    if (strncmp(cmd, "fw-path ", 8) == 0) { cmd_fw_path(cmd + 8); return true; }
    if (strncmp(cmd, "fw-load ", 8) == 0) { cmd_fw_load(cmd + 8); return true; }
    if (strcmp(cmd, "gpu-init") == 0) { cmd_gpu_stack("", false); return true; }
    if (strncmp(cmd, "gpu-init ", 9) == 0) { cmd_gpu_stack(cmd + 9, false); return true; }
    if (strcmp(cmd, "gpu-reinit") == 0) { cmd_gpu_stack("", true); return true; }
    if (strncmp(cmd, "gpu-reinit ", 11) == 0) { cmd_gpu_stack(cmd + 11, true); return true; }
    if (strcmp(cmd, "gpu-devs") == 0) { cmd_gpu_devs(); return true; }
    if (strcmp(cmd, "gpu-info") == 0) { cmd_gpu_info(""); return true; }
    if (strncmp(cmd, "gpu-info ", 9) == 0) { cmd_gpu_info(cmd + 9); return true; }
    if (strcmp(cmd, "gpu-drivers") == 0) { cmd_gpu_drivers(); return true; }
    if (strcmp(cmd, "gpu-api") == 0) { cmd_gpu_api(); return true; }
    if (strcmp(cmd, "gpu-test") == 0) { cmd_gpu_test(); return true; }
    if (strncmp(cmd, "gpu-bind ", 9) == 0) { cmd_gpu_bind(cmd + 9, true); return true; }
    if (strncmp(cmd, "gpu-unbind ", 11) == 0) { cmd_gpu_bind(cmd + 11, false); return true; }
    if (strcmp(cmd, "hw-test") == 0) {
        cmd_mem_info();
        cmd_mmio_test();
        cmd_heap_test();
        cmd_irq_test();
        return true;
    }
    return false;
}

void help_row(uint32_t color, const char *left, const char *right);
void help_section(const char *title);

void shell_hw_help(void) {
    help_section("GPU groundwork");
    help_row(LIMINE_COLOR_AMBER, "gpu-init [PATH] - start GPU stack (fw source)", "gpu-reinit [PATH] - restart it");
    help_row(LIMINE_COLOR_AMBER, "gpu-scan - display controllers, BARs, links", "fw-path [PATH|off] - firmware source");
    help_row(LIMINE_COLOR_AMBER, "fw-load NAME - load firmware NAME, print CRC", "pci-ext [BDF] - PCIe devices");
    help_row(LIMINE_COLOR_AMBER, "mem-info - heap, MMIO, DMA, PAT state", "hw-test - all self tests below");
    help_row(LIMINE_COLOR_AMBER, "mmio-test - ioremap/vmap/cache types", "irq-test - dynamic vectors + IPIs");
    help_row(LIMINE_COLOR_AMBER, "wc-test - UC/WC/WT write speed", "heap-test - heap growth");
    help_section("gpu_core: devices, Mesa translator, demos");
    help_row(LIMINE_COLOR_AMBER, "gpu-devs - GPU device list", "gpu-info [N] - one device in detail");
    help_row(LIMINE_COLOR_AMBER, "gpu-drivers - Linux DRM drivers in the KPI", "gpu-bind BDF|N - bind one");
    help_row(LIMINE_COLOR_AMBER, "gpu-unbind BDF|N - unbind one", "gpu-api - command surface, ring");
    help_row(LIMINE_COLOR_AMBER, "gpu-test - DRM-direct + GL/VK batch test", "gpu-intel - old Intel iGPU scan/bind");
    help_row(LIMINE_COLOR_AMBER, "gpu-prime [llvm|N|auto] - Mesa render device", "gpu-mesa - translator status");
    help_row(LIMINE_COLOR_AMBER, "gpu-shader [gl|vk] - shader self test", "kmscube - kernel GL cube");
    help_row(LIMINE_COLOR_AMBER, "vkkmv - kernel Vulkan cube", "glxgears [SEC] - kernel GL gears");
    help_section("gpu_core: monitors");
    help_row(LIMINE_COLOR_AMBER, "gpu-mons - monitors, modes, layout", "gpu-mon-on N [MODE] - enable");
    help_row(LIMINE_COLOR_AMBER, "gpu-mon-off N - power off (stays off)", "gpu-mon-mode N M|WxH[@HZ] - set");
    help_row(LIMINE_COLOR_AMBER, "gpu-mon-modes N - list modes", "gpu-mon-move N X Y - position");
    help_row(LIMINE_COLOR_AMBER, "gpu-mon-clone DST SRC - clone", "gpu-mon-layout - side by side");
    help_row(LIMINE_COLOR_AMBER, "gpu-mon-bright N 0-100 - brightness", "gpu-bright [N VALUE] - backlight");
}

