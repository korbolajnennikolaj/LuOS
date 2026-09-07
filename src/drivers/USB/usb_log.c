#include "drivers/USB/usb_log.h"

#include "components/drivers.h"
#include "drivers/Video/limine_video_driver.h"

#include <stddef.h>

#define USB_LOG_FONT_WIDTH 8

#define USB_LOG_DEFAULT_COLS 100
#define USB_LOG_MAX_COLS 240
#define USB_LOG_MIN_COLS 60

static struct limine_video_driver *s_v = NULL;
static uint16_t s_grid_cols = 0;

static uint16_t s_row_col = 0;
static uint32_t s_row_color = 0;

static usb_log_level s_min_level = USB_LOG_EVENT;

static int s_row_muted = 0;

usb_log_level usb_log_get_level(void) { return s_min_level; }

usb_log_level usb_log_set_level(usb_log_level lvl) {
    usb_log_level prev = s_min_level;
    if ((unsigned)lvl > (unsigned)USB_LOG_ERROR) lvl = USB_LOG_ERROR;
    s_min_level = lvl;
    return prev;
}

static const char *const s_tag_name[USB_LOG_TAG_COUNT] = {
    [USB_LOG_CORE] = "[CORE] ",
    [USB_LOG_HUB] = "[HUB]  ",
    [USB_LOG_XHCI] = "[XHCI] ",
    [USB_LOG_EHCI] = "[EHCI] ",
    [USB_LOG_OHCI] = "[OHCI] ",
    [USB_LOG_UHCI] = "[UHCI] ",
};

static const uint32_t s_tag_color[USB_LOG_TAG_COUNT] = {
    [USB_LOG_CORE] = LIMINE_COLOR_LIGHT_GRAY,
    [USB_LOG_HUB] = LIMINE_COLOR_TURQUOISE,
    [USB_LOG_XHCI] = LIMINE_COLOR_SKY_BLUE,
    [USB_LOG_EHCI] = LIMINE_COLOR_LIGHT_GREEN,
    [USB_LOG_OHCI] = LIMINE_COLOR_ORANGE,
    [USB_LOG_UHCI] = LIMINE_COLOR_HOT_PINK,
};

static uint32_t level_color(usb_log_level lvl) {
    switch (lvl) {
        case USB_LOG_TRACE: return LIMINE_COLOR_DARK_GRAY;
        case USB_LOG_INFO: return LIMINE_COLOR_LIGHT_GRAY;
        case USB_LOG_EVENT: return LIMINE_COLOR_AMBER;
        case USB_LOG_WARN: return LIMINE_COLOR_GOLD;
        case USB_LOG_ERROR: return LIMINE_COLOR_LIGHT_RED;
    }
    return LIMINE_COLOR_LIGHT_GRAY;
}

void usb_log_init(void) {

    s_v = (struct limine_video_driver *)get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    s_grid_cols = USB_LOG_DEFAULT_COLS;
    if (s_v && s_v->get_display_resolution) {
        uint64_t w = 0, h = 0;
        s_v->get_display_resolution(&w, &h);
        if (w > 0) {
            uint64_t cols = w / USB_LOG_FONT_WIDTH;
            if (cols < USB_LOG_MIN_COLS) cols = USB_LOG_MIN_COLS;
            if (cols > USB_LOG_MAX_COLS) cols = USB_LOG_MAX_COLS;
            s_grid_cols = (uint16_t)cols;
        }
    }
}

static struct limine_video_driver *get_v(void) {

    if (!s_v) usb_log_init();
    return s_v;
}

static void put_str_raw(const char *s, uint32_t color) {
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf || !s) return;
    v->printf(s, color);
}

static void put_hex32_raw(uint32_t val, uint32_t color) {
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf) return;
    const char *h = "0123456789ABCDEF";
    char b[9]; b[8] = 0;
    for (int i = 7; i >= 0; i--) { b[i] = h[val & 0xF]; val >>= 4; }
    v->printf(b, color);
}

static void put_hex64_raw(uint64_t val, uint32_t color) {
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf) return;
    const char *h = "0123456789ABCDEF";
    char b[17]; b[16] = 0;
    for (int i = 15; i >= 0; i--) { b[i] = h[val & 0xF]; val >>= 4; }
    v->printf(b, color);
}

static void put_dec_raw(int32_t val, uint32_t color) {
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf) return;
    uint32_t uval;
    if (val < 0) { v->printf("-", color); uval = (uint32_t)(-val); }
    else uval = (uint32_t)val;
    if (!uval) { v->printf("0", color); return; }
    char b[11]; b[10] = 0; int i = 9;
    while (uval > 0 && i >= 0) { b[i--] = '0' + (uval % 10); uval /= 10; }
    v->printf(b + i + 1, color);
}

static uint16_t strwidth(const char *s) {
    uint16_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

void usb_logrow_begin(usb_log_tag tag, usb_log_level lvl) {
    if ((unsigned)tag >= USB_LOG_TAG_COUNT) tag = USB_LOG_CORE;
    s_row_col = 0;
    s_row_muted = ((unsigned)lvl < (unsigned)s_min_level);
    if (s_row_muted) return;
    s_row_color = level_color(lvl);
    const char *name = s_tag_name[tag];
    put_str_raw(name, s_tag_color[tag]);
    s_row_col += strwidth(name);
}

void usb_logrow_str(const char *s) {
    if (s_row_muted || !s) return;
    put_str_raw(s, s_row_color);
    s_row_col += strwidth(s);
}

void usb_logrow_hex32(uint32_t v) {
    if (s_row_muted) return;
    put_hex32_raw(v, s_row_color);
    s_row_col += 8;
}

void usb_logrow_hex64(uint64_t v) {
    if (s_row_muted) return;
    put_hex64_raw(v, s_row_color);
    s_row_col += 16;
}

void usb_logrow_dec(int32_t v) {
    if (s_row_muted) return;

    uint16_t w = (v < 0) ? 1 : 0;
    uint32_t uval = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    if (!uval) w += 1;
    else while (uval > 0) { w++; uval /= 10; }
    put_dec_raw(v, s_row_color);
    s_row_col += w;
}

void usb_logrow_pad(uint16_t col) {
    if (s_row_muted) return;
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf) return;

    uint16_t max_col = (s_grid_cols > 1) ? (uint16_t)(s_grid_cols - 1) : s_grid_cols;
    uint16_t target = col;
    if (target > max_col) target = max_col;
    while (s_row_col < target) {
        v->printf(" ", s_row_color);
        s_row_col++;
    }
}

void usb_logrow_end(void) {
    if (s_row_muted) { s_row_muted = 0; s_row_col = 0; return; }
    struct limine_video_driver *v = get_v();
    if (v && v->printf) v->printf("\n", s_row_color);
    s_row_col = 0;
}

void usb_log(usb_log_tag tag, usb_log_level lvl, const char *msg) {
    usb_logrow_begin(tag, lvl);
    usb_logrow_str(msg);
    usb_logrow_end();
}

void usb_log_hex(usb_log_tag tag, usb_log_level lvl, const char *msg, uint32_t val) {
    usb_logrow_begin(tag, lvl);
    usb_logrow_str(msg);
    usb_logrow_str(" ");
    usb_logrow_hex32(val);
    usb_logrow_end();
}

void usb_log_hex64(usb_log_tag tag, usb_log_level lvl, const char *msg, uint64_t val) {
    usb_logrow_begin(tag, lvl);
    usb_logrow_str(msg);
    usb_logrow_str(" ");
    usb_logrow_hex64(val);
    usb_logrow_end();
}

void usb_log_dec(usb_log_tag tag, usb_log_level lvl, const char *msg, uint32_t val) {
    usb_logrow_begin(tag, lvl);
    usb_logrow_str(msg);
    usb_logrow_str(" ");
    usb_logrow_dec((int32_t)val);
    usb_logrow_end();
}

#define COL_CTRL 7
#define COL_PORT 12
#define COL_EVENT 17
#define COL_CCS 37
#define COL_PED 44
#define COL_PLS 51
#define COL_SPD 60
#define COL_CHANGE 68

void usb_log_port_table_header(void) {
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf) return;

    usb_logrow_begin(USB_LOG_CORE, USB_LOG_EVENT);
    usb_logrow_str("TAG    ");
    usb_logrow_pad(COL_CTRL);   usb_logrow_str("CTRL");
    usb_logrow_pad(COL_PORT);  usb_logrow_str("PORT");
    usb_logrow_pad(COL_EVENT); usb_logrow_str("EVENT");
    usb_logrow_pad(COL_CCS);   usb_logrow_str("CCS");
    usb_logrow_pad(COL_PED);   usb_logrow_str("PED");
    usb_logrow_pad(COL_PLS);   usb_logrow_str("PLS");
    usb_logrow_pad(COL_SPD);   usb_logrow_str("SPD");
    usb_logrow_pad(COL_CHANGE);usb_logrow_str("CHANGE");
    usb_logrow_end();
}

static const char *xhci_pls_name(uint32_t portsc) {
    uint32_t pls = (portsc >> 5) & 0xFu;
    switch (pls) {
        case 0: return "U0";
        case 1: return "U1";
        case 2: return "U2";
        case 3: return "U3";
        case 4: return "Dis";
        case 5: return "RxD";
        case 7: return "Pol";
        case 9: return "Comp";
        case 15: return "Rsm";
        default: return NULL;
    }
}

static const char *xhci_speed_name(uint8_t speed_id) {
    switch (speed_id) {
        case 1: return "FS";
        case 2: return "LS";
        case 3: return "HS";
        case 4: return "SS";
        case 5: return "SS+";
        default: return "--";
    }
}

void usb_log_port_event(usb_log_tag tag, int ctrl_idx, uint8_t port,
                         const char *event, uint32_t portsc,
                         uint8_t speed_id) {
    struct limine_video_driver *v = get_v();
    if (!v || !v->printf) return;

    uint8_t ccs = 0, ped = 0;
    const char *pls = "---";
    const char *spd = "--";

    const char *change_bits[7];
    int n_change = 0;

    switch (tag) {
        case USB_LOG_XHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 1)) ? 1 : 0;
            { const char *p = xhci_pls_name(portsc); if (p) pls = p; }
            spd = xhci_speed_name(speed_id);
            if (portsc & (1u << 17)) change_bits[n_change++] = "CSC";
            if (portsc & (1u << 18)) change_bits[n_change++] = "PEC";
            if (portsc & (1u << 19)) change_bits[n_change++] = "WRC";
            if (portsc & (1u << 20)) change_bits[n_change++] = "OCC";
            if (portsc & (1u << 21)) change_bits[n_change++] = "PRC";
            if (portsc & (1u << 22)) change_bits[n_change++] = "PLC";
            if (portsc & (1u << 23)) change_bits[n_change++] = "CEC";
            break;

        case USB_LOG_EHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 2)) ? 1 : 0;

            spd = ped ? "HS" : "FS/LS";
            if (portsc & (1u << 1)) change_bits[n_change++] = "CSC";
            if (portsc & (1u << 3)) change_bits[n_change++] = "PEC";
            if (portsc & (1u << 5)) change_bits[n_change++] = "OCC";
            if (portsc & (1u << 13)) change_bits[n_change++] = "OWNER=companion";
            break;

        case USB_LOG_OHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 1)) ? 1 : 0;
            spd = (portsc & (1u << 9)) ? "LS" : "FS";
            if (portsc & (1u << 16)) change_bits[n_change++] = "CSC";
            if (portsc & (1u << 17)) change_bits[n_change++] = "PESC";
            if (portsc & (1u << 18)) change_bits[n_change++] = "PSSC";
            if (portsc & (1u << 19)) change_bits[n_change++] = "OCIC";
            if (portsc & (1u << 20)) change_bits[n_change++] = "PRSC";
            break;

        case USB_LOG_UHCI:
            ccs = (portsc & (1u << 0)) ? 1 : 0;
            ped = (portsc & (1u << 2)) ? 1 : 0;
            spd = (portsc & (1u << 8)) ? "LS" : "FS";
            if (portsc & (1u << 1)) change_bits[n_change++] = "CSC";
            if (portsc & (1u << 3)) change_bits[n_change++] = "PEC";
            break;

        case USB_LOG_HUB: {
            uint16_t wstatus = (uint16_t)(portsc & 0xFFFFu);
            uint16_t wchange = (uint16_t)(portsc >> 16);
            ccs = (wstatus & 0x0001u) ? 1 : 0;
            ped = (wstatus & 0x0002u) ? 1 : 0;
            spd = (wstatus & 0x0400u) ? "HS" : (wstatus & 0x0200u) ? "LS" : "FS";
            if (wchange & 0x0001u) change_bits[n_change++] = "CSC";
            if (wchange & 0x0002u) change_bits[n_change++] = "PEC";
            if (wchange & 0x0004u) change_bits[n_change++] = "SUSP";
            if (wchange & 0x0008u) change_bits[n_change++] = "OCC";
            if (wchange & 0x0010u) change_bits[n_change++] = "PRC";
            break;
        }

        default:
            break;
    }

    usb_logrow_begin(tag, USB_LOG_EVENT);

    usb_logrow_pad(COL_CTRL);
    usb_logrow_str("c"); usb_logrow_dec(ctrl_idx);

    usb_logrow_pad(COL_PORT);
    usb_logrow_str("p");
    if (port < 10) usb_logrow_str("0");
    usb_logrow_dec(port);

    usb_logrow_pad(COL_EVENT);
    usb_logrow_str(event ? event : "?");

    usb_logrow_pad(COL_CCS);
    usb_logrow_str("CCS="); usb_logrow_dec(ccs);

    usb_logrow_pad(COL_PED);
    usb_logrow_str("PED="); usb_logrow_dec(ped);

    usb_logrow_pad(COL_PLS);
    usb_logrow_str("PLS="); usb_logrow_str(pls);

    usb_logrow_pad(COL_SPD);
    usb_logrow_str("SPD="); usb_logrow_str(spd);

    usb_logrow_pad(COL_CHANGE);
    if (n_change == 0) {
        usb_logrow_str("-");
    } else {
        for (int i = 0; i < n_change; i++) {
            if (i) usb_logrow_str(" ");
            usb_logrow_str(change_bits[i]);
        }
    }

    usb_logrow_end();
}
