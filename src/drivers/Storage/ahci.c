#include "ahci.h"

#include "ata.h"
#include "block_device.h"
#include "components/drivers.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/pci.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Video/limine_video_driver.h"

#include <stddef.h>
#include <string.h>

static void ahci_puts(const char *s, uint32_t color) {
    struct limine_video_driver *v = get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    v->printf(s, color);
}

static void ahci_hex32(uint32_t val, uint32_t color) {
    const char h[] = "0123456789ABCDEF";
    char buf[11] = "0x00000000";

    for (int i = 9; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    ahci_puts(buf, color);
}

static void ahci_hex64(uint64_t val, uint32_t color) {
    const char h[] = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    ahci_puts(buf, color);
}

static void ahci_dec(uint64_t v, uint32_t color) {
    char buf[21]; int i = 20; buf[20] = 0;
    if (v == 0) { ahci_puts("0", color); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    ahci_puts(buf + i, color);
}

#define AHCI_COL_INFO LIMINE_COLOR_LIGHT_CYAN
#define AHCI_COL_OK LIMINE_COLOR_LIGHT_GREEN
#define AHCI_COL_ERR LIMINE_COLOR_LIGHT_RED
#define AHCI_COL_DATA LIMINE_COLOR_YELLOW

#define ULOG(s) do { ahci_puts("[AHCI] " s "\n", AHCI_COL_INFO); } while(0)
#define UERR(s) do { ahci_puts("[AHCI] ERR " s "\n", AHCI_COL_ERR); } while(0)

static inline void ahci_io_mb(void)
{
    asm volatile("mfence" ::: "memory");
}

#define AHCI_TIMEOUT_MS 500
static void (*delay_ms)(uint64_t);

static void port_stop(volatile hba_port_t *port)
{
    port->cmd &= ~HBA_CMD_ST;
    int t = AHCI_TIMEOUT_MS;
    while ((port->cmd & HBA_CMD_CR) && t-- > 0) delay_ms(1);
    if (t <= 0) UERR("port_stop: CR timeout (DMA won't stop)");

    port->cmd &= ~HBA_CMD_FRE;
    t = AHCI_TIMEOUT_MS;
    while ((port->cmd & HBA_CMD_FR) && t-- > 0) delay_ms(1);
    if (t <= 0) UERR("port_stop: FR timeout (FIS recv won't stop)");
}

static void port_start(volatile hba_port_t *port)
{
    int t = AHCI_TIMEOUT_MS;
    while ((port->cmd & HBA_CMD_CR) && t-- > 0) delay_ms(1);
    if (t <= 0) UERR("port_start: CR still set before FRE/ST");

    port->cmd |= HBA_CMD_FRE;
    port->cmd |= HBA_CMD_ST;
}

static void *ahci_alloc_aligned(size_t size, size_t align) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { UERR("ahci_alloc_aligned: pmm_alloc_pages failed"); return NULL; }

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
    spin_lock(&ap->lock);

    volatile hba_port_t *port = ap->regs;

    int t = AHCI_TIMEOUT_MS;
    while ((port->tfd & 0x88) && t-- > 0) delay_ms(1);
    if (t <= 0) {
        ahci_puts("[AHCI] ERR cmd: BSY/DRQ timeout TFD=", AHCI_COL_INFO);
        ahci_hex32(port->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_TIMEOUT;
    }

    int slot = port_find_free_slot(port);
    if (slot < 0) {
        UERR("port_issue_cmd: no free command slot");
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

    ahci_puts("[AHCI] cmd ", AHCI_COL_INFO); ahci_puts(write ? "W" : "R", AHCI_COL_INFO);
    ahci_puts(" slot=", AHCI_COL_INFO); ahci_dec(slot, AHCI_COL_DATA);
    ahci_puts(" lba=", AHCI_COL_INFO); ahci_hex64(lba, AHCI_COL_DATA);
    ahci_puts(" n=", AHCI_COL_INFO); ahci_dec(sectors, AHCI_COL_DATA);
    ahci_puts("\n", AHCI_COL_INFO);

    t = AHCI_TIMEOUT_MS * 10;
    while (--t > 0) {
        delay_ms(1);
        if (!(port->ci & (1u << slot))) break;
        if (port->is & HBA_IS_TFES) {
            ahci_puts("[AHCI] ERR cmd TFES IS=", AHCI_COL_INFO); ahci_hex32(port->is, AHCI_COL_DATA);
            ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA);
            ahci_puts(" SERR=", AHCI_COL_INFO); ahci_hex32(port->serr, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
            spin_unlock(&ap->lock);
            return BLOCK_ERR_IO;
        }
    }
    if (t <= 0) {
        ahci_puts("[AHCI] ERR cmd timeout IS=", AHCI_COL_INFO); ahci_hex32(port->is, AHCI_COL_DATA);
        ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA);
        ahci_puts(" CI=", AHCI_COL_INFO); ahci_hex32(port->ci, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_TIMEOUT;
    }
    if (port->is & HBA_IS_TFES) {
        ahci_puts("[AHCI] ERR cmd TFES (late) IS=", AHCI_COL_INFO); ahci_hex32(port->is, AHCI_COL_DATA);
        ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
        spin_unlock(&ap->lock);
        return BLOCK_ERR_IO;
    }

    spin_unlock(&ap->lock);
    return BLOCK_OK;
}

static bool port_identify(ahci_port_t *ap)
{
    ULOG("port_identify: start");

    uint16_t *id = (uint16_t *)ahci_alloc_aligned(512, 512);
    if (!id) { UERR("port_identify: alloc failed"); return false; }

    volatile hba_port_t *port = ap->regs;

    int t = AHCI_TIMEOUT_MS * 2;
    int bsy_waited = 0;
    while ((port->tfd & 0x80) && t-- > 0) { delay_ms(1); bsy_waited++; }
    if (bsy_waited > 0) {
        ahci_puts("[AHCI] port_identify: waited BSY ", AHCI_COL_INFO); ahci_dec(bsy_waited, AHCI_COL_DATA); ahci_puts("ms\n", AHCI_COL_INFO);
    }
    if (port->tfd & 0x80) {
        ahci_puts("[AHCI] ERR port_identify: BSY stuck TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
        return false;
    }

    ahci_puts("[AHCI] port_identify: TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    int slot = port_find_free_slot(port);
    if (slot < 0) { UERR("port_identify: no free slot"); return false; }

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

    ahci_puts("[AHCI] port_identify: IDENTIFY issued slot=", AHCI_COL_INFO); ahci_dec(slot, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    t = AHCI_TIMEOUT_MS * 10;
    while (--t > 0 && (port->ci & (1u << slot))) delay_ms(1);

    if (t <= 0) {
        ahci_puts("[AHCI] ERR port_identify: timeout IS=", AHCI_COL_INFO); ahci_hex32(port->is, AHCI_COL_DATA);
        ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA);
        ahci_puts(" SERR=", AHCI_COL_INFO); ahci_hex32(port->serr, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
        return false;
    }
    if (port->is & HBA_IS_TFES) {
        ahci_puts("[AHCI] ERR port_identify: TFES IS=", AHCI_COL_INFO); ahci_hex32(port->is, AHCI_COL_DATA);
        ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(port->tfd, AHCI_COL_DATA);
        ahci_puts(" SERR=", AHCI_COL_INFO); ahci_hex32(port->serr, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
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

    ahci_puts("[AHCI] port_identify: OK sectors=", AHCI_COL_INFO); ahci_dec(ap->sector_count, AHCI_COL_DATA);
    ahci_puts(" sector_size=", AHCI_COL_INFO); ahci_dec(ap->sector_size, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

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
    ahci_puts("[AHCI] port_init port=", AHCI_COL_INFO); ahci_dec(port_no, AHCI_COL_DATA);
    ahci_puts(" idx=", AHCI_COL_INFO); ahci_dec(idx, AHCI_COL_DATA);
    ahci_puts(" SSTS=", AHCI_COL_INFO); ahci_hex32(regs->ssts, AHCI_COL_DATA);
    ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(regs->tfd, AHCI_COL_DATA);
    ahci_puts(" CMD=", AHCI_COL_INFO); ahci_hex32(regs->cmd, AHCI_COL_DATA);
    ahci_puts(" SERR=", AHCI_COL_INFO); ahci_hex32(regs->serr, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    ap->regs = regs;
    ap->present = false;

    port_stop(regs);

    uint32_t cmd = regs->cmd;
    if (!(cmd & HBA_CMD_SUD) || !(cmd & HBA_CMD_POD)) {
        regs->cmd = cmd | HBA_CMD_SUD | HBA_CMD_POD;
        ahci_io_mb();
        ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(port_no, AHCI_COL_DATA);
        ahci_puts(": SUD/POD set, CMD=", AHCI_COL_INFO); ahci_hex32(regs->cmd, AHCI_COL_DATA);
        ahci_puts("\n", AHCI_COL_INFO);
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
        int retries = 100;
        while (retries-- > 0) {
            if (delay_ms) delay_ms(50);
            ssts = regs->ssts;
            if ((ssts & 0xF) == HBA_SSTS_DET_PRESENT && ((ssts >> 8) & 0xF) == 0x1) break;
        }
        ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(port_no, AHCI_COL_DATA);
        ahci_puts(": after link wait SSTS=", AHCI_COL_INFO);
        ahci_hex32(ssts, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
    }

    if ((ssts & 0xF) != HBA_SSTS_DET_PRESENT) {
        ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(port_no, AHCI_COL_DATA);
        ahci_puts(": DET=", AHCI_COL_INFO); ahci_dec(ssts & 0xF, AHCI_COL_DATA);
        ahci_puts(" (no device) skip\n", AHCI_COL_INFO);
        return false;
    }
    if (((ssts >> 8) & 0xF) != 0x1) {
        ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(port_no, AHCI_COL_DATA);
        ahci_puts(": IPM=", AHCI_COL_INFO); ahci_dec((ssts >> 8) & 0xF, AHCI_COL_DATA); ahci_puts(" (not active) skip\n", AHCI_COL_INFO);
        return false;
    }

    regs->serr = (uint32_t)~0;
    ahci_io_mb();

    ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(port_no, AHCI_COL_DATA);
    ahci_puts(": link up, SIG=", AHCI_COL_INFO); ahci_hex32(regs->sig, AHCI_COL_DATA);
    ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(regs->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    port_stop(regs);
    ahci_puts("[AHCI] port_init: after stop CMD=", AHCI_COL_INFO); ahci_hex32(regs->cmd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    ap->cmd_list = (hba_cmd_header_t *)ahci_alloc_aligned(1024, 1024);
    if (!ap->cmd_list) { UERR("port_init: cmd_list alloc failed"); return false; }

    ap->fis_buf = (uint8_t *)ahci_alloc_aligned(256, 256);
    if (!ap->fis_buf) { UERR("port_init: fis_buf alloc failed"); return false; }

    for (int s = 0; s < AHCI_CMD_SLOTS; s++) {
        ap->cmd_tables[s] = (hba_cmd_table_t *)ahci_alloc_aligned(
                                sizeof(hba_cmd_table_t), 128);
        if (!ap->cmd_tables[s]) {
            ahci_puts("[AHCI] ERR port_init: cmd_table alloc failed slot=", AHCI_COL_INFO);
            ahci_dec(s, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
            return false;
        }
    }

    uint64_t cl_phys = mm_ptr_to_phys(ap->cmd_list);
    uint64_t fis_phys = mm_ptr_to_phys(ap->fis_buf);
    ahci_puts("[AHCI] port_init: CLB=", AHCI_COL_INFO); ahci_hex64(cl_phys, AHCI_COL_DATA);
    ahci_puts(" FB=", AHCI_COL_INFO); ahci_hex64(fis_phys, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    regs->clb = (uint32_t)(cl_phys & 0xFFFFFFFF);
    regs->clbu = (uint32_t)(cl_phys >> 32);
    regs->fb = (uint32_t)(fis_phys & 0xFFFFFFFF);
    regs->fbu = (uint32_t)(fis_phys >> 32);
    regs->is = (uint32_t)~0;
    regs->serr = (uint32_t)~0;
    regs->ie = HBA_IS_DHRS | (1u << 1) | (1u << 2) | (1u << 3)
               | HBA_IS_TFES | HBA_IS_HBFS | HBA_IS_IFS;

    ULOG("port_init: starting port");
    port_start(regs);

    ahci_puts("[AHCI] port_init: after start CMD=", AHCI_COL_INFO); ahci_hex32(regs->cmd, AHCI_COL_DATA);
    ahci_puts(" TFD=", AHCI_COL_INFO); ahci_hex32(regs->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

    int t = 5000;
    int waited = 0;

    while ((regs->tfd & 0x89) && t-- > 0) { delay_ms(1); waited++; }
    if (waited > 0) {
        ahci_puts("[AHCI] port_init: waited BSY/DRQ/ERR ", AHCI_COL_INFO); ahci_dec(waited, AHCI_COL_DATA); ahci_puts("ms\n", AHCI_COL_INFO);
    }
    if (regs->tfd & 0x89) {
        ahci_puts("[AHCI] ERR port_init: BSY/DRQ/ERR stuck TFD=", AHCI_COL_INFO); ahci_hex32(regs->tfd, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
        return false;
    }

    if (!port_identify(ap)) {
        ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(idx, AHCI_COL_DATA); ahci_puts(": identify FAILED\n", AHCI_COL_INFO);
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

    ahci_puts("[AHCI] port ", AHCI_COL_INFO); ahci_dec(idx, AHCI_COL_DATA);
    ahci_puts(": init OK -> sd", AHCI_COL_INFO); { char _dl[2] = { (char)('a' + idx), 0 }; ahci_puts(_dl, AHCI_COL_OK); }; ahci_puts("\n", AHCI_COL_INFO);

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
    ULOG("=== AHCI init start ===");

    pci_init();

    struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (!tsc) { UERR("no TSC driver"); return NULL; }

    delay_ms = tsc->sleep_tsc_ms;

    int disk_idx = 0;
    int pci_count = pci_get_device_count();
    if (pci_count > MAX_PCI_DEVICES) pci_count = MAX_PCI_DEVICES;

    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev =
            (struct pci_device *)device_table[PCI_DEVICE][i];
        if (!dev) continue;

        if (dev->class_code != 0x01) continue;

        ahci_puts("[AHCI] storage dev ", AHCI_COL_INFO);
        ahci_hex32(dev->vendor_id, AHCI_COL_DATA); ahci_puts(":", AHCI_COL_INFO);
        ahci_hex32(dev->device_id, AHCI_COL_DATA);
        ahci_puts(" class=01 sub=", AHCI_COL_INFO); ahci_hex32(dev->subclass, AHCI_COL_DATA);
        ahci_puts(" progif=", AHCI_COL_INFO); ahci_hex32(dev->prog_if, AHCI_COL_DATA);
        ahci_puts(" @ ", AHCI_COL_INFO); ahci_dec(dev->bus, AHCI_COL_DATA);
        ahci_puts(":", AHCI_COL_INFO); ahci_dec(dev->slot, AHCI_COL_DATA);
        ahci_puts(".", AHCI_COL_INFO); ahci_dec(dev->func, AHCI_COL_DATA);
        ahci_puts("\n", AHCI_COL_INFO);

        bool candidate = false;
        if (dev->subclass == 0x06 && dev->prog_if <= 0x02) candidate = true;
        else if (dev->subclass == 0x04) candidate = true;

        if (!candidate) {
            ULOG("  -> not an AHCI-style controller, skipping");
            continue;
        }

        ahci_puts("[AHCI] found HBA ", AHCI_COL_INFO);
        ahci_hex32(dev->vendor_id, AHCI_COL_DATA); ahci_puts(":", AHCI_COL_INFO); ahci_hex32(dev->device_id, AHCI_COL_DATA);
        ahci_puts(" @ bus=", AHCI_COL_INFO); ahci_dec(dev->bus, AHCI_COL_DATA);
        ahci_puts(" slot=", AHCI_COL_INFO); ahci_dec(dev->slot, AHCI_COL_DATA);
        ahci_puts(" func=", AHCI_COL_INFO); ahci_dec(dev->func, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

        pci_enable_bus_mastering(dev);

        bool abar_is_io = false;
        uint64_t bar5_phys = pci_bar_phys(dev, 5, &abar_is_io);
        ahci_puts("[AHCI] ABAR phys=", AHCI_COL_INFO); ahci_hex64(bar5_phys, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

        if (!bar5_phys || abar_is_io) {
            UERR("ABAR (BAR5) empty or in I/O space - not AHCI, skipping");
            continue;
        }

        volatile hba_mem_t *hba = (volatile hba_mem_t *)vmm_map_mmio(bar5_phys, 8 * 1024, 0);
        if (!hba) {
            UERR("failed to map ABAR MMIO window, skipping");
            continue;
        }

        hba->ghc |= HBA_GHC_AE;
        ahci_io_mb();

        uint32_t cap_probe = hba->cap;
        uint32_t pi_probe = hba->pi;

        ahci_puts("[AHCI] HBA virt=", AHCI_COL_INFO); ahci_hex64((uint64_t)hba, AHCI_COL_DATA);
        ahci_puts(" GHC=", AHCI_COL_INFO); ahci_hex32(hba->ghc, AHCI_COL_DATA);
        ahci_puts(" PI=", AHCI_COL_INFO); ahci_hex32(pi_probe, AHCI_COL_DATA);
        ahci_puts(" CAP=", AHCI_COL_INFO); ahci_hex32(cap_probe, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

        if (cap_probe == 0xFFFFFFFFu || pi_probe == 0xFFFFFFFFu) {
            UERR("ABAR reads back all-ones (no memory decode) - skipping");
            continue;
        }
        if (pi_probe == 0) {
            UERR("PI=0, controller implements no ports - skipping");
            continue;
        }

        if (cap_probe & HBA_CAP_SSS) {
            ULOG("CAP.SSS=1: staggered spin-up required, ports will be spun up explicitly");
        }

        ULOG("HBA reset...");
        hba->ghc |= HBA_GHC_HR;
        {
            int t = 1000;
            while ((hba->ghc & HBA_GHC_HR) && t-- > 0) delay_ms(1);
            if (hba->ghc & HBA_GHC_HR) {
                UERR("GHC.HR never cleared — HBA broken?");
            } else {
                ahci_puts("[AHCI] HBA reset done GHC=", AHCI_COL_INFO); ahci_hex32(hba->ghc, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
            }
        }

        hba->ghc |= HBA_GHC_AE;
        ahci_io_mb();
        hba->is = (uint32_t)~0;
        hba->ghc |= HBA_GHC_IE;
        ahci_io_mb();
        ahci_puts("[AHCI] GHC=", AHCI_COL_INFO); ahci_hex32(hba->ghc, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

        uint32_t pi = hba->pi;
        ahci_puts("[AHCI] PI=", AHCI_COL_INFO); ahci_hex32(pi, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);

        for (int p = 0; p < AHCI_MAX_PORTS && disk_idx < AHCI_MAX_DISKS; p++) {
            if (!(pi & (1u << p))) continue;
            ahci_puts("[AHCI] probing port ", AHCI_COL_INFO); ahci_dec(p, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
            volatile hba_port_t *port_regs = &hba->ports[p];
            if (port_init(&drv_ahci.ports[disk_idx], port_regs, disk_idx, p))
                disk_idx++;
        }
    }

    ahci_puts("[AHCI] init done, disks=", AHCI_COL_INFO); ahci_dec(disk_idx, AHCI_COL_DATA); ahci_puts("\n", AHCI_COL_INFO);
    ULOG("=== AHCI init end ===");

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
