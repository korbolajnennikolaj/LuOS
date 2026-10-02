#include "usb_msc_driver.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "drivers/Storage/block_device.h"
#include "drivers/Storage/partition.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern int xhci_get_last_error_code(void);
extern unsigned int xhci_get_last_usbsts(void);
extern unsigned int xhci_read_current_usbsts(void);

static usb_msc_device_t msc_devs[USB_MSC_MAX_DEVICES];
static int msc_dev_count = 0;
static spinlock_t msc_lock = SPINLOCK_INIT;
static void *volatile msc_lock_owner = NULL;
static volatile uint32_t msc_lock_depth = 0;

#define MSC_LOCK_SPINS 4096u

static void *msc_self(void)
{
    void *me = (void *)current_task();
    return me ? me : (void *)&msc_lock;
}

static void msc_bot_enter(void)
{
    void *me = msc_self();
    unsigned spins = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&msc_lock);
        if (msc_lock_depth == 0) {
            msc_lock_owner = me;
            msc_lock_depth = 1;
            spin_unlock_irqrestore(&msc_lock, flags);
            return;
        }
        if (msc_lock_owner == me) {
            msc_lock_depth++;
            spin_unlock_irqrestore(&msc_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&msc_lock, flags);

        ++spins;
        if (spins < MSC_LOCK_SPINS || !current_task()) asm volatile("pause");
        else if (spins < MSC_LOCK_SPINS + 16u) scheduler_yield();
        else scheduler_sleep_ms(1);
    }
}

static void msc_bot_leave(void)
{
    uint64_t flags = spin_lock_irqsave(&msc_lock);
    if (msc_lock_depth > 0) msc_lock_depth--;
    if (msc_lock_depth == 0) msc_lock_owner = NULL;
    spin_unlock_irqrestore(&msc_lock, flags);
}

static void (*delay_ms)(uint64_t) = NULL;
static void (*delay_us)(uint64_t) = NULL;
static uint64_t (*uptime_ms)(void) = NULL;

#define MSC_BULK_HOT_POLLS 400u
#define MSC_BULK_WARM_MS 20u
#define MSC_CBW_TIMEOUT_MS 2000u
#define MSC_DATA_TIMEOUT_MS 5000u
#define MSC_CSW_TIMEOUT_MS 5000u
#define MSC_INIT_MAX_TIMEOUTS 3u

static void msc_bulk_backoff(unsigned poll, uint64_t elapsed_ms)
{
    if (poll < MSC_BULK_HOT_POLLS) {
        asm volatile("pause");
        return;
    }
    if (elapsed_ms < MSC_BULK_WARM_MS) {
        if (delay_us) delay_us(50);
        else if (delay_ms) delay_ms(1);
        return;
    }
    if (current_task()) scheduler_sleep_ms(1);
    else if (delay_ms) delay_ms(1);
}


static usb_cbw_t g_cbw __attribute__((aligned(64)));
static usb_csw_t g_csw __attribute__((aligned(64)));

static uint8_t g_scsi_buf[96] __attribute__((aligned(64)));

#define MSC_DMA_CHUNK 32768u
#define MSC_XFER_CHUNK_DEFAULT 4096u
#define MSC_XFER_CHUNK_XHCI 32768u
static uint8_t g_dma_buf[MSC_DMA_CHUNK] __attribute__((aligned(65536)));

static int g_msc_verbose = 0;

#define MSC_TRACE(...) do { if (g_msc_verbose) LOG_DEBUG(__VA_ARGS__); } while (0)
#define MSC_HEXDUMP(prefix, buf, len) do { if (g_msc_verbose) msc_hexdump(__func__, (prefix), (buf), (len)); } while (0)

int usb_msc_set_verbose(int on)
{
    int prev = g_msc_verbose;
    g_msc_verbose = on ? 1 : 0;
    LOG_INFO("per-command tracing %s", g_msc_verbose ? "enabled" : "disabled");
    return prev;
}

int usb_msc_get_verbose(void)
{
    return g_msc_verbose;
}

static void msc_hexdump(const char *caller, const char *prefix, const uint8_t *buf, uint16_t len)
{
    static const char h[] = "0123456789abcdef";
    char line[3 * 64 + 1];
    size_t pos = 0;

    for (uint16_t i = 0; i < len && pos + 3 < sizeof(line); i++) {
        if (i > 0) line[pos++] = ' ';
        line[pos++] = h[(buf[i] >> 4) & 0xF];
        line[pos++] = h[buf[i] & 0xF];
    }
    line[pos] = '\0';

    logger_printf(LOGGER_LEVEL_DEBUG, caller, "%s[%s]", prefix, line);
}

static inline uint32_t be32(uint32_t v)
{
    return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
    (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF);
}

static struct usb_core_driver *get_usb_core(void)
{
    return (struct usb_core_driver *)get_self_driver(USB_DRIVER, USB_CORE_SLOT);
}

static void msc_check_fatal_usbsts(void)
{
    uint32_t sts = xhci_get_last_usbsts();
    if (sts & ((1u << 0) | (1u << 2) | (1u << 14))) {
        LOG_ERROR("controller halted (%s%s%s), USBSTS=0x%08x - fatal, needs full xHC reset, not endpoint recovery",
                  (sts & (1u << 2)) ? "HSE " : "",
                  (sts & (1u << 14)) ? "HCE " : "",
                  (sts & (1u << 0)) ? "HCH" : "",
                  sts);
    }
}

static int msc_control(usb_msc_device_t *d, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data);

#define MSC_XFER_OK 0
#define MSC_XFER_STALL 1
#define MSC_XFER_TIMEOUT 2
#define MSC_XFER_ERROR 3

static int msc_bulk(usb_msc_device_t *d, uint8_t endpoint, void *buf, uint16_t len, uint8_t direction,
                    uint32_t timeout_ms)
{
    struct usb_core_driver *core = get_usb_core();
    if (!core->bulk_transfer) {
        LOG_ERROR("usb core has no bulk_transfer");
        return MSC_XFER_ERROR;
    }

    int ret = -2;
    int attempts = 0;
    uint64_t t0 = uptime_ms ? uptime_ms() : 0;

    for (unsigned poll = 0; ; poll++) {
        ret = core->bulk_transfer(d->usb_dev, endpoint, buf, len, direction);
        attempts++;
        if (ret != -2) break;

        uint64_t elapsed = uptime_ms ? (uptime_ms() - t0) : (uint64_t)poll;
        if (elapsed >= timeout_ms) break;
        if (!uptime_ms && poll >= 2000u) break;

        msc_bulk_backoff(poll, elapsed);

        if (poll >= MSC_BULK_HOT_POLLS && (poll & 63u) == 63u) {
            if (core->poll_transfers) core->poll_transfers();
        }
    }

    if (ret == 0) {
        d->consecutive_timeouts = 0;
        return MSC_XFER_OK;
    }

    if (ret == -2) {
        LOG_ERROR("bulk timeout after %d polls (%u ms), ep=0x%02x, len=%u",
                  attempts, (unsigned)timeout_ms, (unsigned)endpoint, (unsigned)len);
        if (d->consecutive_timeouts < 255) d->consecutive_timeouts++;

        if (core->reset_endpoint_toggle)
            core->reset_endpoint_toggle(d->usb_dev, endpoint);
        return MSC_XFER_TIMEOUT;
    }

    if (ret == -1) {
        LOG_WARNING("bulk stall after %d polls, ep=0x%02x, cc=%d",
                    attempts, (unsigned)endpoint, xhci_get_last_error_code());
        msc_check_fatal_usbsts();
        return MSC_XFER_STALL;
    }

    LOG_ERROR("bulk error ret=%d ep=0x%02x", ret, (unsigned)endpoint);
    return MSC_XFER_ERROR;
}

static int msc_control(usb_msc_device_t *d, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data)
{
    struct usb_core_driver *core = get_usb_core();
    if (!core->control_transfer) {
        LOG_ERROR("usb core has no control_transfer");
        return MSC_ERR_IO;
    }

    MSC_TRACE("ctrl type=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%u",
              (unsigned)type, (unsigned)req, (unsigned)val, (unsigned)idx, (unsigned)len);

    int ret = core->control_transfer(d->usb_dev, type, req, val, idx, len, data);

    if (ret != 0) {
        LOG_WARNING("ctrl type=0x%02x req=0x%02x failed ret=%d xhci_cc=%d USBSTS=0x%08x",
                    (unsigned)type, (unsigned)req, ret, xhci_get_last_error_code(), xhci_get_last_usbsts());
        msc_check_fatal_usbsts();
    } else {
        MSC_TRACE("ctrl OK");
    }
    return ret;
}

static void msc_bot_reset(usb_msc_device_t *d)
{
    LOG_WARNING("BOT reset on addr %u (ep_in=0x%02x ep_out=0x%02x)",
                (unsigned)(d->usb_dev ? d->usb_dev->address : 0),
                (unsigned)d->ep_bulk_in, (unsigned)d->ep_bulk_out);

    int r1 = msc_control(d, 0x21, USB_MSC_REQ_RESET, 0, 0, 0, NULL);
    LOG_DEBUG("step 1/4 Mass Storage Reset result=%d", r1);

    int r2 = msc_control(d, 0x02, 0x01 , 0x0000,
                         d->ep_bulk_in, 0, NULL);
    LOG_DEBUG("step 2/4 Clear STALL Bulk-IN ep=0x%02x result=%d", (unsigned)d->ep_bulk_in, r2);

    if (delay_ms) delay_ms(10);

    int r3 = msc_control(d, 0x02, 0x01 , 0x0000,
                         d->ep_bulk_out, 0, NULL);
    LOG_DEBUG("step 3/4 Clear STALL Bulk-OUT ep=0x%02x result=%d", (unsigned)d->ep_bulk_out, r3);

    if (r1 != 0 && r2 != 0 && r3 != 0) {
        LOG_ERROR("device addr %u rejects all control requests, giving up on it",
                  (unsigned)(d->usb_dev ? d->usb_dev->address : 0));
        d->consecutive_timeouts = MSC_INIT_MAX_TIMEOUTS;
    }

    {
        struct usb_core_driver *_core = get_usb_core();
        if (_core->reset_endpoint_toggle) {
            _core->reset_endpoint_toggle(d->usb_dev, d->ep_bulk_out);
            _core->reset_endpoint_toggle(d->usb_dev, d->ep_bulk_in);
            LOG_DEBUG("step 4/4 data toggles reset to DATA0");
        } else {
            LOG_WARNING("step 4/4 reset_endpoint_toggle not available");
        }
    }

    if (delay_ms) delay_ms(50);
    LOG_DEBUG("BOT reset done");
}

static void msc_clear_halt(usb_msc_device_t *d, uint8_t ep)
{
    LOG_WARNING("clear halt ep=0x%02x", (unsigned)ep);

    msc_control(d, 0x02, 0x01, 0x0000, ep, 0, NULL);

    struct usb_core_driver *core = get_usb_core();
    if (core->reset_endpoint_toggle)
        core->reset_endpoint_toggle(d->usb_dev, ep);
}

static int msc_execute(usb_msc_device_t *d, const uint8_t *cmd, uint8_t cmd_len, void *data, uint32_t data_len, uint8_t direction)
{
    if (!d || !d->usb_dev || !cmd || cmd_len == 0 || cmd_len > 16)
        return MSC_ERR_PARAM;
    if (data_len > 0xFFFFu) return MSC_ERR_PARAM;

    if (!d->present && d->consecutive_timeouts >= MSC_INIT_MAX_TIMEOUTS) {
        MSC_TRACE("cmd 0x%02x skipped, device stopped responding", (unsigned)cmd[0]);
        return MSC_ERR_IO;
    }

    msc_bot_enter();

    MSC_TRACE("CDB len=%u cmd=0x%02x dir=%s data_len=%u",
              (unsigned)cmd_len, (unsigned)cmd[0], (direction == CBW_FLAGS_IN) ? "IN" : "OUT", data_len);

    uint32_t tag = ++d->cbw_tag;

    g_cbw.dCBWSignature = CBW_SIGNATURE;
    g_cbw.dCBWTag = tag;
    g_cbw.dCBWDataTransferLength = data_len;
    g_cbw.bmCBWFlags = direction;
    g_cbw.bCBWLUN = d->lun;
    g_cbw.bCBWCBLength = cmd_len;
    for (int i = 0; i < 16; i++)
        g_cbw.CBWCB[i] = (i < cmd_len) ? cmd[i] : 0;

    MSC_HEXDUMP("CBW bytes: ", (const uint8_t *)&g_cbw, sizeof(g_cbw));

    int xr = msc_bulk(d, d->ep_bulk_out, &g_cbw, sizeof(g_cbw), 0, MSC_CBW_TIMEOUT_MS);
    if (xr != MSC_XFER_OK) {

        if (xr == MSC_XFER_STALL) msc_clear_halt(d, d->ep_bulk_out);

        LOG_ERROR("CBW for cmd 0x%02x not accepted, BOT reset", (unsigned)cmd[0]);
        msc_bot_reset(d);
        msc_bot_leave();
        return MSC_ERR_IO;
    }

    if (data && data_len > 0) {
        uint8_t ep = (direction == CBW_FLAGS_IN) ? d->ep_bulk_in : d->ep_bulk_out;
        uint8_t dir = (direction == CBW_FLAGS_IN) ? 1 : 0;

        xr = msc_bulk(d, ep, data, (uint16_t)data_len, dir, MSC_DATA_TIMEOUT_MS);

        if (xr == MSC_XFER_STALL) {

            msc_clear_halt(d, ep);
        } else if (xr != MSC_XFER_OK) {

            LOG_ERROR("data phase of cmd 0x%02x unrecoverable, BOT reset", (unsigned)cmd[0]);
            msc_bot_reset(d);
            msc_bot_leave();
            return MSC_ERR_IO;
        }
    }

    int cr = MSC_XFER_ERROR;
    for (int csw_try = 0; csw_try < 2; csw_try++) {
        for (int i = 0; i < (int)sizeof(g_csw); i++)
            ((uint8_t *)&g_csw)[i] = 0;

        cr = msc_bulk(d, d->ep_bulk_in, &g_csw, sizeof(g_csw), 1, MSC_CSW_TIMEOUT_MS);
        if (cr == MSC_XFER_OK) break;

        if (cr == MSC_XFER_STALL && csw_try == 0) {
            msc_clear_halt(d, d->ep_bulk_in);
            continue;
        }
        break;
    }

    if (cr != MSC_XFER_OK) {
        LOG_ERROR("CSW receive for cmd 0x%02x failed", (unsigned)cmd[0]);
        msc_bot_reset(d);
        msc_bot_leave();
        return MSC_ERR_IO;
    }

    MSC_HEXDUMP("CSW bytes: ", (const uint8_t *)&g_csw, sizeof(g_csw));

    if (g_csw.dCSWSignature != CSW_SIGNATURE || g_csw.dCSWTag != tag) {
        LOG_ERROR("CSW invalid: sig=0x%08x tag=0x%08x want=0x%08x",
                  g_csw.dCSWSignature, g_csw.dCSWTag, tag);
        msc_bot_reset(d);
        msc_bot_leave();
        return MSC_ERR_IO;
    }

    if (g_csw.bCSWStatus == CSW_STATUS_PHASE_ERROR) {
        LOG_ERROR("CSW phase error on cmd 0x%02x, BOT reset", (unsigned)cmd[0]);
        msc_bot_reset(d);
        msc_bot_leave();
        return MSC_ERR_IO;
    }

    if (g_csw.bCSWStatus != CSW_STATUS_GOOD) {
        LOG_WARNING("cmd 0x%02x CSW status=0x%02x residue=%u",
                    (unsigned)cmd[0], (unsigned)g_csw.bCSWStatus, g_csw.dCSWDataResidue);
        msc_bot_leave();
        return MSC_ERR_IO;
    }

    if (g_csw.dCSWDataResidue != 0) {
        MSC_TRACE("short transfer, residue=%u", g_csw.dCSWDataResidue);
    }

    msc_bot_leave();
    return MSC_OK;
}

static int msc_scsi_inquiry(usb_msc_device_t *d)
{
    msc_bot_enter();
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    LOG_DEBUG("INQUIRY alloc_len=36");
    for (int i = 0; i < 36; i++) g_scsi_buf[i] = 0;

    int ret = msc_execute(d, cdb, 6, g_scsi_buf, 36, CBW_FLAGS_IN);
    if (ret != 0) {
        LOG_ERROR("INQUIRY failed ret=%d", ret);
        msc_bot_leave();
        return ret;
    }

    MSC_HEXDUMP("INQUIRY raw: ", g_scsi_buf, 36);

    scsi_inquiry_data_t *inq = (scsi_inquiry_data_t *)g_scsi_buf;

    char vendor[9] = {0}, product[17] = {0}, revision[5] = {0};
    for (int i = 0; i < 8; i++) vendor[i] = inq->vendor_id[i];
    for (int i = 0; i < 16; i++) product[i] = inq->product_id[i];
    for (int i = 0; i < 4; i++) revision[i] = inq->product_rev[i];

    LOG_INFO("INQUIRY vendor=\"%s\" product=\"%s\" revision=\"%s\" removable=%s version=0x%02x",
             vendor, product, revision, (inq->removable & 0x80) ? "yes" : "no", (unsigned)inq->version);

    msc_bot_leave();

    return MSC_OK;
}

static int msc_scsi_test_unit_ready(usb_msc_device_t *d)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    int ret = msc_execute(d, cdb, 6, NULL, 0, CBW_FLAGS_OUT);
    if (ret == 0)
        LOG_DEBUG("TEST UNIT READY: device is ready");
    else
        LOG_DEBUG("TEST UNIT READY: device not ready ret=%d", ret);
    return ret;
}

static int msc_scsi_request_sense(usb_msc_device_t *d)
{
    msc_bot_enter();
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    for (int i = 0; i < 18; i++) g_scsi_buf[i] = 0;

    int ret = msc_execute(d, cdb, 6, g_scsi_buf, 18, CBW_FLAGS_IN);
    if (ret != 0) {
        LOG_ERROR("REQUEST SENSE failed ret=%d", ret);
        msc_bot_leave();
        return ret;
    }

    MSC_HEXDUMP("SENSE raw: ", g_scsi_buf, 18);

    scsi_sense_data_t *s = (scsi_sense_data_t *)g_scsi_buf;

    uint8_t resp_code = g_scsi_buf[0] & 0x7F;
    uint8_t sense_key = s->sense_key & 0x0F;

    const char *sk_names[] = {
        "NO_SENSE","RECOVERED","NOT_READY","MEDIUM_ERROR",
        "HARDWARE_ERROR","ILLEGAL_REQUEST","UNIT_ATTENTION",
        "DATA_PROTECT","BLANK_CHECK","VENDOR","COPY_ABORT",
        "ABORTED_COMMAND","?","VOLUME_OVERFLOW","MISCOMPARE","?"
    };

    logger_printf((sense_key == 0x00 || sense_key == 0x06) ? LOGGER_LEVEL_DEBUG : LOGGER_LEVEL_WARNING, __func__,
                  "SENSE response_code=0x%02x sense_key=0x%02x (%s) asc=0x%02x ascq=0x%02x",
                  (unsigned)resp_code, (unsigned)sense_key, sk_names[sense_key],
                  (unsigned)s->asc, (unsigned)s->ascq);

    if (s->asc == 0x04 && s->ascq == 0x01)
        LOG_DEBUG("SENSE: becoming ready (normal spinup)");
    else if (s->asc == 0x28 && s->ascq == 0x00)
        LOG_DEBUG("SENSE: medium changed (unit attention, expected)");
    else if (s->asc == 0x3A)
        LOG_ERROR("SENSE: medium not present");
    else if (s->asc == 0x20 && s->ascq == 0x00)
        LOG_ERROR("SENSE: invalid command opcode");
    else if (s->asc == 0x24 && s->ascq == 0x00)
        LOG_ERROR("SENSE: invalid field in CDB");
    else if (sense_key == 0x00)
        LOG_DEBUG("SENSE: no sense (all good)");

    msc_bot_leave();

    return MSC_OK;
}

static int msc_scsi_read_capacity(usb_msc_device_t *d)
{
    msc_bot_enter();
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0,0,0,0,0,0,0,0,0 };
    for (int i = 0; i < 8; i++) g_scsi_buf[i] = 0;

    int ret = msc_execute(d, cdb, 10, g_scsi_buf, 8, CBW_FLAGS_IN);
    if (ret != 0) {
        LOG_ERROR("READ CAPACITY failed ret=%d", ret);
        msc_bot_leave();
        return ret;
    }

    MSC_HEXDUMP("READ CAPACITY raw: ", g_scsi_buf, 8);

    scsi_read_capacity_t *cap = (scsi_read_capacity_t *)g_scsi_buf;

    uint32_t last_lba_be = cap->lba_last;
    uint32_t blk_size_be = cap->block_size;
    uint32_t last_lba = be32(last_lba_be);
    uint32_t blk_size = be32(blk_size_be);

    LOG_DEBUG("READ CAPACITY: last_lba=0x%08x blk_size=%u (raw 0x%08x 0x%08x)",
              last_lba, blk_size, last_lba_be, blk_size_be);

    if (blk_size == 0) {
        LOG_WARNING("block_size=0 from device, defaulting to 512");
        blk_size = 512;
    }
    if (blk_size != 512 && blk_size != 4096) {
        LOG_WARNING("unusual block_size=%u (expected 512 or 4096)", blk_size);
    }

    d->sector_count = (uint64_t)last_lba + 1;
    d->sector_size = blk_size;

    uint64_t size_mb = (d->sector_count * d->sector_size) >> 20;
    uint64_t size_gb = size_mb >> 10;

    LOG_DEBUG("capacity: sectors=%llu sector_size=%u (~%llu %s)",
              (unsigned long long)d->sector_count, d->sector_size,
              (unsigned long long)(size_gb > 0 ? size_gb : size_mb), size_gb > 0 ? "GB" : "MB");

    msc_bot_leave();

    return MSC_OK;
}

static int msc_blk_read(struct block_device *self, uint64_t lba, uint32_t count, void *buf)
{
    usb_msc_device_t *d = (usb_msc_device_t *)self->priv;
    if (!d || !d->present) return MSC_ERR_IO;
    if (count == 0) return MSC_OK;

    if (d->sector_size == 0) return MSC_ERR_IO;

    msc_bot_enter();

    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {

        uint32_t limit = d->max_xfer ? d->max_xfer : MSC_XFER_CHUNK_DEFAULT;
        if (limit > MSC_DMA_CHUNK) limit = MSC_DMA_CHUNK;
        uint32_t max_sectors = limit / d->sector_size;
        if (max_sectors == 0) max_sectors = 1;
        uint32_t n = count - done;
        if (n > max_sectors) n = max_sectors;

        uint64_t cur_lba = lba + done;
        uint32_t byte_len = n * d->sector_size;
        if (byte_len > MSC_DMA_CHUNK) {
            msc_bot_leave();
            return MSC_ERR_PARAM;
        }

        uint8_t cdb[10] = {
            SCSI_READ_10,
            0,
            (uint8_t)(cur_lba >> 24), (uint8_t)(cur_lba >> 16),
            (uint8_t)(cur_lba >> 8), (uint8_t)(cur_lba),
            0,
            (uint8_t)(n >> 8), (uint8_t)(n),
            0
        };

        int ret = msc_execute(d, cdb, 10, g_dma_buf, byte_len, CBW_FLAGS_IN);
        if (ret != 0) {
            LOG_ERROR("%s: READ(10) lba=%llu count=%u failed ret=%d",
                      self->name, (unsigned long long)cur_lba, n, ret);
            msc_scsi_request_sense(d);
            msc_bot_leave();
            return ret;
        }
        memcpy(dst, g_dma_buf, byte_len);

        dst += byte_len;
        done += n;
    }

    msc_bot_leave();
    return MSC_OK;
}

static int msc_blk_write(struct block_device *self, uint64_t lba, uint32_t count, void *buf)
{
    usb_msc_device_t *d = (usb_msc_device_t *)self->priv;
    if (!d || !d->present) return MSC_ERR_IO;
    if (count == 0) return MSC_OK;

    if (d->sector_size == 0) return MSC_ERR_IO;

    msc_bot_enter();

    uint8_t *src = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint32_t limit = d->max_xfer ? d->max_xfer : MSC_XFER_CHUNK_DEFAULT;
        if (limit > MSC_DMA_CHUNK) limit = MSC_DMA_CHUNK;
        uint32_t max_sectors = limit / d->sector_size;
        if (max_sectors == 0) max_sectors = 1;
        uint32_t n = count - done;
        if (n > max_sectors) n = max_sectors;

        uint64_t cur_lba = lba + done;
        uint32_t byte_len = n * d->sector_size;
        if (byte_len > MSC_DMA_CHUNK) {
            msc_bot_leave();
            return MSC_ERR_PARAM;
        }
        memcpy(g_dma_buf, src, byte_len);

        uint8_t cdb[10] = {
            SCSI_WRITE_10,
            0,
            (uint8_t)(cur_lba >> 24), (uint8_t)(cur_lba >> 16),
            (uint8_t)(cur_lba >> 8), (uint8_t)(cur_lba),
            0,
            (uint8_t)(n >> 8), (uint8_t)(n),
            0
        };

        int ret = msc_execute(d, cdb, 10, g_dma_buf, byte_len, CBW_FLAGS_OUT);
        if (ret != 0) {
            LOG_ERROR("%s: WRITE(10) lba=%llu count=%u failed ret=%d",
                      self->name, (unsigned long long)cur_lba, n, ret);
            msc_scsi_request_sense(d);
            msc_bot_leave();
            return ret;
        }

        src += byte_len;
        done += n;
    }

    msc_bot_leave();
    return MSC_OK;
}

static int msc_blk_flush(struct block_device *self)
{
    (void)self;
    return MSC_OK;
}

static int usb_msc_disk_read(int disk, uint64_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= msc_dev_count) return MSC_ERR_PARAM;
    if (!msc_devs[disk].present) return MSC_ERR_IO;
    return msc_blk_read(&msc_devs[disk].blkdev, lba, count, buf);
}

static int usb_msc_disk_write(int disk, uint64_t lba, uint32_t count, void *buf) {
    if (disk < 0 || disk >= msc_dev_count) return MSC_ERR_PARAM;
    if (!msc_devs[disk].present) return MSC_ERR_IO;
    return msc_blk_write(&msc_devs[disk].blkdev, lba, count, buf);
}

static int usb_msc_disk_get_count(void) {
    return msc_dev_count;
}

static struct usb_msc_driver drv_usb_msc = {
    .disk_count = 0,
    .read = usb_msc_disk_read,
    .write = usb_msc_disk_write,
    .get_disk_count = usb_msc_disk_get_count,
};

struct usb_msc_driver *return_usb_msc_driver(void) {
    drv_usb_msc.disk_count = msc_dev_count;
    return &drv_usb_msc;
}

static int find_free_msc_slot(void) {
    for (int i = 0; i < msc_dev_count; i++)
        if (!msc_devs[i].present) return i;
    if (msc_dev_count < USB_MSC_MAX_DEVICES) return msc_dev_count;
    return -1;
}

static void msc_init_device_locked(struct usb_device *dev);

static void msc_init_device(struct usb_device *dev)
{
    msc_bot_enter();
    msc_init_device_locked(dev);
    msc_bot_leave();
}

static void msc_init_device_locked(struct usb_device *dev)
{
    int slot = find_free_msc_slot();
    if (slot < 0) {
        LOG_ERROR("too many MSC devices (max=%d)", USB_MSC_MAX_DEVICES);
        return;
    }

    bool on_xhci = dev->ctrl && dev->ctrl->type == USB_TYPE_XHCI;
    if (on_xhci)
        LOG_INFO("initializing USB mass storage device addr=%u, USBSTS=0x%08x",
                 (unsigned)dev->address, xhci_read_current_usbsts());
    else
        LOG_INFO("initializing USB mass storage device addr=%u", (unsigned)dev->address);

    usb_msc_device_t *d = &msc_devs[slot];
    for (int i = 0; i < (int)sizeof(usb_msc_device_t); i++)
        ((uint8_t *)d)[i] = 0;

    d->usb_dev = dev;
    d->lun = 0;
    d->cbw_tag = 0;

    d->max_xfer = MSC_XFER_CHUNK_DEFAULT;
    if (on_xhci)
        d->max_xfer = MSC_XFER_CHUNK_XHCI;

    for (int i = 0; i < dev->bulk_ep_count; i++) {
        struct usb_endpoint_info *ep = &dev->bulk_ep[i];
        LOG_DEBUG("bulk ep[%d] addr=0x%02x mps=%u dir=%s", i, (unsigned)ep->address,
                  (unsigned)ep->max_packet_size, (ep->address & 0x80) ? "IN" : "OUT");

        if ((ep->address & 0x80) && !d->ep_bulk_in) {
            d->ep_bulk_in = ep->address;
            d->ep_in_mps = ep->max_packet_size ? ep->max_packet_size : 512;
        } else if (!(ep->address & 0x80) && !d->ep_bulk_out) {
            d->ep_bulk_out = ep->address;
            d->ep_out_mps = ep->max_packet_size ? ep->max_packet_size : 512;
        }
    }

    if (!d->ep_bulk_in || !d->ep_bulk_out) {
        LOG_ERROR("bulk endpoints not found: IN=%s (0x%02x) OUT=%s (0x%02x)",
                  d->ep_bulk_in ? "yes" : "no", (unsigned)d->ep_bulk_in,
                  d->ep_bulk_out ? "yes" : "no", (unsigned)d->ep_bulk_out);
        return;
    }

    LOG_DEBUG("ep_in=0x%02x mps_in=%u ep_out=0x%02x mps_out=%u max_xfer=%u",
              (unsigned)d->ep_bulk_in, (unsigned)d->ep_in_mps,
              (unsigned)d->ep_bulk_out, (unsigned)d->ep_out_mps, d->max_xfer);

    {
        static uint8_t lun_buf[1] __attribute__((aligned(64)));
        lun_buf[0] = 0xFF;
        if (on_xhci) LOG_DEBUG("GET MAX LUN, USBSTS=0x%08x", xhci_read_current_usbsts());
        else LOG_DEBUG("GET MAX LUN");
        int r = msc_control(d, 0xA1, USB_MSC_REQ_GET_MAX_LUN,
                            0, 0, 1, lun_buf);
        if (r == 0)
            LOG_DEBUG("max_lun=%u (using LUN 0)", (unsigned)lun_buf[0]);
        else
            LOG_DEBUG("GET MAX LUN returned %d, device may not support it (normal for single-LUN)", r);
    }

    if (delay_ms) delay_ms(50);

    if (delay_ms) delay_ms(20);
    {
        int inq_ok = 0;
        for (int inq_try = 0; inq_try < 5; inq_try++) {
            if (inq_try > 0) {

                LOG_WARNING("INQUIRY retry %d/5", inq_try + 1);
                if (delay_ms) delay_ms(100);
            }
            if (msc_scsi_inquiry(d) == 0) {
                inq_ok = 1;
                break;
            }
            LOG_WARNING("INQUIRY attempt %d failed", inq_try + 1);
            if (d->consecutive_timeouts >= MSC_INIT_MAX_TIMEOUTS) {
                LOG_ERROR("device addr %u stopped responding during INQUIRY, initialization aborted (replug it)",
                          (unsigned)dev->address);
                return;
            }
            if (delay_ms) delay_ms(50);
        }
        if (!inq_ok) {

            LOG_WARNING("INQUIRY failed after 5 attempts, continuing without INQUIRY data, BOT reset to recover");
            msc_bot_reset(d);
            if (delay_ms) delay_ms(200);
        }
    }

    {
        int ready = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            LOG_DEBUG("TEST UNIT READY attempt %d/10", attempt + 1);

            if (msc_scsi_test_unit_ready(d) == 0) {
                ready = 1;
                LOG_DEBUG("device ready on attempt %d", attempt + 1);
                break;
            }
            if (d->consecutive_timeouts >= MSC_INIT_MAX_TIMEOUTS) {
                LOG_ERROR("device addr %u stopped responding, initialization aborted (replug it)",
                          (unsigned)dev->address);
                return;
            }
            msc_scsi_request_sense(d);
            if (delay_ms) delay_ms(100);
        }
        if (!ready) {
            LOG_ERROR("device not ready after 10 attempts (subclass=0x%02x protocol=0x%02x)",
                      (unsigned)dev->device_subclass, (unsigned)dev->device_protocol);
            return;
        }
    }

    if (msc_scsi_read_capacity(d) != 0) {
        LOG_ERROR("READ CAPACITY failed, trying REQUEST SENSE for diagnostics");
        msc_scsi_request_sense(d);
        return;
    }

    if (d->sector_count == 0) {
        LOG_ERROR("sector_count=0 (device reported empty medium?)");
        return;
    }

    LOG_DEBUG("sanity: sector_count=%llu sector_size=%u ep_in_mps=%u ep_out_mps=%u cbw_tag=0x%08x",
              (unsigned long long)d->sector_count, d->sector_size,
              (unsigned)d->ep_in_mps, (unsigned)d->ep_out_mps, d->cbw_tag);

    if (d->sector_size % 512 != 0) {
        LOG_WARNING("sector_size %u is not a multiple of 512", d->sector_size);
    }

    dev->driver_managed = 1;

    d->present = true;

    struct block_device *bd = &d->blkdev;
    bd->name[0] = 'u'; bd->name[1] = 'd';
    bd->name[2] = (char)('a' + slot);
    bd->name[3] = '\0';
    bd->sector_count = d->sector_count;
    bd->sector_size = d->sector_size;
    bd->priv = d;
    bd->read_sectors = msc_blk_read;
    bd->write_sectors = msc_blk_write;
    bd->flush = msc_blk_flush;

    block_device_register(bd);

    partition_register_all(bd);

    if (slot == msc_dev_count) msc_dev_count++;
    drv_usb_msc.disk_count = msc_dev_count;

    LOG_INFO("registered %s: %llu sectors x %u bytes (%llu MB), %d USB disk(s) total",
             bd->name, (unsigned long long)bd->sector_count, bd->sector_size,
             (unsigned long long)((bd->sector_count * bd->sector_size) >> 20), msc_dev_count);
}

static void usb_msc_on_usb_event(const usb_event_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) return;

    if (evt->type == USB_EVENT_DEVICE_CONN) {
        struct usb_device *dev = (struct usb_device *)evt->device;
        if (!dev || dev->device_class != USB_CLASS_MSC) return;

        for (int j = 0; j < msc_dev_count; j++)
            if (msc_devs[j].present && msc_devs[j].usb_dev == dev) return;

        msc_init_device(dev);
        return;
    }

    if (evt->type == USB_EVENT_DEVICE_DISC) {
        for (int j = 0; j < msc_dev_count; j++) {
            if (msc_devs[j].present && msc_devs[j].usb_dev == evt->device) {
                msc_devs[j].present = false;
                msc_devs[j].usb_dev = NULL;

                partition_unregister_all(&msc_devs[j].blkdev);
                block_device_unregister(&msc_devs[j].blkdev);

                LOG_INFO("device removed, unregistered %s", msc_devs[j].blkdev.name);
            }
        }
    }
}

static void msc_scan_devices(void)
{
    extern int usb_get_device_count(void);
    extern struct usb_device *usb_get_device(int idx);

    int n = usb_get_device_count();
    LOG_DEBUG("scanning %d USB device(s) for mass storage", n);

    int msc_found = 0;
    for (int i = 0; i < n; i++) {
        struct usb_device *dev = usb_get_device(i);
        if (!dev) continue;

        bool is_msc = (dev->device_class == USB_CLASS_MSC);
        LOG_DEBUG("slot %d: addr %u class 0x%02x %s", i, (unsigned)dev->address,
                  (unsigned)dev->device_class, is_msc ? "mass storage" : "skip");

        if (!is_msc) continue;
        msc_found++;

        bool already = false;
        for (int j = 0; j < msc_dev_count; j++) {
            if (msc_devs[j].usb_dev == dev) { already = true; break; }
        }
        if (already) {
            LOG_DEBUG("slot %d already initialized, skipping", i);
            continue;
        }

        msc_init_device(dev);
    }

    LOG_DEBUG("scan done: found %d mass storage device(s)", msc_found);
}

static void *msc_driver_init(void)
{
    {
        struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
        if (tsc) {
            delay_ms = tsc->sleep_tsc_ms;
            delay_us = tsc->sleep_tsc_us;
            uptime_ms = tsc->get_tsc_uptime_ms;
        } else {
            LOG_WARNING("TSC driver not found, delays unavailable, polling may be unreliable");
        }
    }

    {
        struct usb_core_driver *core = get_usb_core();
        if (!core->bulk_transfer) LOG_ERROR("usb core bulk_transfer is NULL");
        if (!core->control_transfer) LOG_ERROR("usb core control_transfer is NULL");
        if (!core->reset_endpoint_toggle) LOG_WARNING("usb core reset_endpoint_toggle is NULL, toggle reset unavailable");
    }

    for (int i = 0; i < USB_MSC_MAX_DEVICES; i++)
        for (int j = 0; j < (int)sizeof(usb_msc_device_t); j++)
            ((uint8_t *)&msc_devs[i])[j] = 0;
    msc_dev_count = 0;

    bool cbw_aligned = ((uintptr_t)&g_cbw & 63) == 0;
    bool csw_aligned = ((uintptr_t)&g_csw & 63) == 0;
    bool scsi_aligned = ((uintptr_t)&g_scsi_buf & 63) == 0;
    LOG_DEBUG("DMA buffers: cbw=0x%llx csw=0x%llx scsi=0x%llx",
              (unsigned long long)(uintptr_t)&g_cbw, (unsigned long long)(uintptr_t)&g_csw,
              (unsigned long long)(uintptr_t)&g_scsi_buf);
    if (!cbw_aligned || !csw_aligned || !scsi_aligned)
        LOG_ERROR("DMA buffers misaligned: cbw=%s csw=%s scsi=%s",
                  cbw_aligned ? "OK" : "BAD", csw_aligned ? "OK" : "BAD", scsi_aligned ? "OK" : "BAD");

    msc_scan_devices();

    drv_usb_msc.disk_count = msc_dev_count;

    usb_event_register_handler(usb_msc_on_usb_event, NULL);

    LOG_INFO("USB mass storage driver ready, %d USB disk(s)", msc_dev_count);

    return (void *)1;
}

struct driver *return_meta_usb_msc_driver(void)
{
    static struct dependency deps[] = {
        MAKE_DEPENDENCY(USB_DRIVER, USB_CORE_SLOT),
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "USB MSC Driver",
        .type = STORAGE_DRIVER,
        .sub_type = USBMSC_STORAGE,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &deps[0], &deps[1] },
        .dependency_count = 2,
        .self = &drv_usb_msc,
        .init = (void *)msc_driver_init,
    };
    return &meta;
}
