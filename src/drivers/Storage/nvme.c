#include "nvme.h"

#include "block_device.h"
#include "components/drivers.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/pci.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Video/limine_video_driver.h"

#include <stddef.h>
#include <string.h>

static void nvme_puts(const char *s, uint32_t color) {
    struct limine_video_driver *v = get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    v->printf(s, color);
}

static void nvme_hex32(uint32_t val, uint32_t color) {
    const char h[] = "0123456789ABCDEF";
    char buf[11] = "0x00000000";

    for (int i = 9; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    nvme_puts(buf, color);
}

static void nvme_hex64(uint64_t val, uint32_t color) {
    const char h[] = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    nvme_puts(buf, color);
}

static void nvme_dec(uint64_t v, uint32_t color) {
    char buf[21]; int i = 20; buf[20] = 0;
    if (v == 0) { nvme_puts("0", color); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    nvme_puts(buf + i, color);
}

#define NCOL_INFO LIMINE_COLOR_LIGHT_CYAN
#define NCOL_OK LIMINE_COLOR_LIGHT_GREEN
#define NCOL_ERR LIMINE_COLOR_LIGHT_RED
#define NCOL_DATA LIMINE_COLOR_YELLOW

#define ULOG(s) do { nvme_puts("[NVME] " s "\n", NCOL_INFO); } while(0)
#define UERR(s) do { nvme_puts("[NVME] ERR " s "\n", NCOL_ERR); } while(0)

#define NVME_DBG 0
#if NVME_DBG
#define NDBG_COM1 0x3F8
static int ndbg_inited = 0;
static inline void ndbg_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t ndbg_inb(uint16_t port) {
    uint8_t r; asm volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static void ndbg_init(void) {
    if (ndbg_inited) return;
    ndbg_inited = 1;
    ndbg_outb(NDBG_COM1 + 1, 0x00);
    ndbg_outb(NDBG_COM1 + 3, 0x80);
    ndbg_outb(NDBG_COM1 + 0, 0x03);
    ndbg_outb(NDBG_COM1 + 1, 0x00);
    ndbg_outb(NDBG_COM1 + 3, 0x03);
    ndbg_outb(NDBG_COM1 + 2, 0xC7);
    ndbg_outb(NDBG_COM1 + 4, 0x0B);
}
static void ndbg_putc(char c) {
    ndbg_init();
    for (int spin = 0; spin < 100000 && !(ndbg_inb(NDBG_COM1 + 5) & 0x20); spin++) {}
    ndbg_outb(NDBG_COM1, (uint8_t)c);
}
static void ndbg_str(const char *s) { while (*s) ndbg_putc(*s++); }
static void ndbg_hex64(uint64_t v) {
    ndbg_str("0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        ndbg_putc(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
    }
}
#else
static void ndbg_str(const char *s) { (void)s; }
static void ndbg_hex64(uint64_t v) { (void)v; }
#endif

static inline void nvme_io_mb(void) {
    asm volatile("mfence" ::: "memory");
}

#define NVME_RESET_TIMEOUT_MS 5000
#define NVME_CMD_TIMEOUT_MS 3000
static void (*delay_ms)(uint64_t);

static void *nvme_alloc_aligned(size_t size, size_t align) {
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE + 1);
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { UERR("nvme_alloc_aligned: pmm_alloc_pages failed"); return NULL; }

    uint64_t virt = mm_phys_to_virt(phys);
    uint64_t aligned = (virt + align - 1) & ~(uint64_t)(align - 1);
    memset((void *)aligned, 0, size);
    return (void *)aligned;
}

static volatile uint32_t *nvme_doorbell(nvme_controller_t *ctrl, uint32_t qid, bool is_cq) {
    volatile uint8_t *base = (volatile uint8_t *)ctrl->regs + 0x1000;
    uint32_t stride = 4u << ctrl->dstrd;
    uint32_t index = (qid * 2) + (is_cq ? 1 : 0);
    return (volatile uint32_t *)(base + (uint64_t)index * stride);
}

static int nvme_submit_poll(nvme_queue_t *q, nvme_sqe_t *sqe) {
    spin_lock(&q->lock);

    uint16_t cid = q->next_cid++;
    sqe->cdw0 = (sqe->cdw0 & 0x0000FFFFu) | ((uint32_t)cid << 16);

    uint32_t tail = q->sq_tail;
    memcpy(&q->sq[tail], sqe, sizeof(nvme_sqe_t));

    nvme_io_mb();

    tail = (tail + 1) % q->sq_entries;
    q->sq_tail = tail;
    *q->sq_doorbell = tail;
    nvme_io_mb();

    int t = NVME_CMD_TIMEOUT_MS;
    nvme_cqe_t *cqe = &q->cq[q->cq_head];
    while (t-- > 0) {
        if (NVME_CQE_PHASE(cqe) == q->expected_phase) break;
        delay_ms(1);
    }
    if (t <= 0) {
        UERR("nvme_submit_poll: completion timeout");
        spin_unlock(&q->lock);
        return BLOCK_ERR_TIMEOUT;
    }

    uint16_t status = cqe->status;
    uint32_t sc = NVME_CQE_SC(cqe);
    uint32_t sct = NVME_CQE_SCT(cqe);

    q->cq_head = (q->cq_head + 1) % q->cq_entries;
    if (q->cq_head == 0) q->expected_phase ^= 1;

    *q->cq_doorbell = q->cq_head;
    nvme_io_mb();

    if (sc != 0 || sct != 0) {
        nvme_puts("[NVME] ERR cmd failed SCT=", NCOL_INFO); nvme_hex32(sct, NCOL_DATA);
        nvme_puts(" SC=", NCOL_INFO); nvme_hex32(sc, NCOL_DATA);
        nvme_puts(" status=", NCOL_INFO); nvme_hex32(status, NCOL_DATA); nvme_puts("\n", NCOL_INFO);
        spin_unlock(&q->lock);
        return BLOCK_ERR_IO;
    }

    spin_unlock(&q->lock);
    return BLOCK_OK;
}

static void nvme_build_prp(void *buf, uint32_t total_bytes, uint64_t *prp_list,
                            uint64_t *out_prp1, uint64_t *out_prp2) {
    uint64_t phys = mm_ptr_to_phys(buf);
    uint64_t page0_base = phys & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t offset = phys - page0_base;
    uint64_t first_chunk = PAGE_SIZE - offset;

    *out_prp1 = phys;

    if (total_bytes <= first_chunk) {
        *out_prp2 = 0;
        return;
    }

    uint64_t remaining = total_bytes - first_chunk;
    uint32_t more_pages = (uint32_t)((remaining + PAGE_SIZE - 1) / PAGE_SIZE);

    if (more_pages == 1) {
        *out_prp2 = page0_base + PAGE_SIZE;
        return;
    }

    for (uint32_t i = 0; i < more_pages; i++) {
        prp_list[i] = page0_base + (uint64_t)(i + 1) * PAGE_SIZE;
    }
    *out_prp2 = mm_ptr_to_phys(prp_list);
}

static bool nvme_queue_alloc(nvme_controller_t *ctrl, nvme_queue_t *q,
                              uint32_t entries, uint32_t qid) {
    q->sq = (nvme_sqe_t *)nvme_alloc_aligned(entries * sizeof(nvme_sqe_t), PAGE_SIZE);
    if (!q->sq) { UERR("nvme_queue_alloc: sq alloc failed"); return false; }

    q->cq = (nvme_cqe_t *)nvme_alloc_aligned(entries * sizeof(nvme_cqe_t), PAGE_SIZE);
    if (!q->cq) { UERR("nvme_queue_alloc: cq alloc failed"); return false; }

    q->sq_entries = entries;
    q->cq_entries = entries;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->expected_phase = 1;
    q->next_cid = (uint16_t)(qid * 1000);
    q->sq_doorbell = nvme_doorbell(ctrl, qid, false);
    q->cq_doorbell = nvme_doorbell(ctrl, qid, true);
    return true;
}

static int nvme_do_identify(nvme_controller_t *ctrl, uint8_t cns, uint32_t nsid, void *out_4k) {
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_IDENTIFY;
    sqe.nsid = nsid;
    sqe.prp1 = mm_ptr_to_phys(out_4k);
    sqe.prp2 = 0;
    sqe.cdw10 = cns;
    return nvme_submit_poll(&ctrl->admin_q, &sqe);
}

static int nvme_create_io_cq(nvme_controller_t *ctrl) {
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_CREATE_CQ;
    sqe.prp1 = mm_ptr_to_phys(ctrl->io_q.cq);
    sqe.cdw10 = ((ctrl->io_q.cq_entries - 1) << 16) | NVME_IO_QUEUE_ID;
    sqe.cdw11 = 1u;
    return nvme_submit_poll(&ctrl->admin_q, &sqe);
}

static int nvme_create_io_sq(nvme_controller_t *ctrl) {
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = NVME_ADMIN_CREATE_SQ;
    sqe.prp1 = mm_ptr_to_phys(ctrl->io_q.sq);
    sqe.cdw10 = ((ctrl->io_q.sq_entries - 1) << 16) | NVME_IO_QUEUE_ID;
    sqe.cdw11 = 1u | ((uint32_t)NVME_IO_QUEUE_ID << 16);
    return nvme_submit_poll(&ctrl->admin_q, &sqe);
}

static int nvme_rw(nvme_controller_t *ctrl, nvme_namespace_t *ns, bool write,
                    uint64_t lba, uint32_t count, void *buf) {
    if (!ctrl || !ns || !buf || count == 0) return BLOCK_ERR_PARAM;

    uint32_t total_bytes = count * ns->sector_size;
    static uint64_t prp_list_storage[512];

    uint64_t prp1, prp2;
    nvme_build_prp(buf, total_bytes, prp_list_storage, &prp1, &prp2);

    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = write ? NVME_CMD_WRITE : NVME_CMD_READ;
    sqe.nsid = ns->nsid;
    sqe.prp1 = prp1;
    sqe.prp2 = prp2;
    sqe.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    sqe.cdw11 = (uint32_t)(lba >> 32);
    sqe.cdw12 = (count - 1) & 0xFFFF;

    return nvme_submit_poll(&ctrl->io_q, &sqe);
}

static int nvme_blk_read(struct block_device *self, uint64_t lba, uint32_t count, void *buf) {
    nvme_namespace_t *ns = (nvme_namespace_t *)self->priv;
    return nvme_rw(ns->ctrl, ns, false, lba, count, buf);
}

static int nvme_blk_write(struct block_device *self, uint64_t lba, uint32_t count, void *buf) {
    nvme_namespace_t *ns = (nvme_namespace_t *)self->priv;
    return nvme_rw(ns->ctrl, ns, true, lba, count, (void *)buf);
}

static int nvme_blk_flush(struct block_device *self) {
    (void)self;

    return BLOCK_OK;
}

static struct nvme_driver drv_nvme;

static bool nvme_controller_init(nvme_controller_t *ctrl, struct pci_device *dev, int *disk_idx) {
    ndbg_str("\n[nvme] controller_init enter\n");
    pci_enable_bus_mastering(dev);
    ndbg_str("[nvme] bus mastering enabled\n");

    ndbg_str("[nvme] bar0="); ndbg_hex64(dev->bar0); ndbg_str(" bar1="); ndbg_hex64(dev->bar1); ndbg_str("\n");

    bool bar_is_io = false;
    uint64_t bar_phys = pci_bar_phys(dev, 0, &bar_is_io);
    if (bar_is_io || !bar_phys) {
        UERR("BAR0 is not a usable memory BAR, skipping");
        return false;
    }
    ndbg_str("[nvme] bar_phys="); ndbg_hex64(bar_phys); ndbg_str("\n");

    nvme_puts("[NVME] BAR0 phys=", NCOL_INFO); nvme_hex64(bar_phys, NCOL_DATA); nvme_puts("\n", NCOL_INFO);

    uint64_t bar_virt = vmm_map_mmio(bar_phys, PAGE_SIZE * 4, 0);
    if (!bar_virt) {
        UERR("failed to map BAR0 MMIO window");
        return false;
    }
    nvme_puts("[NVME] BAR0 virt=", NCOL_INFO); nvme_hex64(bar_virt, NCOL_DATA); nvme_puts("\n", NCOL_INFO);
    ndbg_str("[nvme] BAR0 mapped into page tables\n");

    ctrl->regs = (volatile nvme_regs_t *)bar_virt;
    ndbg_str("[nvme] regs virt="); ndbg_hex64((uint64_t)ctrl->regs); ndbg_str("\n");
    uint64_t cap = ctrl->regs->cap;
    ndbg_str("[nvme] cap read OK cap="); ndbg_hex64(cap); ndbg_str("\n");

    if (cap == 0xFFFFFFFFFFFFFFFFULL || cap == 0) {
        nvme_puts("[NVME] ERR CAP reads back as ", NCOL_ERR); nvme_hex64(cap, NCOL_DATA);
        nvme_puts(" - controller not responding, skipping\n", NCOL_ERR);
        return false;
    }
    ctrl->dstrd = NVME_CAP_DSTRD(cap);
    uint32_t mqes = NVME_CAP_MQES(cap);
    uint32_t to_ms = NVME_CAP_TO(cap) * 500;
    if (to_ms == 0) to_ms = NVME_RESET_TIMEOUT_MS;

    nvme_puts("[NVME] CAP=", NCOL_INFO); nvme_hex64(cap, NCOL_DATA);
    nvme_puts(" DSTRD=", NCOL_INFO); nvme_dec(ctrl->dstrd, NCOL_DATA);
    nvme_puts(" MQES=", NCOL_INFO); nvme_dec(mqes, NCOL_DATA);
    nvme_puts(" TO=", NCOL_INFO); nvme_dec(to_ms, NCOL_DATA); nvme_puts("ms\n", NCOL_INFO);

    if (!NVME_CAP_CSS_NVM(cap)) {
        UERR("controller does not advertise the NVM command set, skipping");
        return false;
    }

    ctrl->regs->cc &= ~NVME_CC_EN;
    {
        int t = (int)to_ms;
        while ((ctrl->regs->csts & NVME_CSTS_RDY) && t-- > 0) delay_ms(1);
        if (ctrl->regs->csts & NVME_CSTS_RDY) {
            UERR("controller never went not-ready after CC.EN=0");
            return false;
        }
    }

    uint32_t admin_entries = NVME_ADMIN_QUEUE_ENTRIES;
    if (admin_entries > mqes) admin_entries = mqes;

    if (!nvme_queue_alloc(ctrl, &ctrl->admin_q, admin_entries, NVME_ADMIN_QUEUE_ID))
        return false;

    ctrl->regs->aqa = ((admin_entries - 1) << 16) | (admin_entries - 1);
    ctrl->regs->asq = mm_ptr_to_phys(ctrl->admin_q.sq);
    ctrl->regs->acq = mm_ptr_to_phys(ctrl->admin_q.cq);

    ctrl->regs->cc = NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_AMS_RR
                    | NVME_CC_SHN_NONE
                    | NVME_CC_IOSQES(6)
                    | NVME_CC_IOCQES(4)
                    | NVME_CC_EN;
    nvme_io_mb();

    {
        int t = (int)to_ms;
        while (!(ctrl->regs->csts & NVME_CSTS_RDY) && t-- > 0) {
            if (ctrl->regs->csts & NVME_CSTS_CFS) {
                UERR("controller reported fatal status (CSTS.CFS) during enable");
                return false;
            }
            delay_ms(1);
        }
        if (!(ctrl->regs->csts & NVME_CSTS_RDY)) {
            UERR("controller never became ready (CSTS.RDY) after CC.EN=1");
            return false;
        }
    }
    ULOG("controller ready (CSTS.RDY=1)");

    void *idbuf = nvme_alloc_aligned(4096, 4096);
    if (!idbuf) { UERR("identify buffer alloc failed"); return false; }

    if (nvme_do_identify(ctrl, NVME_IDENTIFY_CNS_CONTROLLER, 0, idbuf) != BLOCK_OK) {
        UERR("IDENTIFY CONTROLLER failed");
        return false;
    }
    uint8_t *idc = (uint8_t *)idbuf;
    uint32_t nn = *(uint32_t *)(idc + 516);
    nvme_puts("[NVME] IDENTIFY CONTROLLER: NN=", NCOL_INFO); nvme_dec(nn, NCOL_DATA); nvme_puts("\n", NCOL_INFO);

    if (!nvme_queue_alloc(ctrl, &ctrl->io_q, NVME_IO_QUEUE_ENTRIES, NVME_IO_QUEUE_ID)) {
        return false;
    }
    if (nvme_create_io_cq(ctrl) != BLOCK_OK) { UERR("CREATE I/O CQ failed"); return false; }
    if (nvme_create_io_sq(ctrl) != BLOCK_OK) { UERR("CREATE I/O SQ failed"); return false; }
    ULOG("I/O queue pair created");

    if (nn > 32) nn = 32;

    for (uint32_t nsid = 1; nsid <= nn && *disk_idx < NVME_MAX_DISKS; nsid++) {
        memset(idbuf, 0, 4096);
        if (nvme_do_identify(ctrl, NVME_IDENTIFY_CNS_NAMESPACE, nsid, idbuf) != BLOCK_OK) {
            nvme_puts("[NVME] namespace ", NCOL_INFO); nvme_dec(nsid, NCOL_DATA);
            nvme_puts(": identify FAILED, skipping\n", NCOL_INFO);
            continue;
        }

        uint64_t nsze;
        memcpy(&nsze, idc, 8);
        if (nsze == 0) continue;

        uint8_t flbas = idc[26] & 0xF;
        uint32_t lbaf_off = 128 + (uint32_t)flbas * 4;
        uint32_t lbaf;
        memcpy(&lbaf, idc + lbaf_off, 4);
        uint8_t lbads = (uint8_t)((lbaf >> 16) & 0xFF);
        uint32_t sector_size = 1u << lbads;
        if (sector_size < 512 || sector_size > 65536) sector_size = 512;

        nvme_namespace_t *ns = &ctrl->namespaces[ctrl->namespace_count++];
        ns->present = true;
        ns->nsid = nsid;
        ns->sector_count = nsze;
        ns->sector_size = sector_size;
        ns->ctrl = ctrl;

        struct block_device *bd = &ns->blkdev;

        bd->name[0] = 'n'; bd->name[1] = 'd';
        bd->name[2] = (char)('a' + *disk_idx);
        bd->name[3] = '\0';
        bd->sector_count = ns->sector_count;
        bd->sector_size = ns->sector_size;
        bd->priv = ns;
        bd->read_sectors = nvme_blk_read;
        bd->write_sectors = nvme_blk_write;
        bd->flush = nvme_blk_flush;

        nvme_puts("[NVME] namespace ", NCOL_INFO); nvme_dec(nsid, NCOL_DATA);
        nvme_puts(": init OK -> nd", NCOL_INFO);
        { char _dl[2] = { (char)('a' + *disk_idx), 0 }; nvme_puts(_dl, NCOL_OK); }
        nvme_puts(" sectors=", NCOL_INFO); nvme_dec(ns->sector_count, NCOL_DATA);
        nvme_puts(" sector_size=", NCOL_INFO); nvme_dec(ns->sector_size, NCOL_DATA); nvme_puts("\n", NCOL_INFO);

        block_device_register(bd);
        (*disk_idx)++;
    }

    ctrl->present = true;
    return true;
}

static int nvme_read(int disk, uint64_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= drv_nvme.disk_count) return BLOCK_ERR_PARAM;
    for (int c = 0; c < drv_nvme.controller_count; c++) {
        nvme_controller_t *ctrl = &drv_nvme.controllers[c];
        for (int n = 0; n < ctrl->namespace_count; n++) {
            if (disk-- == 0) return nvme_rw(ctrl, &ctrl->namespaces[n], false, lba, count, buf);
        }
    }
    return BLOCK_ERR_PARAM;
}

static int nvme_write(int disk, uint64_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= drv_nvme.disk_count) return BLOCK_ERR_PARAM;
    for (int c = 0; c < drv_nvme.controller_count; c++) {
        nvme_controller_t *ctrl = &drv_nvme.controllers[c];
        for (int n = 0; n < ctrl->namespace_count; n++) {
            if (disk-- == 0) return nvme_rw(ctrl, &ctrl->namespaces[n], true, lba, count, buf);
        }
    }
    return BLOCK_ERR_PARAM;
}

static int nvme_get_disk_count(void) {
    return drv_nvme.disk_count;
}

static struct nvme_driver drv_nvme = {
    .disk_count = 0,
    .controller_count = 0,
    .read = nvme_read,
    .write = nvme_write,
    .get_disk_count = nvme_get_disk_count,
};

struct nvme_driver *return_nvme_driver(void) {
    ndbg_str("\n[nvme] return_nvme_driver ENTER\n");
    ULOG("=== NVMe init start ===");

    pci_init();
    ndbg_str("[nvme] pci_init done\n");

    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc) { UERR("no TSC driver"); return NULL; }
    delay_ms = tsc->sleep_tsc_ms;
    ndbg_str("[nvme] got tsc driver, delay_ms set\n");

    int disk_idx = 0;

    int pci_count = pci_get_device_count();
    if (pci_count > MAX_PCI_DEVICES) pci_count = MAX_PCI_DEVICES;

    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev = (struct pci_device *)device_table[PCI_DEVICE][i];
        if (!dev) continue;

        ndbg_str("[nvme] pci scan i="); ndbg_hex64(i);
        ndbg_str(" class="); ndbg_hex64(dev->class_code);
        ndbg_str(" subclass="); ndbg_hex64(dev->subclass);
        ndbg_str(" progif="); ndbg_hex64(dev->prog_if); ndbg_str("\n");

        if (dev->class_code != 0x01) continue;
        if (dev->subclass != 0x08) continue;
        if (dev->prog_if != 0x02) continue;

        ndbg_str("[nvme] MATCH found at i="); ndbg_hex64(i); ndbg_str("\n");

        if (drv_nvme.controller_count >= NVME_MAX_CONTROLLERS) break;

        nvme_puts("[NVME] found controller ", NCOL_INFO);
        nvme_hex32(dev->vendor_id, NCOL_DATA); nvme_puts(":", NCOL_INFO); nvme_hex32(dev->device_id, NCOL_DATA);
        nvme_puts(" @ bus=", NCOL_INFO); nvme_dec(dev->bus, NCOL_DATA);
        nvme_puts(" slot=", NCOL_INFO); nvme_dec(dev->slot, NCOL_DATA);
        nvme_puts(" func=", NCOL_INFO); nvme_dec(dev->func, NCOL_DATA); nvme_puts("\n", NCOL_INFO);

        nvme_controller_t *ctrl = &drv_nvme.controllers[drv_nvme.controller_count];
        memset(ctrl, 0, sizeof(*ctrl));

        if (nvme_controller_init(ctrl, dev, &disk_idx)) {
            drv_nvme.controller_count++;
        } else {
            nvme_puts("[NVME] controller init FAILED, skipping\n", NCOL_ERR);
        }
    }

    nvme_puts("[NVME] init done, disks=", NCOL_INFO); nvme_dec(disk_idx, NCOL_DATA); nvme_puts("\n", NCOL_INFO);
    ULOG("=== NVMe init end ===");

    drv_nvme.disk_count = disk_idx;
    return disk_idx > 0 ? &drv_nvme : NULL;
}

struct driver *return_meta_nvme_driver(void) {
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "NVMe Storage Driver",
        .type = STORAGE_DRIVER,
        .sub_type = NVME_STORAGE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &drv_nvme,
        .init = (void *)return_nvme_driver,
    };
    return &meta;
}
