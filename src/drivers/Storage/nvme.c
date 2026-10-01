#include "nvme.h"

#include "block_device.h"
#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/pci.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"

#include <stddef.h>
#include <string.h>

#define NVME_DBG 0
#if NVME_DBG
#define NVME_TRACE(...) LOG_DEBUG(__VA_ARGS__)
#else
#define NVME_TRACE(...) do { } while (0)
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
    if (!phys) { LOG_ERROR("pmm_alloc_pages(%u) failed", pages); return NULL; }

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
        LOG_ERROR("completion timeout for opcode 0x%02x cid %u", (unsigned)(sqe->cdw0 & 0xFF), (unsigned)cid);
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
        LOG_ERROR("opcode 0x%02x failed: SCT=0x%x SC=0x%02x status=0x%04x",
                  (unsigned)(sqe->cdw0 & 0xFF), sct, sc, (unsigned)status);
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
    if (!q->sq) { LOG_ERROR("queue %u: submission queue allocation failed", qid); return false; }

    q->cq = (nvme_cqe_t *)nvme_alloc_aligned(entries * sizeof(nvme_cqe_t), PAGE_SIZE);
    if (!q->cq) { LOG_ERROR("queue %u: completion queue allocation failed", qid); return false; }

    q->sq_entries = entries;
    q->cq_entries = entries;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->expected_phase = 1;
    q->next_cid = (uint16_t)(qid * 1000);
    q->sq_doorbell = nvme_doorbell(ctrl, qid, false);
    q->cq_doorbell = nvme_doorbell(ctrl, qid, true);
    LOG_DEBUG("queue %u: %u entries, sq=0x%llx cq=0x%llx", qid, entries,
              (unsigned long long)mm_ptr_to_phys(q->sq), (unsigned long long)mm_ptr_to_phys(q->cq));
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

    NVME_TRACE("ns %u: %s lba=%llu count=%u prp1=0x%llx prp2=0x%llx", ns->nsid, write ? "write" : "read",
               (unsigned long long)lba, count, (unsigned long long)prp1, (unsigned long long)prp2);

    int ret = nvme_submit_poll(&ctrl->io_q, &sqe);
    if (ret != BLOCK_OK)
        LOG_ERROR("%s: %s lba=%llu count=%u failed (%d)", ns->blkdev.name, write ? "write" : "read",
                  (unsigned long long)lba, count, ret);
    return ret;
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
    pci_enable_bus_mastering(dev);
    NVME_TRACE("bus mastering enabled, bar0=0x%llx bar1=0x%llx",
               (unsigned long long)dev->bar0, (unsigned long long)dev->bar1);

    bool bar_is_io = false;
    uint64_t bar_phys = pci_bar_phys(dev, 0, &bar_is_io);
    if (bar_is_io || !bar_phys) {
        LOG_ERROR("BAR0 is not a usable memory BAR (io=%d phys=0x%llx), skipping",
                  (int)bar_is_io, (unsigned long long)bar_phys);
        return false;
    }

    uint64_t bar_virt = vmm_map_mmio(bar_phys, PAGE_SIZE * 4, 0);
    if (!bar_virt) {
        LOG_ERROR("failed to map BAR0 MMIO window at 0x%llx", (unsigned long long)bar_phys);
        return false;
    }
    LOG_DEBUG("BAR0 phys=0x%llx virt=0x%llx", (unsigned long long)bar_phys, (unsigned long long)bar_virt);

    ctrl->regs = (volatile nvme_regs_t *)bar_virt;
    uint64_t cap = ctrl->regs->cap;

    if (cap == 0xFFFFFFFFFFFFFFFFULL || cap == 0) {
        LOG_ERROR("CAP reads back as 0x%016llx - controller not responding, skipping", (unsigned long long)cap);
        return false;
    }
    ctrl->dstrd = NVME_CAP_DSTRD(cap);
    uint32_t mqes = NVME_CAP_MQES(cap);
    uint32_t to_ms = NVME_CAP_TO(cap) * 500;
    if (to_ms == 0) to_ms = NVME_RESET_TIMEOUT_MS;

    LOG_DEBUG("CAP=0x%016llx DSTRD=%u MQES=%u TO=%u ms VS=0x%08x",
              (unsigned long long)cap, (unsigned)ctrl->dstrd, mqes, to_ms, ctrl->regs->vs);

    if (!NVME_CAP_CSS_NVM(cap)) {
        LOG_ERROR("controller does not advertise the NVM command set, skipping");
        return false;
    }

    ctrl->regs->cc &= ~NVME_CC_EN;
    {
        int t = (int)to_ms;
        while ((ctrl->regs->csts & NVME_CSTS_RDY) && t-- > 0) delay_ms(1);
        if (ctrl->regs->csts & NVME_CSTS_RDY) {
            LOG_ERROR("controller never went not-ready after CC.EN=0, CSTS=0x%08x", ctrl->regs->csts);
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
                LOG_ERROR("controller reported fatal status (CSTS.CFS) during enable, CSTS=0x%08x", ctrl->regs->csts);
                return false;
            }
            delay_ms(1);
        }
        if (!(ctrl->regs->csts & NVME_CSTS_RDY)) {
            LOG_ERROR("controller never became ready after CC.EN=1, CSTS=0x%08x", ctrl->regs->csts);
            return false;
        }
    }
    LOG_DEBUG("controller ready (CSTS.RDY=1), admin queue %u entries", admin_entries);

    void *idbuf = nvme_alloc_aligned(4096, 4096);
    if (!idbuf) { LOG_ERROR("identify buffer allocation failed"); return false; }

    if (nvme_do_identify(ctrl, NVME_IDENTIFY_CNS_CONTROLLER, 0, idbuf) != BLOCK_OK) {
        LOG_ERROR("IDENTIFY CONTROLLER failed");
        return false;
    }
    uint8_t *idc = (uint8_t *)idbuf;
    uint32_t nn = *(uint32_t *)(idc + 516);
    {
        char model[41];
        char serial[21];
        memcpy(model, idc + 24, 40);
        memcpy(serial, idc + 4, 20);
        model[40] = '\0';
        serial[20] = '\0';
        for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
        for (int i = 19; i >= 0 && serial[i] == ' '; i--) serial[i] = '\0';
        LOG_INFO("controller \"%s\" serial \"%s\", %u namespace(s)", model, serial, nn);
    }

    if (!nvme_queue_alloc(ctrl, &ctrl->io_q, NVME_IO_QUEUE_ENTRIES, NVME_IO_QUEUE_ID)) {
        return false;
    }
    if (nvme_create_io_cq(ctrl) != BLOCK_OK) { LOG_ERROR("CREATE I/O CQ failed"); return false; }
    if (nvme_create_io_sq(ctrl) != BLOCK_OK) { LOG_ERROR("CREATE I/O SQ failed"); return false; }
    LOG_DEBUG("I/O queue pair created");

    if (nn > 32) nn = 32;

    for (uint32_t nsid = 1; nsid <= nn && *disk_idx < NVME_MAX_DISKS; nsid++) {
        memset(idbuf, 0, 4096);
        if (nvme_do_identify(ctrl, NVME_IDENTIFY_CNS_NAMESPACE, nsid, idbuf) != BLOCK_OK) {
            LOG_WARNING("namespace %u: identify failed, skipping", nsid);
            continue;
        }

        uint64_t nsze;
        memcpy(&nsze, idc, 8);
        if (nsze == 0) {
            LOG_DEBUG("namespace %u: inactive", nsid);
            continue;
        }

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

        LOG_INFO("namespace %u -> %s: %llu sectors x %u bytes (%llu MB)", nsid, bd->name,
                 (unsigned long long)ns->sector_count, ns->sector_size,
                 (unsigned long long)((ns->sector_count * ns->sector_size) >> 20));

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
    pci_init();

    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc) { LOG_ERROR("no TSC driver, NVMe disabled"); return NULL; }
    delay_ms = tsc->sleep_tsc_ms;

    int disk_idx = 0;

    int pci_count = pci_get_device_count();
    if (pci_count > MAX_PCI_DEVICES) pci_count = MAX_PCI_DEVICES;

    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev = (struct pci_device *)device_table[PCI_DEVICE][i];
        if (!dev) continue;

        NVME_TRACE("pci %d class=0x%02x subclass=0x%02x progif=0x%02x", i,
                   (unsigned)dev->class_code, (unsigned)dev->subclass, (unsigned)dev->prog_if);

        if (dev->class_code != 0x01) continue;
        if (dev->subclass != 0x08) continue;
        if (dev->prog_if != 0x02) continue;

        if (drv_nvme.controller_count >= NVME_MAX_CONTROLLERS) {
            LOG_WARNING("more than %d NVMe controllers, ignoring the rest", NVME_MAX_CONTROLLERS);
            break;
        }

        LOG_INFO("found controller %04x:%04x at %02x:%02x.%x",
                 (unsigned)dev->vendor_id, (unsigned)dev->device_id,
                 (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func);

        nvme_controller_t *ctrl = &drv_nvme.controllers[drv_nvme.controller_count];
        memset(ctrl, 0, sizeof(*ctrl));

        if (nvme_controller_init(ctrl, dev, &disk_idx)) {
            drv_nvme.controller_count++;
        } else {
            LOG_ERROR("controller at %02x:%02x.%x failed to initialize, skipping",
                      (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func);
        }
    }

    LOG_INFO("NVMe init done, %d controller(s), %d disk(s)", drv_nvme.controller_count, disk_idx);

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
