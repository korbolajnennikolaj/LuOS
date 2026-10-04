#include "usb_audio.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "drivers/Timer/timer.h"
#include "drivers/USB/usb_controller.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define UA_SURVEY_MS 250
#define UA_SETTLE_MS 500
#define UA_REUSE_MS 2000
#define UA_CLAIM_MS 200
#define UA_STALL_MS 300
#define UA_MAX_CHECKED 64
#define UA_DRIVER_NAME "usb-audio"

typedef struct ua_checked {
    struct usb_device *usb;
    uint32_t generation;
    uint8_t address;
    int8_t fn_index;
    uint16_t vid;
    uint16_t pid;
    uint64_t first_seen_ms;
    uint8_t probed;
} ua_checked;

static struct ua_device ua_pool[UA_MAX_DEVICES];
static uint8_t ua_in_use[UA_MAX_DEVICES];
static uint64_t ua_freed_ms[UA_MAX_DEVICES];
static struct ua_checked ua_checked_list[UA_MAX_CHECKED];
static int ua_checked_count = 0;
static uint64_t ua_last_survey_ms = 0;
static int ua_attached = 0;
static spinlock_t ua_survey_lock = SPINLOCK_INIT;

static uint8_t ua_cfg_buf[UA_CFG_MAX] __attribute__((aligned(4096)));
static uint8_t ua_ctl_buf[UA_CTL_MAX] __attribute__((aligned(64)));

static const uint32_t ua_std_rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000,
                                         64000, 88200, 96000, 176400, 192000, 352800, 384000 };

static struct usb_core_driver *ua_core(void) {
    return (struct usb_core_driver *)get_self_driver(USB_DRIVER, USB_CORE_SLOT);
}

static bool ua_usb_alive(struct ua_device *ua) {
    struct usb_device *u = ua->usb;
    if (!u || !usb_device_alive(u)) return false;
    if (u->address != ua->address || u->ctrl != ua->ctrl || u->generation != ua->generation) return false;
    return u->desc.idVendor == ua->vid && u->desc.idProduct == ua->pid;
}

static int ua_control(struct ua_device *ua, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data) {
    struct usb_core_driver *core = ua_core();
    if (!core || !core->control_transfer || !ua->usb) return -1;
    if (len > UA_CTL_MAX) return -1;
    if (!usb_core_claim(UA_CLAIM_MS)) return -2;
    if (len && data && !(type & 0x80)) memcpy(ua_ctl_buf, data, len);
    if (len && (type & 0x80)) memset(ua_ctl_buf, 0, len);
    int r = core->control_transfer(ua->usb, type, req, val, idx, len, len ? ua_ctl_buf : NULL);
    if (r == 0 && len && data && (type & 0x80)) memcpy(data, ua_ctl_buf, len);
    usb_core_release();
    return r;
}

static uint32_t ua_le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t ua_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static struct ua_unit *ua_unit_get(struct ua_device *ua, uint8_t id) {
    if (!id) return NULL;
    for (uint8_t i = 0; i < ua->unit_count; i++)
        if (ua->units[i].id == id) return &ua->units[i];
    return NULL;
}

static void ua_unit_inputs(struct ua_unit *u, const uint8_t *src, uint8_t n) {
    u->nr_inputs = n > UA_MAX_INPUTS ? UA_MAX_INPUTS : n;
    for (uint8_t i = 0; i < u->nr_inputs; i++) u->inputs[i] = src[i];
    u->source = n ? src[0] : 0;
}

static void ua_parse_ac(struct ua_device *ua, const uint8_t *p, uint8_t dl) {
    if (ua->unit_count >= UA_MAX_UNITS) return;
    struct ua_unit *u = &ua->units[ua->unit_count];
    memset(u, 0, sizeof(*u));
    uint8_t st = p[2];
    bool v2 = ua->uac2;

    if (st == UA_AC_INPUT_TERMINAL && dl >= 8) {
        u->kind = UA_U_IT;
        u->id = p[3];
        u->terminal_type = (uint16_t)(p[4] | (p[5] << 8));
        if (v2 && dl >= 8) u->clock = p[7];
    } else if (st == UA_AC_OUTPUT_TERMINAL && dl >= 9) {
        u->kind = UA_U_OT;
        u->id = p[3];
        u->terminal_type = (uint16_t)(p[4] | (p[5] << 8));
        u->source = p[7];
        if (v2) u->clock = p[8];
    } else if ((st == UA_AC_MIXER_UNIT || st == UA_AC_SELECTOR_UNIT) && dl >= 6) {
        u->kind = (st == UA_AC_MIXER_UNIT) ? UA_U_MIXER : UA_U_SELECTOR;
        u->id = p[3];
        uint8_t n = p[4];
        if (5 + n > dl) n = (uint8_t)(dl - 5);
        ua_unit_inputs(u, p + 5, n);
    } else if (st == UA_AC_FEATURE_UNIT && dl >= 6) {
        u->kind = UA_U_FEATURE;
        u->id = p[3];
        u->source = p[4];
        if (v2) {
            if (dl >= 9) {
                uint32_t m = ua_le32(p + 5);
                u->mute_master = ((m & 0x3u) == 0x3u);
                u->vol_master = (((m >> 2) & 0x3u) == 0x3u);
            }
            if (dl >= 13) {
                uint32_t c = ua_le32(p + 9);
                u->mute_ch = ((c & 0x3u) == 0x3u);
                u->vol_ch = (((c >> 2) & 0x3u) == 0x3u);
            }
        } else {
            uint8_t cs = p[5];
            if (cs >= 1 && dl >= 6 + cs) {
                u->mute_master = (p[6] & 0x01u) != 0;
                u->vol_master = (p[6] & 0x02u) != 0;
            }
            if (cs >= 1 && dl >= 6 + 2 * cs) {
                u->mute_ch = (p[6 + cs] & 0x01u) != 0;
                u->vol_ch = (p[6 + cs] & 0x02u) != 0;
            }
        }
    } else if (!v2 && (st == UA_AC_PROCESSING_UNIT || st == UA_AC_EXTENSION_UNIT) && dl >= 8) {
        if (st == UA_AC_EXTENSION_UNIT && (ua->quirks.flags & AUDIO_USB_Q_NO_XU)) return;
        u->kind = (st == UA_AC_PROCESSING_UNIT) ? UA_U_PROCESSING : UA_U_EXTENSION;
        u->id = p[3];
        uint8_t n = p[6];
        if (7 + n > dl) n = (uint8_t)(dl - 7);
        ua_unit_inputs(u, p + 7, n);
    } else if (v2 && st == UA2_AC_EFFECT_UNIT && dl >= 7) {
        u->kind = UA_U_EFFECT;
        u->id = p[3];
        u->source = p[6];
    } else if (v2 && (st == UA2_AC_PROCESSING_UNIT || st == UA2_AC_EXTENSION_UNIT) && dl >= 8) {
        if (st == UA2_AC_EXTENSION_UNIT && (ua->quirks.flags & AUDIO_USB_Q_NO_XU)) return;
        u->kind = (st == UA2_AC_PROCESSING_UNIT) ? UA_U_PROCESSING : UA_U_EXTENSION;
        u->id = p[3];
        uint8_t n = p[6];
        if (7 + n > dl) n = (uint8_t)(dl - 7);
        ua_unit_inputs(u, p + 7, n);
    } else if (v2 && st == UA2_AC_CLOCK_SOURCE && dl >= 7) {
        u->kind = UA_U_CLK_SRC;
        u->id = p[3];
        u->clk_attr = p[4];
        u->clk_ctrl = p[5];
    } else if (v2 && st == UA2_AC_CLOCK_SELECTOR && dl >= 6) {
        u->kind = UA_U_CLK_SEL;
        u->id = p[3];
        uint8_t n = p[4];
        if (5 + n > dl) n = (uint8_t)(dl - 5);
        ua_unit_inputs(u, p + 5, n);
        if (5 + n < dl) u->clk_ctrl = p[5 + n];
    } else if (v2 && st == UA2_AC_CLOCK_MULTIPLIER && dl >= 5) {
        u->kind = UA_U_CLK_MUL;
        u->id = p[3];
        u->source = p[4];
    } else if (v2 && st == UA2_AC_SAMPLE_RATE_CONVERTER && dl >= 5) {
        u->kind = UA_U_SRC;
        u->id = p[3];
        u->source = p[4];
    } else {
        return;
    }
    ua->unit_count++;
}

static void ua_parse_as(struct ua_device *ua, struct ua_alt *alt, const uint8_t *p, uint8_t dl) {
    uint8_t st = p[2];
    if (st == UA_AS_GENERAL) {
        if (ua->uac2 && dl >= 11) {
            alt->terminal_link = p[3];
            alt->format_type = p[5];
            alt->formats = ua_le32(p + 6);
            alt->channels = p[10];
            alt->format_tag = (alt->format_type == 1 && (alt->formats & 1u)) ? UA_FORMAT_PCM : 0xFFFF;
        } else if (!ua->uac2 && dl >= 7) {
            alt->terminal_link = p[3];
            alt->format_tag = (uint16_t)(p[5] | (p[6] << 8));
        }
    } else if (st == UA_AS_FORMAT_TYPE && dl >= 6 && p[3] == 1) {
        if (ua->uac2) {
            alt->subframe = p[4];
            alt->bits = p[5];
        } else if (dl >= 8) {
            alt->channels = p[4];
            alt->subframe = p[5];
            alt->bits = p[6];
            uint8_t nfreq = p[7];
            if (nfreq == 0 && dl >= 14) {
                alt->continuous = 1;
                alt->rate_min = ua_le24(p + 8);
                alt->rate_max = ua_le24(p + 11);
            } else {
                for (uint8_t f = 0; f < nfreq && alt->rate_count < UA_MAX_RATES && 8 + 3 * f + 3 <= dl; f++)
                    alt->rates[alt->rate_count++] = ua_le24(p + 8 + 3 * f);
            }
        }
    }
}

static bool ua_iface_in_function(struct ua_device *ua, uint8_t iface) {
    if (!ua->iface_mask) return true;
    return iface < 32 && (ua->iface_mask & (1u << iface));
}

static void ua_parse(struct ua_device *ua, const uint8_t *cfg, uint16_t len) {
    const uint8_t *p = cfg;
    const uint8_t *end = cfg + len;
    uint8_t cur_iface = 0xFF, cur_alt = 0;
    bool in_ac = false;
    struct ua_alt *alt = NULL;
    bool vendor_ok = (ua->quirks.flags & AUDIO_USB_Q_VENDOR_CLASS) != 0;
    uint8_t total = 0;

    ua->ac_iface = 0xFF;
    while (p + 2 <= end) {
        uint8_t dl = p[0];
        uint8_t dt = p[1];
        if (dl < 2 || p + dl > end) break;

        if (dt == USB_DESC_INTERFACE && dl >= 9) {
            cur_iface = p[2];
            cur_alt = p[3];
            alt = NULL;
            in_ac = false;
            bool audio = ua_iface_in_function(ua, cur_iface) && (p[5] == USB_CLASS_AUDIO || (vendor_ok && p[5] == 0xFF));
            if (audio && p[6] == USB_SUBCLASS_AUDIOCONTROL) {
                if (ua->ac_iface == 0xFF) {
                    ua->ac_iface = cur_iface;
                    ua->uac_protocol = p[7];
                    ua->uac2 = (p[7] == 0x20);
                }
                in_ac = (cur_iface == ua->ac_iface);
            } else if (audio && p[6] == USB_SUBCLASS_AUDIOSTREAMING && cur_alt != 0 && total < UA_MAX_ALTS) {
                alt = &ua->alts[total++];
                memset(alt, 0, sizeof(*alt));
                alt->iface = cur_iface;
                alt->alt = cur_alt;
            }
        } else if (dt == UA_CS_INTERFACE && dl >= 3) {
            if (in_ac) ua_parse_ac(ua, p, dl);
            else if (alt) ua_parse_as(ua, alt, p, dl);
        } else if (dt == USB_DESC_ENDPOINT && dl >= 7 && alt) {
            uint8_t attr = p[3];
            uint8_t usage = (attr >> 4) & 0x3;
            if ((attr & 0x3) == USB_EP_XFER_ISO) {
                if (usage == 1) {
                    if (!alt->fb_addr) {
                        alt->fb_addr = p[2];
                        alt->fb_attr = attr;
                        alt->fb_mps = (uint16_t)(p[4] | (p[5] << 8));
                        alt->fb_interval = p[6];
                    }
                } else if (!alt->ep_addr) {
                    alt->ep_addr = p[2];
                    alt->ep_attr = attr;
                    alt->ep_mps = (uint16_t)(p[4] | (p[5] << 8));
                    alt->ep_interval = p[6];
                    alt->ep_refresh = (dl >= 9) ? p[7] : 0;
                    alt->sync_ep = (dl >= 9) ? p[8] : 0;
                    alt->direction = (p[2] & 0x80) ? AUDIO_DIR_CAPTURE : AUDIO_DIR_PLAYBACK;
                }
            }
        } else if (dt == UA_CS_ENDPOINT && dl >= 4 && alt && alt->ep_addr) {
            if (p[2] == 1 && !ua->uac2) alt->freq_ctl = (p[3] & 0x01) ? 1 : 0;
        }
        p += dl;
    }

    ua->alt_count = 0;
    for (uint8_t i = 0; i < total; i++) {
        struct ua_alt *a = &ua->alts[i];
        bool pcm = (a->format_tag == UA_FORMAT_PCM || a->format_tag == 0);
        bool sizes_ok = (a->subframe >= 2 && a->subframe <= 4) && a->channels >= 1 && a->channels <= AUDIO_MAX_CHANNELS;
        if (!a->ep_addr || !pcm || !sizes_ok) continue;
        if (a->fb_addr == 0 && a->sync_ep && (a->sync_ep & 0x80)) {
            a->fb_addr = a->sync_ep;
            a->fb_attr = 0x11;
            a->fb_mps = ua->hs ? 4 : 3;
            a->fb_interval = 1;
        }
        a->valid = 1;
        if (ua->alt_count != i) ua->alts[ua->alt_count] = *a;
        ua->alt_count++;
    }
}

static uint8_t ua_format_of(const struct ua_alt *a) {
    switch (a->subframe) {
        case 2: return AUDIO_FMT_S16_LE;
        case 3: return AUDIO_FMT_S24_3LE;
        case 4: return (a->bits && a->bits <= 24) ? AUDIO_FMT_S24_LE : AUDIO_FMT_S32_LE;
        default: return 0xFF;
    }
}

static bool ua_alt_supports_rate(const struct ua_alt *a, uint32_t rate) {
    if (a->continuous) return rate >= a->rate_min && rate <= a->rate_max;
    for (uint8_t i = 0; i < a->rate_count; i++)
        if (a->rates[i] == rate) return true;
    return false;
}

static uint8_t ua_resolve_clock(struct ua_device *ua, uint8_t id, int depth) {
    if (depth > 8) return 0;
    struct ua_unit *u = ua_unit_get(ua, id);
    if (!u) return 0;
    if (u->kind == UA_U_CLK_SRC) return id;
    if (u->kind == UA_U_CLK_MUL) return ua_resolve_clock(ua, u->source, depth + 1);
    if (u->kind == UA_U_CLK_SEL) {
        uint8_t sel = 1;
        uint8_t v = 0;
        if (ua_control(ua, 0xA1, UA2_CUR, (uint16_t)(UA2_CX_CLOCK_SELECTOR_CONTROL << 8),
                       (uint16_t)((id << 8) | ua->ac_iface), 1, &v) == 0 && v >= 1 && v <= u->nr_inputs)
            sel = v;
        if (!u->nr_inputs) return 0;
        return ua_resolve_clock(ua, u->inputs[sel - 1], depth + 1);
    }
    return 0;
}

static void ua_add_rate(struct ua_alt *a, uint32_t r) {
    if (!r) return;
    for (uint8_t i = 0; i < a->rate_count; i++)
        if (a->rates[i] == r) return;
    if (a->rate_count < UA_MAX_RATES) a->rates[a->rate_count++] = r;
}

static void ua_read_clock_rates(struct ua_device *ua, struct ua_alt *a) {
    uint8_t buf[UA_CTL_MAX];
    uint16_t idx = (uint16_t)((a->clock_id << 8) | ua->ac_iface);
    uint16_t val = (uint16_t)(UA2_CS_SAM_FREQ_CONTROL << 8);
    a->rate_count = 0;
    a->continuous = 0;

    if (a->clock_id && ua_control(ua, 0xA1, UA2_RANGE, val, idx, 2, buf) == 0) {
        uint16_t n = (uint16_t)(buf[0] | (buf[1] << 8));
        uint16_t want = (uint16_t)(2 + 12 * n);
        if (n > 0 && want > UA_CTL_MAX) {
            n = (UA_CTL_MAX - 2) / 12;
            want = (uint16_t)(2 + 12 * n);
        }
        if (n > 0 && ua_control(ua, 0xA1, UA2_RANGE, val, idx, want, buf) == 0) {
            for (uint16_t i = 0; i < n; i++) {
                uint32_t mn = ua_le32(buf + 2 + 12 * i);
                uint32_t mx = ua_le32(buf + 6 + 12 * i);
                uint32_t rs = ua_le32(buf + 10 + 12 * i);
                if (mn == mx) {
                    ua_add_rate(a, mn);
                    continue;
                }
                for (size_t k = 0; k < sizeof(ua_std_rates) / sizeof(ua_std_rates[0]); k++) {
                    uint32_t r = ua_std_rates[k];
                    if (r < mn || r > mx) continue;
                    if (rs && (r - mn) % rs) continue;
                    ua_add_rate(a, r);
                }
            }
        }
    }
    if (a->rate_count == 0 && a->clock_id) {
        if (ua_control(ua, 0xA1, UA2_CUR, val, idx, 4, buf) == 0) ua_add_rate(a, ua_le32(buf));
    }
    if (a->rate_count == 0) ua_add_rate(a, 48000);
}

static bool ua_chain_reaches(struct ua_device *ua, uint8_t from, uint8_t target, int depth) {
    if (depth > 16 || !from) return false;
    if (from == target) return true;
    struct ua_unit *u = ua_unit_get(ua, from);
    if (!u || u->kind == UA_U_IT) return false;
    if (u->nr_inputs > 1) {
        for (uint8_t i = 0; i < u->nr_inputs; i++)
            if (ua_chain_reaches(ua, u->inputs[i], target, depth + 1)) return true;
        return false;
    }
    return ua_chain_reaches(ua, u->source, target, depth + 1);
}

static bool ua_take_fu(struct ua_device *ua, struct ua_unit *u, struct ua_fu *fu) {
    if (u->kind != UA_U_FEATURE) return false;
    if (!(u->vol_master || u->vol_ch || u->mute_master || u->mute_ch)) return false;
    fu->id = u->id;
    fu->channel = (u->vol_master || (!u->vol_ch && u->mute_master)) ? 0 : 1;
    fu->has_volume = fu->channel ? u->vol_ch : u->vol_master;
    fu->has_mute = (u->mute_master || u->mute_ch) ? 1 : 0;
    (void)ua;
    return true;
}

static void ua_walk_fu(struct ua_device *ua, uint8_t start, struct ua_fu *fu) {
    uint8_t cur = start;
    for (int d = 0; d < 16 && cur; d++) {
        struct ua_unit *u = ua_unit_get(ua, cur);
        if (!u) return;
        if (ua_take_fu(ua, u, fu)) return;
        if (u->kind == UA_U_IT) return;
        cur = u->source;
    }
}

static void ua_pick_feature_units(struct ua_device *ua) {
    memset(ua->fu, 0, sizeof(ua->fu));
    for (int i = 0; i < ua->alt_count; i++) {
        struct ua_alt *a = &ua->alts[i];
        struct ua_fu *fu = &ua->fu[a->direction];
        if (fu->id) continue;
        if (a->direction == AUDIO_DIR_PLAYBACK) {
            for (uint8_t k = 0; k < ua->unit_count && !fu->id; k++) {
                struct ua_unit *ot = &ua->units[k];
                if (ot->kind != UA_U_OT || ot->terminal_type == UA_TERMINAL_USB_STREAMING) continue;
                if (a->terminal_link && !ua_chain_reaches(ua, ot->source, a->terminal_link, 0)) continue;
                ua_walk_fu(ua, ot->source, fu);
            }
        } else {
            struct ua_unit *ot = ua_unit_get(ua, a->terminal_link);
            if (ot && ot->kind == UA_U_OT) ua_walk_fu(ua, ot->source, fu);
        }
        if (fu->id) {
            fu->channels = a->channels;
            for (int j = 0; j < ua->alt_count; j++)
                if (ua->alts[j].direction == a->direction && ua->alts[j].channels > fu->channels) fu->channels = ua->alts[j].channels;
        }
    }
}

static int16_t ua_get_vol1(struct ua_device *ua, struct ua_fu *fu, uint8_t req) {
    uint8_t b[2] = { 0, 0 };
    int r = ua_control(ua, 0xA1, req, (uint16_t)((UAC_FU_VOLUME_CONTROL << 8) | fu->channel),
                       (uint16_t)((fu->id << 8) | ua->ac_iface), 2, b);
    if (r != 0) return (int16_t)0x8000;
    return (int16_t)(b[0] | (b[1] << 8));
}

static void ua_read_volume_range(struct ua_device *ua, struct ua_fu *fu) {
    if (!fu->id || !fu->has_volume || (ua->quirks.flags & AUDIO_USB_Q_NO_VOLUME)) {
        fu->has_volume = 0;
        return;
    }
    int16_t mn, mx, rs;
    if (ua->uac2) {
        uint8_t b[8];
        if (ua_control(ua, 0xA1, UA2_RANGE, (uint16_t)((UAC_FU_VOLUME_CONTROL << 8) | fu->channel),
                       (uint16_t)((fu->id << 8) | ua->ac_iface), 8, b) != 0) {
            fu->has_volume = 0;
            return;
        }
        mn = (int16_t)(b[2] | (b[3] << 8));
        mx = (int16_t)(b[4] | (b[5] << 8));
        rs = (int16_t)(b[6] | (b[7] << 8));
    } else {
        mn = ua_get_vol1(ua, fu, UAC_GET_MIN);
        mx = ua_get_vol1(ua, fu, UAC_GET_MAX);
        rs = ua_get_vol1(ua, fu, UAC_GET_RES);
        if (mn == (int16_t)0x8000 && mx == (int16_t)0x8000) {
            fu->has_volume = 0;
            return;
        }
    }
    if (mx <= mn) {
        fu->has_volume = 0;
        return;
    }
    fu->vol_min = mn;
    fu->vol_max = mx;
    fu->vol_res = rs > 0 ? rs : 1;
}

static int ua_set_feature(struct ua_device *ua, struct ua_fu *fu, uint8_t control, uint8_t ch, const uint8_t *val, uint16_t len) {
    uint8_t b[4];
    memcpy(b, val, len);
    return ua_control(ua, 0x21, UAC_SET_CUR, (uint16_t)((control << 8) | ch), (uint16_t)((fu->id << 8) | ua->ac_iface), len, b);
}

static int ua_dev_set_volume(struct audio_device *adev, uint8_t direction, uint8_t volume) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    struct ua_fu *fu = &ua->fu[direction];
    if (!fu->has_volume || ua->gone) return AUDIO_ERR_UNSUPPORTED;

    int32_t lo = fu->vol_min;
    int32_t hi = fu->vol_max;
    if (!(ua->quirks.flags & AUDIO_USB_Q_VOLUME_LINEAR) && hi - lo > 50 * 256) lo = hi - 50 * 256;
    int32_t v = (volume == 0) ? fu->vol_min : lo + ((hi - lo) * (int32_t)volume) / AUDIO_VOLUME_MAX;
    if (fu->vol_res > 1) v = lo + ((v - lo) / fu->vol_res) * fu->vol_res;
    uint8_t buf[2] = { (uint8_t)v, (uint8_t)(v >> 8) };

    int r = 0;
    if (fu->channel == 0) {
        r = ua_set_feature(ua, fu, UAC_FU_VOLUME_CONTROL, 0, buf, 2);
    } else {
        uint8_t n = fu->channels ? fu->channels : 2;
        for (uint8_t ch = 1; ch <= n && r == 0; ch++) r = ua_set_feature(ua, fu, UAC_FU_VOLUME_CONTROL, ch, buf, 2);
    }
    if (r == 0 && fu->has_mute && !(ua->quirks.flags & AUDIO_USB_Q_NO_MUTE)) {
        uint8_t m = (volume == 0) || (adev->streams[direction].muted && !adev->streams[direction].sw_mute);
        ua_set_feature(ua, fu, UAC_FU_MUTE_CONTROL, 0, &m, 1);
    }
    return r == 0 ? AUDIO_OK : AUDIO_ERR_UNSUPPORTED;
}

static int ua_dev_set_mute(struct audio_device *adev, uint8_t direction, bool muted) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    struct ua_fu *fu = &ua->fu[direction];
    if (!fu->has_mute || (ua->quirks.flags & AUDIO_USB_Q_NO_MUTE) || ua->gone) return AUDIO_ERR_UNSUPPORTED;
    uint8_t m = muted ? 1 : 0;
    return ua_set_feature(ua, fu, UAC_FU_MUTE_CONTROL, 0, &m, 1) == 0 ? AUDIO_OK : AUDIO_ERR_UNSUPPORTED;
}

static uint32_t ua_alt_packet_max(struct ua_device *ua, const struct ua_alt *a) {
    uint32_t mps = a->ep_mps & 0x7FFu;
    uint32_t mult = ua->hs ? (((a->ep_mps >> 11) & 3u) + 1u) : 1u;
    return mps * mult;
}

static uint8_t ua_interval_uf(struct ua_device *ua, uint8_t binterval) {
    if (!ua->hs) return 8;
    if (binterval < 1) binterval = 1;
    if (binterval > 4) binterval = 4;
    return (uint8_t)(1u << (binterval - 1u));
}

static int ua_iso_open_ep(struct ua_device *ua, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval) {
    struct usb_core_driver *core = ua_core();
    if (!core || !core->iso_open) return USB_ISO_ERR;
    struct usb_endpoint_info ep = {
        .address = addr,
        .attributes = attr,
        .max_packet_size = mps,
        .interval = ua->hs ? (interval ? interval : 1) : 1,
    };
    return core->iso_open(ua->usb, &ep);
}

static void ua_iso_close_ep(struct ua_device *ua, uint8_t addr) {
    struct usb_core_driver *core = ua_core();
    if (core && core->iso_close && ua->usb) core->iso_close(ua->usb, addr);
}

static void ua_stream_reset(struct ua_stream *s) {
    for (int k = 0; k < UA_REQS; k++) {
        s->inflight[k] = 0;
        s->prepared[k] = 0;
        s->req[k].done = 0;
        s->req[k].queued = 0;
    }
    s->fb_inflight = 0;
    s->next_sub = 0;
    s->next_done = 0;
    s->acc = 0;
    s->consecutive_errors = 0;
}

static int ua_stream_open_eps(struct ua_device *ua, uint8_t dir) {
    struct ua_stream *s = &ua->st[dir];
    struct ua_alt *a = &ua->alts[s->cur_alt];
    int r = ua_iso_open_ep(ua, a->ep_addr, a->ep_attr, a->ep_mps, a->ep_interval);
    if (r != USB_ISO_OK) return r;
    s->opened = 1;
    s->fb_opened = 0;
    if (dir == AUDIO_DIR_PLAYBACK && a->fb_addr && !(ua->quirks.flags & AUDIO_USB_Q_NO_FEEDBACK)) {
        if (ua_iso_open_ep(ua, a->fb_addr, a->fb_attr, a->fb_mps ? a->fb_mps : 4, a->fb_interval) == USB_ISO_OK)
            s->fb_opened = 1;
    }
    ua_stream_reset(s);
    s->last_progress_ms = audio_uptime_ms();
    return USB_ISO_OK;
}

static void ua_stream_close_eps(struct ua_device *ua, uint8_t dir) {
    struct ua_stream *s = &ua->st[dir];
    if (s->cur_alt < 0) return;
    struct ua_alt *a = &ua->alts[s->cur_alt];
    if (s->opened) ua_iso_close_ep(ua, a->ep_addr);
    if (s->fb_opened) ua_iso_close_ep(ua, a->fb_addr);
    s->opened = 0;
    s->fb_opened = 0;
    ua_stream_reset(s);
}

static bool ua_alloc_stream_buffers(struct ua_device *ua, uint8_t dir) {
    struct ua_stream *s = &ua->st[dir];
    for (int k = 0; k < UA_REQS; k++) {
        if (s->buf[k]) continue;
        s->buf[k] = audio_dma_alloc(UA_REQ_BYTES, true, &s->buf_phys[k], &s->buf_pages[k]);
        if (!s->buf[k]) return false;
    }
    if (dir == AUDIO_DIR_PLAYBACK && !s->fb_buf) {
        s->fb_buf = audio_dma_alloc(UA_FB_BYTES, true, &s->fb_phys, &s->fb_pages);
        if (!s->fb_buf) return false;
    }
    return true;
}

static void ua_free_buffers(struct ua_device *ua) {
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct ua_stream *s = &ua->st[d];
        for (int k = 0; k < UA_REQS; k++) {
            if (s->buf[k]) audio_dma_free(s->buf[k], s->buf_phys[k], s->buf_pages[k]);
            s->buf[k] = NULL;
        }
        if (s->fb_buf) audio_dma_free(s->fb_buf, s->fb_phys, s->fb_pages);
        s->fb_buf = NULL;
    }
}

static int ua_set_rate(struct ua_device *ua, struct ua_alt *a, uint32_t rate, const char *name) {
    if (ua->quirks.flags & AUDIO_USB_Q_NO_SET_RATE) return 0;
    if (ua->uac2) {
        struct ua_unit *clk = ua_unit_get(ua, a->clock_id);
        if (!clk) return 0;
        bool programmable = (clk->clk_ctrl & 0x3u) == 0x3u;
        if (!programmable && a->rate_count <= 1 && !(ua->quirks.flags & AUDIO_USB_Q_SET_RATE_ALWAYS)) return 0;
        uint8_t b[4] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24) };
        uint16_t idx = (uint16_t)((a->clock_id << 8) | ua->ac_iface);
        if (ua_control(ua, 0x21, UA2_CUR, (uint16_t)(UA2_CS_SAM_FREQ_CONTROL << 8), idx, 4, b) != 0) {
            LOG_WARNING("%s: clock %u: setting %u Hz failed", name, (unsigned)a->clock_id, rate);
            return programmable ? -1 : 0;
        }
        uint8_t g[4];
        if (ua_control(ua, 0xA1, UA2_CUR, (uint16_t)(UA2_CS_SAM_FREQ_CONTROL << 8), idx, 4, g) == 0 && ua_le32(g) != rate)
            LOG_WARNING("%s: clock %u runs at %u Hz instead of %u Hz", name, (unsigned)a->clock_id, ua_le32(g), rate);
        if (!(ua->quirks.flags & AUDIO_USB_Q_CLOCK_NO_VALIDATE)) {
            uint8_t v = 1;
            if (ua_control(ua, 0xA1, UA2_CUR, (uint16_t)(UA2_CS_CLOCK_VALID_CONTROL << 8), idx, 1, &v) == 0 && !v)
                LOG_WARNING("%s: clock %u reports invalid after rate change", name, (unsigned)a->clock_id);
        }
        return 0;
    }
    bool multi_rate = a->continuous || a->rate_count > 1;
    bool want = (a->freq_ctl && multi_rate) || (ua->quirks.flags & AUDIO_USB_Q_SET_RATE_ALWAYS);
    if (!want) return 0;
    uint8_t fr[3] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16) };
    if (ua_control(ua, 0x22, UAC_SET_CUR, (uint16_t)(UAC_EP_SAMPLING_FREQ_CONTROL << 8), a->ep_addr, 3, fr) != 0)
        LOG_WARNING("%s: setting %u Hz on endpoint 0x%02x failed", name, rate, (unsigned)a->ep_addr);
    return 0;
}

static void ua_keepalive(struct ua_device *ua, uint8_t dir);

static void ua_settle(struct ua_device *ua, uint8_t dir, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        audio_delay_ms(1);
        ua_keepalive(ua, dir);
    }
}

static void ua_update_clock_lock(struct ua_device *ua) {
    uint32_t lock = ua->quirks.fixed_rate;
    if (ua->uac2 && !lock) {
        for (uint8_t d = 0; d < AUDIO_DIR_COUNT && !lock; d++) {
            struct ua_stream *s = &ua->st[d];
            if (s->cur_alt < 0) continue;
            uint8_t clk = ua->alts[s->cur_alt].clock_id;
            for (int i = 0; i < ua->alt_count; i++) {
                struct ua_alt *o = &ua->alts[i];
                if (o->direction != d && o->clock_id == clk && ua_alt_supports_rate(o, s->rate)) {
                    lock = s->rate;
                    break;
                }
            }
        }
    }
    ua->adev.fixed_rate = lock;
}

static int ua_dev_open(struct audio_device *adev, uint8_t direction, const struct audio_format *fmt) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT || !fmt) return AUDIO_ERR_PARAM;
    if (ua->gone || !ua_usb_alive(ua)) return AUDIO_ERR_NODEV;
    struct ua_stream *s = &ua->st[direction];
    if (s->cur_alt >= 0) {
        ua_stream_close_eps(ua, direction);
        s->cur_alt = -1;
        ua_keepalive(ua, direction);
    }

    int best = -1;
    bool best_ok = false;
    uint32_t best_pm = 0;
    for (int i = 0; i < ua->alt_count; i++) {
        struct ua_alt *a = &ua->alts[i];
        if (a->direction != direction) continue;
        if (a->channels != fmt->channels || ua_format_of(a) != fmt->format) continue;
        if (!ua_alt_supports_rate(a, fmt->sample_rate)) continue;
        if (ua->quirks.usb_alt > 0 && a->alt != ua->quirks.usb_alt) continue;
        uint32_t pps = ua->hs ? 8000u / ua_interval_uf(ua, a->ep_interval) : 1000u;
        uint32_t need = ((fmt->sample_rate + pps - 1u) / pps + 1u) * audio_frame_bytes(fmt);
        uint32_t pm = ua_alt_packet_max(ua, a);
        bool ok = pm >= need;
        if (best < 0 || (ok && !best_ok) || (ok == best_ok && (ok ? pm < best_pm : pm > best_pm))) {
            best = i;
            best_ok = ok;
            best_pm = pm;
        }
    }
    if (best < 0) return AUDIO_ERR_UNSUPPORTED;
    struct ua_alt *a = &ua->alts[best];
    struct ua_stream *other = &ua->st[direction ^ 1];
    if (ua->uac2 && other->cur_alt >= 0 && ua->alts[other->cur_alt].clock_id == a->clock_id && other->rate != fmt->sample_rate) {
        LOG_WARNING("%s: clock %u already runs at %u Hz", adev->name, (unsigned)a->clock_id, other->rate);
        return AUDIO_ERR_BUSY;
    }

    if (ua->uac2 && ua_set_rate(ua, a, fmt->sample_rate, adev->name) != 0) return AUDIO_ERR_IO;
    ua_keepalive(ua, direction);

    if (ua_control(ua, 0x01, UA_REQ_SET_INTERFACE, a->alt, a->iface, 0, NULL) != 0) {
        LOG_WARNING("%s: SET_INTERFACE %u alt %u failed", adev->name, (unsigned)a->iface, (unsigned)a->alt);
        return AUDIO_ERR_IO;
    }
    ua_settle(ua, direction, ua->quirks.delay_ms ? ua->quirks.delay_ms : 5);
    if (!ua->uac2) ua_set_rate(ua, a, fmt->sample_rate, adev->name);
    ua_keepalive(ua, direction);

    if (!ua_alloc_stream_buffers(ua, direction)) {
        ua_control(ua, 0x01, UA_REQ_SET_INTERFACE, 0, a->iface, 0, NULL);
        return AUDIO_ERR_NOMEM;
    }

    s->cur_alt = (int8_t)best;
    s->rate = fmt->sample_rate;
    s->frame_bytes = audio_frame_bytes(fmt);
    s->interval_uf = ua_interval_uf(ua, a->ep_interval);
    s->pps = ua->hs ? 8000u / s->interval_uf : 1000u;
    s->pkt_max = (uint16_t)ua_alt_packet_max(ua, a);
    uint32_t per_req = s->pps * UA_REQ_MS / 1000u;
    if (per_req < 1) per_req = 1;
    if (per_req > USB_ISO_MAX_PACKETS) per_req = USB_ISO_MAX_PACKETS;
    while (per_req > 1 && per_req * s->pkt_max > UA_REQ_BYTES) per_req--;
    s->pkts_per_req = (uint16_t)per_req;
    s->nominal_q16 = (uint32_t)(((uint64_t)s->rate << 16) / s->pps);
    s->fb_q16 = 0;
    s->paused = 0;
    s->active = 0;
    s->submits = s->completed = s->errors = s->busy = s->restarts = s->fb_updates = 0;
    s->bytes = 0;

    int eps_ok = ua_stream_open_eps(ua, direction);
    ua_keepalive(ua, direction);
    if (eps_ok != USB_ISO_OK) {
        LOG_WARNING("%s: cannot open isochronous endpoint 0x%02x", adev->name, (unsigned)a->ep_addr);
        ua_control(ua, 0x01, UA_REQ_SET_INTERFACE, 0, a->iface, 0, NULL);
        s->cur_alt = -1;
        return AUDIO_ERR_IO;
    }

    adev->streams[direction].hw_buffer_bytes = (uint32_t)(((uint64_t)s->rate * s->frame_bytes / 1000u) * UA_REQ_MS * (UA_REQS + 2));
    ua_update_clock_lock(ua);
    LOG_DEBUG("%s: %s alt %u ep 0x%02x %u Hz, %u packet(s)/s, %u per request, max %u bytes%s", adev->name,
              direction ? "capture" : "playback", (unsigned)a->alt, (unsigned)a->ep_addr, s->rate, s->pps,
              (unsigned)s->pkts_per_req, (unsigned)s->pkt_max, s->fb_opened ? ", feedback" : "");
    return AUDIO_OK;
}

static int ua_dev_close(struct audio_device *adev, uint8_t direction) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    struct ua_stream *s = &ua->st[direction];
    s->active = 0;
    if (s->cur_alt >= 0) {
        uint8_t iface = ua->alts[s->cur_alt].iface;
        if (!ua->gone && ua_usb_alive(ua)) {
            ua_stream_close_eps(ua, direction);
            ua_keepalive(ua, direction);
            ua_control(ua, 0x01, UA_REQ_SET_INTERFACE, 0, iface, 0, NULL);
            ua_keepalive(ua, direction);
        } else {
            s->opened = 0;
            s->fb_opened = 0;
        }
    }
    s->cur_alt = -1;
    ua_update_clock_lock(ua);
    return AUDIO_OK;
}

static void ua_restart_stream(struct ua_device *ua, uint8_t dir, const char *why) {
    struct ua_stream *s = &ua->st[dir];
    s->restarts++;
    if (s->restarts <= 3 || (s->restarts % 50) == 0)
        LOG_WARNING("%s: %s stream %s, restarting (%u)", ua->adev.name, dir ? "capture" : "playback", why, s->restarts);
    ua_stream_close_eps(ua, dir);
    if (ua_stream_open_eps(ua, dir) != USB_ISO_OK) s->active = 0;
}

static uint16_t ua_play_packet_bytes(struct ua_device *ua, struct ua_stream *s) {
    uint32_t frames;
    if (ua->quirks.flags & AUDIO_USB_Q_NO_FRAC) {
        frames = s->nominal_q16 >> 16;
    } else {
        s->acc += s->fb_q16 ? s->fb_q16 : s->nominal_q16;
        frames = s->acc >> 16;
        s->acc &= 0xFFFFu;
    }
    uint32_t bytes = frames * s->frame_bytes;
    uint32_t cap = s->pkt_max - (s->pkt_max % s->frame_bytes);
    if (bytes > cap) bytes = cap;
    return (uint16_t)bytes;
}

static void ua_fb_parse(struct ua_device *ua, struct ua_stream *s, const uint8_t *b, uint16_t len) {
    if (len < 3) return;
    uint32_t v = (len >= 4) ? ua_le32(b) : ua_le24(b);
    uint64_t nominal_ms = ((uint64_t)s->rate << 16) / 1000u;
    uint64_t cand[4];
    int nc = 0;
    if (ua->hs) {
        cand[nc++] = (uint64_t)v * 8u;
        cand[nc++] = (uint64_t)v * 8u / s->interval_uf;
        cand[nc++] = (uint64_t)v << 2;
    } else {
        cand[nc++] = (len == 3 || !(ua->quirks.flags & AUDIO_USB_Q_FB_FULL_SPEED_FMT)) ? ((uint64_t)(v & 0xFFFFFFu) << 2) : (uint64_t)v;
        cand[nc++] = (uint64_t)v;
    }
    for (int i = 0; i < nc; i++) {
        uint64_t f = cand[i];
        if (f < nominal_ms - nominal_ms / 8 || f > nominal_ms + nominal_ms / 8) continue;
        s->fb_q16 = (uint32_t)(f * 1000u / s->pps);
        s->fb_updates++;
        return;
    }
}

static void ua_pump_feedback(struct ua_device *ua, struct ua_stream *s) {
    struct usb_core_driver *core = ua_core();
    struct ua_alt *a = &ua->alts[s->cur_alt];
    if (!s->fb_opened || !s->fb_buf) return;
    if (s->fb_inflight) {
        if (!s->fb_req.done) return;
        s->fb_inflight = 0;
        if (s->fb_req.status == USB_ISO_OK) {
            for (int i = (int)s->fb_req.n_packets - 1; i >= 0; i--) {
                if (s->fb_req.actual[i] < 3) continue;
                ua_fb_parse(ua, s, s->fb_buf + s->fb_req.offsets[i], s->fb_req.actual[i]);
                break;
            }
        }
    }
    uint16_t len = a->fb_mps & 0x7FFu;
    if (len < 3) len = 4;
    if (len > 8) len = 8;
    uint16_t lens[UA_FB_PKTS];
    for (int i = 0; i < UA_FB_PKTS; i++) lens[i] = len;
    usb_iso_request_prepare(&s->fb_req, s->fb_buf, a->fb_addr, UA_FB_PKTS, lens);
    if (core->iso_submit(ua->usb, &s->fb_req) == USB_ISO_OK) s->fb_inflight = 1;
}

static void ua_pump(struct ua_device *ua, uint8_t dir) {
    struct usb_core_driver *core = ua_core();
    struct ua_stream *s = &ua->st[dir];
    if (!core || !core->iso_submit || s->cur_alt < 0 || !s->opened) return;
    struct ua_alt *a = &ua->alts[s->cur_alt];
    struct audio_stream *as = &ua->adev.streams[dir];
    uint64_t now = audio_uptime_ms();

    if (core->iso_poll) core->iso_poll(ua->usb);

    while (s->inflight[s->next_done] && s->req[s->next_done].done) {
        uint8_t k = s->next_done;
        struct usb_iso_request *r = &s->req[k];
        s->inflight[k] = 0;
        s->completed++;
        s->last_progress_ms = now;
        if (r->errors) s->errors += r->errors;
        if (dir == AUDIO_DIR_CAPTURE && r->status == USB_ISO_OK) {
            for (uint16_t i = 0; i < r->n_packets; i++) {
                uint16_t n = r->actual[i];
                if (!n) continue;
                if (n % s->frame_bytes) n = (uint16_t)(n - n % s->frame_bytes);
                if (n) audio_stream_capture(&ua->adev, as, (uint8_t *)r->data + r->offsets[i], n);
                s->bytes += n;
            }
        }
        s->next_done = (uint8_t)((k + 1) % UA_REQS);
    }

    if (s->inflight[s->next_done] && now - s->submit_ms[s->next_done] > UA_STALL_MS + UA_REQ_MS * UA_REQS) {
        ua_restart_stream(ua, dir, "stalled");
        return;
    }

    if (!s->active || s->paused) {
        if (dir == AUDIO_DIR_PLAYBACK) ua_pump_feedback(ua, s);
        return;
    }

    for (int n = 0; n < UA_REQS; n++) {
        uint8_t k = s->next_sub;
        if (s->inflight[k]) break;
        struct usb_iso_request *r = &s->req[k];
        uint16_t lens[USB_ISO_MAX_PACKETS];
        uint32_t total = 0;
        if (s->prepared[k]) {
        } else if (dir == AUDIO_DIR_PLAYBACK) {
            uint32_t acc_save = s->acc;
            for (uint16_t i = 0; i < s->pkts_per_req; i++) {
                lens[i] = ua_play_packet_bytes(ua, s);
                total += lens[i];
            }
            if (total > UA_REQ_BYTES) {
                s->acc = acc_save;
                break;
            }
            audio_stream_render(&ua->adev, as, s->buf[k], total);
            s->bytes += total;
        } else {
            for (uint16_t i = 0; i < s->pkts_per_req; i++) lens[i] = s->pkt_max;
        }
        if (!s->prepared[k]) {
            usb_iso_request_prepare(r, s->buf[k], a->ep_addr, s->pkts_per_req, lens);
            s->prepared[k] = 1;
        }
        int rc = core->iso_submit(ua->usb, r);
        if (rc == USB_ISO_OK) {
            s->prepared[k] = 0;
            s->inflight[k] = 1;
            s->submit_ms[k] = now;
            s->submits++;
            s->consecutive_errors = 0;
            s->next_sub = (uint8_t)((k + 1) % UA_REQS);
            continue;
        }
        if (rc == USB_ISO_BUSY) {
            s->busy++;
            break;
        }
        s->errors++;
        if (++s->consecutive_errors == 100) LOG_WARNING("%s: isochronous submit keeps failing (%d)", ua->adev.name, rc);
        if (s->consecutive_errors > 200 && rc == USB_ISO_ERR) ua_restart_stream(ua, dir, "rejected");
        break;
    }
    if (dir == AUDIO_DIR_PLAYBACK) ua_pump_feedback(ua, s);
}

static void ua_keepalive(struct ua_device *ua, uint8_t dir) {
    uint8_t o = (uint8_t)(dir ^ 1u);
    if (o < AUDIO_DIR_COUNT && ua->st[o].opened && ua->st[o].active && !ua->gone) ua_pump(ua, o);
}

static int ua_dev_start(struct audio_device *adev, uint8_t direction) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    struct ua_stream *s = &ua->st[direction];
    if (s->cur_alt < 0) return AUDIO_ERR_PARAM;
    if (!s->opened && ua_stream_open_eps(ua, direction) != USB_ISO_OK) return AUDIO_ERR_IO;
    s->active = 1;
    s->paused = 0;
    s->last_progress_ms = audio_uptime_ms();
    ua_pump(ua, direction);
    return AUDIO_OK;
}

static int ua_dev_stop(struct audio_device *adev, uint8_t direction) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    struct ua_stream *s = &ua->st[direction];
    s->active = 0;
    if (s->opened && !ua->gone) ua_stream_close_eps(ua, direction);
    return AUDIO_OK;
}

static int ua_dev_pause(struct audio_device *adev, uint8_t direction, bool paused) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    ua->st[direction].paused = paused ? 1 : 0;
    return AUDIO_OK;
}

static uint32_t ua_dev_get_position(struct audio_device *adev, uint8_t direction) {
    if (direction >= AUDIO_DIR_COUNT) return 0;
    return (uint32_t)adev->streams[direction].hw_bytes;
}

static void ua_dev_poll(struct audio_device *adev) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    if (ua->gone) return;
    if (!ua->usb || !ua->usb->valid) {
        ua->st[0].active = ua->st[1].active = 0;
        return;
    }
    for (uint8_t d = 0; d < AUDIO_DIR_COUNT; d++)
        if (ua->st[d].opened && (ua->st[d].active || ua->st[d].inflight[ua->st[d].next_done])) ua_pump(ua, d);
}

static const char *ua_sync_name(uint8_t attr) {
    switch ((attr >> 2) & 3u) {
        case UA_EP_SYNC_ASYNC: return "async";
        case UA_EP_SYNC_ADAPTIVE: return "adaptive";
        case UA_EP_SYNC_SYNC: return "sync";
        default: return "none";
    }
}

static const char *ua_ctrl_name(struct usb_controller *c) {
    if (!c) return "?";
    switch (c->type) {
        case USB_TYPE_XHCI: return "xHCI";
        case USB_TYPE_EHCI: return "EHCI";
        case USB_TYPE_OHCI: return "OHCI";
        default: return "UHCI";
    }
}

static void ua_dev_describe(struct audio_device *adev, void (*out)(const char *line)) {
    struct ua_device *ua = (struct ua_device *)adev->priv;
    char line[192];
    char tmp[128];
    snprintf(line, sizeof(line), "usb %04x:%04x addr %u %s %s-speed, UAC%u, function %u, ac-iface %u \"%s\" \"%s\"",
             (unsigned)ua->vid, (unsigned)ua->pid, (unsigned)ua->address, ua_ctrl_name(ua->ctrl), ua->hs ? "high" : "full",
             ua->uac2 ? 2u : 1u, (unsigned)ua->fn_index, (unsigned)ua->ac_iface, ua->usb ? ua->usb->vendor_str : "",
             ua->usb ? ua->usb->product_str : "");
    out(line);
    if (ua->usb && ua->usb->function_count > 1) {
        size_t pos = (size_t)snprintf(line, sizeof(line), "composite device, functions:");
        for (int i = 0; i < ua->usb->function_count && pos < sizeof(line); i++) {
            struct usb_function_info *f = &ua->usb->functions[i];
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %d:%s%s%s", i, usb_function_name(f),
                                    f->driver ? "->" : "", f->driver ? f->driver : "");
        }
        out(line);
    }
    audio_quirk_flags_string(AUDIO_QUIRK_USB_AUDIO, ua->quirks.flags, tmp, sizeof(tmp));
    snprintf(line, sizeof(line), "quirks: %s", tmp);
    out(line);
    for (int i = 0; i < ua->alt_count; i++) {
        struct ua_alt *a = &ua->alts[i];
        size_t pos = (size_t)snprintf(line, sizeof(line), " iface %u alt %u %s ep 0x%02x %s mps %u int %u %uch %u/%u-bit term %u",
                                      (unsigned)a->iface, (unsigned)a->alt, a->direction ? "in " : "out", (unsigned)a->ep_addr,
                                      ua_sync_name(a->ep_attr), (unsigned)(a->ep_mps & 0x7FF), (unsigned)a->ep_interval,
                                      (unsigned)a->channels, (unsigned)a->bits, (unsigned)a->subframe * 8, (unsigned)a->terminal_link);
        if (a->fb_addr) pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " fb 0x%02x", (unsigned)a->fb_addr);
        if (a->clock_id) pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " clock %u", (unsigned)a->clock_id);
        if (a->freq_ctl) pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " freq-ctl");
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " rates");
        if (a->continuous) snprintf(line + pos, sizeof(line) - pos, " %u-%u", a->rate_min, a->rate_max);
        else
            for (uint8_t r = 0; r < a->rate_count && pos < sizeof(line); r++)
                pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %u", a->rates[r]);
        out(line);
    }
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct ua_fu *fu = &ua->fu[d];
        if (!adev->has_direction[d]) continue;
        snprintf(line, sizeof(line), "%s feature unit %u ch %u volume %s (%d..%d step %d) mute %s", d ? "capture " : "playback",
                 (unsigned)fu->id, (unsigned)fu->channel, fu->has_volume ? "yes" : "no", (int)fu->vol_min, (int)fu->vol_max,
                 (int)fu->vol_res, fu->has_mute ? "yes" : "no");
        out(line);
        struct ua_stream *s = &ua->st[d];
        snprintf(line, sizeof(line), "%s stream: alt %d %s %u Hz, %u pkt/s x %u per request, submits %u done %u busy %u errors %u restarts %u",
                 d ? "capture " : "playback", (int)(s->cur_alt >= 0 ? ua->alts[s->cur_alt].alt : -1),
                 s->active ? (s->paused ? "paused" : "running") : "idle", s->rate, s->pps, (unsigned)s->pkts_per_req, s->submits,
                 s->completed, s->busy, s->errors, s->restarts);
        out(line);
        if (s->fb_opened || s->fb_updates) {
            snprintf(line, sizeof(line), "%s feedback: %u updates, %u.%04u frames/packet (nominal %u.%04u)", d ? "capture " : "playback",
                     s->fb_updates, s->fb_q16 >> 16, (unsigned)(((uint64_t)(s->fb_q16 & 0xFFFF) * 10000u) >> 16),
                     s->nominal_q16 >> 16, (unsigned)(((uint64_t)(s->nominal_q16 & 0xFFFF) * 10000u) >> 16));
            out(line);
        }
    }
}

static const struct audio_device_ops ua_ops = {
    .open = ua_dev_open,
    .close = ua_dev_close,
    .start = ua_dev_start,
    .stop = ua_dev_stop,
    .pause = ua_dev_pause,
    .get_position = ua_dev_get_position,
    .set_volume = ua_dev_set_volume,
    .set_mute = ua_dev_set_mute,
    .poll = ua_dev_poll,
    .describe = ua_dev_describe,
    .remove = NULL,
};

static void ua_fill_caps(struct ua_device *ua) {
    struct audio_device *a = &ua->adev;
    for (int i = 0; i < ua->alt_count; i++) {
        struct ua_alt *alt = &ua->alts[i];
        uint8_t d = alt->direction;
        uint8_t f = ua_format_of(alt);
        if (f == 0xFF) continue;
        a->has_direction[d] = 1;
        a->formats[d] |= (uint16_t)AUDIO_FMT_MASK(f);
        if (alt->channels > a->max_channels[d]) a->max_channels[d] = alt->channels;
        if (alt->continuous) {
            a->rates[d] |= AUDIO_RATE_CONTINUOUS;
            if (!a->rate_min[d] || alt->rate_min < a->rate_min[d]) a->rate_min[d] = alt->rate_min;
            if (alt->rate_max > a->rate_max[d]) a->rate_max[d] = alt->rate_max;
        }
        for (uint8_t r = 0; r < alt->rate_count; r++) a->rates[d] |= audio_rate_to_mask(alt->rates[r]);
    }
    if (ua->quirks.fixed_rate) a->fixed_rate = ua->quirks.fixed_rate;
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        if (ua->quirks.flags & AUDIO_USB_Q_SWAP_LR) a->streams[d].swap_lr = 1;
        if ((ua->quirks.flags & AUDIO_USB_Q_NO_VOLUME) || !ua->fu[d].has_volume) a->streams[d].force_sw_volume = 1;
    }
}

static bool ua_fetch_config(struct ua_device *ua, const uint8_t **cfg, uint16_t *out_len) {
    if (ua->usb->config && ua->usb->config_len >= 9) {
        *cfg = ua->usb->config;
        *out_len = ua->usb->config_len;
        return true;
    }
    memset(ua_cfg_buf, 0, sizeof(ua_cfg_buf));
    if (ua_control(ua, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DESC_CONFIGURATION << 8), 0, 9, ua_cfg_buf) != 0)
        return false;
    uint16_t total = (uint16_t)(ua_cfg_buf[2] | (ua_cfg_buf[3] << 8));
    if (total < 9) return false;
    if (total > UA_CTL_MAX) total = UA_CTL_MAX;
    if (ua_control(ua, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DESC_CONFIGURATION << 8), 0, total, ua_cfg_buf) != 0)
        return false;
    *cfg = ua_cfg_buf;
    *out_len = total;
    return true;
}

static int ua_free_slot(void) {
    uint64_t now = audio_uptime_ms();
    for (int i = 0; i < UA_MAX_DEVICES; i++)
        if (!ua_in_use[i] && (ua_freed_ms[i] == 0 || now - ua_freed_ms[i] >= UA_REUSE_MS)) return i;
    return -1;
}

static const char *ua_function_label(struct usb_device *usb) {
    if (!usb || usb->function_count <= 1) return "";
    for (int i = 0; i < usb->function_count; i++)
        if (usb->functions[i].func_class == 0x0E) return "webcam ";
    return "composite ";
}

static bool ua_attach(struct usb_device *usb, int fn_index) {
    int slot = ua_free_slot();
    if (slot < 0) return false;

    struct ua_device *ua = &ua_pool[slot];
    ua_free_buffers(ua);
    memset(ua, 0, sizeof(*ua));
    ua->usb = usb;
    ua->ctrl = usb->ctrl;
    ua->address = usb->address;
    ua->generation = usb->generation;
    ua->vid = usb->desc.idVendor;
    ua->pid = usb->desc.idProduct;
    ua->slot = (uint8_t)slot;
    ua->st[0].cur_alt = ua->st[1].cur_alt = -1;
    ua->hs = usb->speed_id >= 3;
    ua->fn_index = (int8_t)fn_index;
    ua->fn = (fn_index >= 0 && fn_index < usb->function_count) ? &usb->functions[fn_index] : NULL;
    ua->iface_mask = ua->fn ? ua->fn->iface_mask : 0;

    struct audio_quirk_ids ids = { .usb_id = AUDIO_ID(ua->vid, ua->pid),
                                   .revision = usb->desc.bcdDevice, .has_revision = 1 };
    audio_quirk_resolve(AUDIO_QUIRK_USB_AUDIO, &ids, 0, &ua->quirks);
    if (ua->quirks.flags & AUDIO_USB_Q_SKIP) {
        LOG_INFO("usb %04x:%04x skipped by quirk", (unsigned)ua->vid, (unsigned)ua->pid);
        return false;
    }
    if (ua->fn && !usb_function_claim(usb, ua->fn, UA_DRIVER_NAME)) return false;

    const uint8_t *cfg = NULL;
    uint16_t len = 0;
    if (!ua_fetch_config(ua, &cfg, &len)) goto fail;
    ua_parse(ua, cfg, len);
    if (ua->ac_iface == 0xFF && ua->alt_count == 0) goto fail;

    if (ua->uac2) {
        for (int i = 0; i < ua->alt_count; i++) {
            struct ua_alt *a = &ua->alts[i];
            struct ua_unit *t = ua_unit_get(ua, a->terminal_link);
            uint8_t clk = t ? t->clock : 0;
            a->clock_id = clk ? ua_resolve_clock(ua, clk, 0) : 0;
            bool done = false;
            for (int j = 0; j < i; j++) {
                if (ua->alts[j].clock_id == a->clock_id) {
                    a->rate_count = ua->alts[j].rate_count;
                    memcpy(a->rates, ua->alts[j].rates, sizeof(a->rates));
                    done = true;
                    break;
                }
            }
            if (!done) ua_read_clock_rates(ua, a);
        }
    }

    int play_alts = 0, cap_alts = 0;
    for (int i = 0; i < ua->alt_count; i++) {
        bool cap = ua->alts[i].direction == AUDIO_DIR_CAPTURE;
        if (cap && (ua->quirks.flags & AUDIO_USB_Q_NO_CAPTURE)) ua->alts[i].valid = 0;
        if (!cap && (ua->quirks.flags & AUDIO_USB_Q_NO_PLAYBACK)) ua->alts[i].valid = 0;
    }
    int w = 0;
    for (int i = 0; i < ua->alt_count; i++) {
        if (!ua->alts[i].valid) continue;
        if (w != i) ua->alts[w] = ua->alts[i];
        if (ua->alts[w].direction == AUDIO_DIR_PLAYBACK) play_alts++;
        else cap_alts++;
        w++;
    }
    ua->alt_count = (uint8_t)w;
    if (!play_alts && !cap_alts) {
        LOG_INFO("usb %04x:%04x: audio function %d has no usable PCM stream", (unsigned)ua->vid, (unsigned)ua->pid, fn_index);
        goto fail;
    }

    ua_pick_feature_units(ua);
    ua_read_volume_range(ua, &ua->fu[AUDIO_DIR_PLAYBACK]);
    ua_read_volume_range(ua, &ua->fu[AUDIO_DIR_CAPTURE]);

    struct audio_device *a = &ua->adev;
    a->type = USB_AUDIO;
    a->usb = usb;
    a->ops = &ua_ops;
    a->priv = ua;
    a->vendor_id = AUDIO_ID(ua->vid, ua->pid);
    a->flags = AUDIO_DEV_FLAG_HOTPLUG;
    if (ua->fu[0].has_volume || ua->fu[1].has_volume) a->flags |= AUDIO_DEV_FLAG_HW_VOLUME;
    if (ua->fu[0].has_mute || ua->fu[1].has_mute) a->flags |= AUDIO_DEV_FLAG_HW_MUTE;
    a->quirks = ua->quirks.flags;
    if (ua->quirks.model) snprintf(a->name, sizeof(a->name), "%s", ua->quirks.model);
    else if (usb->product_str[0]) snprintf(a->name, sizeof(a->name), "%s", usb->product_str);
    else snprintf(a->name, sizeof(a->name), "USB Audio %04x:%04x", (unsigned)ua->vid, (unsigned)ua->pid);
    snprintf(a->codec_name, sizeof(a->codec_name), "UAC%u %s%04x:%04x", ua->uac2 ? 2u : 1u, ua_function_label(usb),
             (unsigned)ua->vid, (unsigned)ua->pid);
    snprintf(a->description, sizeof(a->description), "%s%s%s, addr %u", usb->vendor_str, usb->vendor_str[0] ? " " : "",
             usb->product_str, (unsigned)usb->address);
    audio_quirk_set_names(&ua->quirks, a->quirk_names, sizeof(a->quirk_names));
    ua_fill_caps(ua);

    if (audio_core_add_device(a) < 0) goto fail;
    ua_in_use[slot] = 1;
    ua_attached++;

    LOG_INFO("usb %04x:%04x \"%s\": UAC%u %s, %s-speed, function %d, %d playback / %d capture alt(s), volume %s/%s",
             (unsigned)ua->vid, (unsigned)ua->pid, a->name, ua->uac2 ? 2u : 1u, ua_function_label(usb)[0] ? ua_function_label(usb) : "device",
             ua->hs ? "high" : "full", fn_index, play_alts, cap_alts, ua->fu[0].has_volume ? "hw" : "soft",
             ua->fu[1].has_volume ? "hw" : "soft");
    return true;

fail:
    if (ua->fn) usb_function_release(usb, ua->fn, UA_DRIVER_NAME);
    ua_free_buffers(ua);
    return false;
}

static void ua_detach(int slot) {
    struct ua_device *ua = &ua_pool[slot];
    ua->gone = 1;
    ua->st[0].active = ua->st[1].active = 0;
    audio_core_remove_device(&ua->adev);
    if (ua_usb_alive(ua)) {
        for (uint8_t d = 0; d < AUDIO_DIR_COUNT; d++) ua_stream_close_eps(ua, d);
        if (ua->fn) usb_function_release(ua->usb, ua->fn, UA_DRIVER_NAME);
    }
    ua_in_use[slot] = 0;
    ua_freed_ms[slot] = audio_uptime_ms();
    if (ua_attached > 0) ua_attached--;
    LOG_INFO("usb %04x:%04x detached", (unsigned)ua->vid, (unsigned)ua->pid);
}

static struct ua_checked *ua_checked_find(struct usb_device *u, int fn) {
    for (int i = 0; i < ua_checked_count; i++) {
        struct ua_checked *c = &ua_checked_list[i];
        if (c->usb == u && c->fn_index == fn && c->generation == u->generation && c->address == u->address &&
            c->vid == u->desc.idVendor && c->pid == u->desc.idProduct)
            return c;
    }
    return NULL;
}

static struct ua_checked *ua_checked_add(struct usb_device *u, int fn) {
    struct ua_checked *c = NULL;
    for (int i = 0; i < ua_checked_count; i++)
        if (ua_checked_list[i].usb == u && ua_checked_list[i].fn_index == fn) c = &ua_checked_list[i];
    if (!c) {
        if (ua_checked_count >= UA_MAX_CHECKED) {
            for (int i = 0; i < ua_checked_count; i++)
                if (!usb_device_alive(ua_checked_list[i].usb)) { c = &ua_checked_list[i]; break; }
            if (!c) return NULL;
        } else {
            c = &ua_checked_list[ua_checked_count++];
        }
    }
    c->usb = u;
    c->generation = u->generation;
    c->fn_index = (int8_t)fn;
    c->address = u->address;
    c->vid = u->desc.idVendor;
    c->pid = u->desc.idProduct;
    c->first_seen_ms = audio_uptime_ms();
    c->probed = 0;
    return c;
}

static bool ua_already_attached(struct usb_device *u, int fn) {
    for (int i = 0; i < UA_MAX_DEVICES; i++)
        if (ua_in_use[i] && ua_pool[i].usb == u && ua_pool[i].generation == u->generation && ua_pool[i].fn_index == fn) return true;
    return false;
}

static void ua_consider(struct usb_device *u, int fn, bool boot, uint64_t now) {
    if (ua_already_attached(u, fn)) return;
    struct ua_checked *c = ua_checked_find(u, fn);
    if (!c) c = ua_checked_add(u, fn);
    if (!c || c->probed) return;
    if (!boot && now - c->first_seen_ms < UA_SETTLE_MS) return;
    c->probed = 1;
    ua_attach(u, fn);
}

static void ua_survey(bool boot) {
    for (int i = 0; i < UA_MAX_DEVICES; i++) {
        if (!ua_in_use[i]) continue;
        if (!ua_usb_alive(&ua_pool[i])) ua_detach(i);
    }

    uint64_t now = audio_uptime_ms();
    for (int i = 0; i < MAX_USB_DEVICES; i++) {
        struct usb_device *u = (struct usb_device *)device_table[USB_DEVICE][i];
        if (!u || !u->valid || u->device_class == 0x09) continue;

        if (u->function_count == 0) {
            if (u->device_class == USB_CLASS_AUDIO || (u->iso_ep_count > 0 && u->device_class != 0x08 && u->device_class != 0x03))
                ua_consider(u, -1, boot, now);
            continue;
        }

        int it = 0;
        struct usb_function_info *f;
        while ((f = usb_find_function(u, USB_CLASS_AUDIO, &it)) != NULL) {
            int fi = (int)(f - u->functions);
            if (f->driver && strcmp(f->driver, UA_DRIVER_NAME) != 0) continue;
            ua_consider(u, fi, boot, now);
        }

        struct audio_quirk_ids ids = { .usb_id = AUDIO_ID(u->desc.idVendor, u->desc.idProduct) };
        struct audio_quirk_set q;
        audio_quirk_resolve(AUDIO_QUIRK_USB_AUDIO, &ids, 0, &q);
        if (q.flags & AUDIO_USB_Q_VENDOR_CLASS) {
            it = 0;
            while ((f = usb_find_function(u, 0xFF, &it)) != NULL) {
                int fi = (int)(f - u->functions);
                if (f->driver && strcmp(f->driver, UA_DRIVER_NAME) != 0) continue;
                ua_consider(u, fi, boot, now);
            }
        }
    }
}

static void ua_backend_poll(void) {
    uint64_t now = audio_uptime_ms();
    if (now - ua_last_survey_ms < UA_SURVEY_MS) return;
    if (!spin_trylock(&ua_survey_lock)) return;
    ua_last_survey_ms = now;
    ua_survey(false);
    spin_unlock(&ua_survey_lock);
}

static void ua_backend_scan(void) {
    spin_lock(&ua_survey_lock);
    ua_survey(true);
    spin_unlock(&ua_survey_lock);
}

static const struct audio_backend usb_audio_backend = {
    .name = "usb-audio",
    .type = USB_AUDIO,
    .scan = ua_backend_scan,
    .poll = ua_backend_poll,
};

static int ua_get_device_count(void) {
    return ua_attached;
}

static struct ua_device *ua_get_device(int idx) {
    for (int i = 0; i < UA_MAX_DEVICES; i++) {
        if (!ua_in_use[i]) continue;
        if (idx-- == 0) return &ua_pool[i];
    }
    return NULL;
}

static struct usb_audio_driver drv_usb_audio = {
    .get_device_count = ua_get_device_count,
    .get_device = ua_get_device,
};

struct usb_audio_driver *return_usb_audio_driver(void) {
    audio_core_register_backend(&usb_audio_backend);
    ua_backend_scan();
    ua_last_survey_ms = audio_uptime_ms();
    LOG_INFO("USB audio: %d device(s)", ua_attached);
    return &drv_usb_audio;
}

struct driver *return_meta_usb_audio_driver(void) {
    static struct dependency deps[] = {
        MAKE_DEPENDENCY(AUDIO_DRIVER, AUDIO_CORE_SLOT),
        MAKE_DEPENDENCY(USB_DRIVER, USB_CORE_SLOT),
    };
    static struct driver meta = {
        .name = "USB Audio Driver",
        .type = AUDIO_DRIVER,
        .sub_type = USB_AUDIO,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &deps[0], &deps[1] },
        .dependency_count = 2,
        .self = &drv_usb_audio,
        .init = (void *)return_usb_audio_driver,
    };
    return &meta;
}
