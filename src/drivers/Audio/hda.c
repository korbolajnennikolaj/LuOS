#include "hda.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "components/Memory/vmm.h"
#include "components/pci.h"
#include "drivers/Timer/timer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HDA_CMD_TIMEOUT_US 10000
#define HDA_CMD_FAILS_BEFORE_SINGLE 3
#define HDA_JACK_POLL_MS 300
#define HDA_PERIOD_MS 16
#define HDA_PERIODS 4

static struct hda_controller hda_ctrls[HDA_MAX_CONTROLLERS];
static int hda_ctrl_count = 0;
static spinlock_t hda_scan_lock = SPINLOCK_INIT;

static const struct {
    uint16_t vendor;
    const char *name;
} hda_vendor_names[] = {
    { 0x1002, "ATI" }, { 0x1013, "Cirrus Logic" }, { 0x1022, "AMD" }, { 0x1039, "SiS" },
    { 0x10b9, "ULi" }, { 0x10de, "NVIDIA" }, { 0x10ec, "Realtek" }, { 0x1102, "Creative" },
    { 0x1106, "VIA" }, { 0x111d, "IDT" }, { 0x11c1, "LSI" }, { 0x11d4, "Analog Devices" },
    { 0x13f6, "C-Media" }, { 0x14f1, "Conexant" }, { 0x15ad, "VMware" }, { 0x17e8, "Chrontel" },
    { 0x17f3, "RDC" }, { 0x1854, "LG" }, { 0x1aec, "Wolfson" }, { 0x1af4, "QEMU" },
    { 0x434d, "C-Media" }, { 0x6549, "Teradici" }, { 0x8086, "Intel" }, { 0x8384, "SigmaTel" },
};

static const char *hda_vendor_name(uint16_t vendor) {
    for (size_t i = 0; i < sizeof(hda_vendor_names) / sizeof(hda_vendor_names[0]); i++)
        if (hda_vendor_names[i].vendor == vendor) return hda_vendor_names[i].name;
    return NULL;
}

static inline uint8_t hda_r8(struct hda_controller *c, uint32_t off) {
    return *(volatile uint8_t *)(c->mmio + off);
}

static inline uint16_t hda_r16(struct hda_controller *c, uint32_t off) {
    return *(volatile uint16_t *)(c->mmio + off);
}

static inline uint32_t hda_r32(struct hda_controller *c, uint32_t off) {
    return *(volatile uint32_t *)(c->mmio + off);
}

static inline void hda_w8(struct hda_controller *c, uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(c->mmio + off) = v;
}

static inline void hda_w16(struct hda_controller *c, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(c->mmio + off) = v;
}

static inline void hda_w32(struct hda_controller *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->mmio + off) = v;
}

static inline uint32_t hda_sd(int sd, uint32_t reg) {
    return HDA_REG_SD_BASE + (uint32_t)sd * HDA_SD_STRIDE + reg;
}

static inline void hda_mb(void) {
    asm volatile("mfence" ::: "memory");
}

static bool hda_wait8(struct hda_controller *c, uint32_t off, uint8_t mask, uint8_t value, uint32_t timeout_us) {
    for (uint32_t t = 0; t < timeout_us; t += 10) {
        if ((hda_r8(c, off) & mask) == value) return true;
        audio_delay_us(10);
    }
    return (hda_r8(c, off) & mask) == value;
}

static bool hda_wait16(struct hda_controller *c, uint32_t off, uint16_t mask, uint16_t value, uint32_t timeout_us) {
    for (uint32_t t = 0; t < timeout_us; t += 10) {
        if ((hda_r16(c, off) & mask) == value) return true;
        audio_delay_us(10);
    }
    return (hda_r16(c, off) & mask) == value;
}

static bool hda_wait32(struct hda_controller *c, uint32_t off, uint32_t mask, uint32_t value, uint32_t timeout_us) {
    for (uint32_t t = 0; t < timeout_us; t += 10) {
        if ((hda_r32(c, off) & mask) == value) return true;
        audio_delay_us(10);
    }
    return (hda_r32(c, off) & mask) == value;
}

static void hda_pci_update8(struct pci_device *p, uint8_t off, uint8_t mask, uint8_t val) {
    uint32_t d = pci_read_config(p->bus, p->slot, p->func, off & 0xFC);
    uint32_t sh = (uint32_t)(off & 3) * 8;
    uint8_t b = (uint8_t)(d >> sh);
    uint8_t nb = (uint8_t)((b & ~mask) | (val & mask));
    if (nb == b) return;
    d = (d & ~(0xFFu << sh)) | ((uint32_t)nb << sh);
    pci_write_config(p->bus, p->slot, p->func, off & 0xFC, d);
}

static void hda_pci_update16(struct pci_device *p, uint8_t off, uint16_t mask, uint16_t val) {
    uint32_t d = pci_read_config(p->bus, p->slot, p->func, off & 0xFC);
    uint32_t sh = (uint32_t)(off & 2) * 8;
    uint16_t w = (uint16_t)(d >> sh);
    uint16_t nw = (uint16_t)((w & ~mask) | (val & mask));
    if (nw == w) return;
    d = (d & ~(0xFFFFu << sh)) | ((uint32_t)nw << sh);
    pci_write_config(p->bus, p->slot, p->func, off & 0xFC, d);
}

static void hda_stop_corb_rirb(struct hda_controller *c) {
    hda_w8(c, HDA_REG_CORBCTL, 0);
    hda_w8(c, HDA_REG_RIRBCTL, 0);
    hda_wait8(c, HDA_REG_CORBCTL, HDA_CORBCTL_RUN, 0, 1000);
    hda_wait8(c, HDA_REG_RIRBCTL, HDA_RIRBCTL_DMAEN, 0, 1000);
}

static uint32_t hda_single_command(struct hda_controller *c, uint32_t cmd, bool *ok) {
    *ok = false;
    if (!hda_wait16(c, HDA_REG_ICIS, HDA_ICIS_ICB, 0, 1000)) {
        hda_w16(c, HDA_REG_ICIS, 0);
        audio_delay_us(20);
    }
    hda_w16(c, HDA_REG_ICIS, (uint16_t)(hda_r16(c, HDA_REG_ICIS) | HDA_ICIS_IRV));
    hda_w32(c, HDA_REG_ICOI, cmd);
    hda_w16(c, HDA_REG_ICIS, (uint16_t)(hda_r16(c, HDA_REG_ICIS) | HDA_ICIS_ICB));
    for (uint32_t t = 0; t < HDA_CMD_TIMEOUT_US; t += 10) {
        uint16_t st = hda_r16(c, HDA_REG_ICIS);
        if ((st & (HDA_ICIS_ICB | HDA_ICIS_IRV)) == HDA_ICIS_IRV) {
            *ok = true;
            return hda_r32(c, HDA_REG_ICII);
        }
        audio_delay_us(10);
    }
    return 0;
}

static uint32_t hda_corb_command(struct hda_controller *c, uint32_t cmd, uint8_t cad, bool *ok) {
    *ok = false;
    hda_w8(c, HDA_REG_RIRBSTS, HDA_RIRBSTS_INTFL | HDA_RIRBSTS_OIS);
    uint16_t wp = (uint16_t)((c->corb_wp + 1) % c->corb_entries);
    c->corb[wp] = cmd;
    hda_mb();
    hda_w16(c, HDA_REG_CORBWP, wp);
    c->corb_wp = wp;

    for (uint32_t t = 0; t < HDA_CMD_TIMEOUT_US; t += 5) {
        uint16_t rwp = (uint16_t)(hda_r16(c, HDA_REG_RIRBWP) & 0xFF) % c->rirb_entries;
        uint32_t resp = 0;
        bool found = false;
        while (c->rirb_rp != rwp) {
            c->rirb_rp = (uint16_t)((c->rirb_rp + 1) % c->rirb_entries);
            hda_mb();
            uint64_t e = c->rirb[c->rirb_rp];
            uint32_t ex = (uint32_t)(e >> 32);
            if (ex & 0x10) continue;
            if ((ex & 0xF) != cad) continue;
            resp = (uint32_t)e;
            found = true;
        }
        if (found) {
            hda_w8(c, HDA_REG_RIRBSTS, HDA_RIRBSTS_INTFL | HDA_RIRBSTS_OIS);
            *ok = true;
            return resp;
        }
        audio_delay_us(5);
    }
    return 0;
}

static uint32_t hda_command(struct hda_controller *c, uint8_t cad, uint8_t nid, uint32_t verb_payload) {
    if (!c || !c->mmio) return 0;
    uint32_t cmd = ((uint32_t)(cad & 0xF) << 28) | ((uint32_t)(nid & 0x7F) << 20) | (verb_payload & 0xFFFFF);
    bool ok = false;
    uint32_t r = 0;

    spin_lock(&c->cmd_lock);
    if (!c->single_cmd) {
        if (c->quirks.flags & AUDIO_HDAC_Q_RIRB_DELAY) audio_delay_us(20);
        r = hda_corb_command(c, cmd, cad, &ok);
        if (!ok) {
            c->cmd_timeouts++;
            if (c->cmd_timeouts >= HDA_CMD_FAILS_BEFORE_SINGLE) {
                LOG_WARNING("%s: CORB/RIRB not answering, switching to immediate command mode", c->name);
                hda_stop_corb_rirb(c);
                c->single_cmd = 1;
            }
        } else {
            c->cmd_timeouts = 0;
        }
    }
    if (c->single_cmd && !ok) r = hda_single_command(c, cmd, &ok);
    spin_unlock(&c->cmd_lock);

    if (!ok) LOG_DEBUG("%s: codec %u nid 0x%02x verb 0x%05x timed out", c->name, (unsigned)cad, (unsigned)nid,
                       (unsigned)(verb_payload & 0xFFFFF));
    return ok ? r : 0;
}

static inline uint32_t hda_verb(struct hda_codec_dev *cd, uint8_t nid, uint16_t verb, uint16_t param) {
    return hda_command(cd->ctrl, cd->cad, nid, ((uint32_t)verb << 8) | param);
}

static inline uint32_t hda_param(struct hda_codec_dev *cd, uint8_t nid, uint8_t param) {
    return hda_verb(cd, nid, HDA_VERB_GET_PARAM, param);
}

static uint32_t hda_quirk_verb(void *backend, uint8_t nid, uint32_t verb_payload) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)backend;
    return hda_command(cd->ctrl, cd->cad, nid, verb_payload);
}

static struct hda_widget *hda_widget_get(struct hda_codec_dev *cd, uint8_t nid) {
    if (!cd->widgets || nid < cd->first_nid) return NULL;
    uint32_t i = (uint32_t)(nid - cd->first_nid);
    if (i >= cd->widget_count) return NULL;
    return &cd->widgets[i];
}

static void hda_set_amp(struct hda_codec_dev *cd, uint8_t nid, bool input, uint8_t index, uint8_t gain, bool mute) {
    uint16_t payload = (uint16_t)((input ? HDA_AMP_SET_IN : HDA_AMP_SET_OUT) | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                                  ((uint16_t)(index & 0xF) << HDA_AMP_SET_INDEX_SHIFT) |
                                  (mute ? HDA_AMP_MUTE : 0) | (gain & 0x7F));
    hda_verb(cd, nid, HDA_VERB_SET_AMP, payload);
}

static uint32_t hda_amp_caps(struct hda_codec_dev *cd, struct hda_widget *w, bool input) {
    (void)cd;
    if (!w) return 0;
    if (input) return (w->caps & HDA_WCAP_IN_AMP) ? w->amp_in_caps : 0;
    return (w->caps & HDA_WCAP_OUT_AMP) ? w->amp_out_caps : 0;
}

static void hda_power_up(struct hda_codec_dev *cd, uint8_t nid) {
    struct hda_widget *w = hda_widget_get(cd, nid);
    if (w && !(w->caps & HDA_WCAP_POWER)) return;
    hda_verb(cd, nid, HDA_VERB_SET_POWER, 0x00);
}

static void hda_read_conns(struct hda_codec_dev *cd, struct hda_widget *w) {
    w->conn_count = 0;
    if (!(w->caps & HDA_WCAP_CONN_LIST)) return;

    uint32_t len_param = hda_param(cd, w->nid, HDA_PARAM_CONN_LEN);
    bool long_form = (len_param & 0x80) != 0;
    uint32_t len = len_param & 0x7F;
    uint32_t per = long_form ? 2 : 4;
    uint32_t bits = long_form ? 16 : 8;
    uint32_t mask = long_form ? 0x7FFF : 0x7F;
    uint32_t range_flag = long_form ? 0x8000 : 0x80;
    uint32_t prev = 0;

    for (uint32_t i = 0; i < len; i += per) {
        uint32_t resp = hda_verb(cd, w->nid, HDA_VERB_GET_CONN_LIST, (uint16_t)i);
        for (uint32_t j = 0; j < per && i + j < len; j++) {
            uint32_t e = (resp >> (j * bits)) & ((1u << bits) - 1);
            uint32_t nid = e & mask;
            if ((e & range_flag) && prev && nid > prev) {
                for (uint32_t n = prev + 1; n <= nid && w->conn_count < HDA_MAX_CONNS; n++)
                    w->conns[w->conn_count++] = (uint8_t)n;
            } else if (w->conn_count < HDA_MAX_CONNS) {
                w->conns[w->conn_count++] = (uint8_t)nid;
            }
            prev = nid;
        }
    }
}

static void hda_parse_widgets(struct hda_codec_dev *cd) {
    for (uint8_t i = 0; i < cd->widget_count; i++) {
        struct hda_widget *w = &cd->widgets[i];
        memset(w, 0, sizeof(*w));
        w->nid = (uint8_t)(cd->first_nid + i);
        w->caps = hda_param(cd, w->nid, HDA_PARAM_WIDGET_CAPS);
        w->type = (uint8_t)HDA_WCAP_TYPE(w->caps);

        bool afg_amps = (cd->quirks.flags & AUDIO_HDA_Q_AMP_PARAM_AFG) != 0;
        if (w->caps & HDA_WCAP_IN_AMP)
            w->amp_in_caps = ((w->caps & HDA_WCAP_AMP_OVRD) && !afg_amps) ? hda_param(cd, w->nid, HDA_PARAM_AMP_IN_CAPS) : cd->afg_amp_in;
        if (w->caps & HDA_WCAP_OUT_AMP)
            w->amp_out_caps = ((w->caps & HDA_WCAP_AMP_OVRD) && !afg_amps) ? hda_param(cd, w->nid, HDA_PARAM_AMP_OUT_CAPS) : cd->afg_amp_out;

        if (w->type == HDA_WIDGET_AUDIO_OUT || w->type == HDA_WIDGET_AUDIO_IN) {
            if (w->caps & HDA_WCAP_FORMAT_OVRD) {
                w->pcm_caps = hda_param(cd, w->nid, HDA_PARAM_PCM);
                w->stream_fmts = hda_param(cd, w->nid, HDA_PARAM_STREAM_FMTS);
            } else {
                w->pcm_caps = cd->afg_pcm_caps;
                w->stream_fmts = cd->afg_stream_fmts;
            }
        }

        if (w->type == HDA_WIDGET_PIN) {
            w->pincap = hda_param(cd, w->nid, HDA_PARAM_PIN_CAPS);
            w->config_bios = hda_verb(cd, w->nid, HDA_VERB_GET_CONFIG, 0);
            w->config = w->config_bios;
            if (audio_quirk_apply_pins(&cd->quirks, w->nid, &w->config))
                LOG_INFO("%s: pin 0x%02x config 0x%08x -> 0x%08x (quirk)", cd->adev.name, (unsigned)w->nid,
                         w->config_bios, w->config);
        }

        hda_read_conns(cd, w);
    }
}

static bool hda_path_contains(const struct hda_path *p, uint8_t depth, uint8_t nid) {
    for (uint8_t i = 0; i < depth; i++)
        if (p->nodes[i] == nid) return true;
    return false;
}

enum hda_target { HDA_TARGET_DAC, HDA_TARGET_NID };

static bool hda_search(struct hda_codec_dev *cd, uint8_t nid, uint8_t depth, uint8_t limit, enum hda_target target,
                       uint8_t target_nid, bool digital, struct hda_path *path) {
    struct hda_widget *w = hda_widget_get(cd, nid);
    if (!w || depth >= HDA_MAX_PATH || depth >= limit) return false;

    path->nodes[depth] = nid;
    path->idx[depth] = 0;
    path->len = (uint8_t)(depth + 1);

    if (depth > 0) {
        if (target == HDA_TARGET_DAC && w->type == HDA_WIDGET_AUDIO_OUT) {
            bool w_digital = (w->caps & HDA_WCAP_DIGITAL) != 0;
            return w_digital == digital;
        }
        if (target == HDA_TARGET_NID && nid == target_nid) return true;
        if (w->type != HDA_WIDGET_MIXER && w->type != HDA_WIDGET_SELECTOR) return false;
        if (target == HDA_TARGET_DAC && (w->used & 2)) return false;
        if (target == HDA_TARGET_NID && (w->used & 1)) return false;
    }

    bool single = (w->type != HDA_WIDGET_MIXER);
    for (uint8_t i = 0; i < w->conn_count; i++) {
        if (single && w->claimed && w->claim_idx != i) continue;
        uint8_t child = w->conns[i];
        if (hda_path_contains(path, (uint8_t)(depth + 1), child)) continue;
        path->idx[depth] = i;
        if (hda_search(cd, child, (uint8_t)(depth + 1), limit, target, target_nid, digital, path)) return true;
    }
    path->len = depth;
    return false;
}

static bool hda_find_path(struct hda_codec_dev *cd, uint8_t start, enum hda_target target, uint8_t target_nid,
                          bool digital, struct hda_path *out) {
    for (uint8_t limit = 2; limit <= HDA_MAX_PATH; limit++) {
        struct hda_path p;
        memset(&p, 0, sizeof(p));
        if (hda_search(cd, start, 0, limit, target, target_nid, digital, &p)) {
            *out = p;
            return true;
        }
    }
    return false;
}

static void hda_claim_path(struct hda_codec_dev *cd, const struct hda_path *p, uint8_t use_bit) {
    for (uint8_t i = 0; i < p->len; i++) {
        struct hda_widget *w = hda_widget_get(cd, p->nodes[i]);
        if (!w) continue;
        w->used |= use_bit;
        if (i + 1 < p->len && w->type != HDA_WIDGET_MIXER && w->conn_count > 1) {
            w->claimed = 1;
            w->claim_idx = p->idx[i];
        }
    }
}

static uint8_t hda_out_vref(uint64_t flags) {
    if (flags & AUDIO_HDA_Q_OVREF100) return HDA_PINCTL_VREF_100;
    if (flags & AUDIO_HDA_Q_OVREF80) return HDA_PINCTL_VREF_80;
    if (flags & AUDIO_HDA_Q_OVREF50) return HDA_PINCTL_VREF_50;
    return HDA_PINCTL_VREF_HIZ;
}

static uint8_t hda_in_vref(struct hda_widget *pin, uint64_t flags, bool mic) {
    uint32_t vcaps = (pin->pincap >> HDA_PINCAP_VREF_SHIFT) & 0xFF;
    if (flags & AUDIO_HDA_Q_IVREF100) return (vcaps & (1u << 5)) ? HDA_PINCTL_VREF_100 : 0;
    if (flags & AUDIO_HDA_Q_IVREF80) return (vcaps & (1u << 4)) ? HDA_PINCTL_VREF_80 : 0;
    if (flags & AUDIO_HDA_Q_IVREF50) return (vcaps & (1u << 1)) ? HDA_PINCTL_VREF_50 : 0;
    if (!mic) return HDA_PINCTL_VREF_HIZ;
    if (vcaps & (1u << 4)) return HDA_PINCTL_VREF_80;
    if (vcaps & (1u << 1)) return HDA_PINCTL_VREF_50;
    return HDA_PINCTL_VREF_HIZ;
}

static void hda_set_eapd(struct hda_codec_dev *cd, struct hda_widget *w, bool on) {
    if (!w || !(w->pincap & HDA_PINCAP_EAPD)) return;
    if (cd->quirks.flags & AUDIO_HDA_Q_NO_EAPD) return;
    bool inv = (cd->quirks.flags & AUDIO_HDA_Q_EAPDINV) != 0;
    uint32_t cur = hda_verb(cd, w->nid, HDA_VERB_GET_EAPD, 0) & 0x7;
    if (on != inv) cur |= 0x2;
    else cur &= ~0x2u;
    hda_verb(cd, w->nid, HDA_VERB_SET_EAPD, (uint16_t)cur);
}

static uint8_t hda_unity_gain(uint32_t caps) {
    uint8_t off = (uint8_t)HDA_AMPCAP_OFFSET(caps);
    uint8_t steps = (uint8_t)HDA_AMPCAP_STEPS(caps);
    return off > steps ? steps : off;
}

static void hda_pick_volume(struct hda_codec_dev *cd, struct hda_output *o) {
    o->vol_nid = 0;
    for (int i = (int)o->path.len - 1; i >= 0; i--) {
        struct hda_widget *w = hda_widget_get(cd, o->path.nodes[i]);
        uint32_t caps = hda_amp_caps(cd, w, false);
        if (caps && HDA_AMPCAP_STEPS(caps) > 0 && HDA_AMPCAP_OFFSET(caps) > 0) {
            o->vol_nid = w->nid;
            o->vol_in_amp = 0;
            o->vol_index = 0;
            return;
        }
    }
    for (int i = (int)o->path.len - 2; i >= 0; i--) {
        struct hda_widget *w = hda_widget_get(cd, o->path.nodes[i]);
        uint32_t caps = hda_amp_caps(cd, w, true);
        if (caps && HDA_AMPCAP_STEPS(caps) > 0 && HDA_AMPCAP_OFFSET(caps) > 0) {
            o->vol_nid = w->nid;
            o->vol_in_amp = 1;
            o->vol_index = o->path.idx[i];
            return;
        }
    }
}

static void hda_configure_output(struct hda_codec_dev *cd, struct hda_output *o) {
    for (uint8_t i = 0; i < o->path.len; i++) {
        uint8_t nid = o->path.nodes[i];
        struct hda_widget *w = hda_widget_get(cd, nid);
        if (!w) continue;
        hda_power_up(cd, nid);

        bool has_next = (i + 1 < o->path.len);
        if (has_next && w->conn_count > 1 && w->type != HDA_WIDGET_MIXER)
            hda_verb(cd, nid, HDA_VERB_SET_CONN_SEL, o->path.idx[i]);

        uint32_t in_caps = hda_amp_caps(cd, w, true);
        if (has_next && (w->caps & HDA_WCAP_IN_AMP))
            hda_set_amp(cd, nid, true, o->path.idx[i], hda_unity_gain(in_caps), false);

        uint32_t out_caps = hda_amp_caps(cd, w, false);
        if (w->caps & HDA_WCAP_OUT_AMP)
            hda_set_amp(cd, nid, false, 0, hda_unity_gain(out_caps), false);
    }

    struct hda_widget *pin = hda_widget_get(cd, o->pin);
    if (!pin) return;
    uint8_t ctl = HDA_PINCTL_OUT_EN | hda_out_vref(cd->quirks.flags);
    if (o->device == HDA_DEV_HP_OUT && (pin->pincap & HDA_PINCAP_HP_DRIVE)) ctl |= HDA_PINCTL_HP_EN;
    hda_verb(cd, o->pin, HDA_VERB_SET_PIN_CTL, ctl);
    hda_set_eapd(cd, pin, true);
    o->enabled = 1;
}

static void hda_configure_input(struct hda_codec_dev *cd) {
    struct hda_path *p = &cd->in_path;
    for (uint8_t i = 0; i < p->len; i++) {
        uint8_t nid = p->nodes[i];
        struct hda_widget *w = hda_widget_get(cd, nid);
        if (!w) continue;
        hda_power_up(cd, nid);
        bool has_next = (i + 1 < p->len);
        if (has_next && w->conn_count > 1 && w->type != HDA_WIDGET_MIXER)
            hda_verb(cd, nid, HDA_VERB_SET_CONN_SEL, p->idx[i]);
        if (has_next && (w->caps & HDA_WCAP_IN_AMP))
            hda_set_amp(cd, nid, true, p->idx[i], hda_unity_gain(hda_amp_caps(cd, w, true)), false);
        if (i > 0 && (w->caps & HDA_WCAP_OUT_AMP))
            hda_set_amp(cd, nid, false, 0, hda_unity_gain(hda_amp_caps(cd, w, false)), false);
    }

    struct hda_widget *pin = hda_widget_get(cd, cd->in_pin);
    if (!pin) return;
    bool mic = HDA_CFG_DEVICE(pin->config) == HDA_DEV_MIC_IN;
    uint8_t ctl = HDA_PINCTL_IN_EN | hda_in_vref(pin, cd->quirks.flags, mic);
    hda_verb(cd, pin->nid, HDA_VERB_SET_PIN_CTL, ctl);
    if (pin->caps & HDA_WCAP_IN_AMP)
        hda_set_amp(cd, pin->nid, true, 0, 0, false);

    cd->in_vol_nid = 0;
    for (uint8_t i = 0; i + 1 < p->len; i++) {
        struct hda_widget *w = hda_widget_get(cd, p->nodes[i]);
        uint32_t caps = hda_amp_caps(cd, w, true);
        if (caps && HDA_AMPCAP_STEPS(caps) > 0) {
            cd->in_vol_nid = w->nid;
            cd->in_vol_in_amp = 1;
            cd->in_vol_index = p->idx[i];
            break;
        }
    }
}

static int hda_output_rank(const struct hda_widget *w) {
    uint8_t as = (uint8_t)HDA_CFG_ASSOC(w->config);
    return ((as == 0 ? 16 : as) << 4) | (int)HDA_CFG_SEQ(w->config);
}

static bool hda_pin_is_digital(const struct hda_widget *w) {
    uint8_t dev = (uint8_t)HDA_CFG_DEVICE(w->config);
    return (w->caps & HDA_WCAP_DIGITAL) || dev == HDA_DEV_SPDIF_OUT || dev == HDA_DEV_DIGITAL_OUT ||
           (w->pincap & (HDA_PINCAP_HDMI | HDA_PINCAP_DP));
}

static void hda_add_output(struct hda_codec_dev *cd, struct hda_widget *w, bool digital) {
    if (cd->out_count >= HDA_MAX_OUTPUTS) return;
    struct hda_path path;
    if (!hda_find_path(cd, w->nid, HDA_TARGET_DAC, 0, digital, &path)) {
        LOG_DEBUG("%s: no DAC path for pin 0x%02x", cd->adev.name, (unsigned)w->nid);
        return;
    }
    hda_claim_path(cd, &path, 1);

    struct hda_output *o = &cd->outs[cd->out_count++];
    memset(o, 0, sizeof(*o));
    o->pin = w->nid;
    o->device = (uint8_t)HDA_CFG_DEVICE(w->config);
    o->assoc = (uint8_t)HDA_CFG_ASSOC(w->config);
    o->seq = (uint8_t)HDA_CFG_SEQ(w->config);
    o->jack_sense = (w->pincap & HDA_PINCAP_PRES_DETECT) && HDA_CFG_CONN(w->config) == AUDIO_HDA_PIN_CONN_JACK &&
                    !(HDA_CFG_MISC(w->config) & 0x1);
    o->path = path;

    uint8_t dac = path.nodes[path.len - 1];
    bool known = false;
    for (uint8_t i = 0; i < cd->dac_count; i++)
        if (cd->dacs[i] == dac) known = true;
    if (!known && cd->dac_count < HDA_MAX_DACS) cd->dacs[cd->dac_count++] = dac;
    hda_pick_volume(cd, o);
}

static void hda_build_outputs(struct hda_codec_dev *cd) {
    uint64_t flags = cd->quirks.flags;
    bool ignore_cfg = (flags & AUDIO_HDA_Q_IGNORE_PIN_CONFIG) != 0;
    bool allow_digital = (flags & AUDIO_HDA_Q_DIGITAL_OUT) != 0;
    struct hda_widget *cand[HDA_MAX_OUTPUTS * 2];
    int n = 0;
    int digital_pins = 0;

    for (int pass = 0; pass < 2 && n == 0; pass++) {
        bool loose = ignore_cfg || pass == 1;
        for (uint8_t i = 0; i < cd->widget_count && n < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
            struct hda_widget *w = &cd->widgets[i];
            if (w->type != HDA_WIDGET_PIN || !(w->pincap & HDA_PINCAP_OUT)) continue;
            uint8_t dev = (uint8_t)HDA_CFG_DEVICE(w->config);
            bool digital = hda_pin_is_digital(w);
            if (digital) {
                if (pass == 0) digital_pins++;
                if (!allow_digital) continue;
            }
            if (!loose) {
                if (HDA_CFG_CONN(w->config) == AUDIO_HDA_PIN_CONN_NONE) continue;
                if (!digital && dev != HDA_DEV_LINE_OUT && dev != HDA_DEV_SPEAKER && dev != HDA_DEV_HP_OUT) continue;
            } else if (dev == HDA_DEV_MIC_IN || dev == HDA_DEV_LINE_IN || dev == HDA_DEV_CD) {
                continue;
            }
            cand[n++] = w;
        }
        if (n == 0 && pass == 0 && !ignore_cfg)
            LOG_DEBUG("%s: no usable output pin configs, trying fallback", cd->adev.name);
    }

    for (int i = 1; i < n; i++) {
        struct hda_widget *k = cand[i];
        int j = i - 1;
        while (j >= 0 && hda_output_rank(cand[j]) > hda_output_rank(k)) {
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = k;
    }

    for (int i = 0; i < n; i++) hda_add_output(cd, cand[i], hda_pin_is_digital(cand[i]));

    cd->is_digital = (cd->out_count > 0);
    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_widget *w = hda_widget_get(cd, cd->outs[i].pin);
        if (w && !hda_pin_is_digital(w)) cd->is_digital = 0;
    }
    if (cd->out_count == 0 && digital_pins)
        LOG_INFO("%s: only digital (HDMI/DP/S/PDIF) outputs, set DIGITAL_OUT quirk to use them", cd->adev.name);
}

static int hda_input_rank(const struct hda_widget *w) {
    uint8_t dev = (uint8_t)HDA_CFG_DEVICE(w->config);
    uint8_t conn = (uint8_t)HDA_CFG_CONN(w->config);
    if (dev == HDA_DEV_MIC_IN && conn == AUDIO_HDA_PIN_CONN_JACK) return 0;
    if (dev == HDA_DEV_MIC_IN) return 1;
    if (dev == HDA_DEV_LINE_IN) return 2;
    if (dev == HDA_DEV_AUX) return 3;
    return 9;
}

static void hda_build_input(struct hda_codec_dev *cd) {
    cd->has_input = 0;
    if (cd->quirks.flags & AUDIO_HDA_Q_NO_CAPTURE) return;

    struct hda_widget *best[8];
    int n = 0;
    for (uint8_t i = 0; i < cd->widget_count && n < 8; i++) {
        struct hda_widget *w = &cd->widgets[i];
        if (w->type != HDA_WIDGET_PIN || !(w->pincap & HDA_PINCAP_IN)) continue;
        if (w->used & 1) continue;
        if (HDA_CFG_CONN(w->config) == AUDIO_HDA_PIN_CONN_NONE) continue;
        if (hda_input_rank(w) >= 9 || (w->caps & HDA_WCAP_DIGITAL)) continue;
        best[n++] = w;
    }
    for (int i = 1; i < n; i++) {
        struct hda_widget *k = best[i];
        int j = i - 1;
        while (j >= 0 && hda_input_rank(best[j]) > hda_input_rank(k)) {
            best[j + 1] = best[j];
            j--;
        }
        best[j + 1] = k;
    }

    for (int p = 0; p < n; p++) {
        for (uint8_t i = 0; i < cd->widget_count; i++) {
            struct hda_widget *adc = &cd->widgets[i];
            if (adc->type != HDA_WIDGET_AUDIO_IN || (adc->caps & HDA_WCAP_DIGITAL)) continue;
            struct hda_path path;
            if (!hda_find_path(cd, adc->nid, HDA_TARGET_NID, best[p]->nid, false, &path)) continue;
            hda_claim_path(cd, &path, 2);
            cd->in_path = path;
            cd->in_pin = best[p]->nid;
            cd->adc = adc->nid;
            cd->has_input = 1;
            return;
        }
    }
}

static void hda_apply_gpio(struct hda_codec_dev *cd) {
    uint32_t mask = cd->quirks.gpio_mask;
    uint32_t dir = cd->quirks.gpio_dir;
    uint32_t data = cd->quirks.gpio_data;
    uint32_t qbits = (uint32_t)(cd->quirks.flags & AUDIO_HDA_Q_GPIO_MASK);
    mask |= qbits;
    dir |= qbits;
    data |= qbits;
    if (!mask) return;

    uint32_t count = cd->gpio_caps & 0xFF;
    if (count == 0) LOG_WARNING("%s: GPIO quirk requested but codec reports no GPIOs", cd->adev.name);
    if (count && count < 32) mask &= (1u << count) - 1;
    if (!mask) return;

    uint32_t cur_mask = hda_verb(cd, cd->afg, HDA_VERB_GET_GPIO_MASK, 0) & 0xFF;
    uint32_t cur_dir = hda_verb(cd, cd->afg, HDA_VERB_GET_GPIO_DIR, 0) & 0xFF;
    uint32_t cur_data = hda_verb(cd, cd->afg, HDA_VERB_GET_GPIO_DATA, 0) & 0xFF;
    cur_mask |= mask;
    cur_dir = (cur_dir & ~mask) | (dir & mask);
    cur_data = (cur_data & ~mask) | (data & mask);
    hda_verb(cd, cd->afg, HDA_VERB_SET_GPIO_MASK, (uint16_t)cur_mask);
    hda_verb(cd, cd->afg, HDA_VERB_SET_GPIO_DIR, (uint16_t)cur_dir);
    audio_delay_ms(1);
    hda_verb(cd, cd->afg, HDA_VERB_SET_GPIO_DATA, (uint16_t)cur_data);
    LOG_INFO("%s: GPIO mask 0x%02x dir 0x%02x data 0x%02x", cd->adev.name, cur_mask, cur_dir, cur_data);
}

static void hda_quirk_ctx_init(struct hda_codec_dev *cd, struct audio_quirk_ctx *ctx, enum audio_quirk_stage stage) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->domain = AUDIO_QUIRK_HDA_CODEC;
    ctx->stage = stage;
    ctx->dev = &cd->adev;
    ctx->backend = cd;
    ctx->codec_addr = cd->cad;
    ctx->afg_nid = cd->afg;
    ctx->verb = hda_quirk_verb;
}

static uint16_t hda_format_reg(const struct audio_format *fmt, uint32_t pcm_caps) {
    uint16_t base = 0, mult = 0, div = 0;
    switch (fmt->sample_rate) {
        case 8000: div = 5; break;
        case 11025: base = 1; div = 3; break;
        case 16000: div = 2; break;
        case 22050: base = 1; div = 1; break;
        case 24000: div = 1; break;
        case 32000: mult = 1; div = 2; break;
        case 44100: base = 1; break;
        case 48000: break;
        case 88200: base = 1; mult = 1; break;
        case 96000: mult = 1; break;
        case 176400: base = 1; mult = 3; break;
        case 192000: mult = 3; break;
        default: return 0xFFFF;
    }
    uint16_t bits;
    switch (fmt->format) {
        case AUDIO_FMT_U8: bits = 0; break;
        case AUDIO_FMT_S16_LE: bits = 1; break;
        case AUDIO_FMT_S32_LE:
            if (pcm_caps & HDA_PCM_BITS_32) bits = 4;
            else if (pcm_caps & HDA_PCM_BITS_24) bits = 3;
            else if (pcm_caps & HDA_PCM_BITS_20) bits = 2;
            else return 0xFFFF;
            break;
        default: return 0xFFFF;
    }
    if (fmt->channels == 0 || fmt->channels > 16) return 0xFFFF;
    return (uint16_t)((base << 14) | (mult << 11) | (div << 8) | (bits << 4) | (uint16_t)(fmt->channels - 1));
}

static uint32_t hda_read_position(struct hda_codec_dev *cd, struct hda_stream_hw *hw) {
    struct hda_controller *c = cd->ctrl;
    uint32_t pos;
    if ((c->quirks.flags & AUDIO_HDAC_Q_DMAPOS) && c->dmapos) pos = c->dmapos[hw->sd * 2];
    else pos = hda_r32(c, hda_sd(hw->sd, HDA_SD_LPIB));
    if (pos >= hw->buf_size) pos %= hw->buf_size;
    return pos;
}

static void hda_free_stream_buffers(struct hda_stream_hw *hw) {
    if (hw->buf) audio_dma_free(hw->buf, hw->buf_phys, hw->buf_pages);
    if (hw->bdl) audio_dma_free(hw->bdl, hw->bdl_phys, hw->bdl_pages);
    hw->buf = NULL;
    hw->bdl = NULL;
    hw->buf_phys = hw->bdl_phys = 0;
    hw->buf_pages = hw->bdl_pages = 0;
    hw->buf_size = 0;
}

static void hda_sd_halt(struct hda_controller *c, int sd) {
    uint32_t ctl = hda_r32(c, hda_sd(sd, HDA_SD_CTL));
    if (ctl & HDA_SDCTL_RUN) {
        hda_w32(c, hda_sd(sd, HDA_SD_CTL), ctl & ~(HDA_SDCTL_RUN | HDA_SDCTL_IOCE | HDA_SDCTL_FEIE | HDA_SDCTL_DEIE));
        hda_wait32(c, hda_sd(sd, HDA_SD_CTL), HDA_SDCTL_RUN, 0, 10000);
    }
    hda_w8(c, hda_sd(sd, HDA_SD_STS), HDA_SDSTS_BCIS | HDA_SDSTS_FIFOE | HDA_SDSTS_DESE);
}

static int hda_sd_program(struct hda_codec_dev *cd, struct hda_stream_hw *hw, uint8_t direction) {
    struct hda_controller *c = cd->ctrl;
    int sd = hw->sd;

    hda_sd_halt(c, sd);

    uint32_t ctl = hda_r32(c, hda_sd(sd, HDA_SD_CTL));
    hda_w32(c, hda_sd(sd, HDA_SD_CTL), ctl | HDA_SDCTL_SRST);
    bool in_reset = hda_wait32(c, hda_sd(sd, HDA_SD_CTL), HDA_SDCTL_SRST, HDA_SDCTL_SRST, 10000);
    ctl = hda_r32(c, hda_sd(sd, HDA_SD_CTL));
    hda_w32(c, hda_sd(sd, HDA_SD_CTL), ctl & ~HDA_SDCTL_SRST);
    bool out_reset = hda_wait32(c, hda_sd(sd, HDA_SD_CTL), HDA_SDCTL_SRST, 0, 10000);
    if ((!in_reset || !out_reset) && !(c->quirks.flags & AUDIO_HDAC_Q_NO_STREAM_RESET_WAIT))
        LOG_WARNING("%s: stream %d reset handshake incomplete (enter %d exit %d)", c->name, sd, (int)in_reset, (int)out_reset);

    for (uint32_t i = 0; i < hw->periods; i++) {
        hw->bdl[i].addr = hw->buf_phys + (uint64_t)i * hw->period_bytes;
        hw->bdl[i].len = hw->period_bytes;
        hw->bdl[i].ioc = 0;
    }
    hda_mb();

    hda_w32(c, hda_sd(sd, HDA_SD_BDPL), (uint32_t)hw->bdl_phys);
    hda_w32(c, hda_sd(sd, HDA_SD_BDPU), (uint32_t)(hw->bdl_phys >> 32));
    hda_w32(c, hda_sd(sd, HDA_SD_CBL), hw->buf_size);
    hda_w16(c, hda_sd(sd, HDA_SD_LVI), (uint16_t)(hw->periods - 1));
    hda_w16(c, hda_sd(sd, HDA_SD_FMT), hw->fmt_reg);

    ctl = hda_r32(c, hda_sd(sd, HDA_SD_CTL));
    ctl &= ~(0xFu << HDA_SDCTL_STRM_SHIFT);
    ctl &= ~(HDA_SDCTL_IOCE | HDA_SDCTL_FEIE | HDA_SDCTL_DEIE | HDA_SDCTL_RUN);
    ctl |= (uint32_t)(hw->tag & 0xF) << HDA_SDCTL_STRM_SHIFT;
    if (sd >= c->iss + c->oss) {
        if (direction == AUDIO_DIR_PLAYBACK) ctl |= HDA_SDCTL_DIR;
        else ctl &= ~HDA_SDCTL_DIR;
    }
    hda_w32(c, hda_sd(sd, HDA_SD_CTL), ctl);
    hda_w8(c, hda_sd(sd, HDA_SD_STS), HDA_SDSTS_BCIS | HDA_SDSTS_FIFOE | HDA_SDSTS_DESE);

    if (c->dmapos) c->dmapos[sd * 2] = 0;
    hw->last_pos = 0;
    hw->played = 0;
    hw->written = 0;
    hw->sw_off = 0;
    return AUDIO_OK;
}

static void hda_pump_playback(struct hda_codec_dev *cd, struct hda_stream_hw *hw, bool prefill) {
    struct audio_stream *s = &cd->adev.streams[AUDIO_DIR_PLAYBACK];
    uint32_t fb = audio_frame_bytes(&s->hw_fmt);
    if (!fb || !hw->buf) return;

    if (!prefill) {
        uint32_t pos = hda_read_position(cd, hw);
        uint32_t delta = (pos + hw->buf_size - hw->last_pos) % hw->buf_size;
        hw->last_pos = pos;
        hw->played += delta;
        if (hw->written < hw->played) {
            hw->xruns++;
            s->underruns++;
            uint32_t rem = pos % fb;
            hw->written = hw->played + (rem ? fb - rem : 0);
            hw->sw_off = (uint32_t)(hw->written % hw->buf_size);
        }
    }

    uint64_t queued = hw->written - hw->played;
    if (queued + hw->guard_bytes >= hw->buf_size) return;
    uint32_t space = (uint32_t)(hw->buf_size - hw->guard_bytes - queued);
    space -= space % fb;

    while (space > 0) {
        uint32_t chunk = hw->buf_size - hw->sw_off;
        if (chunk > space) chunk = space;
        chunk -= chunk % fb;
        if (chunk == 0) {
            hw->sw_off = 0;
            continue;
        }
        audio_stream_render(&cd->adev, s, hw->buf + hw->sw_off, chunk);
        hw->sw_off = (hw->sw_off + chunk) % hw->buf_size;
        hw->written += chunk;
        space -= chunk;
    }
    hda_mb();
}

static void hda_pump_capture(struct hda_codec_dev *cd, struct hda_stream_hw *hw) {
    struct audio_stream *s = &cd->adev.streams[AUDIO_DIR_CAPTURE];
    uint32_t fb = audio_frame_bytes(&s->hw_fmt);
    if (!fb || !hw->buf) return;

    uint32_t pos = hda_read_position(cd, hw);
    pos -= pos % fb;
    while (hw->sw_off != pos) {
        uint32_t end = (pos > hw->sw_off) ? pos : hw->buf_size;
        uint32_t chunk = end - hw->sw_off;
        audio_stream_capture(&cd->adev, s, hw->buf + hw->sw_off, chunk);
        hw->sw_off = (hw->sw_off + chunk) % hw->buf_size;
        hw->played += chunk;
    }
}

static void hda_codec_stream_setup(struct hda_codec_dev *cd, uint8_t direction, uint16_t fmt_reg, uint8_t tag, uint8_t channels) {
    if (direction == AUDIO_DIR_PLAYBACK) {
        for (uint8_t i = 0; i < cd->dac_count; i++) {
            uint8_t dac = cd->dacs[i];
            hda_power_up(cd, dac);
            hda_verb(cd, dac, HDA_VERB_SET_CONV_FMT, fmt_reg);
            if (channels > 2) hda_verb(cd, dac, HDA_VERB_SET_CHAN_COUNT, (uint16_t)(channels - 1));
            hda_verb(cd, dac, HDA_VERB_SET_CONV_STREAM, (uint16_t)((tag & 0xF) << 4));
            struct hda_widget *w = hda_widget_get(cd, dac);
            if (w && (w->caps & HDA_WCAP_DIGITAL)) hda_verb(cd, dac, HDA_VERB_SET_DIGI_CONV_1, 0x01);
        }
    } else if (cd->has_input) {
        hda_power_up(cd, cd->adc);
        hda_verb(cd, cd->adc, HDA_VERB_SET_CONV_FMT, fmt_reg);
        hda_verb(cd, cd->adc, HDA_VERB_SET_CONV_STREAM, (uint16_t)((tag & 0xF) << 4));
    }
}

static void hda_codec_stream_release(struct hda_codec_dev *cd, uint8_t direction) {
    if (direction == AUDIO_DIR_PLAYBACK) {
        for (uint8_t i = 0; i < cd->dac_count; i++) hda_verb(cd, cd->dacs[i], HDA_VERB_SET_CONV_STREAM, 0);
    } else if (cd->has_input) {
        hda_verb(cd, cd->adc, HDA_VERB_SET_CONV_STREAM, 0);
    }
}

static int hda_dev_open(struct audio_device *adev, uint8_t direction, const struct audio_format *fmt) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT || !fmt) return AUDIO_ERR_PARAM;
    if (direction == AUDIO_DIR_PLAYBACK && cd->dac_count == 0) return AUDIO_ERR_UNSUPPORTED;
    if (direction == AUDIO_DIR_CAPTURE && !cd->has_input) return AUDIO_ERR_UNSUPPORTED;

    struct hda_stream_hw *hw = &cd->hw[direction];
    if (hw->sd < 0) return AUDIO_ERR_BUSY;

    uint32_t caps = (direction == AUDIO_DIR_PLAYBACK) ? hda_widget_get(cd, cd->dacs[0])->pcm_caps
                                                     : hda_widget_get(cd, cd->adc)->pcm_caps;
    uint16_t fmt_reg = hda_format_reg(fmt, caps);
    if (fmt_reg == 0xFFFF) return AUDIO_ERR_UNSUPPORTED;

    uint32_t fb = audio_frame_bytes(fmt);
    uint32_t raw = (fmt->sample_rate / 1000 + 1) * fb * HDA_PERIOD_MS;
    uint32_t period = (raw + 127) & ~127u;
    while (period % fb) period += 128;

    uint32_t size = period * HDA_PERIODS;
    if (!hw->buf || hw->buf_size != size) {
        hda_free_stream_buffers(hw);
        hw->buf = (uint8_t *)audio_dma_alloc(size, !cd->ctrl->ok64, &hw->buf_phys, &hw->buf_pages);
        hw->bdl = (struct hda_bdl_entry *)audio_dma_alloc(sizeof(struct hda_bdl_entry) * 256, !cd->ctrl->ok64, &hw->bdl_phys, &hw->bdl_pages);
        if (!hw->buf || !hw->bdl) {
            hda_free_stream_buffers(hw);
            return AUDIO_ERR_NOMEM;
        }
        hw->buf_size = size;
    }
    memset(hw->buf, 0, hw->buf_size);
    hw->period_bytes = period;
    hw->periods = HDA_PERIODS;
    hw->guard_bytes = period;
    hw->fmt_reg = fmt_reg;
    hw->running = 0;
    hw->prepared = 1;

    hda_codec_stream_setup(cd, direction, fmt_reg, hw->tag, fmt->channels);
    adev->streams[direction].hw_buffer_bytes = size;

    struct audio_quirk_ctx ctx;
    hda_quirk_ctx_init(cd, &ctx, AUDIO_QUIRK_STAGE_OPEN);
    audio_quirk_run_hooks(&cd->quirks, &ctx);

    LOG_DEBUG("%s: %s stream sd %d tag %u fmt 0x%04x buffer %u x %u", adev->name,
              direction ? "capture" : "playback", hw->sd, (unsigned)hw->tag, (unsigned)fmt_reg,
              (unsigned)hw->periods, (unsigned)period);
    return AUDIO_OK;
}

static int hda_dev_start(struct audio_device *adev, uint8_t direction) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    struct hda_stream_hw *hw = &cd->hw[direction];
    if (!hw->prepared) return AUDIO_ERR_PARAM;

    hda_sd_program(cd, hw, direction);
    if (direction == AUDIO_DIR_PLAYBACK) hda_pump_playback(cd, hw, true);

    struct hda_controller *c = cd->ctrl;
    uint32_t ctl = hda_r32(c, hda_sd(hw->sd, HDA_SD_CTL));
    hda_w32(c, hda_sd(hw->sd, HDA_SD_CTL), ctl | HDA_SDCTL_RUN);
    if (!hda_wait32(c, hda_sd(hw->sd, HDA_SD_CTL), HDA_SDCTL_RUN, HDA_SDCTL_RUN, 1000)) {
        LOG_ERROR("%s: stream %d did not start", adev->name, hw->sd);
        return AUDIO_ERR_IO;
    }
    hw->running = 1;
    return AUDIO_OK;
}

static int hda_dev_stop(struct audio_device *adev, uint8_t direction) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    struct hda_stream_hw *hw = &cd->hw[direction];
    if (hw->sd >= 0) hda_sd_halt(cd->ctrl, hw->sd);
    hw->running = 0;
    return AUDIO_OK;
}

static int hda_dev_pause(struct audio_device *adev, uint8_t direction, bool paused) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    struct hda_stream_hw *hw = &cd->hw[direction];
    struct hda_controller *c = cd->ctrl;
    if (!hw->prepared || hw->sd < 0) return AUDIO_ERR_PARAM;
    uint32_t ctl = hda_r32(c, hda_sd(hw->sd, HDA_SD_CTL));
    if (paused) {
        hda_w32(c, hda_sd(hw->sd, HDA_SD_CTL), ctl & ~HDA_SDCTL_RUN);
        hda_wait32(c, hda_sd(hw->sd, HDA_SD_CTL), HDA_SDCTL_RUN, 0, 10000);
        hw->running = 0;
    } else {
        hda_w32(c, hda_sd(hw->sd, HDA_SD_CTL), ctl | HDA_SDCTL_RUN);
        hw->running = 1;
    }
    return AUDIO_OK;
}

static int hda_dev_close(struct audio_device *adev, uint8_t direction) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    struct hda_stream_hw *hw = &cd->hw[direction];
    if (hw->running) hda_dev_stop(adev, direction);
    hda_codec_stream_release(cd, direction);
    hda_free_stream_buffers(hw);
    hw->prepared = 0;

    struct audio_quirk_ctx ctx;
    hda_quirk_ctx_init(cd, &ctx, AUDIO_QUIRK_STAGE_CLOSE);
    audio_quirk_run_hooks(&cd->quirks, &ctx);
    return AUDIO_OK;
}

static uint32_t hda_dev_get_position(struct audio_device *adev, uint8_t direction) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    return (uint32_t)cd->hw[direction].played;
}

static void hda_apply_volume_amp(struct hda_codec_dev *cd, uint8_t nid, bool in_amp, uint8_t index, uint8_t volume, bool mute,
                                 bool capture) {
    struct hda_widget *w = hda_widget_get(cd, nid);
    uint32_t caps = hda_amp_caps(cd, w, in_amp);
    if (!caps) return;
    uint32_t steps = HDA_AMPCAP_STEPS(caps);
    uint32_t off = HDA_AMPCAP_OFFSET(caps);
    if (off > steps) off = steps;
    uint32_t gain;
    if (capture) {
        gain = off ? (off * volume) / 75 : (steps * volume) / 100;
        if (gain > steps) gain = steps;
    } else {
        gain = (off * volume + AUDIO_VOLUME_MAX / 2) / AUDIO_VOLUME_MAX;
    }
    bool m = mute || volume == 0;
    if (m && !(caps & HDA_AMPCAP_MUTE)) gain = 0;
    hda_set_amp(cd, nid, in_amp, index, (uint8_t)gain, m && (caps & HDA_AMPCAP_MUTE));
}

static int hda_dev_set_volume(struct audio_device *adev, uint8_t direction, uint8_t volume) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    bool mute = adev->streams[direction].muted && !adev->streams[direction].sw_mute;
    if (direction == AUDIO_DIR_CAPTURE) {
        if (!cd->in_vol_nid) return AUDIO_ERR_UNSUPPORTED;
        hda_apply_volume_amp(cd, cd->in_vol_nid, cd->in_vol_in_amp, cd->in_vol_index, volume, mute, true);
        return AUDIO_OK;
    }
    if (cd->quirks.flags & AUDIO_HDA_Q_SOFTPCMVOL) return AUDIO_ERR_UNSUPPORTED;
    int applied = 0;
    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_output *o = &cd->outs[i];
        if (!o->vol_nid) continue;
        bool dup = false;
        for (uint8_t j = 0; j < i; j++)
            if (cd->outs[j].vol_nid == o->vol_nid && cd->outs[j].vol_in_amp == o->vol_in_amp &&
                cd->outs[j].vol_index == o->vol_index) dup = true;
        if (dup) {
            applied++;
            continue;
        }
        hda_apply_volume_amp(cd, o->vol_nid, o->vol_in_amp, o->vol_index, volume, mute, false);
        applied++;
    }
    if (applied < cd->out_count) return AUDIO_ERR_UNSUPPORTED;
    return applied ? AUDIO_OK : AUDIO_ERR_UNSUPPORTED;
}

static int hda_dev_set_mute(struct audio_device *adev, uint8_t direction, bool muted) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    uint8_t volume = adev->streams[direction].volume;
    if (direction == AUDIO_DIR_CAPTURE) {
        if (!cd->in_vol_nid) return AUDIO_ERR_UNSUPPORTED;
        struct hda_widget *w = hda_widget_get(cd, cd->in_vol_nid);
        if (!(hda_amp_caps(cd, w, cd->in_vol_in_amp) & HDA_AMPCAP_MUTE)) return AUDIO_ERR_UNSUPPORTED;
        hda_apply_volume_amp(cd, cd->in_vol_nid, cd->in_vol_in_amp, cd->in_vol_index, volume, muted, true);
        return AUDIO_OK;
    }
    if (cd->out_count == 0) return AUDIO_ERR_UNSUPPORTED;
    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_output *o = &cd->outs[i];
        struct hda_widget *w = hda_widget_get(cd, o->vol_nid);
        if (!o->vol_nid || !(hda_amp_caps(cd, w, o->vol_in_amp) & HDA_AMPCAP_MUTE)) return AUDIO_ERR_UNSUPPORTED;
    }
    if (cd->quirks.flags & AUDIO_HDA_Q_SOFTPCMVOL) volume = AUDIO_VOLUME_MAX;
    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_output *o = &cd->outs[i];
        hda_apply_volume_amp(cd, o->vol_nid, o->vol_in_amp, o->vol_index, volume, muted, false);
    }
    return AUDIO_OK;
}

static bool hda_jack_present(struct hda_codec_dev *cd, uint8_t nid) {
    struct hda_widget *w = hda_widget_get(cd, nid);
    if (!w) return false;
    if (w->pincap & HDA_PINCAP_TRIGGER) {
        hda_verb(cd, nid, HDA_VERB_SET_PIN_SENSE, 0);
        audio_delay_us(50);
    }
    bool present = (hda_verb(cd, nid, HDA_VERB_GET_PIN_SENSE, 0) & 0x80000000u) != 0;
    if (cd->quirks.flags & AUDIO_HDA_Q_SENSEINV) present = !present;
    return present;
}

static void hda_automute(struct hda_codec_dev *cd) {
    uint64_t flags = cd->quirks.flags;
    if (flags & (AUDIO_HDA_Q_NO_AUTOMUTE | AUDIO_HDA_Q_SPEAKER_ALWAYS_ON)) return;

    bool have_hp = false, have_spk = false, hp = false;
    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_output *o = &cd->outs[i];
        if (o->device == HDA_DEV_HP_OUT && o->jack_sense) {
            have_hp = true;
            if (hda_jack_present(cd, o->pin)) hp = true;
        }
        if (o->device == HDA_DEV_SPEAKER) have_spk = true;
    }
    if (!have_hp || !have_spk) return;
    if ((int8_t)hp == cd->hp_present) return;
    cd->hp_present = (int8_t)hp;

    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_output *o = &cd->outs[i];
        if (o->device != HDA_DEV_SPEAKER) continue;
        struct hda_widget *pin = hda_widget_get(cd, o->pin);
        uint8_t ctl = hp ? 0 : (uint8_t)(HDA_PINCTL_OUT_EN | hda_out_vref(flags));
        hda_verb(cd, o->pin, HDA_VERB_SET_PIN_CTL, ctl);
        if (!(flags & AUDIO_HDA_Q_EAPD_ALL_PINS)) hda_set_eapd(cd, pin, !hp);
        o->enabled = !hp;
    }
    LOG_INFO("%s: headphones %s, speakers %s", cd->adev.name, hp ? "plugged" : "unplugged", hp ? "muted" : "on");
}

static void hda_dev_poll(struct audio_device *adev) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;

    struct hda_stream_hw *out = &cd->hw[AUDIO_DIR_PLAYBACK];
    if (out->running) hda_pump_playback(cd, out, false);

    struct hda_stream_hw *in = &cd->hw[AUDIO_DIR_CAPTURE];
    if (in->running) hda_pump_capture(cd, in);

    uint64_t now = audio_uptime_ms();
    if (now - cd->last_jack_ms >= HDA_JACK_POLL_MS) {
        cd->last_jack_ms = now;
        hda_automute(cd);
    }
}

static void hda_describe_path(char *buf, size_t cap, const struct hda_path *p) {
    size_t pos = 0;
    buf[0] = '\0';
    for (uint8_t i = 0; i < p->len && pos + 6 < cap; i++) {
        int w = snprintf(buf + pos, cap - pos, "%s0x%02x", i ? ">" : "", (unsigned)p->nodes[i]);
        if (w < 0) break;
        pos += (size_t)w;
    }
}

static const char *hda_widget_type_name(uint8_t type) {
    switch (type) {
        case HDA_WIDGET_AUDIO_OUT: return "dac";
        case HDA_WIDGET_AUDIO_IN: return "adc";
        case HDA_WIDGET_MIXER: return "mix";
        case HDA_WIDGET_SELECTOR: return "sel";
        case HDA_WIDGET_PIN: return "pin";
        case HDA_WIDGET_POWER: return "pwr";
        case HDA_WIDGET_VOLUME_KNOB: return "knob";
        case HDA_WIDGET_BEEP: return "beep";
        case HDA_WIDGET_VENDOR: return "vend";
        default: return "?";
    }
}

static void hda_dev_describe(struct audio_device *adev, void (*out)(const char *line)) {
    struct hda_codec_dev *cd = (struct hda_codec_dev *)adev->priv;
    struct hda_controller *c = cd->ctrl;
    char line[192];
    char tmp[96];

    snprintf(line, sizeof(line), "controller %s pci %04x:%04x subsys %04x:%04x gcap 0x%04x iss %u oss %u bss %u%s",
             c->name, (unsigned)(c->pci_id >> 16), (unsigned)(c->pci_id & 0xFFFF), (unsigned)(c->subsys >> 16),
             (unsigned)(c->subsys & 0xFFFF), (unsigned)c->gcap, (unsigned)c->iss, (unsigned)c->oss, (unsigned)c->bss,
             c->single_cmd ? " single-cmd" : "");
    out(line);
    audio_quirk_flags_string(AUDIO_QUIRK_HDA_CONTROLLER, c->quirks.flags, tmp, sizeof(tmp));
    snprintf(line, sizeof(line), "controller quirks: %s", tmp);
    out(line);
    snprintf(line, sizeof(line), "codec %u afg 0x%02x id %08x rev %08x subsys %08x gpio caps %08x",
             (unsigned)cd->cad, (unsigned)cd->afg, cd->vendor_id, cd->revision, cd->subsystem, cd->gpio_caps);
    out(line);
    audio_quirk_flags_string(AUDIO_QUIRK_HDA_CODEC, cd->quirks.flags, tmp, sizeof(tmp));
    snprintf(line, sizeof(line), "codec quirks: %s", tmp);
    out(line);

    for (uint8_t i = 0; i < cd->widget_count; i++) {
        struct hda_widget *w = &cd->widgets[i];
        size_t pos = (size_t)snprintf(line, sizeof(line), " 0x%02x %-4s caps %08x", (unsigned)w->nid,
                                      hda_widget_type_name(w->type), w->caps);
        if (w->type == HDA_WIDGET_PIN) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " pincap %08x cfg %08x %s %s %s as=%u seq=%u",
                                    w->pincap, w->config, audio_hda_pin_conn_name((uint8_t)HDA_CFG_CONN(w->config)),
                                    audio_hda_pin_device_name((uint8_t)HDA_CFG_DEVICE(w->config)),
                                    audio_hda_pin_location_name((uint8_t)HDA_CFG_LOCATION(w->config)),
                                    (unsigned)HDA_CFG_ASSOC(w->config), (unsigned)HDA_CFG_SEQ(w->config));
            if (w->config != w->config_bios && pos < sizeof(line))
                pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " (bios %08x)", w->config_bios);
        }
        if (w->conn_count && pos < sizeof(line)) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " <-");
            for (uint8_t k = 0; k < w->conn_count && pos + 6 < sizeof(line); k++)
                pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %02x%s", (unsigned)w->conns[k],
                                        (w->claimed && w->claim_idx == k) ? "*" : "");
        }
        out(line);
    }

    for (uint8_t i = 0; i < cd->out_count; i++) {
        struct hda_output *o = &cd->outs[i];
        hda_describe_path(tmp, sizeof(tmp), &o->path);
        snprintf(line, sizeof(line), "out %u: %s pin 0x%02x as %u seq %u path %s vol 0x%02x%s%s%s", (unsigned)i,
                 audio_hda_pin_device_name(o->device), (unsigned)o->pin, (unsigned)o->assoc, (unsigned)o->seq, tmp,
                 (unsigned)o->vol_nid, o->vol_in_amp ? "(in)" : "", o->jack_sense ? " jack" : "",
                 o->enabled ? "" : " (muted by jack)");
        out(line);
    }
    if (cd->has_input) {
        hda_describe_path(tmp, sizeof(tmp), &cd->in_path);
        snprintf(line, sizeof(line), "in: pin 0x%02x adc 0x%02x path %s vol 0x%02x", (unsigned)cd->in_pin,
                 (unsigned)cd->adc, tmp, (unsigned)cd->in_vol_nid);
        out(line);
    }
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct hda_stream_hw *hw = &cd->hw[d];
        if (hw->sd < 0) continue;
        snprintf(line, sizeof(line), "%s: sd %d tag %u %s fmt 0x%04x buf %u pos %u played %llu written %llu xruns %u",
                 d ? "capture" : "playback", hw->sd, (unsigned)hw->tag, hw->running ? "running" : "idle",
                 (unsigned)hw->fmt_reg, (unsigned)hw->buf_size, hw->buf_size ? (unsigned)hda_read_position(cd, hw) : 0,
                 (unsigned long long)hw->played, (unsigned long long)hw->written, (unsigned)hw->xruns);
        out(line);
    }
}

static const struct audio_device_ops hda_ops = {
    .open = hda_dev_open,
    .close = hda_dev_close,
    .start = hda_dev_start,
    .stop = hda_dev_stop,
    .pause = hda_dev_pause,
    .get_position = hda_dev_get_position,
    .set_volume = hda_dev_set_volume,
    .set_mute = hda_dev_set_mute,
    .poll = hda_dev_poll,
    .describe = hda_dev_describe,
    .remove = NULL,
};

static void hda_codec_name(struct hda_codec_dev *cd, char *buf, size_t cap) {
    if (cd->quirks.model) {
        snprintf(buf, cap, "%s", cd->quirks.model);
        return;
    }
    uint16_t v = (uint16_t)(cd->vendor_id >> 16);
    uint16_t d = (uint16_t)(cd->vendor_id & 0xFFFF);
    const char *vn = hda_vendor_name(v);
    if (v == 0x10ec) snprintf(buf, cap, "Realtek ALC%x", (unsigned)d);
    else if (vn) snprintf(buf, cap, "%s %04x", vn, (unsigned)d);
    else snprintf(buf, cap, "HDA codec %08x", cd->vendor_id);
}

static void hda_fill_caps(struct hda_codec_dev *cd) {
    struct audio_device *a = &cd->adev;
    uint32_t rates = 0x7FF;
    uint32_t bits = HDA_PCM_BITS_16 | HDA_PCM_BITS_20 | HDA_PCM_BITS_24 | HDA_PCM_BITS_32 | HDA_PCM_BITS_8;
    for (uint8_t i = 0; i < cd->dac_count; i++) {
        struct hda_widget *w = hda_widget_get(cd, cd->dacs[i]);
        if (!w) continue;
        rates &= w->pcm_caps & 0x7FF;
        bits &= w->pcm_caps;
    }
    if (cd->dac_count) {
        a->has_direction[AUDIO_DIR_PLAYBACK] = 1;
        a->rates[AUDIO_DIR_PLAYBACK] = rates ? rates : AUDIO_RATE_48000;
        a->formats[AUDIO_DIR_PLAYBACK] = (uint16_t)AUDIO_FMT_MASK(AUDIO_FMT_S16_LE);
        if (bits & (HDA_PCM_BITS_20 | HDA_PCM_BITS_24 | HDA_PCM_BITS_32))
            a->formats[AUDIO_DIR_PLAYBACK] |= (uint16_t)AUDIO_FMT_MASK(AUDIO_FMT_S32_LE);
        a->max_channels[AUDIO_DIR_PLAYBACK] = 2;
    }
    if (cd->has_input) {
        struct hda_widget *w = hda_widget_get(cd, cd->adc);
        uint32_t ir = w ? (w->pcm_caps & 0x7FF) : 0;
        a->has_direction[AUDIO_DIR_CAPTURE] = 1;
        a->rates[AUDIO_DIR_CAPTURE] = ir ? ir : AUDIO_RATE_48000;
        a->formats[AUDIO_DIR_CAPTURE] = (uint16_t)AUDIO_FMT_MASK(AUDIO_FMT_S16_LE);
        if (w && (w->pcm_caps & (HDA_PCM_BITS_20 | HDA_PCM_BITS_24 | HDA_PCM_BITS_32)))
            a->formats[AUDIO_DIR_CAPTURE] |= (uint16_t)AUDIO_FMT_MASK(AUDIO_FMT_S32_LE);
        a->max_channels[AUDIO_DIR_CAPTURE] = 2;
    }
    if (cd->quirks.flags & AUDIO_HDA_Q_FIXEDRATE) a->fixed_rate = 48000;
    if (cd->quirks.fixed_rate) a->fixed_rate = cd->quirks.fixed_rate;
    if (cd->quirks.flags & AUDIO_HDA_Q_SOFTPCMVOL) a->streams[AUDIO_DIR_PLAYBACK].force_sw_volume = 1;
}

static void hda_codec_attach(struct hda_controller *c, uint8_t cad, uint8_t afg, uint32_t vendor_id, uint32_t revision) {
    if (c->device_count >= HDA_MAX_DEVICES) return;

    struct hda_codec_dev *cd = (struct hda_codec_dev *)kmalloc(sizeof(struct hda_codec_dev));
    if (!cd) return;
    memset(cd, 0, sizeof(*cd));
    cd->ctrl = c;
    cd->cad = cad;
    cd->afg = afg;
    cd->vendor_id = vendor_id;
    cd->revision = revision;
    cd->hp_present = -1;
    cd->hw[0].sd = -1;
    cd->hw[1].sd = -1;
    snprintf(cd->adev.name, sizeof(cd->adev.name), "hda%d.%u", (int)(c - hda_ctrls), (unsigned)cad);

    hda_verb(cd, afg, HDA_VERB_SET_POWER, 0x00);
    audio_delay_ms(10);

    cd->subsystem = hda_verb(cd, afg, HDA_VERB_GET_SUBSYSTEM, 0);
    cd->afg_pcm_caps = hda_param(cd, afg, HDA_PARAM_PCM);
    cd->afg_stream_fmts = hda_param(cd, afg, HDA_PARAM_STREAM_FMTS);
    cd->afg_amp_in = hda_param(cd, afg, HDA_PARAM_AMP_IN_CAPS);
    cd->afg_amp_out = hda_param(cd, afg, HDA_PARAM_AMP_OUT_CAPS);
    cd->gpio_caps = hda_param(cd, afg, HDA_PARAM_GPIO_COUNT);

    struct audio_quirk_ids ids = {
        .pci_id = c->pci_id,
        .subsys = c->subsys,
        .codec_id = vendor_id,
        .codec_subsys = cd->subsystem,
        .revision = (uint8_t)(revision >> 8),
        .has_revision = 1,
    };
    audio_quirk_resolve(AUDIO_QUIRK_HDA_CODEC, &ids, 0, &cd->quirks);

    char cname[48];
    hda_codec_name(cd, cname, sizeof(cname));
    snprintf(cd->adev.codec_name, sizeof(cd->adev.codec_name), "%s", cname);

    if (cd->quirks.flags & AUDIO_HDA_Q_SKIP) {
        LOG_INFO("%s: codec %s skipped by quirk", c->name, cname);
        kfree(cd);
        return;
    }

    struct audio_quirk_ctx ctx;
    hda_quirk_ctx_init(cd, &ctx, AUDIO_QUIRK_STAGE_ATTACH);
    audio_quirk_run_hooks(&cd->quirks, &ctx);

    if (cd->quirks.flags & AUDIO_HDA_Q_FUNC_RESET) {
        hda_verb(cd, afg, HDA_VERB_FUNC_RESET, 0);
        audio_delay_ms(20);
        hda_verb(cd, afg, HDA_VERB_SET_POWER, 0x00);
        audio_delay_ms(10);
    }

    uint32_t sub = hda_param(cd, afg, HDA_PARAM_SUBORDINATE);
    cd->first_nid = (uint8_t)((sub >> 16) & 0xFF);
    cd->widget_count = (uint8_t)(sub & 0xFF);
    if (cd->widget_count == 0 || cd->first_nid == 0) {
        LOG_WARNING("%s: codec %u AFG 0x%02x reports no widgets", c->name, (unsigned)cad, (unsigned)afg);
        kfree(cd);
        return;
    }
    cd->widgets = (struct hda_widget *)kmalloc(sizeof(struct hda_widget) * cd->widget_count);
    if (!cd->widgets) {
        kfree(cd);
        return;
    }
    hda_parse_widgets(cd);

    hda_quirk_ctx_init(cd, &ctx, AUDIO_QUIRK_STAGE_INIT);
    ctx.pin_first = cd->first_nid;
    ctx.pin_count = cd->widget_count;
    hda_apply_gpio(cd);
    audio_quirk_run_verbs(&cd->quirks, &ctx);
    audio_quirk_run_coefs(&cd->quirks, &ctx);
    audio_quirk_run_hooks(&cd->quirks, &ctx);

    hda_build_outputs(cd);
    hda_build_input(cd);

    if (cd->out_count == 0 && !cd->has_input) {
        LOG_INFO("%s: codec %u %s has no usable analog path, not registered", c->name, (unsigned)cad, cname);
        kfree(cd->widgets);
        kfree(cd);
        return;
    }

    for (uint8_t i = 0; i < cd->out_count; i++) hda_configure_output(cd, &cd->outs[i]);
    if (cd->quirks.flags & AUDIO_HDA_Q_EAPD_ALL_PINS) {
        for (uint8_t i = 0; i < cd->widget_count; i++)
            if (cd->widgets[i].type == HDA_WIDGET_PIN) hda_set_eapd(cd, &cd->widgets[i], true);
    }
    if (cd->has_input) hda_configure_input(cd);

    hda_quirk_ctx_init(cd, &ctx, AUDIO_QUIRK_STAGE_CONFIGURED);
    audio_quirk_run_hooks(&cd->quirks, &ctx);

    int k = c->device_count;
    if (cd->out_count && c->oss + c->bss > k) {
        cd->hw[AUDIO_DIR_PLAYBACK].sd = (k < c->oss) ? c->iss + k : c->iss + c->oss + (k - c->oss);
        cd->hw[AUDIO_DIR_PLAYBACK].tag = (uint8_t)(1 + 2 * k);
    }
    if (cd->has_input && c->iss > k) {
        cd->hw[AUDIO_DIR_CAPTURE].sd = k;
        cd->hw[AUDIO_DIR_CAPTURE].tag = (uint8_t)(2 + 2 * k);
    }
    if (cd->hw[AUDIO_DIR_PLAYBACK].sd < 0) cd->dac_count = 0;
    if (cd->hw[AUDIO_DIR_CAPTURE].sd < 0) cd->has_input = 0;

    struct audio_device *a = &cd->adev;
    a->type = HDA_AUDIO;
    a->pci = c->pci;
    a->ops = &hda_ops;
    a->priv = cd;
    a->vendor_id = c->pci_id;
    a->subsystem_id = c->subsys;
    a->codec_id = vendor_id;
    a->quirks = cd->quirks.flags;
    if (cd->is_digital) a->flags |= AUDIO_DEV_FLAG_DIGITAL;
    snprintf(a->name, sizeof(a->name), "%s", cname);
    snprintf(a->description, sizeof(a->description), "%s, codec %u", c->name, (unsigned)cad);
    {
        char n1[48], n2[48];
        audio_quirk_set_names(&c->quirks, n1, sizeof(n1));
        audio_quirk_set_names(&cd->quirks, n2, sizeof(n2));
        snprintf(a->quirk_names, sizeof(a->quirk_names), "%s%s%s", n1, (n1[0] && n2[0]) ? "," : "", n2);
    }
    hda_fill_caps(cd);

    bool vol_hw = false;
    for (uint8_t i = 0; i < cd->out_count; i++) if (cd->outs[i].vol_nid) vol_hw = true;
    if (vol_hw && !(cd->quirks.flags & AUDIO_HDA_Q_SOFTPCMVOL)) a->flags |= AUDIO_DEV_FLAG_HW_VOLUME | AUDIO_DEV_FLAG_HW_MUTE;

    if (audio_core_add_device(a) < 0) {
        kfree(cd->widgets);
        kfree(cd);
        return;
    }
    c->devices[c->device_count++] = cd;

    char qn[96];
    audio_quirk_flags_string(AUDIO_QUIRK_HDA_CODEC, cd->quirks.flags, qn, sizeof(qn));
    LOG_INFO("%s: codec %u %s (%08x subsys %08x) %u output(s), %u DAC(s), input %s, quirks %s", c->name,
             (unsigned)cad, cname, vendor_id, cd->subsystem, (unsigned)cd->out_count, (unsigned)cd->dac_count,
             cd->has_input ? "yes" : "no", qn);
}

static void hda_probe_codec(struct hda_controller *c, uint8_t cad) {
    struct hda_codec_dev probe;
    memset(&probe, 0, sizeof(probe));
    probe.ctrl = c;
    probe.cad = cad;

    uint32_t vendor = hda_param(&probe, 0, HDA_PARAM_VENDOR_ID);
    if (vendor == 0 || vendor == 0xFFFFFFFFu) {
        LOG_WARNING("%s: codec %u did not answer", c->name, (unsigned)cad);
        return;
    }
    uint32_t revision = hda_param(&probe, 0, HDA_PARAM_REVISION_ID);
    uint32_t sub = hda_param(&probe, 0, HDA_PARAM_SUBORDINATE);
    uint8_t start = (uint8_t)((sub >> 16) & 0xFF);
    uint8_t count = (uint8_t)(sub & 0xFF);
    LOG_DEBUG("%s: codec %u vendor %08x rev %08x, %u function group(s) from 0x%02x", c->name, (unsigned)cad,
              vendor, revision, (unsigned)count, (unsigned)start);

    bool found = false;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start + i);
        uint32_t type = hda_param(&probe, nid, HDA_PARAM_FUNC_GROUP) & 0xFF;
        if (type == 0x01) {
            found = true;
            hda_codec_attach(c, cad, nid, vendor, revision);
        } else if (type == 0x02) {
            LOG_DEBUG("%s: codec %u is a modem function group, ignored", c->name, (unsigned)cad);
        }
    }
    if (!found) LOG_DEBUG("%s: codec %u has no audio function group", c->name, (unsigned)cad);
}

static bool hda_setup_corb_rirb(struct hda_controller *c) {
    hda_stop_corb_rirb(c);

    uint8_t csize = hda_r8(c, HDA_REG_CORBSIZE);
    uint8_t ccode = 0;
    c->corb_entries = 2;
    if (csize & 0x40) { ccode = 2; c->corb_entries = 256; }
    else if (csize & 0x20) { ccode = 1; c->corb_entries = 16; }
    hda_w8(c, HDA_REG_CORBSIZE, (uint8_t)((csize & ~0x3) | ccode));

    uint8_t rsize = hda_r8(c, HDA_REG_RIRBSIZE);
    uint8_t rcode = 0;
    c->rirb_entries = 2;
    if (rsize & 0x40) { rcode = 2; c->rirb_entries = 256; }
    else if (rsize & 0x20) { rcode = 1; c->rirb_entries = 16; }
    hda_w8(c, HDA_REG_RIRBSIZE, (uint8_t)((rsize & ~0x3) | rcode));

    if (!c->corb) c->corb = (uint32_t *)audio_dma_alloc(4096, !c->ok64, &c->corb_phys, &c->corb_pages);
    if (!c->rirb) c->rirb = (uint64_t *)audio_dma_alloc(4096, !c->ok64, &c->rirb_phys, &c->rirb_pages);
    if (!c->corb || !c->rirb) return false;

    hda_w32(c, HDA_REG_CORBLBASE, (uint32_t)c->corb_phys);
    hda_w32(c, HDA_REG_CORBUBASE, (uint32_t)(c->corb_phys >> 32));
    hda_w16(c, HDA_REG_CORBWP, 0);
    hda_w16(c, HDA_REG_CORBRP, HDA_CORBRP_RST);
    if (!(c->quirks.flags & AUDIO_HDAC_Q_CORB_RP_NO_ACK)) {
        if (!hda_wait16(c, HDA_REG_CORBRP, HDA_CORBRP_RST, HDA_CORBRP_RST, 1000))
            LOG_DEBUG("%s: CORBRP reset bit did not latch", c->name);
    }
    hda_w16(c, HDA_REG_CORBRP, 0);
    if (!hda_wait16(c, HDA_REG_CORBRP, HDA_CORBRP_RST, 0, 1000))
        LOG_WARNING("%s: CORBRP reset did not clear", c->name);

    hda_w32(c, HDA_REG_RIRBLBASE, (uint32_t)c->rirb_phys);
    hda_w32(c, HDA_REG_RIRBUBASE, (uint32_t)(c->rirb_phys >> 32));
    hda_w16(c, HDA_REG_RIRBWP, HDA_RIRBWP_RST);
    hda_w16(c, HDA_REG_RINTCNT, 1);
    hda_w8(c, HDA_REG_RIRBSTS, HDA_RIRBSTS_INTFL | HDA_RIRBSTS_OIS);

    hda_w8(c, HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    hda_w8(c, HDA_REG_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_RINTCTL);
    if (!hda_wait8(c, HDA_REG_CORBCTL, HDA_CORBCTL_RUN, HDA_CORBCTL_RUN, 1000)) return false;

    c->corb_wp = (uint16_t)(hda_r16(c, HDA_REG_CORBWP) & 0xFF) % c->corb_entries;
    c->rirb_rp = (uint16_t)(hda_r16(c, HDA_REG_RIRBWP) & 0xFF) % c->rirb_entries;
    return true;
}

static bool hda_reset_link(struct hda_controller *c) {
    bool long_reset = (c->quirks.flags & AUDIO_HDAC_Q_LONG_RESET) != 0;

    hda_w16(c, HDA_REG_STATESTS, 0x7FFF);
    hda_w32(c, HDA_REG_GCTL, hda_r32(c, HDA_REG_GCTL) & ~HDA_GCTL_CRST);
    if (!hda_wait32(c, HDA_REG_GCTL, HDA_GCTL_CRST, 0, long_reset ? 500000 : 100000)) {
        LOG_ERROR("%s: controller did not enter reset", c->name);
        return false;
    }
    audio_delay_us(long_reset ? 1000 : 200);
    hda_w32(c, HDA_REG_GCTL, hda_r32(c, HDA_REG_GCTL) | HDA_GCTL_CRST);
    if (!hda_wait32(c, HDA_REG_GCTL, HDA_GCTL_CRST, HDA_GCTL_CRST, long_reset ? 500000 : 100000)) {
        LOG_ERROR("%s: controller did not leave reset", c->name);
        return false;
    }
    audio_delay_ms(long_reset ? 10 : 1);
    if (c->quirks.delay_ms) audio_delay_ms(c->quirks.delay_ms);

    for (int i = 0; i < 100 && !(hda_r16(c, HDA_REG_STATESTS) & 0x7FFF); i++) audio_delay_ms(1);
    c->codec_mask = hda_r16(c, HDA_REG_STATESTS) & 0x7FFF;
    hda_w16(c, HDA_REG_STATESTS, 0x7FFF);
    return true;
}

static void hda_controller_name(struct hda_controller *c) {
    if (c->quirks.model) {
        snprintf(c->name, sizeof(c->name), "%s", c->quirks.model);
        return;
    }
    const char *vn = hda_vendor_name((uint16_t)(c->pci_id >> 16));
    if (vn) snprintf(c->name, sizeof(c->name), "%s HDA %04x", vn, (unsigned)(c->pci_id & 0xFFFF));
    else snprintf(c->name, sizeof(c->name), "HDA %04x:%04x", (unsigned)(c->pci_id >> 16), (unsigned)(c->pci_id & 0xFFFF));
}

static bool hda_controller_init(struct hda_controller *c, struct pci_device *pci) {
    c->pci = pci;
    spin_lock_init(&c->cmd_lock);
    c->pci_id = AUDIO_ID(pci->vendor_id, pci->device_id);
    uint32_t ss = pci_read_config(pci->bus, pci->slot, pci->func, 0x2C);
    c->subsys = AUDIO_ID(ss & 0xFFFF, ss >> 16);
    c->revision = (uint8_t)(pci_read_config(pci->bus, pci->slot, pci->func, 0x08) & 0xFF);

    struct audio_quirk_ids ids = {
        .pci_id = c->pci_id,
        .subsys = c->subsys,
        .revision = c->revision,
        .has_revision = 1,
    };
    audio_quirk_resolve(AUDIO_QUIRK_HDA_CONTROLLER, &ids, AUDIO_HDAC_Q_64BIT, &c->quirks);
    hda_controller_name(c);

    if (c->quirks.flags & AUDIO_HDAC_Q_SKIP) {
        LOG_INFO("%s at %02x:%02x.%x skipped by quirk", c->name, (unsigned)pci->bus, (unsigned)pci->slot, (unsigned)pci->func);
        return false;
    }

    pci_enable_bus_mastering(pci);

    if (c->quirks.flags & AUDIO_HDAC_Q_INTEL_TCSEL) hda_pci_update8(pci, 0x44, 0x07, 0x00);
    if (c->quirks.flags & AUDIO_HDAC_Q_SNOOP_ATI) hda_pci_update8(pci, 0x42, 0x07, 0x02);
    if (c->quirks.flags & AUDIO_HDAC_Q_SNOOP_NVIDIA) {
        hda_pci_update8(pci, 0x4E, 0x0F, 0x0F);
        hda_pci_update8(pci, 0x4D, 0x01, 0x01);
        hda_pci_update8(pci, 0x4C, 0x01, 0x01);
    }
    if (c->quirks.flags & AUDIO_HDAC_Q_SNOOP_INTEL_SCH) hda_pci_update16(pci, 0x78, 0x0800, 0x0000);

    bool is_io = false;
    c->mmio_phys = pci_bar_phys(pci, 0, &is_io);
    if (!c->mmio_phys || is_io) {
        LOG_ERROR("%s: BAR0 is not a memory BAR", c->name);
        return false;
    }
    uint64_t virt = vmm_map_mmio(c->mmio_phys, HDA_MMIO_SIZE, 0);
    if (!virt) {
        LOG_ERROR("%s: cannot map BAR0 at 0x%llx", c->name, (unsigned long long)c->mmio_phys);
        return false;
    }
    c->mmio = (volatile uint8_t *)virt;

    c->gcap = hda_r16(c, HDA_REG_GCAP);
    if (c->gcap == 0xFFFF) {
        LOG_ERROR("%s: registers read back as 0xFFFF", c->name);
        return false;
    }
    c->oss = (uint8_t)((c->gcap >> 12) & 0xF);
    c->iss = (uint8_t)((c->gcap >> 8) & 0xF);
    c->bss = (uint8_t)((c->gcap >> 3) & 0x1F);
    c->ok64 = (c->gcap & 1) && (c->quirks.flags & AUDIO_HDAC_Q_64BIT);

    hda_w32(c, HDA_REG_INTCTL, 0);
    for (int sd = 0; sd < c->iss + c->oss + c->bss; sd++) hda_sd_halt(c, sd);
    hda_stop_corb_rirb(c);
    hda_w32(c, HDA_REG_DPLBASE, 0);
    hda_w32(c, HDA_REG_DPUBASE, 0);

    if (!hda_reset_link(c)) return false;
    hda_w16(c, HDA_REG_WAKEEN, 0);
    hda_w32(c, HDA_REG_INTCTL, 0);

    uint16_t mask = c->codec_mask;
    if (c->quirks.codec_probe_mask) mask &= (uint16_t)c->quirks.codec_probe_mask;
    LOG_INFO("%s at %02x:%02x.%x: HDA %u.%u, %u in / %u out / %u bidi streams, codec mask 0x%04x", c->name,
             (unsigned)pci->bus, (unsigned)pci->slot, (unsigned)pci->func, (unsigned)hda_r8(c, HDA_REG_VMAJ),
             (unsigned)hda_r8(c, HDA_REG_VMIN), (unsigned)c->iss, (unsigned)c->oss, (unsigned)c->bss, (unsigned)mask);
    if (!mask) {
        LOG_WARNING("%s: no codecs present on the link", c->name);
        return false;
    }

    c->single_cmd = (c->quirks.flags & AUDIO_HDAC_Q_SINGLE_CMD) ? 1 : 0;
    if (!c->single_cmd && !hda_setup_corb_rirb(c)) {
        LOG_WARNING("%s: CORB/RIRB setup failed, using immediate commands", c->name);
        hda_stop_corb_rirb(c);
        c->single_cmd = 1;
    }

    c->dmapos = (uint32_t *)audio_dma_alloc(4096, !c->ok64, &c->dmapos_phys, &c->dmapos_pages);
    if (c->dmapos && (c->quirks.flags & AUDIO_HDAC_Q_DMAPOS)) {
        hda_w32(c, HDA_REG_DPUBASE, (uint32_t)(c->dmapos_phys >> 32));
        hda_w32(c, HDA_REG_DPLBASE, (uint32_t)c->dmapos_phys | 1u);
    }

    for (uint8_t cad = 0; cad < HDA_MAX_CODECS; cad++)
        if (mask & (1u << cad)) hda_probe_codec(c, cad);

    return true;
}

static bool hda_pci_known(struct pci_device *pci) {
    for (int i = 0; i < hda_ctrl_count; i++)
        if (hda_ctrls[i].pci == pci) return true;
    return false;
}

static void hda_scan(void) {
    spin_lock(&hda_scan_lock);
    int pci_count = pci_get_device_count();
    if (pci_count > MAX_PCI_DEVICES) pci_count = MAX_PCI_DEVICES;

    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev = (struct pci_device *)device_table[PCI_DEVICE][i];
        if (!dev) continue;
        if (dev->class_code != PCI_CLASS_MULTIMEDIA || dev->subclass != PCI_SUBCLASS_HDA) continue;
        if (hda_pci_known(dev)) continue;
        if (hda_ctrl_count >= HDA_MAX_CONTROLLERS) {
            LOG_WARNING("more than %d HDA controllers, ignoring the rest", HDA_MAX_CONTROLLERS);
            break;
        }
        struct hda_controller *c = &hda_ctrls[hda_ctrl_count];
        memset(c, 0, sizeof(*c));
        if (!hda_controller_init(c, dev)) {
            c->pci = dev;
            c->mmio = NULL;
        }
        hda_ctrl_count++;
    }
    spin_unlock(&hda_scan_lock);
}

static const struct audio_backend hda_backend = {
    .name = "hda",
    .type = HDA_AUDIO,
    .scan = hda_scan,
    .poll = NULL,
};

static int hda_get_controller_count(void) {
    return hda_ctrl_count;
}

static struct hda_controller *hda_get_controller(int idx) {
    if (idx < 0 || idx >= hda_ctrl_count) return NULL;
    return hda_ctrls[idx].mmio ? &hda_ctrls[idx] : NULL;
}

static struct hda_driver drv_hda = {
    .get_controller_count = hda_get_controller_count,
    .get_controller = hda_get_controller,
    .codec_command = hda_command,
};

struct hda_driver *return_hda_driver(void) {
    audio_core_register_backend(&hda_backend);
    hda_scan();
    int devs = 0;
    for (int i = 0; i < hda_ctrl_count; i++) devs += hda_ctrls[i].device_count;
    LOG_INFO("HDA: %d controller(s), %d codec device(s)", hda_ctrl_count, devs);
    return &drv_hda;
}

struct driver *return_meta_hda_driver(void) {
    static struct dependency deps[] = {
        MAKE_DEPENDENCY(AUDIO_DRIVER, AUDIO_CORE_SLOT),
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "HD Audio Driver",
        .type = AUDIO_DRIVER,
        .sub_type = HDA_AUDIO,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &deps[0], &deps[1] },
        .dependency_count = 2,
        .self = &drv_hda,
        .init = (void *)return_hda_driver,
    };
    return &meta;
}
