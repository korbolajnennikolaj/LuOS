#include "audio_quirks.h"

#include "audio_core.h"

#include "components/logger.h"
#include "kernel/scheduler/spinlock.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct audio_quirk_table *quirk_tables[AUDIO_QUIRK_MAX_TABLES];
static int quirk_table_count = 0;
static bool quirks_initialized = false;
static spinlock_t quirk_lock = SPINLOCK_INIT;

static const char *const hdac_flag_names[AUDIO_HDAC_Q_FLAG_COUNT] = {
    "64BIT", "DMAPOS", "MSI", "SINGLE_CMD", "INTEL_TCSEL", "SNOOP_ATI", "SNOOP_NVIDIA",
    "SNOOP_INTEL_SCH", "CORB_RP_NO_ACK", "RIRB_DELAY", "LONG_RESET", "SKIP", "NO_STREAM_RESET_WAIT",
};

static const char *const hda_flag_names[AUDIO_HDA_Q_FLAG_COUNT] = {
    "GPIO0", "GPIO1", "GPIO2", "GPIO3", "GPIO4", "GPIO5", "GPIO6", "GPIO7",
    "GPIOFLUSH", "SOFTPCMVOL", "FIXEDRATE", "FORCESTEREO", "EAPDINV", NULL, "SENSEINV", NULL,
    "IVREF50", "IVREF80", "IVREF100", "OVREF50", "OVREF80", "OVREF100", NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "NO_AUTOMUTE", "NO_EAPD", "FUNC_RESET", "IGNORE_PIN_CONFIG", "SPEAKER_ALWAYS_ON",
    "DIGITAL_OUT", "SKIP", "NO_CAPTURE", "EAPD_ALL_PINS", "AMP_PARAM_AFG",
};

static const char *const ac97c_flag_names[AUDIO_AC97C_Q_FLAG_COUNT] = {
    "IGNORE_PCR", "IGNORE_RESET", "SIS_REGS", "ICH4_IOSE", "PREFER_MMIO", "FIXED_48K", "SKIP",
    "NO_SEMAPHORE",
};

static const char *const ac97_flag_names[AUDIO_AC97_Q_FLAG_COUNT] = {
    "EAPD_INV", "RDCD_BUG", "NO_VRA", "MASTER_5BIT", "NO_HEADPHONE", "NO_EAPD", "SWAP_LR",
    "SOFT_VOLUME", "MIC_BOOST",
};

static const char *const usb_flag_names[AUDIO_USB_Q_FLAG_COUNT] = {
    "NO_FRAC", "INP_ASYNC", "NO_XU", "BAD_ADC", "VENDOR_CLASS", "SWAP_LR", "NO_SET_RATE",
    "NO_VOLUME", "NO_MUTE", "SKIP", "SET_RATE_ALWAYS", "VOLUME_LINEAR", "NO_FEEDBACK", "NO_CAPTURE",
    "NO_PLAYBACK", "FB_FULL_SPEED_FMT", "CLOCK_NO_VALIDATE",
};

static const char *const hda_pin_devices[16] = {
    "Line-out", "Speaker", "Headphones", "CD", "SPDIF-out", "Digital-out", "Modem-line", "Modem-handset",
    "Line-in", "AUX", "Mic", "Telephony", "SPDIF-in", "Digital-in", "Res.E", "Other",
};

static const char *const hda_pin_conns[4] = { "Jack", "None", "Fixed", "Both" };

static const char *const hda_pin_colors[16] = {
    "Unknown", "Black", "Grey", "Blue", "Green", "Red", "Orange", "Yellow",
    "Purple", "Pink", "Res.A", "Res.B", "Res.C", "Res.D", "White", "Other",
};

static const char *const hda_pin_ctypes[16] = {
    "Unknown", "1/8", "1/4", "ATAPI", "RCA", "Optical", "Digital", "Analog",
    "DIN", "XLR", "RJ-11", "Combo", "0xc", "0xd", "0xe", "Other",
};

static const struct {
    uint8_t loc;
    const char *name;
} hda_pin_locations[] = {
    { 0x00, "N/A" }, { 0x01, "Rear" }, { 0x02, "Front" }, { 0x03, "Left" }, { 0x04, "Right" },
    { 0x05, "Top" }, { 0x06, "Bottom" }, { 0x07, "Rear-panel" }, { 0x08, "Drive-bay" },
    { 0x10, "Internal" }, { 0x17, "Riser" }, { 0x18, "Onboard" }, { 0x18, "Digital-display" },
    { 0x19, "ATAPI" }, { 0x20, "External" }, { 0x21, "Ext-Rear" }, { 0x22, "Ext-Front" },
    { 0x23, "Ext-Left" }, { 0x24, "Ext-Right" }, { 0x25, "Ext-Top" }, { 0x26, "Ext-Bottom" },
    { 0x30, "Other" }, { 0x36, "Other-Bottom" }, { 0x37, "Lid-In" }, { 0x38, "Lid-Out" },
};

const char *audio_hda_pin_device_name(uint8_t dev) { return hda_pin_devices[dev & 0xF]; }
const char *audio_hda_pin_conn_name(uint8_t conn) { return hda_pin_conns[conn & 0x3]; }
const char *audio_hda_pin_color_name(uint8_t color) { return hda_pin_colors[color & 0xF]; }
const char *audio_hda_pin_ctype_name(uint8_t ctype) { return hda_pin_ctypes[ctype & 0xF]; }

const char *audio_hda_pin_location_name(uint8_t loc) {
    for (size_t i = 0; i < sizeof(hda_pin_locations) / sizeof(hda_pin_locations[0]); i++)
        if (hda_pin_locations[i].loc == (loc & 0x3F)) return hda_pin_locations[i].name;
    return "?";
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

const char *audio_quirk_domain_name(enum audio_quirk_domain domain) {
    switch (domain) {
        case AUDIO_QUIRK_HDA_CONTROLLER: return "hda-controller";
        case AUDIO_QUIRK_HDA_CODEC: return "hda-codec";
        case AUDIO_QUIRK_AC97_CONTROLLER: return "ac97-controller";
        case AUDIO_QUIRK_AC97_CODEC: return "ac97-codec";
        case AUDIO_QUIRK_USB_AUDIO: return "usb-audio";
    }
    return "?";
}

int audio_quirk_flag_count(enum audio_quirk_domain domain) {
    switch (domain) {
        case AUDIO_QUIRK_HDA_CONTROLLER: return AUDIO_HDAC_Q_FLAG_COUNT;
        case AUDIO_QUIRK_HDA_CODEC: return AUDIO_HDA_Q_FLAG_COUNT;
        case AUDIO_QUIRK_AC97_CONTROLLER: return AUDIO_AC97C_Q_FLAG_COUNT;
        case AUDIO_QUIRK_AC97_CODEC: return AUDIO_AC97_Q_FLAG_COUNT;
        case AUDIO_QUIRK_USB_AUDIO: return AUDIO_USB_Q_FLAG_COUNT;
    }
    return 0;
}

const char *audio_quirk_flag_name(enum audio_quirk_domain domain, int bit) {
    if (bit < 0 || bit >= audio_quirk_flag_count(domain)) return NULL;
    switch (domain) {
        case AUDIO_QUIRK_HDA_CONTROLLER: return hdac_flag_names[bit];
        case AUDIO_QUIRK_HDA_CODEC: return hda_flag_names[bit];
        case AUDIO_QUIRK_AC97_CONTROLLER: return ac97c_flag_names[bit];
        case AUDIO_QUIRK_AC97_CODEC: return ac97_flag_names[bit];
        case AUDIO_QUIRK_USB_AUDIO: return usb_flag_names[bit];
    }
    return NULL;
}

int audio_quirk_register_table(const struct audio_quirk_table *table) {
    if (!table || !table->entries || table->count == 0) return AUDIO_ERR_PARAM;
    if ((int)table->domain < 0 || (int)table->domain >= AUDIO_QUIRK_DOMAIN_COUNT) return AUDIO_ERR_PARAM;

    spin_lock(&quirk_lock);
    for (int i = 0; i < quirk_table_count; i++) {
        if (quirk_tables[i] == table) {
            spin_unlock(&quirk_lock);
            return i;
        }
    }
    if (quirk_table_count >= AUDIO_QUIRK_MAX_TABLES) {
        spin_unlock(&quirk_lock);
        LOG_ERROR("quirk table registry full, '%s' dropped", table->name ? table->name : "?");
        return AUDIO_ERR_BUSY;
    }
    int idx = quirk_table_count;
    quirk_tables[quirk_table_count++] = table;
    spin_unlock(&quirk_lock);

    LOG_DEBUG("quirk table '%s' (%s, %u entries) registered", table->name ? table->name : "?",
              audio_quirk_domain_name(table->domain), (unsigned)table->count);
    return idx;
}

int audio_quirk_table_count(void) {
    return quirk_table_count;
}

const struct audio_quirk_table *audio_quirk_get_table(int idx) {
    if (idx < 0 || idx >= quirk_table_count) return NULL;
    return quirk_tables[idx];
}

void audio_quirks_init(void) {
    if (quirks_initialized) return;
    quirks_initialized = true;

    uint32_t entries = 0;
    for (uint32_t i = 0; i < audio_builtin_quirk_table_count; i++) {
        const struct audio_quirk_table *t = audio_builtin_quirk_tables[i];
        if (!t) continue;
        if (audio_quirk_register_table(t) >= 0) entries += t->count;
    }
    LOG_INFO("%d quirk table(s), %u entries", quirk_table_count, (unsigned)entries);
}

bool audio_quirk_id_match(uint32_t pattern, uint32_t value) {
    if (pattern == AUDIO_ID_ANY || pattern == AUDIO_ID_ALL) return true;
    if (pattern == value) return true;
    if ((pattern & 0xFFFF0000u) == 0xFFFF0000u && (pattern & 0xFFFFu) == (value & 0xFFFFu)) return true;
    if ((pattern & 0x0000FFFFu) == 0x0000FFFFu && (pattern & 0xFFFF0000u) == (value & 0xFFFF0000u)) return true;
    return false;
}

bool audio_quirk_matches(const struct audio_quirk_match *m, const struct audio_quirk_ids *ids) {
    if (!m || !ids) return false;

    if (!audio_quirk_id_match(m->pci_id, ids->pci_id)) return false;
    if (!audio_quirk_id_match(m->subsys, ids->subsys)) return false;

    if (m->codec_mask) {
        if ((ids->codec_id & m->codec_mask) != (m->codec_id & m->codec_mask)) return false;
    } else if (!audio_quirk_id_match(m->codec_id, ids->codec_id)) {
        return false;
    }

    if (!audio_quirk_id_match(m->codec_subsys, ids->codec_subsys)) return false;
    if (!audio_quirk_id_match(m->usb_id, ids->usb_id)) return false;

    if (m->rev_max) {
        if (!ids->has_revision) return false;
        if (ids->revision < m->rev_min || ids->revision > m->rev_max) return false;
    }
    return true;
}

void audio_quirk_resolve(enum audio_quirk_domain domain, const struct audio_quirk_ids *ids,
                         uint64_t defaults, struct audio_quirk_set *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->domain = domain;
    out->flags = defaults;
    out->usb_iface = -1;
    out->usb_alt = -1;
    if (!ids) return;

    for (int t = 0; t < quirk_table_count; t++) {
        const struct audio_quirk_table *table = quirk_tables[t];
        if (!table || table->domain != domain) continue;

        for (uint32_t e = 0; e < table->count; e++) {
            const struct audio_quirk *q = &table->entries[e];
            if (!audio_quirk_matches(&q->match, ids)) continue;

            out->flags |= q->set;
            out->flags &= ~q->clear;

            if (q->gpio_mask) {
                out->gpio_mask = (out->gpio_mask & ~q->gpio_mask) | q->gpio_mask;
                out->gpio_dir = (out->gpio_dir & ~q->gpio_mask) | (q->gpio_dir & q->gpio_mask);
                out->gpio_data = (out->gpio_data & ~q->gpio_mask) | (q->gpio_data & q->gpio_mask);
            }
            if (q->fixed_rate) out->fixed_rate = q->fixed_rate;
            if (q->codec_probe_mask) out->codec_probe_mask = q->codec_probe_mask;
            if (q->delay_ms) out->delay_ms = q->delay_ms;
            if (q->usb_iface || q->usb_alt) {
                out->usb_iface = q->usb_iface;
                out->usb_alt = q->usb_alt;
            }
            if (!out->model && q->model) out->model = q->model;

            if (out->match_count < AUDIO_QUIRK_MAX_MATCHES) {
                out->matches[out->match_count++] = q;
            } else {
                LOG_WARNING("%s: more than %d matching quirks, '%s' flags applied but patches ignored",
                            audio_quirk_domain_name(domain), AUDIO_QUIRK_MAX_MATCHES, q->name ? q->name : "?");
            }
        }
    }
}

static int pin_lookup_name(const char *value, const char *const *names, int count) {
    for (int i = 0; i < count; i++)
        if (names[i] && str_ieq(names[i], value)) return i;
    return -1;
}

static int pin_lookup_location(const char *value) {
    for (size_t i = 0; i < sizeof(hda_pin_locations) / sizeof(hda_pin_locations[0]); i++)
        if (str_ieq(hda_pin_locations[i].name, value)) return hda_pin_locations[i].loc;
    return -1;
}

static int pin_lookup_device_alias(const char *value) {
    static const struct { const char *alias; uint8_t dev; } aliases[] = {
        { "lineout", 0 }, { "line_out", 0 }, { "speakers", 1 }, { "spk", 1 }, { "headphone", 2 },
        { "hp", 2 }, { "hpout", 2 }, { "spdifout", 4 }, { "digitalout", 5 }, { "hdmi", 5 },
        { "linein", 8 }, { "line_in", 8 }, { "microphone", 10 }, { "spdifin", 12 }, { "digitalin", 13 },
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (str_ieq(aliases[i].alias, value)) return aliases[i].dev;
    return -1;
}

static bool pin_parse_number(const char *value, long *out) {
    char *end = NULL;
    long v = strtol(value, &end, 0);
    if (!end || end == value || *end != '\0') return false;
    *out = v;
    return true;
}

static uint32_t pin_set_field(uint32_t config, int shift, uint32_t mask, long value) {
    config &= ~(mask << shift);
    config |= ((uint32_t)value & mask) << shift;
    return config;
}

int audio_quirk_parse_pin_string(uint32_t config, const char *str, uint32_t *out) {
    if (!str || !out) return AUDIO_ERR_PARAM;

    char key[24];
    char value[32];
    const char *p = str;
    int errors = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        size_t kl = 0;
        while (*p && *p != '=' && *p != ' ' && *p != '\t') {
            if (kl + 1 < sizeof(key)) key[kl++] = *p;
            p++;
        }
        key[kl] = '\0';
        if (*p != '=') {
            errors++;
            while (*p && *p != ' ' && *p != '\t') p++;
            continue;
        }
        p++;

        size_t vl = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ',') {
            if (vl + 1 < sizeof(value)) value[vl++] = *p;
            p++;
        }
        value[vl] = '\0';

        long num = 0;
        bool is_num = pin_parse_number(value, &num);
        int idx = -1;

        if (str_ieq(key, "seq")) {
            if (is_num) config = pin_set_field(config, 0, 0xF, num); else errors++;
        } else if (str_ieq(key, "as")) {
            if (is_num) config = pin_set_field(config, 4, 0xF, num); else errors++;
        } else if (str_ieq(key, "misc")) {
            if (is_num) config = pin_set_field(config, 8, 0xF, num); else errors++;
        } else if (str_ieq(key, "color")) {
            idx = is_num ? (int)num : pin_lookup_name(value, hda_pin_colors, 16);
            if (idx >= 0) config = pin_set_field(config, 12, 0xF, idx); else errors++;
        } else if (str_ieq(key, "ctype")) {
            idx = is_num ? (int)num : pin_lookup_name(value, hda_pin_ctypes, 16);
            if (idx >= 0) config = pin_set_field(config, 16, 0xF, idx); else errors++;
        } else if (str_ieq(key, "device")) {
            idx = is_num ? (int)num : pin_lookup_name(value, hda_pin_devices, 16);
            if (idx < 0) idx = pin_lookup_device_alias(value);
            if (idx >= 0) config = pin_set_field(config, 20, 0xF, idx); else errors++;
        } else if (str_ieq(key, "loc")) {
            idx = is_num ? (int)num : pin_lookup_location(value);
            if (idx >= 0) config = pin_set_field(config, 24, 0x3F, idx); else errors++;
        } else if (str_ieq(key, "conn")) {
            idx = is_num ? (int)num : pin_lookup_name(value, hda_pin_conns, 4);
            if (idx >= 0) config = pin_set_field(config, 30, 0x3, idx); else errors++;
        } else if (str_ieq(key, "config")) {
            if (is_num) config = (uint32_t)num; else errors++;
        } else {
            errors++;
        }
    }

    *out = config;
    return errors ? AUDIO_ERR_PARAM : AUDIO_OK;
}

uint32_t audio_quirk_pin_apply(uint32_t config, const struct audio_pin_patch *patch) {
    if (!patch) return config;
    switch (patch->type) {
        case AUDIO_PIN_PATCH_OVERRIDE:
            return patch->value;
        case AUDIO_PIN_PATCH_MASK:
            return (config & ~patch->mask) | (patch->value & patch->mask);
        case AUDIO_PIN_PATCH_DISABLE:
            return AUDIO_HDA_PIN_CONFIG_NONE;
        case AUDIO_PIN_PATCH_STRING: {
            uint32_t out = config;
            if (audio_quirk_parse_pin_string(config, patch->str, &out) != AUDIO_OK)
                LOG_WARNING("pin 0x%02x: unparsed parts in patch \"%s\"", (unsigned)patch->nid,
                            patch->str ? patch->str : "");
            return out;
        }
        default:
            return config;
    }
}

int audio_quirk_apply_pins(const struct audio_quirk_set *set, uint8_t nid, uint32_t *config) {
    if (!set || !config) return 0;
    int applied = 0;
    for (int i = 0; i < set->match_count; i++) {
        const struct audio_quirk *q = set->matches[i];
        if (!q->pins) continue;
        for (const struct audio_pin_patch *p = q->pins; p->nid; p++) {
            if (p->nid != nid) continue;
            uint32_t before = *config;
            *config = audio_quirk_pin_apply(*config, p);
            applied++;
            LOG_DEBUG("quirk '%s': pin 0x%02x config 0x%08x -> 0x%08x", q->name ? q->name : "?",
                      (unsigned)nid, before, *config);
        }
    }
    return applied;
}

int audio_quirk_run_verbs(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx) {
    if (!set || !ctx || !ctx->verb) return 0;
    int n = 0;
    for (int i = 0; i < set->match_count; i++) {
        const struct audio_quirk *q = set->matches[i];
        if (!q->verbs) continue;
        for (const struct audio_verb *v = q->verbs; v->verb; v++) {
            uint32_t payload = ((uint32_t)v->verb << 8) | (uint32_t)v->param;
            ctx->verb(ctx->backend, v->nid, payload);
            n++;
        }
        LOG_DEBUG("quirk '%s': verb sequence sent", q->name ? q->name : "?");
    }
    return n;
}

int audio_quirk_run_coefs(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx) {
    if (!set || !ctx || !ctx->verb) return 0;
    int n = 0;
    for (int i = 0; i < set->match_count; i++) {
        const struct audio_quirk *q = set->matches[i];
        if (!q->coefs) continue;
        for (const struct audio_coef *c = q->coefs; c->nid; c++) {
            ctx->verb(ctx->backend, c->nid, (0x500u << 8) | c->index);
            uint32_t old = ctx->verb(ctx->backend, c->nid, (0xC00u << 8)) & 0xFFFFu;
            uint32_t val = (old & ~(uint32_t)c->mask) | ((uint32_t)c->value & c->mask);
            ctx->verb(ctx->backend, c->nid, (0x500u << 8) | c->index);
            ctx->verb(ctx->backend, c->nid, (0x400u << 8) | (val & 0xFFFFu));
            LOG_DEBUG("quirk '%s': coef nid 0x%02x idx 0x%02x 0x%04x -> 0x%04x", q->name ? q->name : "?",
                      (unsigned)c->nid, (unsigned)c->index, old, val);
            n++;
        }
    }
    return n;
}

int audio_quirk_run_regs(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx) {
    if (!set || !ctx || !ctx->reg_read || !ctx->reg_write) return 0;
    int n = 0;
    for (int i = 0; i < set->match_count; i++) {
        const struct audio_quirk *q = set->matches[i];
        if (!q->regs) continue;
        for (const struct audio_reg_patch *r = q->regs; r->reg || r->mask; r++) {
            uint16_t old = ctx->reg_read(ctx->backend, r->reg);
            uint16_t val = (uint16_t)((old & ~r->mask) | (r->value & r->mask));
            ctx->reg_write(ctx->backend, r->reg, val);
            LOG_DEBUG("quirk '%s': reg 0x%02x 0x%04x -> 0x%04x", q->name ? q->name : "?",
                      (unsigned)r->reg, (unsigned)old, (unsigned)val);
            n++;
        }
    }
    return n;
}

int audio_quirk_run_hooks(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx) {
    if (!set || !ctx) return 0;
    int n = 0;
    for (int i = 0; i < set->match_count; i++) {
        const struct audio_quirk *q = set->matches[i];
        if (!q->hook) continue;
        int r = q->hook(q, ctx);
        if (r < 0)
            LOG_WARNING("quirk '%s': hook failed at stage %d (%d)", q->name ? q->name : "?", (int)ctx->stage, r);
        n++;
    }
    return n;
}

void audio_quirk_set_names(const struct audio_quirk_set *set, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (!set) return;
    size_t pos = 0;
    for (int i = 0; i < set->match_count; i++) {
        const char *n = set->matches[i]->name;
        if (!n) continue;
        int w = snprintf(buf + pos, cap - pos, "%s%s", pos ? "," : "", n);
        if (w < 0 || (size_t)w >= cap - pos) {
            buf[cap - 1] = '\0';
            return;
        }
        pos += (size_t)w;
    }
}

void audio_quirk_flags_string(enum audio_quirk_domain domain, uint64_t flags, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    size_t pos = 0;
    int count = audio_quirk_flag_count(domain);
    for (int bit = 0; bit < 64; bit++) {
        if (!(flags & (1ull << bit))) continue;
        const char *n = (bit < count) ? audio_quirk_flag_name(domain, bit) : NULL;
        char tmp[16];
        if (!n) {
            snprintf(tmp, sizeof(tmp), "bit%d", bit);
            n = tmp;
        }
        int w = snprintf(buf + pos, cap - pos, "%s%s", pos ? "|" : "", n);
        if (w < 0 || (size_t)w >= cap - pos) {
            buf[cap - 1] = '\0';
            return;
        }
        pos += (size_t)w;
    }
    if (pos == 0) snprintf(buf, cap, "none");
}
