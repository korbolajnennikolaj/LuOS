#include "usb_msc_driver.h"

#include "components/drivers.h"
#include "drivers/Storage/block_device.h"
#include "drivers/Storage/partition.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/USB/usb_controller.h"
#include "drivers/USB/usb_core.h"
#include "drivers/USB/usb_event.h"
#include "drivers/Video/limine_video_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern int xhci_get_last_error_code(void);
extern unsigned int xhci_get_last_usbsts(void);
extern unsigned int xhci_read_current_usbsts(void);

static usb_msc_device_t msc_devs[USB_MSC_MAX_DEVICES];
static int msc_dev_count = 0;

static void (*delay_ms)(uint64_t) = NULL;

static usb_cbw_t g_cbw __attribute__((aligned(64)));
static usb_csw_t g_csw __attribute__((aligned(64)));

static uint8_t g_scsi_buf[96] __attribute__((aligned(64)));

#define MSC_DMA_CHUNK 4096u
static uint8_t g_dma_buf[MSC_DMA_CHUNK] __attribute__((aligned(4096)));

#define MSC_COL_INFO 0x00AAFFAA
#define MSC_COL_ERR 0x00FF4444
#define MSC_COL_DATA 0x00FFFF00
#define MSC_COL_OK 0x0044FF44
#define MSC_COL_DBG 0x00AAAAFF
#define MSC_COL_TRACE 0x00888888
#define MSC_COL_WARN 0x00FFAA00
#define MSC_COL_SEP 0x00666666

#define MSC_COL_NOTE 0x0088DDFF

static int g_msc_verbose = 0;
static int g_msc_line_muted = 0;
static int g_msc_at_line_start = 1;

int usb_msc_set_verbose(int on)
{
    int prev = g_msc_verbose;
    g_msc_verbose = on ? 1 : 0;
    g_msc_line_muted = 0;
    g_msc_at_line_start = 1;
    return prev;
}

static int msc_color_is_quiet_worthy(uint32_t color)
{
    return color == MSC_COL_ERR || color == MSC_COL_WARN ||
           color == MSC_COL_NOTE;
}

static void msc_puts(const char *s, uint32_t color)
{
    if (!s) return;

    if (!g_msc_verbose) {
        if (g_msc_at_line_start)
            g_msc_line_muted = !msc_color_is_quiet_worthy(color);

        int ends_line = 0;
        for (const char *p = s; *p; p++) ends_line = (*p == '\n');
        int muted = g_msc_line_muted;
        g_msc_at_line_start = ends_line;

        if (muted) return;
    }

    struct limine_video_driver *v =
    (struct limine_video_driver *)get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    if (v && v->printf) v->printf(s, color);
}

static void msc_hex8(uint8_t v, uint32_t col)
{
    static const char h[] = "0123456789ABCDEF";
    char buf[5] = "0x00";
    buf[2] = h[(v >> 4) & 0xF];
    buf[3] = h[v & 0xF];
    buf[4] = '\0';
    msc_puts(buf, col);
}

static void msc_hex16(uint16_t v, uint32_t col)
{
    static const char h[] = "0123456789ABCDEF";
    char buf[7] = "0x0000";
    buf[2] = h[(v >> 12) & 0xF];
    buf[3] = h[(v >> 8) & 0xF];
    buf[4] = h[(v >> 4) & 0xF];
    buf[5] = h[v & 0xF];
    buf[6] = '\0';
    msc_puts(buf, col);
}

static void msc_hex32(uint32_t v, uint32_t col)
{
    static const char h[] = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[10] = '\0';
    msc_puts(buf, col);
}

static void msc_dec(uint64_t v, uint32_t col)
{
    char buf[21]; int i = 20; buf[20] = '\0';
    if (v == 0) { msc_puts("0", col); return; }
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    msc_puts(buf + i, col);
}

static void msc_hexdump(const char *prefix, const uint8_t *buf, uint16_t len, uint32_t col)
{
    msc_puts(prefix, col);
    msc_puts("[", MSC_COL_TRACE);
    for (uint16_t i = 0; i < len; i++) {
        if (i > 0) msc_puts(" ", MSC_COL_TRACE);
        msc_hex8(buf[i], col);
    }
    msc_puts("]\n", MSC_COL_TRACE);
}

static void msc_sep(const char *label)
{
    msc_puts("--- ", MSC_COL_SEP);
    msc_puts(label, MSC_COL_SEP);
    msc_puts(" ---\n", MSC_COL_SEP);
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
        msc_puts("[MSC]  !!! CONTROLLER HALTED (", MSC_COL_ERR);
        if (sts & (1u << 2)) msc_puts("HSE ", MSC_COL_ERR);
        if (sts & (1u << 14)) msc_puts("HCE ", MSC_COL_ERR);
        if (sts & (1u << 0)) msc_puts("HCH ", MSC_COL_ERR);
        msc_puts(") - fatal, needs full xHC reset, not endpoint recovery !!!\n", MSC_COL_ERR);
    }
}

static int msc_control(usb_msc_device_t *d, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data);

static void msc_qemu_yield(usb_msc_device_t *d)
{
    static uint8_t _desc_buf[18] __attribute__((aligned(64)));
    for (int _i = 0; _i < 18; _i++) _desc_buf[_i] = 0;

    msc_control(d, 0x80, 0x06 , 0x0100 , 0, 18, _desc_buf);

}

static int msc_bulk_submit(usb_msc_device_t *d, uint8_t endpoint, void *buf, uint16_t len, uint8_t direction)
{
    struct usb_core_driver *core = get_usb_core();
    if (!core->bulk_transfer) return MSC_ERR_IO;
    msc_puts("[MSC]  bulk ep=", MSC_COL_TRACE);
    msc_hex8(endpoint, MSC_COL_DATA);
    msc_puts(" dir=", MSC_COL_TRACE);
    msc_puts(direction ? "IN" : "OUT", MSC_COL_DATA);
    msc_puts(" len=", MSC_COL_TRACE);
    msc_dec(len, MSC_COL_DATA);
    msc_puts(" ...\n", MSC_COL_TRACE);
    return core->bulk_transfer(d->usb_dev, endpoint, buf, len, direction);
}

static int msc_bulk(usb_msc_device_t *d, uint8_t endpoint, void *buf, uint16_t len, uint8_t direction)
{
    struct usb_core_driver *core = get_usb_core();
    if (!core->bulk_transfer) {
        msc_puts("[MSC] msc_bulk: ERR bulk_transfer==NULL\n", MSC_COL_ERR);
        return MSC_ERR_IO;
    }

    msc_puts("[MSC]  bulk ep=", MSC_COL_TRACE);
    msc_hex8(endpoint, MSC_COL_DATA);
    msc_puts(" dir=", MSC_COL_TRACE);
    msc_puts(direction ? "IN" : "OUT", MSC_COL_DATA);
    msc_puts(" len=", MSC_COL_TRACE);
    msc_dec(len, MSC_COL_DATA);
    msc_puts(" ...\n", MSC_COL_TRACE);

    int ret;
    int attempts = 0;
    for (int ms = 0; ms < 500; ms++) {
        ret = core->bulk_transfer(d->usb_dev, endpoint, buf, len, direction);
        attempts++;
        if (ret != -2) break;
        if (delay_ms) delay_ms(1);

        if ((ms & 63) == 63) {
            struct usb_core_driver *_fcore = get_usb_core();
            if (_fcore->poll_transfers) _fcore->poll_transfers();
        }
    }

    if (ret == -2) {

        msc_puts("[MSC]  bulk TIMEOUT after ", MSC_COL_ERR);
        msc_dec(attempts, MSC_COL_DATA);
        msc_puts(" polls, ep=", MSC_COL_ERR);
        msc_hex8(endpoint, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);

        {
            struct usb_core_driver *_rc = get_usb_core();
            if (_rc->reset_endpoint_toggle)
                _rc->reset_endpoint_toggle(d->usb_dev, endpoint);
        }
        return MSC_ERR_TIMEOUT;
    }
    if (ret == MSC_ERR_TIMEOUT ) {

        msc_puts("[MSC]  bulk STALL/ERR after ", MSC_COL_ERR);
        msc_dec(attempts, MSC_COL_DATA);
        msc_puts(" polls, ep=", MSC_COL_ERR);
        msc_hex8(endpoint, MSC_COL_DATA);
        msc_puts(" xhci_cc=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)xhci_get_last_error_code(), MSC_COL_DATA);
        msc_puts(" USBSTS=", MSC_COL_ERR);
        msc_hex32(xhci_get_last_usbsts(), MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        msc_check_fatal_usbsts();
        return MSC_ERR_IO;
    }

    if (ret != 0) {
        msc_puts("[MSC]  bulk ERR ret=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts(" ep=", MSC_COL_ERR);
        msc_hex8(endpoint, MSC_COL_DATA);
        msc_puts(" after ", MSC_COL_ERR);
        msc_dec(attempts, MSC_COL_DATA);
        msc_puts(" polls\n", MSC_COL_ERR);
    } else {
        msc_puts("[MSC]  bulk OK polls=", MSC_COL_TRACE);
        msc_dec(attempts, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_TRACE);
    }
    return ret;
}

static int msc_control(usb_msc_device_t *d, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data)
{
    struct usb_core_driver *core = get_usb_core();
    if (!core->control_transfer) {
        msc_puts("[MSC]  control_transfer==NULL\n", MSC_COL_ERR);
        return MSC_ERR_IO;
    }

    msc_puts("[MSC]  ctrl type=", MSC_COL_TRACE);
    msc_hex8(type, MSC_COL_DATA);
    msc_puts(" req=", MSC_COL_TRACE);
    msc_hex8(req, MSC_COL_DATA);
    msc_puts(" val=", MSC_COL_TRACE);
    msc_hex16(val, MSC_COL_DATA);
    msc_puts(" idx=", MSC_COL_TRACE);
    msc_hex16(idx, MSC_COL_DATA);
    msc_puts(" len=", MSC_COL_TRACE);
    msc_dec(len, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_TRACE);

    int ret = core->control_transfer(d->usb_dev, type, req, val, idx, len, data);

    if (ret != 0) {
        msc_puts("[MSC]  ctrl ERR ret=", MSC_COL_WARN);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts(" xhci_cc=", MSC_COL_WARN);
        msc_dec((uint64_t)(int32_t)xhci_get_last_error_code(), MSC_COL_DATA);
        msc_puts(" USBSTS=", MSC_COL_WARN);
        msc_hex32(xhci_get_last_usbsts(), MSC_COL_DATA);
        msc_puts("\n", MSC_COL_WARN);
        msc_check_fatal_usbsts();
    } else {
        msc_puts("[MSC]  ctrl OK\n", MSC_COL_TRACE);
    }
    return ret;
}

static void msc_bot_reset(usb_msc_device_t *d)
{
    msc_sep("BOT RESET");
    msc_puts("[MSC] BOT Reset: step 1/4 — Mass Storage Reset (class req)\n",
             MSC_COL_INFO);
    int r1 = msc_control(d, 0x21, USB_MSC_REQ_RESET, 0, 0, 0, NULL);
    msc_puts("[MSC] BOT Reset: step1 result=", MSC_COL_DBG);
    msc_dec((uint64_t)(int32_t)r1, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    msc_puts("[MSC] BOT Reset: step 2/4 — Clear STALL Bulk-IN ep=",
             MSC_COL_INFO);
    msc_hex8(d->ep_bulk_in, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);
    int r2 = msc_control(d, 0x02, 0x01 , 0x0000,
                         d->ep_bulk_in, 0, NULL);
    msc_puts("[MSC] BOT Reset: step2 result=", MSC_COL_DBG);
    msc_dec((uint64_t)(int32_t)r2, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    if (delay_ms) delay_ms(10);

    msc_puts("[MSC] BOT Reset: step 3/4 — Clear STALL Bulk-OUT ep=",
             MSC_COL_INFO);
    msc_hex8(d->ep_bulk_out, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);
    int r3 = msc_control(d, 0x02, 0x01 , 0x0000,
                         d->ep_bulk_out, 0, NULL);
    msc_puts("[MSC] BOT Reset: step3 result=", MSC_COL_DBG);
    msc_dec((uint64_t)(int32_t)r3, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    msc_puts("[MSC] BOT Reset: step 4/4 — reset toggle DATA0\n", MSC_COL_INFO);
    {
        struct usb_core_driver *_core = get_usb_core();
        if (_core->reset_endpoint_toggle) {
            _core->reset_endpoint_toggle(d->usb_dev, d->ep_bulk_out);
            msc_puts("[MSC] BOT Reset:   toggle OUT reset OK\n", MSC_COL_DBG);
            _core->reset_endpoint_toggle(d->usb_dev, d->ep_bulk_in);
            msc_puts("[MSC] BOT Reset:   toggle IN  reset OK\n", MSC_COL_DBG);
        } else {
            msc_puts("[MSC] BOT Reset: WARN reset_endpoint_toggle not available\n",
                     MSC_COL_WARN);
        }
    }

    msc_puts("[MSC] BOT Reset: sleeping 50ms...\n", MSC_COL_DBG);
    if (delay_ms) delay_ms(50);
    msc_puts("[MSC] BOT Reset: done\n", MSC_COL_OK);
}

static int msc_execute(usb_msc_device_t *d, const uint8_t *cmd, uint8_t cmd_len, void *data, uint32_t data_len, uint8_t direction)
{
    int ret;

    msc_puts("[MSC]  execute: CDB len=", MSC_COL_DBG);
    msc_dec(cmd_len, MSC_COL_DATA);
    msc_puts(" cmd=", MSC_COL_DBG);
    msc_hex8(cmd[0], MSC_COL_DATA);
    msc_puts(" dir=", MSC_COL_DBG);
    msc_puts((direction == CBW_FLAGS_IN) ? "IN" : "OUT", MSC_COL_DATA);
    msc_puts(" data_len=", MSC_COL_DBG);
    msc_dec(data_len, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    g_cbw.dCBWSignature = CBW_SIGNATURE;
    g_cbw.dCBWTag = ++d->cbw_tag;
    g_cbw.dCBWDataTransferLength = data_len;
    g_cbw.bmCBWFlags = direction;
    g_cbw.bCBWLUN = d->lun;
    g_cbw.bCBWCBLength = cmd_len;
    for (int i = 0; i < 16; i++)
        g_cbw.CBWCB[i] = (i < cmd_len) ? cmd[i] : 0;

    msc_puts("[MSC]  CBW: tag=", MSC_COL_DBG);
    msc_hex32(g_cbw.dCBWTag, MSC_COL_DATA);
    msc_puts(" sig=", MSC_COL_DBG);
    msc_hex32(g_cbw.dCBWSignature, MSC_COL_DATA);
    msc_puts(" lun=", MSC_COL_DBG);
    msc_dec(g_cbw.bCBWLUN, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);
    msc_hexdump("[MSC]  CBW bytes: ", (const uint8_t *)&g_cbw,
                sizeof(g_cbw), MSC_COL_TRACE);

    msc_puts("[MSC]  >> sending CBW (" , MSC_COL_DBG);
    msc_dec(sizeof(g_cbw), MSC_COL_DATA);
    msc_puts(" bytes) to ep_out=", MSC_COL_DBG);
    msc_hex8(d->ep_bulk_out, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    ret = msc_bulk(d, d->ep_bulk_out, &g_cbw, sizeof(g_cbw), 0 );
    if (ret != 0) {
        msc_puts("[MSC]  CBW FAILED ret=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts(", invoking BOT reset\n", MSC_COL_ERR);
        msc_bot_reset(d);
        return MSC_ERR_IO;
    }
    msc_puts("[MSC]  CBW sent OK\n", MSC_COL_DBG);

    if (data && data_len > 0) {
        uint8_t ep = (direction == CBW_FLAGS_IN) ? d->ep_bulk_in
        : d->ep_bulk_out;
        uint8_t dir = (direction == CBW_FLAGS_IN) ? 1 : 0;
        uint16_t xfer_len = (uint16_t)(data_len > 0xFFFF ? 0xFFFF : data_len);

        msc_puts("[MSC]  >> data phase ep=", MSC_COL_DBG);
        msc_hex8(ep, MSC_COL_DATA);
        msc_puts(" dir=", MSC_COL_DBG);
        msc_puts(dir ? "IN" : "OUT", MSC_COL_DATA);
        msc_puts(" xfer_len=", MSC_COL_DBG);
        msc_dec(xfer_len, MSC_COL_DATA);
        if (data_len > 0xFFFF) {
            msc_puts(" (TRUNCATED from ", MSC_COL_WARN);
            msc_dec(data_len, MSC_COL_DATA);
            msc_puts(")", MSC_COL_WARN);
        }
        msc_puts("\n", MSC_COL_DBG);

        int data_ok = 0;
        for (int _dr = 0; _dr < 3; _dr++) {
            if (_dr > 0) {
                msc_puts("[MSC]  data phase retry ", MSC_COL_WARN);
                msc_dec(_dr + 1, MSC_COL_DATA);
                msc_puts("/3 ep=", MSC_COL_WARN);
                msc_hex8(ep, MSC_COL_DATA);
                msc_puts(" clearing STALL\n", MSC_COL_WARN);
                msc_control(d, 0x02, 0x01 , 0x0000, ep, 0, NULL);
                {
                    struct usb_core_driver *_rc = get_usb_core();
                    if (_rc->reset_endpoint_toggle)
                        _rc->reset_endpoint_toggle(d->usb_dev, ep);
                }
                if (delay_ms) delay_ms(10);
            }
            ret = msc_bulk(d, ep, data, xfer_len, dir);
            if (ret == 0) { data_ok = 1; break; }

            msc_puts("[MSC]  data phase attempt ", MSC_COL_WARN);
            msc_dec(_dr + 1, MSC_COL_DATA);
            msc_puts(" failed ret=", MSC_COL_WARN);
            msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
            msc_puts("\n", MSC_COL_WARN);
        }
        if (!data_ok) {
            msc_puts("[MSC]  data phase FAILED after 3 retries\n", MSC_COL_ERR);
            msc_bot_reset(d);
            return MSC_ERR_IO;
        }
        msc_puts("[MSC]  data phase OK\n", MSC_COL_DBG);
    } else {
        msc_puts("[MSC]  >> no data phase (data_len=0 or buf=NULL)\n", MSC_COL_TRACE);
    }

    for (int i = 0; i < (int)sizeof(g_csw); i++)
        ((uint8_t *)&g_csw)[i] = 0;

    msc_puts("[MSC]  >> receiving CSW (", MSC_COL_DBG);
    msc_dec(sizeof(g_csw), MSC_COL_DATA);
    msc_puts(" bytes) from ep_in=", MSC_COL_DBG);
    msc_hex8(d->ep_bulk_in, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    ret = msc_bulk(d, d->ep_bulk_in, &g_csw, sizeof(g_csw), 1 );
    if (ret != 0) {
        msc_puts("[MSC]  CSW recv FAILED ret=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        msc_bot_reset(d);
        return MSC_ERR_IO;
    }

    msc_hexdump("[MSC]  CSW bytes: ", (const uint8_t *)&g_csw,
                sizeof(g_csw), MSC_COL_TRACE);
    msc_puts("[MSC]  CSW: sig=", MSC_COL_DBG);
    msc_hex32(g_csw.dCSWSignature, MSC_COL_DATA);
    msc_puts(" tag=", MSC_COL_DBG);
    msc_hex32(g_csw.dCSWTag, MSC_COL_DATA);
    msc_puts(" residue=", MSC_COL_DBG);
    msc_hex32(g_csw.dCSWDataResidue, MSC_COL_DATA);
    msc_puts(" status=", MSC_COL_DBG);
    msc_hex8(g_csw.bCSWStatus, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    if (g_csw.dCSWSignature != CSW_SIGNATURE) {
        msc_puts("[MSC]  ERR CSW bad signature: got=", MSC_COL_ERR);
        msc_hex32(g_csw.dCSWSignature, MSC_COL_DATA);
        msc_puts(" expected=", MSC_COL_ERR);
        msc_hex32(CSW_SIGNATURE, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        msc_bot_reset(d);
        return MSC_ERR_IO;
    }

    if (g_csw.dCSWTag != d->cbw_tag) {
        msc_puts("[MSC]  ERR CSW tag mismatch: got=", MSC_COL_ERR);
        msc_hex32(g_csw.dCSWTag, MSC_COL_DATA);
        msc_puts(" expected=", MSC_COL_ERR);
        msc_hex32(d->cbw_tag, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        msc_bot_reset(d);
        return MSC_ERR_IO;
    }

    if (g_csw.bCSWStatus == CSW_STATUS_PHASE_ERROR) {
        msc_puts("[MSC]  ERR CSW phase error (protocol violation)\n", MSC_COL_ERR);
        msc_bot_reset(d);
        return MSC_ERR_IO;
    }

    if (g_csw.bCSWStatus != CSW_STATUS_GOOD) {
        msc_puts("[MSC]  CSW status FAILED=", MSC_COL_ERR);
        msc_hex8(g_csw.bCSWStatus, MSC_COL_DATA);
        msc_puts(" residue=", MSC_COL_ERR);
        msc_hex32(g_csw.dCSWDataResidue, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        return MSC_ERR_IO;
    }

    if (g_csw.dCSWDataResidue != 0) {
        msc_puts("[MSC]  residue=", MSC_COL_DBG);
        msc_hex32(g_csw.dCSWDataResidue, MSC_COL_DATA);
        msc_puts(" (short transfer?)\n", MSC_COL_WARN);
    }

    msc_puts("[MSC]  execute: OK\n", MSC_COL_DBG);
    return MSC_OK;
}

static int msc_scsi_inquiry(usb_msc_device_t *d)
{
    msc_sep("SCSI INQUIRY");
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    msc_puts("[MSC] INQUIRY: CDB alloc_len=36\n", MSC_COL_DBG);
    for (int i = 0; i < 36; i++) g_scsi_buf[i] = 0;

    int ret = msc_execute(d, cdb, 6, g_scsi_buf, 36, CBW_FLAGS_IN);
    if (ret != 0) {
        msc_puts("[MSC] INQUIRY FAILED ret=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        return ret;
    }

    msc_hexdump("[MSC] INQUIRY raw: ", g_scsi_buf, 36, MSC_COL_TRACE);

    scsi_inquiry_data_t *inq = (scsi_inquiry_data_t *)g_scsi_buf;

    msc_puts("[MSC] INQUIRY peripheral_type=", MSC_COL_DBG);

    msc_puts(" removable=", MSC_COL_DBG);
    msc_puts((inq->removable & 0x80) ? "YES" : "NO", MSC_COL_DATA);
    msc_puts(" version=", MSC_COL_DBG);
    msc_hex8(inq->version, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    char vendor[9] = {0}, product[17] = {0}, revision[5] = {0};
    for (int i = 0; i < 8; i++) vendor[i] = inq->vendor_id[i];
    for (int i = 0; i < 16; i++) product[i] = inq->product_id[i];
    for (int i = 0; i < 4; i++) revision[i] = inq->product_rev[i];

    msc_puts("[MSC] INQUIRY vendor=", MSC_COL_NOTE); msc_puts(vendor, MSC_COL_DATA);
    msc_puts(" product=", MSC_COL_INFO); msc_puts(product, MSC_COL_DATA);
    msc_puts(" revision=", MSC_COL_INFO); msc_puts(revision, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);

    return MSC_OK;
}

static int msc_scsi_test_unit_ready(usb_msc_device_t *d)
{
    msc_puts("[MSC] TEST UNIT READY\n", MSC_COL_DBG);
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    int ret = msc_execute(d, cdb, 6, NULL, 0, CBW_FLAGS_OUT);
    if (ret == 0)
        msc_puts("[MSC] TEST UNIT READY: device is ready\n", MSC_COL_OK);
    else {
        msc_puts("[MSC] TEST UNIT READY: device NOT ready ret=", MSC_COL_DBG);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_WARN);
    }
    return ret;
}

static int msc_scsi_request_sense(usb_msc_device_t *d)
{
    msc_puts("[MSC] REQUEST SENSE\n", MSC_COL_DBG);
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    for (int i = 0; i < 18; i++) g_scsi_buf[i] = 0;

    int ret = msc_execute(d, cdb, 6, g_scsi_buf, 18, CBW_FLAGS_IN);
    if (ret != 0) {
        msc_puts("[MSC] REQUEST SENSE failed ret=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        return ret;
    }

    msc_hexdump("[MSC] SENSE raw: ", g_scsi_buf, 18, MSC_COL_TRACE);

    scsi_sense_data_t *s = (scsi_sense_data_t *)g_scsi_buf;

    uint8_t resp_code = g_scsi_buf[0] & 0x7F;
    uint8_t sense_key = s->sense_key & 0x0F;

    const char *sk_names[] = {
        "NO_SENSE","RECOVERED","NOT_READY","MEDIUM_ERROR",
        "HARDWARE_ERROR","ILLEGAL_REQUEST","UNIT_ATTENTION",
        "DATA_PROTECT","BLANK_CHECK","VENDOR","COPY_ABORT",
        "ABORTED_COMMAND","?","VOLUME_OVERFLOW","MISCOMPARE","?"
    };

    msc_puts("[MSC] SENSE response_code=", MSC_COL_INFO);
    msc_hex8(resp_code, MSC_COL_DATA);
    msc_puts(" sense_key=", MSC_COL_INFO);
    msc_hex8(sense_key, MSC_COL_DATA);
    msc_puts(" (", MSC_COL_INFO);
    msc_puts(sk_names[sense_key], MSC_COL_DATA);
    msc_puts(") asc=", MSC_COL_INFO);
    msc_hex8(s->asc, MSC_COL_DATA);
    msc_puts(" ascq=", MSC_COL_INFO);
    msc_hex8(s->ascq, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);

    if (s->asc == 0x04 && s->ascq == 0x01)
        msc_puts("[MSC] SENSE: becoming ready (normal spinup)\n", MSC_COL_DBG);
    else if (s->asc == 0x28 && s->ascq == 0x00)
        msc_puts("[MSC] SENSE: medium changed (unit attention, expected)\n", MSC_COL_DBG);
    else if (s->asc == 0x3A)
        msc_puts("[MSC] SENSE: medium not present!\n", MSC_COL_ERR);
    else if (s->asc == 0x20 && s->ascq == 0x00)
        msc_puts("[MSC] SENSE: invalid command opcode\n", MSC_COL_ERR);
    else if (s->asc == 0x24 && s->ascq == 0x00)
        msc_puts("[MSC] SENSE: invalid field in CDB\n", MSC_COL_ERR);
    else if (sense_key == 0x00)
        msc_puts("[MSC] SENSE: no sense (all good)\n", MSC_COL_OK);

    return MSC_OK;
}

static int msc_scsi_read_capacity(usb_msc_device_t *d)
{
    msc_sep("SCSI READ CAPACITY(10)");
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0,0,0,0,0,0,0,0,0 };
    for (int i = 0; i < 8; i++) g_scsi_buf[i] = 0;

    int ret = msc_execute(d, cdb, 10, g_scsi_buf, 8, CBW_FLAGS_IN);
    if (ret != 0) {
        msc_puts("[MSC] READ CAPACITY FAILED ret=", MSC_COL_ERR);
        msc_dec((uint64_t)(int32_t)ret, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        return ret;
    }

    msc_hexdump("[MSC] READ CAPACITY raw: ", g_scsi_buf, 8, MSC_COL_TRACE);

    scsi_read_capacity_t *cap = (scsi_read_capacity_t *)g_scsi_buf;

    uint32_t last_lba_be = cap->lba_last;
    uint32_t blk_size_be = cap->block_size;
    uint32_t last_lba = be32(last_lba_be);
    uint32_t blk_size = be32(blk_size_be);

    msc_puts("[MSC] READ CAPACITY: raw_last_lba_be=", MSC_COL_DBG);
    msc_hex32(last_lba_be, MSC_COL_DATA);
    msc_puts(" raw_blk_size_be=", MSC_COL_DBG);
    msc_hex32(blk_size_be, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    msc_puts("[MSC] READ CAPACITY: last_lba=", MSC_COL_DBG);
    msc_hex32(last_lba, MSC_COL_DATA);
    msc_puts(" blk_size=", MSC_COL_DBG);
    msc_dec(blk_size, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    if (blk_size == 0) {
        msc_puts("[MSC] WARN block_size=0 from device, defaulting to 512\n",
                 MSC_COL_WARN);
        blk_size = 512;
    }
    if (blk_size != 512 && blk_size != 4096) {
        msc_puts("[MSC] WARN unusual block_size=", MSC_COL_WARN);
        msc_dec(blk_size, MSC_COL_DATA);
        msc_puts(" (expected 512 or 4096)\n", MSC_COL_WARN);
    }

    d->sector_count = (uint64_t)last_lba + 1;
    d->sector_size = blk_size;

    uint64_t size_mb = (d->sector_count * d->sector_size) >> 20;
    uint64_t size_gb = size_mb >> 10;

    msc_puts("[MSC] capacity: sectors=", MSC_COL_INFO);
    msc_dec(d->sector_count, MSC_COL_DATA);
    msc_puts(" sector_size=", MSC_COL_INFO);
    msc_dec(d->sector_size, MSC_COL_DATA);
    msc_puts(" (~", MSC_COL_INFO);
    if (size_gb > 0) {
        msc_dec(size_gb, MSC_COL_DATA);
        msc_puts(" GB", MSC_COL_INFO);
    } else {
        msc_dec(size_mb, MSC_COL_DATA);
        msc_puts(" MB", MSC_COL_INFO);
    }
    msc_puts(")\n", MSC_COL_INFO);

    return MSC_OK;
}

static int msc_blk_read(struct block_device *self, uint64_t lba, uint32_t count, void *buf)
{
    usb_msc_device_t *d = (usb_msc_device_t *)self->priv;
    if (!d || !d->present) return MSC_ERR_IO;
    if (count == 0) return MSC_OK;

    if (d->sector_size == 0) return MSC_ERR_IO;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {

        uint32_t max_sectors = MSC_DMA_CHUNK / d->sector_size;
        if (max_sectors == 0) max_sectors = 1;
        uint32_t n = count - done;
        if (n > max_sectors) n = max_sectors;

        uint64_t cur_lba = lba + done;
        uint32_t byte_len = n * d->sector_size;
        if (byte_len > MSC_DMA_CHUNK) {

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
            msc_scsi_request_sense(d);
            return ret;
        }
        memcpy(dst, g_dma_buf, byte_len);

        dst += byte_len;
        done += n;
    }

    return MSC_OK;
}

static int msc_blk_write(struct block_device *self, uint64_t lba, uint32_t count, void *buf)
{
    usb_msc_device_t *d = (usb_msc_device_t *)self->priv;
    if (!d || !d->present) return MSC_ERR_IO;
    if (count == 0) return MSC_OK;

    if (d->sector_size == 0) return MSC_ERR_IO;

    uint8_t *src = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint32_t max_sectors = MSC_DMA_CHUNK / d->sector_size;
        if (max_sectors == 0) max_sectors = 1;
        uint32_t n = count - done;
        if (n > max_sectors) n = max_sectors;

        uint64_t cur_lba = lba + done;
        uint32_t byte_len = n * d->sector_size;
        if (byte_len > MSC_DMA_CHUNK) return MSC_ERR_PARAM;
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
            msc_scsi_request_sense(d);
            return ret;
        }

        src += byte_len;
        done += n;
    }

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

static void msc_init_device(struct usb_device *dev)
{
    msc_sep("MSC DEVICE INIT");

    int slot = find_free_msc_slot();
    if (slot < 0) {
        msc_puts("[MSC] ERR: too many MSC devices (max=", MSC_COL_ERR);
        msc_dec(USB_MSC_MAX_DEVICES, MSC_COL_DATA);
        msc_puts(")\n", MSC_COL_ERR);
        return;
    }

    msc_puts("[MSC] found USB Mass Storage device addr=", MSC_COL_INFO);
    msc_dec(dev->address, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);

    msc_puts("[MSC] USBSTS at MSC handoff (before any MSC request)=", MSC_COL_INFO);
    msc_hex32(xhci_read_current_usbsts(), MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);

    usb_msc_device_t *d = &msc_devs[slot];
    for (int i = 0; i < (int)sizeof(usb_msc_device_t); i++)
        ((uint8_t *)d)[i] = 0;

    d->usb_dev = dev;
    d->lun = 0;
    d->cbw_tag = 0;

    msc_puts("[MSC] scanning bulk endpoints:\n", MSC_COL_DBG);
    for (int i = 0; i < dev->bulk_ep_count; i++) {
        struct usb_endpoint_info *ep = &dev->bulk_ep[i];
        msc_puts("[MSC]   ep[", MSC_COL_TRACE);
        msc_dec(i, MSC_COL_DATA);
        msc_puts("] addr=", MSC_COL_TRACE);
        msc_hex8(ep->address, MSC_COL_DATA);
        msc_puts(" mps=", MSC_COL_TRACE);
        msc_dec(ep->max_packet_size, MSC_COL_DATA);
        msc_puts(" dir=", MSC_COL_TRACE);
        msc_puts((ep->address & 0x80) ? "IN" : "OUT", MSC_COL_DATA);
        msc_puts("\n", MSC_COL_TRACE);

        if ((ep->address & 0x80) && !d->ep_bulk_in) {
            d->ep_bulk_in = ep->address;
            d->ep_in_mps = ep->max_packet_size ? ep->max_packet_size : 512;
            msc_puts("[MSC]   -> selected as Bulk-IN\n", MSC_COL_OK);
        } else if (!(ep->address & 0x80) && !d->ep_bulk_out) {
            d->ep_bulk_out = ep->address;
            d->ep_out_mps = ep->max_packet_size ? ep->max_packet_size : 512;
            msc_puts("[MSC]   -> selected as Bulk-OUT\n", MSC_COL_OK);
        }
    }

    if (!d->ep_bulk_in || !d->ep_bulk_out) {
        msc_puts("[MSC] ERR: bulk endpoints not found\n", MSC_COL_ERR);
        msc_puts("[MSC]   Bulk-IN  found=", MSC_COL_ERR);
        msc_puts(d->ep_bulk_in ? "YES" : "NO", MSC_COL_DATA);
        msc_puts(" addr=", MSC_COL_ERR); msc_hex8(d->ep_bulk_in, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        msc_puts("[MSC]   Bulk-OUT found=", MSC_COL_ERR);
        msc_puts(d->ep_bulk_out ? "YES" : "NO", MSC_COL_DATA);
        msc_puts(" addr=", MSC_COL_ERR); msc_hex8(d->ep_bulk_out, MSC_COL_DATA);
        msc_puts("\n", MSC_COL_ERR);
        return;
    }

    msc_puts("[MSC] ep_in=", MSC_COL_INFO); msc_hex8(d->ep_bulk_in, MSC_COL_DATA);
    msc_puts(" mps_in=", MSC_COL_INFO); msc_dec(d->ep_in_mps, MSC_COL_DATA);
    msc_puts(" ep_out=", MSC_COL_INFO); msc_hex8(d->ep_bulk_out, MSC_COL_DATA);
    msc_puts(" mps_out=", MSC_COL_INFO); msc_dec(d->ep_out_mps, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);

    msc_sep("GET MAX LUN");
    msc_puts("[MSC] USBSTS just before GET MAX LUN=", MSC_COL_INFO);
    msc_hex32(xhci_read_current_usbsts(), MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);
    {
        static uint8_t lun_buf[1] __attribute__((aligned(64)));
        lun_buf[0] = 0xFF;
        msc_puts("[MSC] GET MAX LUN (bmReqType=0xA1, bReq=0xFE)\n", MSC_COL_DBG);
        int r = msc_control(d, 0xA1, USB_MSC_REQ_GET_MAX_LUN,
                            0, 0, 1, lun_buf);
        if (r == 0) {
            msc_puts("[MSC] max_lun=", MSC_COL_INFO);
            msc_dec(lun_buf[0], MSC_COL_DATA);
            msc_puts(" (using LUN 0)\n", MSC_COL_INFO);
        } else {
            msc_puts("[MSC] GET MAX LUN returned err=", MSC_COL_DBG);
            msc_dec((uint64_t)(int32_t)r, MSC_COL_DATA);
            msc_puts(" — device may not support it (normal for single-LUN)\n",
                     MSC_COL_WARN);
        }
    }

    if (delay_ms) delay_ms(50);

    if (delay_ms) delay_ms(20);
    {
        int inq_ok = 0;
        for (int inq_try = 0; inq_try < 5; inq_try++) {
            if (inq_try > 0) {

                msc_puts("[MSC] INQUIRY retry ", MSC_COL_WARN);
                msc_dec(inq_try + 1, MSC_COL_DATA);
                msc_puts("/5\n", MSC_COL_WARN);
                if (delay_ms) delay_ms(100);
            }
            if (msc_scsi_inquiry(d) == 0) {
                inq_ok = 1;
                break;
            }
            msc_puts("[MSC] INQUIRY attempt ", MSC_COL_WARN);
            msc_dec(inq_try + 1, MSC_COL_DATA);
            msc_puts(" failed\n", MSC_COL_WARN);
            if (delay_ms) delay_ms(50);
        }
        if (!inq_ok) {

            msc_puts("[MSC] WARN: INQUIRY failed after 5 attempts — continuing without INQUIRY data\n", MSC_COL_WARN);
            msc_puts("[MSC]   BOT Reset to recover\n", MSC_COL_WARN);
            msc_bot_reset(d);
            if (delay_ms) delay_ms(200);
        }
    }

    msc_sep("TEST UNIT READY loop");
    {
        int ready = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            msc_puts("[MSC] TUR attempt ", MSC_COL_DBG);
            msc_dec(attempt + 1, MSC_COL_DATA);
            msc_puts("/10\n", MSC_COL_DBG);

            if (msc_scsi_test_unit_ready(d) == 0) {
                ready = 1;
                msc_puts("[MSC] device ready on attempt ", MSC_COL_OK);
                msc_dec(attempt + 1, MSC_COL_DATA);
                msc_puts("\n", MSC_COL_OK);
                break;
            }
            msc_puts("[MSC] not ready, reading sense...\n", MSC_COL_DBG);
            msc_scsi_request_sense(d);
            msc_puts("[MSC] waiting 100ms before retry\n", MSC_COL_DBG);
            if (delay_ms) delay_ms(100);
        }
        if (!ready) {
            msc_puts("[MSC] ERR: device not ready after 10 attempts\n", MSC_COL_ERR);
            msc_puts("[MSC]   device subclass=", MSC_COL_WARN);
            msc_hex8(dev->device_subclass, MSC_COL_DATA);
            msc_puts(" protocol=", MSC_COL_WARN);
            msc_hex8(dev->device_protocol, MSC_COL_DATA);
            msc_puts("\n", MSC_COL_WARN);
            return;
        }
    }

    if (msc_scsi_read_capacity(d) != 0) {
        msc_puts("[MSC] ERR: READ CAPACITY failed\n", MSC_COL_ERR);
        msc_puts("[MSC]   Trying REQUEST SENSE for diagnostics...\n", MSC_COL_WARN);
        msc_scsi_request_sense(d);
        return;
    }

    if (d->sector_count == 0) {
        msc_puts("[MSC] ERR: sector_count=0 (device reported empty medium?)\n",
                 MSC_COL_ERR);
        return;
    }

    msc_sep("SANITY CHECKS");
    msc_puts("[MSC] sector_count=", MSC_COL_DBG); msc_dec(d->sector_count, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);
    msc_puts("[MSC] sector_size=", MSC_COL_DBG); msc_dec(d->sector_size, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);
    msc_puts("[MSC] ep_in_mps=", MSC_COL_DBG); msc_dec(d->ep_in_mps, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);
    msc_puts("[MSC] ep_out_mps=", MSC_COL_DBG); msc_dec(d->ep_out_mps, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);
    msc_puts("[MSC] cbw_tag=", MSC_COL_DBG); msc_hex32(d->cbw_tag, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);

    if (d->sector_size % 512 != 0) {
        msc_puts("[MSC] WARN sector_size not a multiple of 512!\n", MSC_COL_WARN);
    }

    dev->driver_managed = 1;
    msc_puts("[MSC] dev->driver_managed set to 1\n", MSC_COL_DBG);

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

    msc_puts("[MSC] registering block_device name=", MSC_COL_DBG);
    msc_puts(bd->name, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    block_device_register(bd);

    partition_register_all(bd);

    msc_puts("[MSC] registered -> ", MSC_COL_NOTE);
    msc_puts(bd->name, MSC_COL_OK);
    msc_puts(" sectors=", MSC_COL_INFO);
    msc_dec(bd->sector_count, MSC_COL_DATA);
    msc_puts(" sector_size=", MSC_COL_INFO);
    msc_dec(bd->sector_size, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_OK);

    if (slot == msc_dev_count) msc_dev_count++;
    drv_usb_msc.disk_count = msc_dev_count;
    msc_puts("[MSC] total USB MSC devices now: ", MSC_COL_INFO);
    msc_dec(msc_dev_count, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);
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

                msc_puts("[MSC] device removed, unregistered ", MSC_COL_NOTE);
                msc_puts(msc_devs[j].blkdev.name, MSC_COL_DATA);
                msc_puts("\n", MSC_COL_WARN);
            }
        }
    }
}

static void msc_scan_devices(void)
{
    msc_sep("USB DEVICE SCAN");
    msc_puts("[MSC] scanning USB devices for Mass Storage...\n", MSC_COL_INFO);

    extern int usb_get_device_count(void);
    extern struct usb_device *usb_get_device(int idx);

    int n = usb_get_device_count();
    msc_puts("[MSC] usb_get_device_count()=", MSC_COL_DBG);
    msc_dec((uint64_t)n, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_DBG);

    if (n <= 0) {
        msc_puts("[MSC] WARN: no USB devices found\n", MSC_COL_WARN);
    }

    int msc_found = 0;
    for (int i = 0; i < n; i++) {
        struct usb_device *dev = usb_get_device(i);
        if (!dev) {
            msc_puts("[MSC]   slot ", MSC_COL_TRACE);
            msc_dec(i, MSC_COL_DATA);
            msc_puts(": NULL, skipping\n", MSC_COL_TRACE);
            continue;
        }

        bool is_msc = (dev->device_class == USB_CLASS_MSC);
        msc_puts(is_msc ? " [MSC!]" : " [skip]", is_msc ? MSC_COL_OK : MSC_COL_TRACE);
        msc_puts("\n", MSC_COL_TRACE);

        if (!is_msc) continue;
        msc_found++;

        bool already = false;
        for (int j = 0; j < msc_dev_count; j++) {
            if (msc_devs[j].usb_dev == dev) { already = true; break; }
        }
        if (already) {
            msc_puts("[MSC]   already initialized, skipping\n", MSC_COL_TRACE);
            continue;
        }

        msc_init_device(dev);
    }

    msc_puts("[MSC] scan done: found ", MSC_COL_INFO);
    msc_dec(msc_found, MSC_COL_DATA);
    msc_puts(" MSC device(s)\n", MSC_COL_INFO);
}

static void *msc_driver_init(void)
{
    msc_sep("USB MSC DRIVER INIT START");
    msc_puts("[MSC] USB Mass Storage driver init\n", MSC_COL_INFO);
    msc_puts("[MSC] compiled: " __DATE__ " " __TIME__ "\n", MSC_COL_TRACE);

    msc_puts("[MSC] resolving delay_ms via TIMER_DRIVER/TSC_TIMER...\n", MSC_COL_DBG);
    {
        struct tsc_driver *tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
        if (tsc) {
            delay_ms = tsc->sleep_tsc_ms;
            msc_puts("[MSC] delay_ms resolved OK (tsc=", MSC_COL_OK);
            msc_hex32((uint32_t)(uintptr_t)tsc, MSC_COL_DATA);
            msc_puts(")\n", MSC_COL_OK);
        } else {
            msc_puts("[MSC] WARN: TSC driver not found, delay_ms=NULL — "
            "polling may be unreliable\n", MSC_COL_WARN);
        }
    }

    msc_puts("[MSC] resolving usb_core (USB_DRIVER/USB_CORE_SLOT)...\n", MSC_COL_DBG);
    {
        struct usb_core_driver *core = get_usb_core();
        msc_puts("[MSC]   bulk_transfer=", MSC_COL_DBG);
        msc_puts(core->bulk_transfer ? "OK" : "NULL!", core->bulk_transfer ? MSC_COL_OK : MSC_COL_ERR);
        msc_puts("\n", MSC_COL_DBG);
        msc_puts("[MSC]   control_transfer=", MSC_COL_DBG);
        msc_puts(core->control_transfer ? "OK" : "NULL!", core->control_transfer ? MSC_COL_OK : MSC_COL_ERR);
        msc_puts("\n", MSC_COL_DBG);
        msc_puts("[MSC]   reset_endpoint_toggle=",MSC_COL_DBG);
        msc_puts(core->reset_endpoint_toggle? "OK" : "NULL (toggle reset unavailable)",
                 core->reset_endpoint_toggle? MSC_COL_OK : MSC_COL_WARN);
        msc_puts("\n", MSC_COL_DBG);
    }

    msc_puts("[MSC] zeroing device pool (max=", MSC_COL_DBG);
    msc_dec(USB_MSC_MAX_DEVICES, MSC_COL_DATA);
    msc_puts(")\n", MSC_COL_DBG);
    for (int i = 0; i < USB_MSC_MAX_DEVICES; i++)
        for (int j = 0; j < (int)sizeof(usb_msc_device_t); j++)
            ((uint8_t *)&msc_devs[i])[j] = 0;
    msc_dev_count = 0;

    msc_puts("[MSC] DMA buffers:\n", MSC_COL_DBG);
    msc_puts("[MSC]   g_cbw=", MSC_COL_DBG); msc_hex32((uint32_t)(uintptr_t)&g_cbw, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);
    msc_puts("[MSC]   g_csw=", MSC_COL_DBG); msc_hex32((uint32_t)(uintptr_t)&g_csw, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);
    msc_puts("[MSC]   g_scsi_buf=", MSC_COL_DBG); msc_hex32((uint32_t)(uintptr_t)&g_scsi_buf, MSC_COL_DATA); msc_puts("\n", MSC_COL_DBG);

    bool cbw_aligned = ((uintptr_t)&g_cbw & 63) == 0;
    bool csw_aligned = ((uintptr_t)&g_csw & 63) == 0;
    bool scsi_aligned = ((uintptr_t)&g_scsi_buf & 63) == 0;
    msc_puts("[MSC]   alignment (64-byte): cbw=", MSC_COL_DBG);
    msc_puts(cbw_aligned ? "OK" : "MISALIGNED!", cbw_aligned ? MSC_COL_OK : MSC_COL_ERR);
    msc_puts(" csw=", MSC_COL_DBG);
    msc_puts(csw_aligned ? "OK" : "MISALIGNED!", csw_aligned ? MSC_COL_OK : MSC_COL_ERR);
    msc_puts(" scsi=", MSC_COL_DBG);
    msc_puts(scsi_aligned ? "OK" : "MISALIGNED!", scsi_aligned ? MSC_COL_OK : MSC_COL_ERR);
    msc_puts("\n", MSC_COL_DBG);

    msc_scan_devices();

    drv_usb_msc.disk_count = msc_dev_count;

    usb_event_register_handler(usb_msc_on_usb_event, NULL);

    msc_sep("USB MSC DRIVER INIT DONE");
    msc_puts("[MSC] init done, usb disks found=", MSC_COL_INFO);
    msc_dec((uint64_t)msc_dev_count, MSC_COL_DATA);
    msc_puts("\n", MSC_COL_INFO);

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
