#include "ahci.h"

#include "kernel/scheduler/scheduler.h"

#include "ata.h"
#include "block_device.h"
#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/pci.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"

#include <stddef.h>
#include <string.h>

static inline void ahci_io_mb(void)
{
    asm volatile("mfence" ::: "memory");
}

#define AHCI_TIMEOUT_MS 500

#define AHCI_DBG 0
#if AHCI_DBG
#define AHCI_TRACE(...) LOG_DEBUG(__VA_ARGS__)
#else
#define AHCI_TRACE(...) do { } while (0)
#endif

#define AHCI_LINK_SETTLE_MS 100
#define AHCI_LINK_TIMEOUT_MS 600

static void ahci_lock(spinlock_t *l)
{
    while (!spin_trylock(l)) {
        if (current_task()) scheduler_yield();
        else asm volatile("pause");
    }
}

static void (*delay_ms)(uint64_t);

static void port_stop(volatile hba_port_t *port)
{
    port->cmd &= ~HBA_CMD_ST;
    int t = AHCI_TIMEOUT_MS;
    while ((port->cmd & HBA_CMD_CR) && t-- > 0) delay_ms(1);
    if (t <= 0) LOG_WARNING("CR timeout, DMA engine won't stop, CMD=0x%08x", port->cmd);

    port->cmd &= ~HBA_CMD_FRE;
    t = AHCI_TIMEOUT_MS;
    while ((port->cmd & HBA_CMD_FR) && t-- > 0) delay_ms(1);
    if (t <= 0) LOG_WARNING("FR timeout, FIS receive won't stop, CMD=0x%08x", port->cmd);
}

static void port_start(volatile hba_port_t *port)
{
    int t = AHCI_TIMEOUT_MS;
    while ((port->cmd & HBA_CMD_CR) && t-- > 0) delay_ms(1);
    if (t <= 0) LOG_WARNING("CR still set before FRE/ST, CMD=0x%08x", port->cmd);

    port->cmd |= HBA_CMD_FRE;
    port->cmd |= HBA_CMD_ST;
}

static void *ahci_alloc_aligned(size_t size, size_t align) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { LOG_ERROR("pmm_alloc_pages failed for %llu bytes", (unsigned long long)size); return NULL; }

    uint64_t virt = mm_phys_to_virt(phys);
    uint64_t aligned = (virt + align - 1) & ~(uint64_t)(align - 1);
    memset((void *)aligned, 0, size);
    return (void *)aligned;
}

static int port_find_free_slot(volatile hba_port_t *port)
{
    uint32_t slots_used = port->sact | port->ci;
    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        if ((slots_used & (1u << i)) == 0) return i;
    }
    return -1;
}

static int port_issue_cmd(ahci_port_t *ap, bool write, uint64_t lba, uint32_t sectors, void *buf)
{
    ahci_lock(&ap->lock);

    volatile hba_port_t *port = ap->regs;

    int t = AHCI_TIMEOUT_MS;
    while ((port->tfd & 0x88) && t-- > 0) delay_ms(1);
    if (t <= 0) {
        LOG_ERROR("%s: BSY/DRQ timeout before command, TFD=0x%08x", ap->blkdev.name, port->tfd);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_TIMEOUT;
    }

    int slot = port_find_free_slot(port);
    if (slot < 0) {
        LOG_ERROR("%s: no free command slot, CI=0x%08x SACT=0x%08x", ap->blkdev.name, port->ci, port->sact);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_TIMEOUT;
    }

    hba_cmd_header_t *hdr = &ap->cmd_list[slot];
    hdr->prdtl = 1;
    hdr->flags = (uint16_t)((sizeof(uint8_t[20]) / 4) | (write ? (1u << 6) : 0));
    hdr->prdbc = 0;

    uint64_t tbl_phys = mm_ptr_to_phys(ap->cmd_tables[slot]);
    hdr->ctba = (uint32_t)(tbl_phys & 0xFFFFFFFF);
    hdr->ctbau = (uint32_t)(tbl_phys >> 32);

    hba_cmd_table_t *tbl = ap->cmd_tables[slot];
    memset(tbl, 0, sizeof(hba_cmd_table_t));

    uint8_t *cfis = tbl->cfis;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;
    cfis[2] = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    cfis[7] = 0x40;

    cfis[4] = (uint8_t)(lba);
    cfis[5] = (uint8_t)(lba >> 8);
    cfis[6] = (uint8_t)(lba >> 16);
    cfis[8] = (uint8_t)(lba >> 24);
    cfis[9] = (uint8_t)(lba >> 32);
    cfis[10] = (uint8_t)(lba >> 40);

    cfis[12] = (uint8_t)(sectors);
    cfis[13] = (uint8_t)(sectors >> 8);

    uint64_t buf_phys = mm_ptr_to_phys(buf);
    tbl->prdt[0].dba = (uint32_t)(buf_phys & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    tbl->prdt[0].dbc = (sectors * ap->sector_size) - 1;

    ahci_io_mb();

    port->is = (uint32_t)~0;
    port->ci = (1u << slot);

    AHCI_TRACE("%s: %c slot=%d lba=0x%llx n=%u", ap->blkdev.name, write ? 'W' : 'R', slot, (unsigned long long)lba, sectors);

    t = AHCI_TIMEOUT_MS * 10;
    while (--t > 0) {
        delay_ms(1);
        if (!(port->ci & (1u << slot))) break;
        if (port->is & HBA_IS_TFES) {
            LOG_ERROR("%s: %s lba=%llu n=%u task file error IS=0x%08x TFD=0x%08x SERR=0x%08x", ap->blkdev.name,
                      write ? "write" : "read", (unsigned long long)lba, sectors, port->is, port->tfd, port->serr);
            spin_unlock(&ap->lock);
            return BLOCK_ERR_IO;
        }
    }
    if (t <= 0) {
        LOG_ERROR("%s: %s lba=%llu n=%u timed out IS=0x%08x TFD=0x%08x CI=0x%08x", ap->blkdev.name,
                  write ? "write" : "read", (unsigned long long)lba, sectors, port->is, port->tfd, port->ci);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_TIMEOUT;
    }
    if (port->is & HBA_IS_TFES) {
        LOG_ERROR("%s: %s lba=%llu n=%u late task file error IS=0x%08x TFD=0x%08x", ap->blkdev.name,
                  write ? "write" : "read", (unsigned long long)lba, sectors, port->is, port->tfd);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_IO;
    }

    spin_unlock(&ap->lock);
    return BLOCK_OK;
}

static bool port_identify(ahci_port_t *ap)
{
    LOG_DEBUG("IDENTIFY DEVICE");

    uint16_t *id = (uint16_t *)ahci_alloc_aligned(512, 512);
    if (!id) { LOG_ERROR("identify buffer allocation failed"); return false; }

    volatile hba_port_t *port = ap->regs;

    int t = AHCI_TIMEOUT_MS * 2;
    int bsy_waited = 0;
    while ((port->tfd & 0x80) && t-- > 0) { delay_ms(1); bsy_waited++; }
    if (bsy_waited > 0) {
        LOG_DEBUG("waited %d ms for BSY to clear", bsy_waited);
    }
    if (port->tfd & 0x80) {
        LOG_ERROR("BSY stuck, TFD=0x%08x", port->tfd);
        return false;
    }

    LOG_DEBUG("TFD=0x%08x before IDENTIFY", port->tfd);

    int slot = port_find_free_slot(port);
    if (slot < 0) { LOG_ERROR("no free command slot for IDENTIFY"); return false; }

    hba_cmd_header_t *hdr = &ap->cmd_list[slot];
    hdr->prdtl = 1;
    hdr->flags = (uint16_t)(sizeof(uint8_t[20]) / 4);
    hdr->prdbc = 0;

    uint64_t tbl_phys = mm_ptr_to_phys(ap->cmd_tables[slot]);
    hdr->ctba = (uint32_t)(tbl_phys & 0xFFFFFFFF);
    hdr->ctbau = (uint32_t)(tbl_phys >> 32);

    hba_cmd_table_t *tbl = ap->cmd_tables[slot];
    memset(tbl, 0, sizeof(hba_cmd_table_t));

    tbl->cfis[0] = FIS_TYPE_REG_H2D;
    tbl->cfis[1] = 0x80;
    tbl->cfis[2] = ATA_CMD_IDENTIFY;

    uint64_t id_phys = mm_ptr_to_phys(id);
    tbl->prdt[0].dba = (uint32_t)(id_phys & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)(id_phys >> 32);
    tbl->prdt[0].dbc = 511;

    ahci_io_mb();
    port->is = (uint32_t)~0;
    port->ci = (1u << slot);

    LOG_DEBUG("IDENTIFY issued in slot %d", slot);

    t = AHCI_TIMEOUT_MS * 10;
    while (--t > 0 && (port->ci & (1u << slot))) {
        if (port->is & HBA_IS_TFES) break;
        delay_ms(1);
    }

    if (t <= 0) {
        LOG_ERROR("IDENTIFY timed out IS=0x%08x TFD=0x%08x SERR=0x%08x", port->is, port->tfd, port->serr);
        return false;
    }
    if (port->is & HBA_IS_TFES) {
        LOG_ERROR("IDENTIFY task file error IS=0x%08x TFD=0x%08x SERR=0x%08x", port->is, port->tfd, port->serr);
        return false;
    }

    ap->sector_count = (uint64_t)id[100]
                     | ((uint64_t)id[101] << 16)
                     | ((uint64_t)id[102] << 32)
                     | ((uint64_t)id[103] << 48);

    if (ap->sector_count == 0)
        ap->sector_count = ((uint32_t)id[61] << 16) | id[60];

    ap->sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1 << 12)))
        ap->sector_size = 512 * (1u << (id[106] & 0xF));

    LOG_DEBUG("IDENTIFY OK: sectors=%llu sector_size=%u", (unsigned long long)ap->sector_count, (unsigned)ap->sector_size);

    return true;
}

static int ahci_blk_read(struct block_device *self, uint64_t lba, uint32_t count, void *buf) {
    ahci_port_t *ap = (ahci_port_t *)self->priv;
    return port_issue_cmd(ap, false, lba, count, buf);
}

static int ahci_blk_write(struct block_device *self, uint64_t lba, uint32_t count, void *buf) {
    ahci_port_t *ap = (ahci_port_t *)self->priv;
    return port_issue_cmd(ap, true, lba, count, (void *)buf);
}

static int ahci_blk_flush(struct block_device *self) {
    (void)self;
    return BLOCK_OK;
}

static bool port_init(ahci_port_t *ap, volatile hba_port_t *regs, int idx, int port_no)
{
    LOG_DEBUG("port %d (disk %d): SSTS=0x%08x TFD=0x%08x CMD=0x%08x SERR=0x%08x",
              port_no, idx, regs->ssts, regs->tfd, regs->cmd, regs->serr);

    ap->regs = regs;
    ap->present = false;

    port_stop(regs);

    uint32_t cmd = regs->cmd;
    if (!(cmd & HBA_CMD_SUD) || !(cmd & HBA_CMD_POD)) {
        regs->cmd = cmd | HBA_CMD_SUD | HBA_CMD_POD;
        ahci_io_mb();
        LOG_DEBUG("port %d: SUD/POD set, CMD=0x%08x", port_no, regs->cmd);
        if (delay_ms) delay_ms(20);
    }

    regs->sctl = (regs->sctl & ~HBA_SCTL_IPM_MASK) | HBA_SCTL_IPM_NO_LOWPOWER;
    ahci_io_mb();

    uint32_t ssts = regs->ssts;
    if ((ssts & 0xF) != HBA_SSTS_DET_PRESENT) {
        regs->sctl = (regs->sctl & ~HBA_SCTL_DET_MASK) | HBA_SCTL_DET_COMRESET;
        ahci_io_mb();
        if (delay_ms) delay_ms(2);
        regs->sctl = regs->sctl & ~HBA_SCTL_DET_MASK;
        ahci_io_mb();
    }

    ssts = regs->ssts;
    if ((ssts & 0xF) != HBA_SSTS_DET_PRESENT || ((ssts >> 8) & 0xF) != 0x1) {
        int waited = 0;
        while (waited < AHCI_LINK_TIMEOUT_MS) {
            uint32_t det = ssts & 0xFu;
            uint32_t ipm = (ssts >> 8) & 0xFu;
            if (det == HBA_SSTS_DET_PRESENT && ipm == 0x1u) break;
            if (waited >= AHCI_LINK_SETTLE_MS && (det == 0u || det == 4u)) break;
            if (delay_ms) delay_ms(1);
            waited++;
            ssts = regs->ssts;
        }
        LOG_DEBUG("port %d: after link wait SSTS=0x%08x", port_no, ssts);
    }

    if ((ssts & 0xF) != HBA_SSTS_DET_PRESENT) {
        LOG_DEBUG("port %d: DET=%u, no device", port_no, ssts & 0xF);
        return false;
    }
    if (((ssts >> 8) & 0xF) != 0x1) {
        LOG_WARNING("port %d: device present but IPM=%u (not active), skipping", port_no, (ssts >> 8) & 0xF);
        return false;
    }

    regs->serr = (uint32_t)~0;
    ahci_io_mb();

    LOG_DEBUG("port %d: link up, SIG=0x%08x TFD=0x%08x", port_no, regs->sig, regs->tfd);


    port_stop(regs);
    LOG_DEBUG("port %d: stopped, CMD=0x%08x", port_no, regs->cmd);

    ap->cmd_list = (hba_cmd_header_t *)ahci_alloc_aligned(1024, 1024);
    if (!ap->cmd_list) { LOG_ERROR("port %d: command list allocation failed", port_no); return false; }

    ap->fis_buf = (uint8_t *)ahci_alloc_aligned(256, 256);
    if (!ap->fis_buf) { LOG_ERROR("port %d: FIS buffer allocation failed", port_no); return false; }

    for (int s = 0; s < AHCI_CMD_SLOTS; s++) {
        ap->cmd_tables[s] = (hba_cmd_table_t *)ahci_alloc_aligned(
                                sizeof(hba_cmd_table_t), 128);
        if (!ap->cmd_tables[s]) {
            LOG_ERROR("port %d: command table allocation failed for slot %d", port_no, s);
            return false;
        }
    }

    uint64_t cl_phys = mm_ptr_to_phys(ap->cmd_list);
    uint64_t fis_phys = mm_ptr_to_phys(ap->fis_buf);
    LOG_DEBUG("port %d: CLB=0x%llx FB=0x%llx", port_no, (unsigned long long)cl_phys, (unsigned long long)fis_phys);

    regs->clb = (uint32_t)(cl_phys & 0xFFFFFFFF);
    regs->clbu = (uint32_t)(cl_phys >> 32);
    regs->fb = (uint32_t)(fis_phys & 0xFFFFFFFF);
    regs->fbu = (uint32_t)(fis_phys >> 32);
    regs->is = (uint32_t)~0;
    regs->serr = (uint32_t)~0;
    regs->ie = 0;

        port_start(regs);

    LOG_DEBUG("port %d: started, CMD=0x%08x TFD=0x%08x", port_no, regs->cmd, regs->tfd);

    int t = 5000;
    int waited = 0;

    while ((regs->tfd & 0x89) && t-- > 0) { delay_ms(1); waited++; }
    if (waited > 0) {
        LOG_DEBUG("port %d: waited %d ms for BSY/DRQ/ERR to clear", port_no, waited);
    }
    if (regs->tfd & 0x89) {
        LOG_ERROR("port %d: BSY/DRQ/ERR stuck, TFD=0x%08x", port_no, regs->tfd);
        return false;
    }

    if (regs->sig == 0xEB140101u || regs->sig == 0xC33C0101u || regs->sig == 0x96690101u) {
        LOG_INFO("port %d: %s device (SIG=0x%08x) is not a SATA disk, skipping", port_no,
                 regs->sig == 0xEB140101u ? "ATAPI" : (regs->sig == 0x96690101u ? "port multiplier" : "SEMB"),
                 regs->sig);
        port_stop(regs);
        return false;
    }

    if (!port_identify(ap)) {
        LOG_WARNING("port %d: IDENTIFY failed, skipping", port_no);
        return false;
    }

    ap->present = true;

    struct block_device *bd = &ap->blkdev;
    bd->name[0] = 's'; bd->name[1] = 'd';
    bd->name[2] = (char)('a' + idx); bd->name[3] = '\0';
    bd->sector_count = ap->sector_count;
    bd->sector_size = ap->sector_size;
    bd->priv = ap;
    bd->read_sectors = ahci_blk_read;
    bd->write_sectors = ahci_blk_write;
    bd->flush = ahci_blk_flush;

    LOG_INFO("port %d -> %s: %llu sectors x %u bytes (%llu MB)", port_no, bd->name,
             (unsigned long long)ap->sector_count, (unsigned)ap->sector_size,
             (unsigned long long)((ap->sector_count * ap->sector_size) >> 20));

    block_device_register(bd);
    return true;
}

static struct ahci_driver drv_ahci;

static int ahci_read(int disk, uint64_t lba, uint32_t count, void *buf)
{
    if (disk < 0 || disk >= drv_ahci.disk_count)
        return BLOCK_ERR_PARAM;

    return port_issue_cmd(&drv_ahci.ports[disk],
                          false, lba, count, buf);
}

static int ahci_write(int disk, uint64_t lba, uint32_t count, void *buf)
{
    if (disk < 0 || disk >= drv_ahci.disk_count)
        return BLOCK_ERR_PARAM;

    return port_issue_cmd(&drv_ahci.ports[disk],
                          true, lba, count, buf);
}

static int ahci_get_disk_count(void)
{
    return drv_ahci.disk_count;
}

static struct ahci_driver drv_ahci = {
    .disk_count = 0,
    .read = ahci_read,
    .write = ahci_write,
    .get_disk_count = ahci_get_disk_count,
};

struct ahci_driver *return_ahci_driver(void)
{
    
    pci_init();

    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc) { LOG_ERROR("no TSC driver, AHCI disabled"); return NULL; }

    delay_ms = tsc->sleep_tsc_ms;

    int disk_idx = 0;
    int pci_count = pci_get_device_count();
    if (pci_count > MAX_PCI_DEVICES) pci_count = MAX_PCI_DEVICES;

    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev =
            (struct pci_device *)device_table[PCI_DEVICE][i];
        if (!dev) continue;

        if (dev->class_code != 0x01) continue;

        LOG_DEBUG("storage device %04x:%04x class 01/%02x/%02x at %02x:%02x.%x",
                  (unsigned)dev->vendor_id, (unsigned)dev->device_id, (unsigned)dev->subclass,
                  (unsigned)dev->prog_if, (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func);

        bool candidate = false;
        if (dev->subclass == 0x06 && dev->prog_if <= 0x02) candidate = true;
        else if (dev->subclass == 0x04) candidate = true;

        if (!candidate) {
            LOG_DEBUG("%02x:%02x.%x is not an AHCI-style controller, skipping",
                      (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func);
            continue;
        }

        LOG_INFO("found HBA %04x:%04x at %02x:%02x.%x", (unsigned)dev->vendor_id, (unsigned)dev->device_id,
                 (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func);

        pci_enable_bus_mastering(dev);

        bool abar_is_io = false;
        uint64_t bar5_phys = pci_bar_phys(dev, 5, &abar_is_io);
        LOG_DEBUG("ABAR phys=0x%llx", (unsigned long long)bar5_phys);

        if (!bar5_phys || abar_is_io) {
            LOG_ERROR("ABAR (BAR5) empty or in I/O space - not AHCI, skipping");
            continue;
        }

        volatile hba_mem_t *hba = (volatile hba_mem_t *)vmm_map_mmio(bar5_phys, 8 * 1024, 0);
        if (!hba) {
            LOG_ERROR("failed to map ABAR MMIO window, skipping");
            continue;
        }

        hba->ghc |= HBA_GHC_AE;
        ahci_io_mb();

        uint32_t cap_probe = hba->cap;
        uint32_t pi_probe = hba->pi;

        LOG_DEBUG("HBA virt=0x%llx GHC=0x%08x PI=0x%08x CAP=0x%08x",
                  (unsigned long long)(uint64_t)hba, hba->ghc, pi_probe, cap_probe);

        if (cap_probe == 0xFFFFFFFFu || pi_probe == 0xFFFFFFFFu) {
            LOG_ERROR("ABAR reads back all-ones (no memory decode) - skipping");
            continue;
        }
        if (pi_probe == 0) {
            LOG_ERROR("PI=0, controller implements no ports - skipping");
            continue;
        }

        if (cap_probe & HBA_CAP_SSS) {
            LOG_DEBUG("CAP.SSS=1: staggered spin-up required, ports will be spun up explicitly");
        }

        LOG_DEBUG("HBA reset");
        hba->ghc |= HBA_GHC_HR;
        {
            int t = 1000;
            while ((hba->ghc & HBA_GHC_HR) && t-- > 0) delay_ms(1);
            if (hba->ghc & HBA_GHC_HR) {
                LOG_ERROR("GHC.HR never cleared, HBA broken?");
            } else {
                LOG_DEBUG("HBA reset done, GHC=0x%08x", hba->ghc);
            }
        }

        hba->ghc |= HBA_GHC_AE;
        ahci_io_mb();
        hba->is = (uint32_t)~0;
        ahci_io_mb();
        LOG_DEBUG("AHCI enabled, GHC=0x%08x", hba->ghc);

        uint32_t pi = hba->pi;
        LOG_INFO("HBA AHCI %u.%u, %u ports implemented (PI=0x%08x), %u command slots",
                 (hba->vs >> 16) & 0xFFFF, (hba->vs >> 8) & 0xFF, (unsigned)__builtin_popcount(pi), pi,
                 ((cap_probe >> 8) & 0x1F) + 1);

        for (int p = 0; p < AHCI_MAX_PORTS && disk_idx < AHCI_MAX_DISKS; p++) {
            if (!(pi & (1u << p))) continue;
            LOG_DEBUG("probing port %d", p);
            volatile hba_port_t *port_regs = &hba->ports[p];
            if (port_init(&drv_ahci.ports[disk_idx], port_regs, disk_idx, p))
                disk_idx++;
        }
    }

    LOG_INFO("AHCI init done, %d disk(s)", disk_idx);

    drv_ahci.disk_count = disk_idx;
    return disk_idx > 0 ? &drv_ahci : NULL;
}

struct driver *return_meta_ahci_driver(void)
{
    static struct dependency dep[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "AHCI Storage Driver",
        .type = STORAGE_DRIVER,
        .sub_type = AHCI_STORAGE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &dep[0] },
        .dependency_count = 1,
        .self = &drv_ahci,
        .init = (void *)return_ahci_driver,
    };
    return &meta;
}
