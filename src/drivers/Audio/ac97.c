#include "ac97.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/vmm.h"
#include "components/pci.h"
#include "drivers/Timer/timer.h"

#include <ports.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static struct ac97_controller *ac97_ctrls[AC97_MAX_CONTROLLERS];
static struct pci_device *ac97_seen[AC97_MAX_CONTROLLERS * 2];
static int ac97_ctrl_count = 0;
static int ac97_seen_count = 0;
static spinlock_t ac97_scan_lock = SPINLOCK_INIT;

static const struct {
    const char tag[4];
    const char *name;
} ac97_vendor_tags[] = {
    { "ADS", "Analog Devices" }, { "AKM", "Asahi Kasei" }, { "ALG", "Realtek" }, { "CMI", "C-Media" },
    { "CRY", "Cirrus Logic" }, { "CXT", "Conexant" }, { "EMC", "eMicro" }, { "ICE", "ICEnsemble" },
    { "NSC", "National Semiconductor" }, { "PSC", "Philips" }, { "SIL", "Silicon Laboratories" },
    { "TRA", "TriTech" }, { "VIA", "VIA" }, { "WEC", "Winbond" }, { "WML", "Wolfson" }, { "YMH", "Yamaha" },
};

static inline uint16_t nam_r16(struct ac97_controller *c, uint8_t reg) {
    if (c->mmio) return *(volatile uint16_t *)(c->nam_mem + reg);
    return inw((uint16_t)(c->nam_io + reg));
}

static inline void nam_w16(struct ac97_controller *c, uint8_t reg, uint16_t v) {
    if (c->mmio) *(volatile uint16_t *)(c->nam_mem + reg) = v;
    else outw((uint16_t)(c->nam_io + reg), v);
}

static inline uint8_t nabm_r8(struct ac97_controller *c, uint8_t reg) {
    if (c->mmio) return *(volatile uint8_t *)(c->nabm_mem + reg);
    return inb((uint16_t)(c->nabm_io + reg));
}

static inline uint16_t nabm_r16(struct ac97_controller *c, uint8_t reg) {
    if (c->mmio) return *(volatile uint16_t *)(c->nabm_mem + reg);
    return inw((uint16_t)(c->nabm_io + reg));
}

static inline uint32_t nabm_r32(struct ac97_controller *c, uint8_t reg) {
    if (c->mmio) return *(volatile uint32_t *)(c->nabm_mem + reg);
    return inl((uint16_t)(c->nabm_io + reg));
}

static inline void nabm_w8(struct ac97_controller *c, uint8_t reg, uint8_t v) {
    if (c->mmio) *(volatile uint8_t *)(c->nabm_mem + reg) = v;
    else outb((uint16_t)(c->nabm_io + reg), v);
}

static inline void nabm_w16(struct ac97_controller *c, uint8_t reg, uint16_t v) {
    if (c->mmio) *(volatile uint16_t *)(c->nabm_mem + reg) = v;
    else outw((uint16_t)(c->nabm_io + reg), v);
}

static inline void nabm_w32(struct ac97_controller *c, uint8_t reg, uint32_t v) {
    if (c->mmio) *(volatile uint32_t *)(c->nabm_mem + reg) = v;
    else outl((uint16_t)(c->nabm_io + reg), v);
}

static void ac97_codec_semaphore(struct ac97_controller *c) {
    if (c->ctrl_quirks.flags & AUDIO_AC97C_Q_NO_SEMAPHORE) return;
    for (int i = 0; i < 100; i++) {
        if (!(nabm_r8(c, AC97_REG_CAS) & 0x01)) return;
        audio_delay_us(10);
    }
}

static uint16_t ac97_read(struct ac97_controller *c, uint8_t reg) {
    ac97_codec_semaphore(c);
    uint16_t v = nam_r16(c, reg);
    if (c->codec_quirks.flags & AUDIO_AC97_Q_RDCD_BUG) {
        ac97_codec_semaphore(c);
        v = nam_r16(c, reg);
    }
    uint32_t sta = nabm_r32(c, AC97_REG_GLOB_STA);
    if (sta & AC97_GLOB_STA_RCS) {
        nabm_w32(c, AC97_REG_GLOB_STA, AC97_GLOB_STA_RCS);
        c->read_timeouts++;
    }
    return v;
}

static void ac97_write(struct ac97_controller *c, uint8_t reg, uint16_t v) {
    ac97_codec_semaphore(c);
    nam_w16(c, reg, v);
}

static uint16_t ac97_quirk_reg_read(void *backend, uint8_t reg) {
    return ac97_read((struct ac97_controller *)backend, reg);
}

static void ac97_quirk_reg_write(void *backend, uint8_t reg, uint16_t value) {
    ac97_write((struct ac97_controller *)backend, reg, value);
}

static void ac97_quirk_ctx_init(struct ac97_controller *c, struct audio_quirk_ctx *ctx, enum audio_quirk_domain domain,
                                enum audio_quirk_stage stage) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->domain = domain;
    ctx->stage = stage;
    ctx->dev = &c->adev;
    ctx->backend = c;
    ctx->reg_read = ac97_quirk_reg_read;
    ctx->reg_write = ac97_quirk_reg_write;
}

static void ac97_run_hooks(struct ac97_controller *c, enum audio_quirk_stage stage) {
    struct audio_quirk_ctx ctx;
    ac97_quirk_ctx_init(c, &ctx, AUDIO_QUIRK_AC97_CONTROLLER, stage);
    audio_quirk_run_hooks(&c->ctrl_quirks, &ctx);
    ac97_quirk_ctx_init(c, &ctx, AUDIO_QUIRK_AC97_CODEC, stage);
    audio_quirk_run_hooks(&c->codec_quirks, &ctx);
}

static uint8_t ac97_box_reg(struct ac97_controller *c, struct ac97_stream_hw *hw, uint8_t reg) {
    if (reg == AC97_BOX_SR) reg = c->reg_sr;
    else if (reg == AC97_BOX_PICB) reg = c->reg_picb;
    return (uint8_t)(hw->box + reg);
}

static void ac97_box_reset(struct ac97_controller *c, struct ac97_stream_hw *hw) {
    nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_CR), 0);
    for (int i = 0; i < 100; i++) {
        if (nabm_r16(c, ac97_box_reg(c, hw, AC97_BOX_SR)) & AC97_SR_DCH) break;
        audio_delay_us(10);
    }
    nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_CR), AC97_CR_RR);
    for (int i = 0; i < 100; i++) {
        if (!(nabm_r8(c, ac97_box_reg(c, hw, AC97_BOX_CR)) & AC97_CR_RR)) break;
        audio_delay_us(10);
    }
    nabm_w16(c, ac97_box_reg(c, hw, AC97_BOX_SR), AC97_SR_CLEAR);
}

static bool ac97_cold_reset(struct ac97_controller *c) {
    uint64_t cflags = c->ctrl_quirks.flags;
    if (!(cflags & AUDIO_AC97C_Q_IGNORE_RESET)) {
        uint32_t cnt = nabm_r32(c, AC97_REG_GLOB_CNT);
        cnt &= ~(AC97_GLOB_CNT_COLD | AC97_GLOB_CNT_WARM | AC97_GLOB_CNT_SHUT | AC97_GLOB_CNT_GIE | AC97_GLOB_CNT_PCM_MASK);
        nabm_w32(c, AC97_REG_GLOB_CNT, cnt);
        audio_delay_ms(1);
        nabm_w32(c, AC97_REG_GLOB_CNT, cnt | AC97_GLOB_CNT_COLD);
    }

    if (cflags & AUDIO_AC97C_Q_IGNORE_PCR) {
        audio_delay_ms(600);
        return true;
    }

    for (int i = 0; i < 1000; i++) {
        if (nabm_r32(c, AC97_REG_GLOB_STA) & AC97_GLOB_STA_PCR) return true;
        audio_delay_ms(1);
    }

    uint32_t cnt = nabm_r32(c, AC97_REG_GLOB_CNT);
    nabm_w32(c, AC97_REG_GLOB_CNT, cnt | AC97_GLOB_CNT_WARM);
    for (int i = 0; i < 500; i++) {
        if (!(nabm_r32(c, AC97_REG_GLOB_CNT) & AC97_GLOB_CNT_WARM) &&
            (nabm_r32(c, AC97_REG_GLOB_STA) & AC97_GLOB_STA_PCR)) return true;
        audio_delay_ms(1);
    }
    return (nabm_r32(c, AC97_REG_GLOB_STA) & AC97_GLOB_STA_PCR) != 0;
}

static void ac97_codec_name(struct ac97_controller *c, char *buf, size_t cap) {
    if (c->codec_quirks.model) {
        snprintf(buf, cap, "%s", c->codec_quirks.model);
        return;
    }
    char tag[4] = { (char)(c->codec_id >> 24), (char)(c->codec_id >> 16), (char)(c->codec_id >> 8), 0 };
    for (size_t i = 0; i < sizeof(ac97_vendor_tags) / sizeof(ac97_vendor_tags[0]); i++) {
        if (memcmp(ac97_vendor_tags[i].tag, tag, 3) == 0) {
            snprintf(buf, cap, "%s AC97 %02x", ac97_vendor_tags[i].name, (unsigned)(c->codec_id & 0xFF));
            return;
        }
    }
    if ((c->codec_id >> 16) == 0x8384) {
        snprintf(buf, cap, "SigmaTel AC97 %04x", (unsigned)(c->codec_id & 0xFFFF));
        return;
    }
    snprintf(buf, cap, "AC97 codec %08x", c->codec_id);
}

static uint16_t ac97_atten(uint8_t volume, uint8_t bits) {
    uint32_t max = (bits >= 6) ? 63 : 31;
    uint32_t range = 31;
    if (volume >= AUDIO_VOLUME_MAX) return 0;
    uint32_t a = ((uint32_t)(AUDIO_VOLUME_MAX - volume) * range + AUDIO_VOLUME_MAX / 2) / AUDIO_VOLUME_MAX;
    if (a > max) a = max;
    return (uint16_t)a;
}

static void ac97_set_out_volume(struct ac97_controller *c, uint8_t volume, bool mute) {
    uint16_t a = ac97_atten(volume, c->master_bits);
    uint16_t v = (uint16_t)((a << 8) | a);
    if (mute || volume == 0) v |= AC97_MUTE;
    ac97_write(c, AC97_MIX_MASTER, v);
    if (!(c->codec_quirks.flags & AUDIO_AC97_Q_NO_HEADPHONE)) ac97_write(c, AC97_MIX_HEADPHONE, v);
}

static void ac97_set_in_volume(struct ac97_controller *c, uint8_t volume, bool mute) {
    uint16_t g = (uint16_t)((15u * volume) / AUDIO_VOLUME_MAX);
    uint16_t v = (uint16_t)((g << 8) | g);
    if (mute || volume == 0) v |= AC97_MUTE;
    ac97_write(c, AC97_MIX_REC_GAIN, v);
}

static void ac97_set_eapd(struct ac97_controller *c, bool on) {
    if (c->codec_quirks.flags & AUDIO_AC97_Q_NO_EAPD) return;
    bool inv = (c->codec_quirks.flags & AUDIO_AC97_Q_EAPD_INV) != 0;
    uint16_t pd = ac97_read(c, AC97_MIX_POWERDOWN);
    bool power_down_bit = (on == inv);
    if (power_down_bit) pd |= AC97_PD_EAPD;
    else pd &= (uint16_t)~AC97_PD_EAPD;
    ac97_write(c, AC97_MIX_POWERDOWN, pd);
}

static uint32_t ac97_set_rate(struct ac97_controller *c, uint8_t reg, uint32_t rate) {
    if (!c->vra) return 48000;
    ac97_write(c, reg, (uint16_t)rate);
    audio_delay_ms(1);
    uint32_t got = ac97_read(c, reg);
    return got ? got : 48000;
}

static void ac97_mixer_init(struct ac97_controller *c) {
    ac97_write(c, AC97_MIX_RESET, 0);
    audio_delay_ms(10);
    for (int i = 0; i < 500; i++) {
        if ((ac97_read(c, AC97_MIX_POWERDOWN) & AC97_PD_READY) == AC97_PD_READY) break;
        audio_delay_ms(1);
    }
    c->caps = ac97_read(c, AC97_MIX_RESET);
    c->codec_id = ((uint32_t)ac97_read(c, AC97_MIX_VENDOR_ID1) << 16) | ac97_read(c, AC97_MIX_VENDOR_ID2);
    c->ext_id = ac97_read(c, AC97_MIX_EXT_ID);

    struct audio_quirk_ids ids = {
        .pci_id = c->pci_id,
        .subsys = c->subsys,
        .codec_id = c->codec_id,
        .revision = (uint8_t)(c->codec_id & 0xFF),
        .has_revision = 1,
    };
    audio_quirk_resolve(AUDIO_QUIRK_AC97_CODEC, &ids, 0, &c->codec_quirks);

    if (c->codec_quirks.flags & AUDIO_AC97_Q_MASTER_5BIT) {
        c->master_bits = 5;
    } else {
        ac97_write(c, AC97_MIX_MASTER, 0x2020 | AC97_MUTE);
        uint16_t probe = ac97_read(c, AC97_MIX_MASTER);
        c->master_bits = ((probe & 0x2020) == 0x2020) ? 6 : 5;
    }

    c->vra = 0;
    bool fixed = (c->ctrl_quirks.flags & AUDIO_AC97C_Q_FIXED_48K) || (c->codec_quirks.flags & AUDIO_AC97_Q_NO_VRA);
    if ((c->ext_id & AC97_EXT_VRA) && !fixed) {
        uint16_t st = ac97_read(c, AC97_MIX_EXT_STAT);
        ac97_write(c, AC97_MIX_EXT_STAT, (uint16_t)(st | AC97_EXT_VRA));
        audio_delay_ms(1);
        if (ac97_read(c, AC97_MIX_EXT_STAT) & AC97_EXT_VRA) c->vra = 1;
    }

    ac97_write(c, AC97_MIX_PCM_OUT, 0x0808);
    ac97_set_out_volume(c, AUDIO_VOLUME_DEFAULT, false);
    ac97_write(c, AC97_MIX_MONO, AC97_MUTE);
    ac97_write(c, AC97_MIX_BEEP, AC97_MUTE);
    ac97_write(c, AC97_MIX_PHONE, AC97_MUTE);
    ac97_write(c, AC97_MIX_LINE_IN, AC97_MUTE | 0x0808);
    ac97_write(c, AC97_MIX_CD, AC97_MUTE | 0x0808);
    ac97_write(c, AC97_MIX_VIDEO, AC97_MUTE | 0x0808);
    ac97_write(c, AC97_MIX_AUX, AC97_MUTE | 0x0808);
    uint16_t mic = AC97_MUTE | 0x0008;
    if (c->codec_quirks.flags & AUDIO_AC97_Q_MIC_BOOST) mic |= 0x0040;
    ac97_write(c, AC97_MIX_MIC, mic);
    ac97_write(c, AC97_MIX_REC_SEL, 0x0000);
    ac97_set_in_volume(c, AUDIO_VOLUME_DEFAULT, false);
    ac97_set_eapd(c, true);

    c->has_adc = (ac97_read(c, AC97_MIX_POWERDOWN) & 0x0001) ? 1 : 0;

    struct audio_quirk_ctx ctx;
    ac97_quirk_ctx_init(c, &ctx, AUDIO_QUIRK_AC97_CODEC, AUDIO_QUIRK_STAGE_INIT);
    audio_quirk_run_regs(&c->codec_quirks, &ctx);
    ac97_quirk_ctx_init(c, &ctx, AUDIO_QUIRK_AC97_CONTROLLER, AUDIO_QUIRK_STAGE_INIT);
    audio_quirk_run_regs(&c->ctrl_quirks, &ctx);
    ac97_run_hooks(c, AUDIO_QUIRK_STAGE_INIT);
}

static uint32_t ac97_position(struct ac97_controller *c, struct ac97_stream_hw *hw) {
    uint8_t civ = nabm_r8(c, ac97_box_reg(c, hw, AC97_BOX_CIV)) & 0x1F;
    uint16_t picb = nabm_r16(c, ac97_box_reg(c, hw, AC97_BOX_PICB));
    uint32_t remaining = c->picb_bytes ? picb : (uint32_t)picb * 2;
    if (remaining > hw->period_bytes) remaining = hw->period_bytes;
    uint32_t pos = (uint32_t)(civ % AC97_PERIODS) * hw->period_bytes + (hw->period_bytes - remaining);
    return pos % hw->buf_size;
}

static void ac97_chase_lvi(struct ac97_controller *c, struct ac97_stream_hw *hw) {
    uint8_t civ = nabm_r8(c, ac97_box_reg(c, hw, AC97_BOX_CIV)) & 0x1F;
    nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_LVI), (uint8_t)((civ + AC97_BDL_ENTRIES - 1) & 0x1F));
}

static void ac97_pump_playback(struct ac97_controller *c, struct ac97_stream_hw *hw, bool prefill) {
    struct audio_stream *s = &c->adev.streams[AUDIO_DIR_PLAYBACK];
    uint32_t fb = audio_frame_bytes(&s->hw_fmt);
    if (!fb || !hw->buf) return;

    if (!prefill) {
        uint32_t pos = ac97_position(c, hw);
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

    uint32_t guard = hw->period_bytes;
    uint64_t queued = hw->written - hw->played;
    if (queued + guard < hw->buf_size) {
        uint32_t space = (uint32_t)(hw->buf_size - guard - queued);
        space -= space % fb;
        while (space > 0) {
            uint32_t chunk = hw->buf_size - hw->sw_off;
            if (chunk > space) chunk = space;
            chunk -= chunk % fb;
            if (!chunk) {
                hw->sw_off = 0;
                continue;
            }
            audio_stream_render(&c->adev, s, hw->buf + hw->sw_off, chunk);
            hw->sw_off = (hw->sw_off + chunk) % hw->buf_size;
            hw->written += chunk;
            space -= chunk;
        }
    }
    asm volatile("mfence" ::: "memory");
}

static void ac97_pump_capture(struct ac97_controller *c, struct ac97_stream_hw *hw) {
    struct audio_stream *s = &c->adev.streams[AUDIO_DIR_CAPTURE];
    uint32_t fb = audio_frame_bytes(&s->hw_fmt);
    if (!fb || !hw->buf) return;
    uint32_t pos = ac97_position(c, hw);
    pos -= pos % fb;
    while (hw->sw_off != pos) {
        uint32_t end = (pos > hw->sw_off) ? pos : hw->buf_size;
        uint32_t chunk = end - hw->sw_off;
        audio_stream_capture(&c->adev, s, hw->buf + hw->sw_off, chunk);
        hw->sw_off = (hw->sw_off + chunk) % hw->buf_size;
        hw->played += chunk;
    }
}

static void ac97_free_buffers(struct ac97_stream_hw *hw) {
    if (hw->buf) audio_dma_free(hw->buf, hw->buf_phys, hw->buf_pages);
    if (hw->bdl) audio_dma_free(hw->bdl, hw->bdl_phys, hw->bdl_pages);
    hw->buf = NULL;
    hw->bdl = NULL;
    hw->buf_phys = hw->bdl_phys = 0;
    hw->buf_pages = hw->bdl_pages = 0;
    hw->buf_size = 0;
}

static int ac97_dev_open(struct audio_device *adev, uint8_t direction, const struct audio_format *fmt) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    if (direction >= AUDIO_DIR_COUNT || !fmt) return AUDIO_ERR_PARAM;
    if (direction == AUDIO_DIR_CAPTURE && !c->has_adc) return AUDIO_ERR_UNSUPPORTED;
    if (fmt->format != AUDIO_FMT_S16_LE || fmt->channels != 2) return AUDIO_ERR_UNSUPPORTED;

    struct ac97_stream_hw *hw = &c->hw[direction];
    uint8_t rate_reg = (direction == AUDIO_DIR_PLAYBACK) ? AC97_MIX_FRONT_RATE : AC97_MIX_ADC_RATE;
    uint32_t got = ac97_set_rate(c, rate_reg, fmt->sample_rate);
    if (got != fmt->sample_rate) {
        LOG_WARNING("%s: codec runs at %u Hz instead of %u Hz", adev->name, got, fmt->sample_rate);
        return AUDIO_ERR_UNSUPPORTED;
    }
    hw->rate = got;

    uint32_t fb = audio_frame_bytes(fmt);
    uint32_t period = ((fmt->sample_rate * AC97_PERIOD_MS) / 1000) * fb;
    period = (period + 7) & ~7u;
    while (period % fb) period += 8;
    if (period / 2 > 0xFFFE) period = 0xFFFE * 2 - ((0xFFFE * 2) % fb);
    uint32_t size = period * AC97_PERIODS;

    if (!hw->buf || hw->buf_size != size) {
        ac97_free_buffers(hw);
        hw->buf = (uint8_t *)audio_dma_alloc(size, true, &hw->buf_phys, &hw->buf_pages);
        hw->bdl = (struct ac97_bdl_entry *)audio_dma_alloc(sizeof(struct ac97_bdl_entry) * AC97_BDL_ENTRIES, true,
                                                           &hw->bdl_phys, &hw->bdl_pages);
        if (!hw->buf || !hw->bdl) {
            ac97_free_buffers(hw);
            return AUDIO_ERR_NOMEM;
        }
        hw->buf_size = size;
    }
    memset(hw->buf, 0, hw->buf_size);
    hw->period_bytes = period;

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        hw->bdl[i].addr = (uint32_t)(hw->buf_phys + (uint64_t)(i % AC97_PERIODS) * period);
        hw->bdl[i].samples = (uint16_t)(period / 2);
        hw->bdl[i].flags = 0;
    }
    asm volatile("mfence" ::: "memory");

    hw->prepared = 1;
    hw->running = 0;
    adev->streams[direction].hw_buffer_bytes = size;
    ac97_run_hooks(c, AUDIO_QUIRK_STAGE_OPEN);
    return AUDIO_OK;
}

static void ac97_program_box(struct ac97_controller *c, struct ac97_stream_hw *hw) {
    ac97_box_reset(c, hw);
    nabm_w32(c, ac97_box_reg(c, hw, AC97_BOX_BDBAR), (uint32_t)hw->bdl_phys);
    nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_LVI), AC97_BDL_ENTRIES - 1);
    hw->last_pos = 0;
    hw->played = 0;
    hw->written = 0;
    hw->sw_off = 0;
}

static int ac97_dev_start(struct audio_device *adev, uint8_t direction) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    struct ac97_stream_hw *hw = &c->hw[direction];
    if (!hw->prepared) return AUDIO_ERR_PARAM;
    ac97_program_box(c, hw);
    if (direction == AUDIO_DIR_PLAYBACK) ac97_pump_playback(c, hw, true);
    ac97_chase_lvi(c, hw);
    nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_CR), AC97_CR_RPBM);
    hw->running = 1;
    return AUDIO_OK;
}

static int ac97_dev_stop(struct audio_device *adev, uint8_t direction) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    struct ac97_stream_hw *hw = &c->hw[direction];
    ac97_box_reset(c, hw);
    hw->running = 0;
    return AUDIO_OK;
}

static int ac97_dev_pause(struct audio_device *adev, uint8_t direction, bool paused) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    struct ac97_stream_hw *hw = &c->hw[direction];
    if (!hw->prepared) return AUDIO_ERR_PARAM;
    if (paused) {
        nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_CR), 0);
        hw->running = 0;
    } else {
        ac97_chase_lvi(c, hw);
        nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_CR), AC97_CR_RPBM);
        hw->running = 1;
    }
    return AUDIO_OK;
}

static int ac97_dev_close(struct audio_device *adev, uint8_t direction) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    struct ac97_stream_hw *hw = &c->hw[direction];
    if (hw->running) ac97_dev_stop(adev, direction);
    ac97_free_buffers(hw);
    hw->prepared = 0;
    ac97_run_hooks(c, AUDIO_QUIRK_STAGE_CLOSE);
    return AUDIO_OK;
}

static uint32_t ac97_dev_get_position(struct audio_device *adev, uint8_t direction) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    return (uint32_t)c->hw[direction].played;
}

static int ac97_dev_set_volume(struct audio_device *adev, uint8_t direction, uint8_t volume) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    struct audio_stream *s = &adev->streams[direction];
    bool mute = s->muted && !s->sw_mute;
    if (direction == AUDIO_DIR_PLAYBACK) {
        if (c->codec_quirks.flags & AUDIO_AC97_Q_SOFT_VOLUME) return AUDIO_ERR_UNSUPPORTED;
        ac97_set_out_volume(c, volume, mute);
    } else {
        ac97_set_in_volume(c, volume, mute);
    }
    return AUDIO_OK;
}

static int ac97_dev_set_mute(struct audio_device *adev, uint8_t direction, bool muted) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    uint8_t volume = adev->streams[direction].volume;
    if (direction == AUDIO_DIR_PLAYBACK) {
        if (c->codec_quirks.flags & AUDIO_AC97_Q_SOFT_VOLUME) volume = AUDIO_VOLUME_MAX;
        ac97_set_out_volume(c, volume, muted);
    } else {
        ac97_set_in_volume(c, volume, muted);
    }
    return AUDIO_OK;
}

static void ac97_dev_poll(struct audio_device *adev) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct ac97_stream_hw *hw = &c->hw[d];
        if (!hw->running) continue;

        uint16_t sr = nabm_r16(c, ac97_box_reg(c, hw, AC97_BOX_SR));
        if (sr & AC97_SR_CLEAR) nabm_w16(c, ac97_box_reg(c, hw, AC97_BOX_SR), sr & AC97_SR_CLEAR);

        if (d == AUDIO_DIR_PLAYBACK) ac97_pump_playback(c, hw, false);
        else ac97_pump_capture(c, hw);
        ac97_chase_lvi(c, hw);

        if (sr & AC97_SR_DCH) {
            hw->restarts++;
            nabm_w8(c, ac97_box_reg(c, hw, AC97_BOX_CR), AC97_CR_RPBM);
        }
    }
}

static void ac97_dev_describe(struct audio_device *adev, void (*out)(const char *line)) {
    struct ac97_controller *c = (struct ac97_controller *)adev->priv;
    char line[160];
    char tmp[96];

    snprintf(line, sizeof(line), "controller %s pci %04x:%04x subsys %04x:%04x %s nam 0x%llx nabm 0x%llx", c->name,
             (unsigned)(c->pci_id >> 16), (unsigned)(c->pci_id & 0xFFFF), (unsigned)(c->subsys >> 16),
             (unsigned)(c->subsys & 0xFFFF), c->mmio ? "mmio" : "io",
             c->mmio ? (unsigned long long)(uintptr_t)c->nam_mem : (unsigned long long)c->nam_io,
             c->mmio ? (unsigned long long)(uintptr_t)c->nabm_mem : (unsigned long long)c->nabm_io);
    out(line);
    audio_quirk_flags_string(AUDIO_QUIRK_AC97_CONTROLLER, c->ctrl_quirks.flags, tmp, sizeof(tmp));
    snprintf(line, sizeof(line), "controller quirks: %s", tmp);
    out(line);
    snprintf(line, sizeof(line), "codec %08x caps %04x ext %04x vra %u master %u-bit adc %u glob_cnt %08x glob_sta %08x rd_timeouts %u",
             c->codec_id, (unsigned)c->caps, (unsigned)c->ext_id, (unsigned)c->vra, (unsigned)c->master_bits,
             (unsigned)c->has_adc, nabm_r32(c, AC97_REG_GLOB_CNT), nabm_r32(c, AC97_REG_GLOB_STA), c->read_timeouts);
    out(line);
    audio_quirk_flags_string(AUDIO_QUIRK_AC97_CODEC, c->codec_quirks.flags, tmp, sizeof(tmp));
    snprintf(line, sizeof(line), "codec quirks: %s", tmp);
    out(line);

    for (int r = 0; r < 0x40; r += 16) {
        size_t pos = (size_t)snprintf(line, sizeof(line), " %02x:", r);
        for (int k = 0; k < 16 && pos < sizeof(line); k += 2)
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %04x", (unsigned)ac97_read(c, (uint8_t)(r + k)));
        out(line);
    }
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct ac97_stream_hw *hw = &c->hw[d];
        if (!hw->prepared) continue;
        snprintf(line, sizeof(line), "%s: box 0x%02x %s rate %u civ %u lvi %u sr %04x pos %u played %llu xruns %u restarts %u",
                 d ? "capture" : "playback", (unsigned)hw->box, hw->running ? "running" : "idle", hw->rate,
                 (unsigned)nabm_r8(c, ac97_box_reg(c, hw, AC97_BOX_CIV)), (unsigned)nabm_r8(c, ac97_box_reg(c, hw, AC97_BOX_LVI)),
                 (unsigned)nabm_r16(c, ac97_box_reg(c, hw, AC97_BOX_SR)), (unsigned)ac97_position(c, hw),
                 (unsigned long long)hw->played, hw->xruns, hw->restarts);
        out(line);
    }
}

static const struct audio_device_ops ac97_ops = {
    .open = ac97_dev_open,
    .close = ac97_dev_close,
    .start = ac97_dev_start,
    .stop = ac97_dev_stop,
    .pause = ac97_dev_pause,
    .get_position = ac97_dev_get_position,
    .set_volume = ac97_dev_set_volume,
    .set_mute = ac97_dev_set_mute,
    .poll = ac97_dev_poll,
    .describe = ac97_dev_describe,
    .remove = NULL,
};

static void ac97_fill_caps(struct ac97_controller *c) {
    struct audio_device *a = &c->adev;
    uint32_t rates = AUDIO_RATE_48000;
    if (c->vra) {
        rates = AUDIO_RATE_8000 | AUDIO_RATE_11025 | AUDIO_RATE_16000 | AUDIO_RATE_22050 | AUDIO_RATE_32000 |
                AUDIO_RATE_44100 | AUDIO_RATE_48000 | AUDIO_RATE_CONTINUOUS;
        a->rate_min[AUDIO_DIR_PLAYBACK] = a->rate_min[AUDIO_DIR_CAPTURE] = 8000;
        a->rate_max[AUDIO_DIR_PLAYBACK] = a->rate_max[AUDIO_DIR_CAPTURE] = 48000;
    }
    a->has_direction[AUDIO_DIR_PLAYBACK] = 1;
    a->rates[AUDIO_DIR_PLAYBACK] = rates;
    a->formats[AUDIO_DIR_PLAYBACK] = (uint16_t)AUDIO_FMT_MASK(AUDIO_FMT_S16_LE);
    a->max_channels[AUDIO_DIR_PLAYBACK] = 2;
    if (c->has_adc) {
        a->has_direction[AUDIO_DIR_CAPTURE] = 1;
        a->rates[AUDIO_DIR_CAPTURE] = rates;
        a->formats[AUDIO_DIR_CAPTURE] = (uint16_t)AUDIO_FMT_MASK(AUDIO_FMT_S16_LE);
        a->max_channels[AUDIO_DIR_CAPTURE] = 2;
    }
    if (!c->vra) a->fixed_rate = 48000;
    if (c->codec_quirks.fixed_rate) a->fixed_rate = c->codec_quirks.fixed_rate;
    if (c->ctrl_quirks.fixed_rate) a->fixed_rate = c->ctrl_quirks.fixed_rate;
    if (c->codec_quirks.flags & AUDIO_AC97_Q_SOFT_VOLUME) a->streams[AUDIO_DIR_PLAYBACK].force_sw_volume = 1;
    if (c->codec_quirks.flags & AUDIO_AC97_Q_SWAP_LR) {
        a->streams[AUDIO_DIR_PLAYBACK].swap_lr = 1;
        a->streams[AUDIO_DIR_CAPTURE].swap_lr = 1;
    }
}

static bool ac97_map_bars(struct ac97_controller *c) {
    struct pci_device *pci = c->pci;
    bool io0 = false, io1 = false, io2 = false, io3 = false;
    uint64_t b0 = pci_bar_phys(pci, 0, &io0);
    uint64_t b1 = pci_bar_phys(pci, 1, &io1);
    uint64_t b2 = pci_bar_phys(pci, 2, &io2);
    uint64_t b3 = pci_bar_phys(pci, 3, &io3);

    bool have_io = b0 && b1 && io0 && io1;
    bool have_mem = b2 && b3 && !io2 && !io3;
    bool want_mem = (c->ctrl_quirks.flags & AUDIO_AC97C_Q_PREFER_MMIO) != 0;

    if (have_mem && (want_mem || !have_io)) {
        uint64_t nam = vmm_map_mmio(b2, 512, 0);
        uint64_t nabm = vmm_map_mmio(b3, 256, 0);
        if (nam && nabm) {
            c->mmio = 1;
            c->nam_mem = (volatile uint8_t *)nam;
            c->nabm_mem = (volatile uint8_t *)nabm;
            return true;
        }
    }
    if (have_io) {
        if (c->ctrl_quirks.flags & AUDIO_AC97C_Q_ICH4_IOSE) {
            uint32_t d = pci_read_config(pci->bus, pci->slot, pci->func, 0x40);
            if (!(d & (1u << 8))) pci_write_config(pci->bus, pci->slot, pci->func, 0x40, d | (1u << 8));
        }
        c->mmio = 0;
        c->nam_io = (uint16_t)b0;
        c->nabm_io = (uint16_t)b1;
        return true;
    }
    return false;
}

static void ac97_controller_name(struct ac97_controller *c) {
    if (c->ctrl_quirks.model) snprintf(c->name, sizeof(c->name), "%s", c->ctrl_quirks.model);
    else snprintf(c->name, sizeof(c->name), "AC97 %04x:%04x", (unsigned)(c->pci_id >> 16), (unsigned)(c->pci_id & 0xFFFF));
}

static bool ac97_attach(struct pci_device *pci) {
    if (ac97_ctrl_count >= AC97_MAX_CONTROLLERS) return false;
    struct ac97_controller *c = (struct ac97_controller *)kmalloc(sizeof(struct ac97_controller));
    if (!c) return false;
    memset(c, 0, sizeof(*c));
    c->pci = pci;
    c->pci_id = AUDIO_ID(pci->vendor_id, pci->device_id);
    uint32_t ss = pci_read_config(pci->bus, pci->slot, pci->func, 0x2C);
    c->subsys = AUDIO_ID(ss & 0xFFFF, ss >> 16);
    c->revision = (uint8_t)(pci_read_config(pci->bus, pci->slot, pci->func, 0x08) & 0xFF);
    c->adev.vendor_id = c->pci_id;
    c->adev.subsystem_id = c->subsys;

    struct audio_quirk_ids ids = {
        .pci_id = c->pci_id,
        .subsys = c->subsys,
        .revision = c->revision,
        .has_revision = 1,
    };
    audio_quirk_resolve(AUDIO_QUIRK_AC97_CONTROLLER, &ids, 0, &c->ctrl_quirks);
    ac97_controller_name(c);

    if (c->ctrl_quirks.flags & AUDIO_AC97C_Q_SKIP) {
        LOG_INFO("%s skipped by quirk", c->name);
        kfree(c);
        return false;
    }

    pci_enable_bus_mastering(pci);
    if (!ac97_map_bars(c)) {
        LOG_ERROR("%s: no usable NAM/NABM BARs", c->name);
        kfree(c);
        return false;
    }

    if (c->ctrl_quirks.flags & AUDIO_AC97C_Q_SIS_REGS) {
        c->reg_sr = AC97_SIS_BOX_SR;
        c->reg_picb = AC97_SIS_BOX_PICB;
        c->picb_bytes = 1;
    } else {
        c->reg_sr = AC97_BOX_SR;
        c->reg_picb = AC97_BOX_PICB;
        c->picb_bytes = 0;
    }
    c->hw[AUDIO_DIR_PLAYBACK].box = AC97_BOX_PCM_OUT;
    c->hw[AUDIO_DIR_CAPTURE].box = AC97_BOX_PCM_IN;

    ac97_box_reset(c, &c->hw[AUDIO_DIR_PLAYBACK]);
    ac97_box_reset(c, &c->hw[AUDIO_DIR_CAPTURE]);
    ac97_box_reset(c, &(struct ac97_stream_hw){ .box = AC97_BOX_MIC_IN });

    struct audio_quirk_ctx ctx;
    ac97_quirk_ctx_init(c, &ctx, AUDIO_QUIRK_AC97_CONTROLLER, AUDIO_QUIRK_STAGE_ATTACH);
    audio_quirk_run_hooks(&c->ctrl_quirks, &ctx);

    if (!ac97_cold_reset(c)) {
        LOG_WARNING("%s: primary codec not ready (GLOB_STA %08x)", c->name, nabm_r32(c, AC97_REG_GLOB_STA));
        kfree(c);
        return false;
    }
    if (c->ctrl_quirks.flags & AUDIO_AC97C_Q_SIS_REGS)
        nabm_w16(c, AC97_REG_SIS_UNMUTE, (uint16_t)(nabm_r16(c, AC97_REG_SIS_UNMUTE) | 0x0001));

    ac97_mixer_init(c);
    if (c->codec_id == 0 || c->codec_id == 0xFFFFFFFFu) {
        LOG_WARNING("%s: codec id reads 0x%08x, no codec", c->name, c->codec_id);
        kfree(c);
        return false;
    }

    char cname[48];
    ac97_codec_name(c, cname, sizeof(cname));

    struct audio_device *a = &c->adev;
    a->type = AC97_AUDIO;
    a->pci = pci;
    a->ops = &ac97_ops;
    a->priv = c;
    a->vendor_id = c->pci_id;
    a->subsystem_id = c->subsys;
    a->codec_id = c->codec_id;
    a->quirks = c->codec_quirks.flags | (c->ctrl_quirks.flags << 32);
    a->flags = AUDIO_DEV_FLAG_HW_VOLUME | AUDIO_DEV_FLAG_HW_MUTE;
    snprintf(a->name, sizeof(a->name), "%s", cname);
    snprintf(a->codec_name, sizeof(a->codec_name), "%s", cname);
    snprintf(a->description, sizeof(a->description), "%s", c->name);
    {
        char n1[48], n2[48];
        audio_quirk_set_names(&c->ctrl_quirks, n1, sizeof(n1));
        audio_quirk_set_names(&c->codec_quirks, n2, sizeof(n2));
        snprintf(a->quirk_names, sizeof(a->quirk_names), "%s%s%s", n1, (n1[0] && n2[0]) ? "," : "", n2);
    }
    ac97_fill_caps(c);

    ac97_run_hooks(c, AUDIO_QUIRK_STAGE_CONFIGURED);

    if (audio_core_add_device(a) < 0) {
        kfree(c);
        return false;
    }
    ac97_ctrls[ac97_ctrl_count++] = c;

    char q1[64], q2[64];
    audio_quirk_flags_string(AUDIO_QUIRK_AC97_CONTROLLER, c->ctrl_quirks.flags, q1, sizeof(q1));
    audio_quirk_flags_string(AUDIO_QUIRK_AC97_CODEC, c->codec_quirks.flags, q2, sizeof(q2));
    LOG_INFO("%s at %02x:%02x.%x (%s): codec %s (%08x), %s, %u-bit master, ADC %s, quirks %s / %s", c->name,
             (unsigned)pci->bus, (unsigned)pci->slot, (unsigned)pci->func, c->mmio ? "mmio" : "io", cname, c->codec_id,
             c->vra ? "variable rate" : "48 kHz only", (unsigned)c->master_bits, c->has_adc ? "yes" : "no", q1, q2);
    return true;
}

static bool ac97_seen_before(struct pci_device *pci) {
    for (int i = 0; i < ac97_seen_count; i++)
        if (ac97_seen[i] == pci) return true;
    return false;
}

static void ac97_scan(void) {
    spin_lock(&ac97_scan_lock);
    int pci_count = pci_get_device_count();
    if (pci_count > MAX_PCI_DEVICES) pci_count = MAX_PCI_DEVICES;
    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev = (struct pci_device *)device_table[PCI_DEVICE][i];
        if (!dev) continue;
        if (dev->class_code != PCI_CLASS_MULTIMEDIA || dev->subclass != PCI_SUBCLASS_AC97) continue;
        if (ac97_seen_before(dev)) continue;
        if (ac97_seen_count < (int)(sizeof(ac97_seen) / sizeof(ac97_seen[0]))) ac97_seen[ac97_seen_count++] = dev;
        ac97_attach(dev);
    }
    spin_unlock(&ac97_scan_lock);
}

static const struct audio_backend ac97_backend = {
    .name = "ac97",
    .type = AC97_AUDIO,
    .scan = ac97_scan,
    .poll = NULL,
};

static int ac97_get_controller_count(void) {
    return ac97_ctrl_count;
}

static struct ac97_controller *ac97_get_controller(int idx) {
    if (idx < 0 || idx >= ac97_ctrl_count) return NULL;
    return ac97_ctrls[idx];
}

static struct ac97_driver drv_ac97 = {
    .get_controller_count = ac97_get_controller_count,
    .get_controller = ac97_get_controller,
    .codec_read = ac97_read,
    .codec_write = ac97_write,
};

struct ac97_driver *return_ac97_driver(void) {
    audio_core_register_backend(&ac97_backend);
    ac97_scan();
    LOG_INFO("AC97: %d controller(s)", ac97_ctrl_count);
    return &drv_ac97;
}

struct driver *return_meta_ac97_driver(void) {
    static struct dependency deps[] = {
        MAKE_DEPENDENCY(AUDIO_DRIVER, AUDIO_CORE_SLOT),
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "AC97 Audio Driver",
        .type = AUDIO_DRIVER,
        .sub_type = AC97_AUDIO,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &deps[0], &deps[1] },
        .dependency_count = 2,
        .self = &drv_ac97,
        .init = (void *)return_ac97_driver,
    };
    return &meta;
}
