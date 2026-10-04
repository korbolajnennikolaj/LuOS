#include "audio_core.h"

#include "audio_quirks.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "components/Memory/mm.h"
#include "components/Memory/pmm.h"
#include "drivers/Timer/timer.h"
#include "kernel/scheduler/scheduler.h"

#include <stddef.h>
#include <string.h>

static spinlock_t audio_registry_lock = SPINLOCK_INIT;
static int audio_device_slots = 0;
static volatile int audio_default_device = -1;

static const struct audio_backend *audio_backends[AUDIO_MAX_BACKENDS];
static int audio_backend_count = 0;

static struct tsc_driver *audio_tsc = NULL;
static bool audio_core_ready = false;

static const uint16_t audio_volume_table[AUDIO_VOLUME_MAX + 1] = {
    0, 110, 116, 123, 130, 138, 146, 155, 164, 174, 184, 195, 207, 219, 232, 246, 260, 276, 292, 309,
    328, 347, 368, 389, 413, 437, 463, 490, 519, 550, 583, 617, 654, 693, 734, 777, 823, 872, 924, 978,
    1036, 1098, 1163, 1232, 1305, 1382, 1464, 1550, 1642, 1740, 1843, 1952, 2068, 2190, 2320, 2457,
    2603, 2757, 2920, 3093, 3277, 3471, 3677, 3894, 4125, 4370, 4629, 4903, 5193, 5501, 5827, 6172,
    6538, 6925, 7336, 7771, 8231, 8719, 9235, 9783, 10362, 10976, 11627, 12315, 13045, 13818, 14637,
    15504, 16423, 17396, 18427, 19519, 20675, 21900, 23198, 24573, 26029, 27571, 29205, 30935, 32768,
};

static const int16_t audio_sine_table[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

static const uint32_t audio_rate_values[AUDIO_RATE_COUNT] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000,
};

static struct tsc_driver *audio_get_tsc(void) {
    if (!audio_tsc) audio_tsc = get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return audio_tsc;
}

void audio_delay_us(uint64_t us) {
    struct tsc_driver *tsc = audio_get_tsc();
    if (tsc && tsc->sleep_tsc_us) {
        tsc->sleep_tsc_us(us);
        return;
    }
    for (volatile uint64_t i = 0; i < us * 2000ull; i++) asm volatile("pause");
}

void audio_delay_ms(uint64_t ms) {
    struct tsc_driver *tsc = audio_get_tsc();
    if (tsc && tsc->sleep_tsc_ms) {
        tsc->sleep_tsc_ms(ms);
        return;
    }
    for (volatile uint64_t i = 0; i < ms * 2000000ull; i++) asm volatile("pause");
}

uint64_t audio_uptime_ms(void) {
    struct tsc_driver *tsc = audio_get_tsc();
    return (tsc && tsc->get_tsc_uptime_ms) ? tsc->get_tsc_uptime_ms() : 0;
}

void *audio_dma_alloc(uint32_t size, bool below_4g, uint64_t *phys_out, uint32_t *pages_out) {
    if (size == 0) return NULL;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys = below_4g ? pmm_alloc_dma32_pages(pages) : pmm_alloc_pages(pages);
    if (!phys) {
        LOG_ERROR("cannot allocate %u DMA page(s)%s", (unsigned)pages, below_4g ? " below 4 GiB" : "");
        return NULL;
    }
    if (below_4g && phys + (uint64_t)pages * PAGE_SIZE > PMM_DMA32_LIMIT) {
        pmm_free_dma32_pages(phys, pages);
        LOG_ERROR("DMA buffer at 0x%llx is above 4 GiB, controller needs 32-bit addresses",
                  (unsigned long long)phys);
        return NULL;
    }

    void *virt = (void *)mm_phys_to_virt(phys);
    memset(virt, 0, (size_t)pages * PAGE_SIZE);
    if (phys_out) *phys_out = phys;
    if (pages_out) *pages_out = pages;
    return virt;
}

void audio_dma_free(void *virt, uint64_t phys, uint32_t pages) {
    (void)virt;
    if (phys && pages) pmm_free_dma32_pages(phys, pages);
}

uint32_t audio_sample_bytes(uint8_t format) {
    switch (format) {
        case AUDIO_FMT_U8: return 1;
        case AUDIO_FMT_S16_LE: return 2;
        case AUDIO_FMT_S24_LE: return 4;
        case AUDIO_FMT_S32_LE: return 4;
        case AUDIO_FMT_S24_3LE: return 3;
        default: return 0;
    }
}

const char *audio_format_name(uint8_t format) {
    switch (format) {
        case AUDIO_FMT_U8: return "U8";
        case AUDIO_FMT_S16_LE: return "S16_LE";
        case AUDIO_FMT_S24_LE: return "S24_LE";
        case AUDIO_FMT_S32_LE: return "S32_LE";
        case AUDIO_FMT_S24_3LE: return "S24_3LE";
        default: return "?";
    }
}

const char *audio_type_name(enum AUDIO_TYPE type) {
    switch (type) {
        case USB_AUDIO: return "USB";
        case HDA_AUDIO: return "HDA";
        case AC97_AUDIO: return "AC97";
    }
    return "?";
}

const char *audio_state_name(uint8_t state) {
    switch (state) {
        case AUDIO_STATE_STOPPED: return "stopped";
        case AUDIO_STATE_PLAYING: return "running";
        case AUDIO_STATE_PAUSED: return "paused";
        default: return "?";
    }
}

uint32_t audio_frame_bytes(const struct audio_format *fmt) {
    if (!fmt) return 0;
    return audio_sample_bytes(fmt->format) * (uint32_t)fmt->channels;
}

uint32_t audio_rate_to_mask(uint32_t sample_rate) {
    for (int i = 0; i < AUDIO_RATE_COUNT; i++)
        if (audio_rate_values[i] == sample_rate) return 1u << i;
    return 0;
}

uint32_t audio_mask_to_rate(uint32_t mask_bit) {
    for (int i = 0; i < AUDIO_RATE_COUNT; i++)
        if (mask_bit == (1u << i)) return audio_rate_values[i];
    return 0;
}

uint32_t audio_volume_gain_q15(uint8_t volume) {
    if (volume > AUDIO_VOLUME_MAX) volume = AUDIO_VOLUME_MAX;
    return audio_volume_table[volume];
}

bool audio_device_supports_rate(const struct audio_device *dev, uint8_t direction, uint32_t rate) {
    if (!dev || direction >= AUDIO_DIR_COUNT) return false;
    if (dev->fixed_rate) return rate == dev->fixed_rate;
    if (dev->rates[direction] & AUDIO_RATE_CONTINUOUS) {
        uint32_t lo = dev->rate_min[direction] ? dev->rate_min[direction] : AUDIO_RATE_MIN;
        uint32_t hi = dev->rate_max[direction] ? dev->rate_max[direction] : AUDIO_RATE_MAX;
        if (rate >= lo && rate <= hi) return true;
    }
    uint32_t mask = audio_rate_to_mask(rate);
    return mask && (dev->rates[direction] & mask);
}

int audio_negotiate_format(const struct audio_device *dev, uint8_t direction,
                           const struct audio_format *want, struct audio_format *out) {
    if (!dev || !want || !out || direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    if (!dev->has_direction[direction]) return AUDIO_ERR_UNSUPPORTED;

    uint32_t rate = want->sample_rate;
    if (!audio_device_supports_rate(dev, direction, rate)) {
        static const uint32_t prefer[] = { 48000, 44100, 96000, 32000, 22050, 16000, 8000, 88200, 176400, 192000, 11025 };
        rate = 0;
        if (dev->fixed_rate) rate = dev->fixed_rate;
        for (size_t i = 0; !rate && i < sizeof(prefer) / sizeof(prefer[0]); i++)
            if (audio_device_supports_rate(dev, direction, prefer[i])) rate = prefer[i];
        if (!rate) return AUDIO_ERR_UNSUPPORTED;
    }

    uint16_t fmts = dev->formats[direction];
    uint8_t format = want->format;
    if (!(fmts & AUDIO_FMT_MASK(format))) {
        static const uint8_t prefer_fmt[] = { AUDIO_FMT_S16_LE, AUDIO_FMT_S32_LE, AUDIO_FMT_S24_LE, AUDIO_FMT_S24_3LE, AUDIO_FMT_U8 };
        bool deep = (want->format == AUDIO_FMT_S24_LE || want->format == AUDIO_FMT_S32_LE || want->format == AUDIO_FMT_S24_3LE);
        format = 0xFF;
        if (deep) {
            if (fmts & AUDIO_FMT_MASK(AUDIO_FMT_S32_LE)) format = AUDIO_FMT_S32_LE;
            else if (fmts & AUDIO_FMT_MASK(AUDIO_FMT_S24_LE)) format = AUDIO_FMT_S24_LE;
            else if (fmts & AUDIO_FMT_MASK(AUDIO_FMT_S24_3LE)) format = AUDIO_FMT_S24_3LE;
        }
        for (size_t i = 0; format == 0xFF && i < sizeof(prefer_fmt); i++)
            if (fmts & AUDIO_FMT_MASK(prefer_fmt[i])) format = prefer_fmt[i];
        if (format == 0xFF) return AUDIO_ERR_UNSUPPORTED;
    }

    uint8_t max_ch = dev->max_channels[direction] ? dev->max_channels[direction] : 2;
    uint8_t ch = want->channels;
    if (ch < 2) ch = 2;
    if (ch > max_ch) ch = max_ch;
    if (ch > AUDIO_MAX_CHANNELS) ch = AUDIO_MAX_CHANNELS;
    if (ch == 0) ch = 1;

    out->sample_rate = rate;
    out->channels = ch;
    out->format = format;
    switch (format) {
        case AUDIO_FMT_U8: out->bits_per_sample = 8; break;
        case AUDIO_FMT_S16_LE: out->bits_per_sample = 16; break;
        case AUDIO_FMT_S24_LE:
        case AUDIO_FMT_S24_3LE: out->bits_per_sample = 24; break;
        default: out->bits_per_sample = 32; break;
    }
    return AUDIO_OK;
}

static inline uint32_t ring_index(const struct audio_stream *s, uint32_t pos) {
    return (pos >= s->ring_size) ? pos - s->ring_size : pos;
}

static inline uint32_t ring_advance(const struct audio_stream *s, uint32_t pos, uint32_t n) {
    pos += n;
    if (pos >= 2 * s->ring_size) pos -= 2 * s->ring_size;
    return pos;
}

uint32_t audio_stream_fill(const struct audio_stream *s) {
    if (!s || !s->ring || !s->ring_size) return 0;
    uint32_t w = s->write_pos;
    uint32_t r = s->read_pos;
    return (w >= r) ? (w - r) : (2 * s->ring_size - r + w);
}

uint32_t audio_stream_space(const struct audio_stream *s) {
    if (!s || !s->ring || !s->ring_size) return 0;
    return s->ring_size - audio_stream_fill(s);
}

static uint32_t ring_write(struct audio_stream *s, const uint8_t *src, uint32_t len) {
    uint32_t space = audio_stream_space(s);
    if (len > space) len = space;
    uint32_t w = s->write_pos;
    uint32_t done = 0;
    while (done < len) {
        uint32_t idx = ring_index(s, w);
        uint32_t chunk = s->ring_size - idx;
        if (chunk > len - done) chunk = len - done;
        memcpy(s->ring + idx, src + done, chunk);
        done += chunk;
        w = ring_advance(s, w, chunk);
    }
    __atomic_store_n(&s->write_pos, w, __ATOMIC_RELEASE);
    return len;
}

static uint32_t ring_read(struct audio_stream *s, uint8_t *dst, uint32_t len) {
    uint32_t fill = audio_stream_fill(s);
    if (len > fill) len = fill;
    uint32_t r = s->read_pos;
    uint32_t done = 0;
    while (done < len) {
        uint32_t idx = ring_index(s, r);
        uint32_t chunk = s->ring_size - idx;
        if (chunk > len - done) chunk = len - done;
        if (dst) memcpy(dst + done, s->ring + idx, chunk);
        done += chunk;
        r = ring_advance(s, r, chunk);
    }
    __atomic_store_n(&s->read_pos, r, __ATOMIC_RELEASE);
    return len;
}

static inline int32_t sample_decode(const uint8_t *p, uint8_t format) {
    switch (format) {
        case AUDIO_FMT_U8:
            return ((int32_t)p[0] - 128) * 16777216;
        case AUDIO_FMT_S16_LE:
            return (int32_t)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) * 65536;
        case AUDIO_FMT_S24_LE:
        case AUDIO_FMT_S24_3LE: {
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            return (int32_t)(v << 8);
        }
        case AUDIO_FMT_S32_LE:
            return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        default:
            return 0;
    }
}

static inline void sample_encode(uint8_t *p, uint8_t format, int32_t v) {
    switch (format) {
        case AUDIO_FMT_U8:
            p[0] = (uint8_t)((v >> 24) + 128);
            break;
        case AUDIO_FMT_S16_LE: {
            int32_t s = v >> 16;
            p[0] = (uint8_t)s;
            p[1] = (uint8_t)(s >> 8);
            break;
        }
        case AUDIO_FMT_S24_LE: {
            int32_t s = v >> 8;
            p[0] = (uint8_t)s;
            p[1] = (uint8_t)(s >> 8);
            p[2] = (uint8_t)(s >> 16);
            p[3] = (uint8_t)(s >> 24);
            break;
        }
        case AUDIO_FMT_S24_3LE: {
            int32_t s = v >> 8;
            p[0] = (uint8_t)s;
            p[1] = (uint8_t)(s >> 8);
            p[2] = (uint8_t)(s >> 16);
            break;
        }
        case AUDIO_FMT_S32_LE:
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            p[2] = (uint8_t)(v >> 16);
            p[3] = (uint8_t)(v >> 24);
            break;
        default:
            break;
    }
}

static void frame_decode(const uint8_t *src, const struct audio_format *in, int32_t *out, uint8_t out_ch, bool swap_lr) {
    int32_t tmp[AUDIO_MAX_CHANNELS];
    uint32_t sb = audio_sample_bytes(in->format);
    uint8_t in_ch = in->channels;
    if (in_ch > AUDIO_MAX_CHANNELS) in_ch = AUDIO_MAX_CHANNELS;
    for (uint8_t c = 0; c < in_ch; c++) tmp[c] = sample_decode(src + c * sb, in->format);

    if (in_ch == out_ch) {
        for (uint8_t c = 0; c < out_ch; c++) out[c] = tmp[c];
    } else if (in_ch == 1) {
        for (uint8_t c = 0; c < out_ch; c++) out[c] = tmp[0];
    } else if (out_ch == 1) {
        out[0] = (tmp[0] >> 1) + (tmp[1] >> 1);
    } else {
        for (uint8_t c = 0; c < out_ch; c++) out[c] = (c < in_ch) ? tmp[c] : tmp[c & 1];
    }

    if (swap_lr && out_ch >= 2) {
        int32_t t = out[0];
        out[0] = out[1];
        out[1] = t;
    }
}

static inline int32_t apply_gain(int32_t v, uint32_t gain) {
    if (gain >= 32768) return v;
    return (int32_t)(((int64_t)v * (int64_t)gain) >> 15);
}

void audio_stream_reset_converter(struct audio_stream *s) {
    if (!s) return;
    memset(&s->conv, 0, sizeof(s->conv));
    uint32_t in_rate = (s->direction == AUDIO_DIR_PLAYBACK) ? s->fmt.sample_rate : s->hw_fmt.sample_rate;
    uint32_t out_rate = (s->direction == AUDIO_DIR_PLAYBACK) ? s->hw_fmt.sample_rate : s->fmt.sample_rate;
    if (!in_rate || !out_rate) {
        s->conv.step = 0x10000;
        return;
    }
    s->conv.step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    if (s->conv.step == 0) s->conv.step = 1;
}

static uint32_t stream_gain(const struct audio_stream *s) {
    if (s->muted && s->sw_mute) return 0;
    if (s->sw_volume || s->force_sw_volume) return audio_volume_gain_q15(s->volume);
    return 32768;
}

static bool stream_is_passthrough(const struct audio_stream *s, uint32_t gain) {
    return gain >= 32768 && !s->swap_lr &&
           s->fmt.format == s->hw_fmt.format &&
           s->fmt.channels == s->hw_fmt.channels &&
           s->fmt.sample_rate == s->hw_fmt.sample_rate;
}

uint32_t audio_stream_render(struct audio_device *dev, struct audio_stream *s, uint8_t *dst, uint32_t dst_bytes) {
    (void)dev;
    if (!s || !dst || dst_bytes == 0) return 0;

    uint32_t hw_fb = audio_frame_bytes(&s->hw_fmt);
    if (!hw_fb) {
        memset(dst, 0, dst_bytes);
        return 0;
    }

    uint32_t out_frames = dst_bytes / hw_fb;

    if (!s->opened || !s->ring || s->state != AUDIO_STATE_PLAYING) {
        memset(dst, 0, dst_bytes);
        s->silence_bytes += dst_bytes;
        return 0;
    }

    uint32_t src_fb = audio_frame_bytes(&s->fmt);
    uint32_t gain = stream_gain(s);
    uint32_t produced = 0;

    if (stream_is_passthrough(s, gain)) {
        uint32_t fill = audio_stream_fill(s);
        uint32_t want = out_frames * hw_fb;
        uint32_t take = (fill / hw_fb) * hw_fb;
        if (take > want) take = want;
        ring_read(s, dst, take);
        produced = take / hw_fb;
    } else {
        uint8_t ch = s->hw_fmt.channels;
        uint32_t hw_sb = audio_sample_bytes(s->hw_fmt.format);
        struct audio_convert_state *cv = &s->conv;
        uint8_t frame[AUDIO_MAX_CHANNELS * 4];

        for (uint32_t o = 0; o < out_frames; o++) {
            bool starved = false;
            while (cv->primed < 2 || cv->frac >= 0x10000) {
                if (audio_stream_fill(s) < src_fb) {
                    starved = true;
                    break;
                }
                int32_t in[AUDIO_MAX_CHANNELS];
                ring_read(s, frame, src_fb);
                frame_decode(frame, &s->fmt, in, ch, s->swap_lr);
                if (cv->primed == 0) {
                    memcpy(cv->cur, in, sizeof(int32_t) * ch);
                    cv->primed = 1;
                    continue;
                }
                memcpy(cv->prev, cv->cur, sizeof(int32_t) * ch);
                memcpy(cv->cur, in, sizeof(int32_t) * ch);
                if (cv->primed == 1) cv->primed = 2;
                else cv->frac -= 0x10000;
            }
            if (starved) break;

            uint8_t *out = dst + (size_t)o * hw_fb;
            for (uint8_t c = 0; c < ch; c++) {
                int64_t a = cv->prev[c];
                int64_t b = cv->cur[c];
                int32_t v = (int32_t)(a + (((b - a) * (int64_t)cv->frac) >> 16));
                sample_encode(out + c * hw_sb, s->hw_fmt.format, apply_gain(v, gain));
            }
            cv->frac += cv->step;
            produced++;
        }
    }

    uint32_t real_bytes = produced * hw_fb;
    if (real_bytes < dst_bytes) {
        memset(dst + real_bytes, 0, dst_bytes - real_bytes);
        if (produced < out_frames) {
            s->silence_bytes += (out_frames - produced) * hw_fb;
            if (!s->draining) s->underruns++;
        }
    }

    s->frames_done += produced;
    s->hw_bytes += real_bytes;
    return real_bytes;
}

uint32_t audio_stream_capture(struct audio_device *dev, struct audio_stream *s, const uint8_t *src, uint32_t src_bytes) {
    (void)dev;
    if (!s || !src || !s->opened || !s->ring || s->state != AUDIO_STATE_PLAYING) return 0;

    uint32_t hw_fb = audio_frame_bytes(&s->hw_fmt);
    uint32_t out_fb = audio_frame_bytes(&s->fmt);
    if (!hw_fb || !out_fb) return 0;

    uint32_t in_frames = src_bytes / hw_fb;
    uint32_t written = 0;
    uint32_t gain = stream_gain(s);

    if (stream_is_passthrough(s, gain)) {
        uint32_t len = in_frames * hw_fb;
        uint32_t space = (audio_stream_space(s) / out_fb) * out_fb;
        if (len > space) {
            s->overruns++;
            len = space;
        }
        written = ring_write(s, src, len);
        s->frames_done += written / out_fb;
        s->hw_bytes += in_frames * hw_fb;
        return written;
    }

    uint8_t ch = s->fmt.channels;
    uint32_t sb = audio_sample_bytes(s->fmt.format);
    struct audio_convert_state *cv = &s->conv;
    uint8_t frame[AUDIO_MAX_CHANNELS * 4];
    bool overrun = false;

    for (uint32_t i = 0; i < in_frames; i++) {
        int32_t in[AUDIO_MAX_CHANNELS];
        frame_decode(src + (size_t)i * hw_fb, &s->hw_fmt, in, ch, s->swap_lr);
        if (cv->primed == 0) {
            memcpy(cv->prev, in, sizeof(int32_t) * ch);
            memcpy(cv->cur, in, sizeof(int32_t) * ch);
            cv->primed = 2;
        } else {
            memcpy(cv->prev, cv->cur, sizeof(int32_t) * ch);
            memcpy(cv->cur, in, sizeof(int32_t) * ch);
        }

        while (cv->frac < 0x10000) {
            for (uint8_t c = 0; c < ch; c++) {
                int64_t a = cv->prev[c];
                int64_t b = cv->cur[c];
                int32_t v = (int32_t)(a + (((b - a) * (int64_t)cv->frac) >> 16));
                sample_encode(frame + c * sb, s->fmt.format, apply_gain(v, gain));
            }
            if (audio_stream_space(s) >= out_fb) {
                ring_write(s, frame, out_fb);
                written += out_fb;
                s->frames_done++;
            } else {
                overrun = true;
            }
            cv->frac += cv->step;
        }
        cv->frac -= 0x10000;
    }

    if (overrun) s->overruns++;
    s->hw_bytes += in_frames * hw_fb;
    return written;
}

int audio_get_device_count(void) {
    return audio_device_slots;
}

struct audio_device *audio_get_device(int idx) {
    if (idx < 0 || idx >= MAX_AUDIO_DEVICES) return NULL;
    struct audio_device *dev = (struct audio_device *)device_table[AUDIO_DEVICE][idx];
    if (!dev || !dev->valid) return NULL;
    return dev;
}

static int audio_pick_default(int exclude) {
    int fallback = -1;
    for (int i = 0; i < audio_device_slots; i++) {
        if (i == exclude) continue;
        struct audio_device *d = audio_get_device(i);
        if (!d || !d->has_direction[AUDIO_DIR_PLAYBACK]) continue;
        if (!(d->flags & AUDIO_DEV_FLAG_DIGITAL)) return i;
        if (fallback < 0) fallback = i;
    }
    return fallback;
}

int audio_core_add_device(struct audio_device *dev) {
    if (!dev || !dev->ops) return AUDIO_ERR_PARAM;

    uint64_t flags = spin_lock_irqsave(&audio_registry_lock);
    int slot = -1;
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++) {
        if (!device_table[AUDIO_DEVICE][i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&audio_registry_lock, flags);
        LOG_ERROR("audio device table full, '%s' not registered", dev->name);
        return AUDIO_ERR_BUSY;
    }

    spin_lock_init(&dev->lock);
    dev->index = slot;
    dev->flags &= ~AUDIO_DEV_FLAG_REMOVED;
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct audio_stream *s = &dev->streams[d];
        uint8_t vol = s->volume;
        uint8_t muted = s->muted;
        uint8_t swap = s->swap_lr;
        uint8_t force_sw = s->force_sw_volume;
        memset(s, 0, sizeof(*s));
        s->direction = (uint8_t)d;
        s->state = AUDIO_STATE_STOPPED;
        s->volume = vol ? vol : AUDIO_VOLUME_DEFAULT;
        s->muted = muted;
        s->swap_lr = swap;
        s->force_sw_volume = force_sw;
    }
    dev->valid = 1;
    device_table[AUDIO_DEVICE][slot] = dev;
    if (slot + 1 > audio_device_slots) audio_device_slots = slot + 1;

    bool make_default = false;
    if (dev->has_direction[AUDIO_DIR_PLAYBACK]) {
        int cur = audio_default_device;
        struct audio_device *cd = (cur >= 0) ? (struct audio_device *)device_table[AUDIO_DEVICE][cur] : NULL;
        if (!cd) make_default = true;
        else if ((cd->flags & AUDIO_DEV_FLAG_DIGITAL) && !(dev->flags & AUDIO_DEV_FLAG_DIGITAL)) make_default = true;
        else if (dev->flags & AUDIO_DEV_FLAG_HOTPLUG) make_default = true;
    }
    if (make_default) audio_default_device = slot;
    spin_unlock_irqrestore(&audio_registry_lock, flags);

    LOG_INFO("audio%d: %s \"%s\"%s%s%s", slot, audio_type_name(dev->type), dev->name,
             dev->has_direction[AUDIO_DIR_PLAYBACK] ? " playback" : "",
             dev->has_direction[AUDIO_DIR_CAPTURE] ? " capture" : "",
             make_default ? " (default)" : "");
    return slot;
}

static void audio_stream_release(struct audio_device *dev, int d) {
    struct audio_stream *s = &dev->streams[d];
    if (!s->opened) return;
    if (s->state != AUDIO_STATE_STOPPED && dev->ops && dev->ops->stop) dev->ops->stop(dev, (uint8_t)d);
    if (dev->ops && dev->ops->close) dev->ops->close(dev, (uint8_t)d);
    s->state = AUDIO_STATE_STOPPED;
    s->opened = 0;
    uint8_t *ring = s->ring;
    s->ring = NULL;
    s->ring_size = 0;
    s->read_pos = 0;
    s->write_pos = 0;
    if (ring) kfree(ring);
}

void audio_core_remove_device(struct audio_device *dev) {
    if (!dev) return;
    int slot = dev->index;
    if (slot < 0 || slot >= MAX_AUDIO_DEVICES || device_table[AUDIO_DEVICE][slot] != dev) return;

    spin_lock(&dev->lock);
    dev->flags |= AUDIO_DEV_FLAG_REMOVED;
    for (int d = 0; d < AUDIO_DIR_COUNT; d++) {
        struct audio_stream *s = &dev->streams[d];
        if (!s->opened) continue;
        s->state = AUDIO_STATE_STOPPED;
        s->opened = 0;
        uint8_t *ring = s->ring;
        s->ring = NULL;
        s->ring_size = 0;
        if (ring) kfree(ring);
    }
    dev->valid = 0;
    spin_unlock(&dev->lock);

    uint64_t flags = spin_lock_irqsave(&audio_registry_lock);
    device_table[AUDIO_DEVICE][slot] = NULL;
    while (audio_device_slots > 0 && !device_table[AUDIO_DEVICE][audio_device_slots - 1]) audio_device_slots--;
    bool was_default = (audio_default_device == slot);
    if (was_default) audio_default_device = audio_pick_default(slot);
    int new_default = audio_default_device;
    spin_unlock_irqrestore(&audio_registry_lock, flags);

    LOG_INFO("audio%d: \"%s\" removed%s", slot, dev->name, was_default ? ", default device changed" : "");
    if (was_default && new_default >= 0) LOG_INFO("audio%d is now the default device", new_default);
}

int audio_core_register_backend(const struct audio_backend *backend) {
    if (!backend) return AUDIO_ERR_PARAM;
    uint64_t flags = spin_lock_irqsave(&audio_registry_lock);
    for (int i = 0; i < audio_backend_count; i++) {
        if (audio_backends[i] == backend) {
            spin_unlock_irqrestore(&audio_registry_lock, flags);
            return i;
        }
    }
    if (audio_backend_count >= AUDIO_MAX_BACKENDS) {
        spin_unlock_irqrestore(&audio_registry_lock, flags);
        LOG_ERROR("backend table full, '%s' dropped", backend->name ? backend->name : "?");
        return AUDIO_ERR_BUSY;
    }
    int idx = audio_backend_count;
    audio_backends[audio_backend_count++] = backend;
    spin_unlock_irqrestore(&audio_registry_lock, flags);
    LOG_DEBUG("backend '%s' registered", backend->name ? backend->name : "?");
    return idx;
}

int audio_core_backend_count(void) {
    return audio_backend_count;
}

const struct audio_backend *audio_core_get_backend(int idx) {
    if (idx < 0 || idx >= audio_backend_count) return NULL;
    return audio_backends[idx];
}

static struct audio_device *core_dev(int idx) {
    if (idx < 0) idx = audio_default_device;
    return audio_get_device(idx);
}

static int core_open(int dev_idx, uint8_t direction, const struct audio_format *fmt) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT || !dev->has_direction[direction]) return AUDIO_ERR_UNSUPPORTED;

    struct audio_format want;
    if (fmt) want = *fmt;
    else {
        want.sample_rate = AUDIO_DEFAULT_RATE;
        want.channels = AUDIO_DEFAULT_CHANNELS;
        want.format = AUDIO_DEFAULT_FORMAT;
        want.bits_per_sample = 16;
    }
    if (want.format >= AUDIO_FMT_COUNT || want.channels == 0 || want.channels > AUDIO_MAX_CHANNELS ||
        want.sample_rate < AUDIO_RATE_MIN || want.sample_rate > AUDIO_RATE_MAX)
        return AUDIO_ERR_PARAM;

    struct audio_format hw;
    int r = audio_negotiate_format(dev, direction, &want, &hw);
    if (r != AUDIO_OK) return r;

    uint32_t fb = audio_frame_bytes(&want);
    uint32_t ring_size = AUDIO_RING_SIZE - (AUDIO_RING_SIZE % fb);
    uint8_t *ring = (uint8_t *)kmalloc(ring_size);
    if (!ring) return AUDIO_ERR_NOMEM;
    memset(ring, 0, ring_size);

    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[direction];
    if (s->opened || !dev->valid) {
        spin_unlock(&dev->lock);
        kfree(ring);
        return dev->valid ? AUDIO_ERR_BUSY : AUDIO_ERR_NODEV;
    }

    s->direction = direction;
    s->state = AUDIO_STATE_STOPPED;
    s->fmt = want;
    s->hw_fmt = hw;
    s->ring = ring;
    s->ring_size = ring_size;
    s->read_pos = 0;
    s->write_pos = 0;
    s->period_bytes = AUDIO_PERIOD_SIZE - (AUDIO_PERIOD_SIZE % fb);
    s->frames_done = 0;
    s->underruns = 0;
    s->overruns = 0;
    s->hw_bytes = 0;
    s->silence_bytes = 0;
    s->draining = 0;
    s->hw_buffer_bytes = 0;
    s->sw_volume = 0;
    s->sw_mute = 0;
    audio_stream_reset_converter(s);

    r = dev->ops->open ? dev->ops->open(dev, direction, &s->hw_fmt) : AUDIO_ERR_UNSUPPORTED;
    if (r != AUDIO_OK) {
        s->ring = NULL;
        s->ring_size = 0;
        spin_unlock(&dev->lock);
        kfree(ring);
        LOG_WARNING("audio%d: %s open failed (%d)", dev->index, direction ? "capture" : "playback", r);
        return r;
    }
    s->opened = 1;
    audio_stream_reset_converter(s);

    if (s->force_sw_volume || !dev->ops->set_volume ||
        dev->ops->set_volume(dev, direction, s->volume) != AUDIO_OK)
        s->sw_volume = 1;
    if (!dev->ops->set_mute || dev->ops->set_mute(dev, direction, s->muted != 0) != AUDIO_OK)
        s->sw_mute = 1;
    spin_unlock(&dev->lock);

    LOG_DEBUG("audio%d: %s open %u Hz %u ch %s -> hw %u Hz %u ch %s%s", dev->index,
              direction ? "capture" : "playback", want.sample_rate, (unsigned)want.channels,
              audio_format_name(want.format), hw.sample_rate, (unsigned)hw.channels,
              audio_format_name(hw.format), s->sw_volume ? " (soft volume)" : "");
    return AUDIO_OK;
}

static int core_close(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    audio_stream_release(dev, direction);
    spin_unlock(&dev->lock);
    return AUDIO_OK;
}

static int core_write(int dev_idx, const void *data, uint32_t len) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (!data) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[AUDIO_DIR_PLAYBACK];
    if (!s->opened || !s->ring) {
        spin_unlock(&dev->lock);
        return AUDIO_ERR_PARAM;
    }
    s->draining = 0;
    uint32_t n = ring_write(s, (const uint8_t *)data, len);
    spin_unlock(&dev->lock);
    return (int)n;
}

static int core_read(int dev_idx, void *data, uint32_t len) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (!data) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[AUDIO_DIR_CAPTURE];
    if (!s->opened || !s->ring) {
        spin_unlock(&dev->lock);
        return AUDIO_ERR_PARAM;
    }
    uint32_t fb = audio_frame_bytes(&s->fmt);
    uint32_t fill = audio_stream_fill(s);
    if (len > fill) len = fill;
    if (fb) len -= len % fb;
    uint32_t n = ring_read(s, (uint8_t *)data, len);
    spin_unlock(&dev->lock);
    return (int)n;
}

static int core_start(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[direction];
    if (!s->opened) {
        spin_unlock(&dev->lock);
        return AUDIO_ERR_PARAM;
    }
    if (s->state == AUDIO_STATE_PLAYING) {
        spin_unlock(&dev->lock);
        return AUDIO_OK;
    }
    uint8_t prev = s->state;
    s->state = AUDIO_STATE_PLAYING;
    int r = AUDIO_OK;
    if (prev == AUDIO_STATE_PAUSED && dev->ops->pause) r = dev->ops->pause(dev, direction, false);
    else if (dev->ops->start) r = dev->ops->start(dev, direction);
    if (r != AUDIO_OK) s->state = prev;
    spin_unlock(&dev->lock);
    return r;
}

static int core_stop(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[direction];
    if (!s->opened) {
        spin_unlock(&dev->lock);
        return AUDIO_ERR_PARAM;
    }
    int r = AUDIO_OK;
    if (s->state != AUDIO_STATE_STOPPED && dev->ops->stop) r = dev->ops->stop(dev, direction);
    s->state = AUDIO_STATE_STOPPED;
    s->draining = 0;
    s->read_pos = 0;
    s->write_pos = 0;
    audio_stream_reset_converter(s);
    spin_unlock(&dev->lock);
    return r;
}

static int core_pause(int dev_idx, uint8_t direction, bool paused) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[direction];
    if (!s->opened) {
        spin_unlock(&dev->lock);
        return AUDIO_ERR_PARAM;
    }
    int r = AUDIO_OK;
    if (paused && s->state == AUDIO_STATE_PLAYING) {
        if (dev->ops->pause) r = dev->ops->pause(dev, direction, true);
        else if (dev->ops->stop) r = dev->ops->stop(dev, direction);
        if (r == AUDIO_OK) s->state = AUDIO_STATE_PAUSED;
    } else if (!paused && s->state == AUDIO_STATE_PAUSED) {
        if (dev->ops->pause) r = dev->ops->pause(dev, direction, false);
        else if (dev->ops->start) r = dev->ops->start(dev, direction);
        if (r == AUDIO_OK) s->state = AUDIO_STATE_PLAYING;
    }
    spin_unlock(&dev->lock);
    return r;
}

static uint32_t core_available(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev || direction >= AUDIO_DIR_COUNT) return 0;
    struct audio_stream *s = &dev->streams[direction];
    if (!s->opened) return 0;
    return (direction == AUDIO_DIR_PLAYBACK) ? audio_stream_space(s) : audio_stream_fill(s);
}

static int core_set_volume(int dev_idx, uint8_t direction, uint8_t volume) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    if (volume > AUDIO_VOLUME_MAX) volume = AUDIO_VOLUME_MAX;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[direction];
    s->volume = volume;
    int r = AUDIO_OK;
    if (s->force_sw_volume || !dev->ops->set_volume) {
        s->sw_volume = 1;
    } else {
        r = dev->ops->set_volume(dev, direction, volume);
        s->sw_volume = (r != AUDIO_OK);
        r = AUDIO_OK;
    }
    spin_unlock(&dev->lock);
    return r;
}

static int core_get_volume(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    return dev->streams[direction].volume;
}

static int core_set_mute(int dev_idx, uint8_t direction, bool muted) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    spin_lock(&dev->lock);
    struct audio_stream *s = &dev->streams[direction];
    s->muted = muted ? 1 : 0;
    if (!dev->ops->set_mute || dev->ops->set_mute(dev, direction, muted) != AUDIO_OK) s->sw_mute = 1;
    else s->sw_mute = 0;
    spin_unlock(&dev->lock);
    return AUDIO_OK;
}

static int core_get_mute(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    return dev->streams[direction].muted;
}

static int core_set_default_device(int dev_idx) {
    struct audio_device *dev = audio_get_device(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (!dev->has_direction[AUDIO_DIR_PLAYBACK] && !dev->has_direction[AUDIO_DIR_CAPTURE]) return AUDIO_ERR_UNSUPPORTED;
    audio_default_device = dev_idx;
    LOG_INFO("audio%d is now the default device", dev_idx);
    return AUDIO_OK;
}

static int core_get_default_device(void) {
    int d = audio_default_device;
    if (d >= 0 && !audio_get_device(d)) return -1;
    return d;
}

static int core_get_state(int dev_idx, uint8_t direction) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (direction >= AUDIO_DIR_COUNT) return AUDIO_ERR_PARAM;
    return dev->streams[direction].opened ? dev->streams[direction].state : AUDIO_ERR_PARAM;
}

static void audio_wait_tick(void) {
    struct task *t = current_task();
    if (t) scheduler_sleep_ms(2);
    else audio_delay_ms(2);
}

static int core_write_blocking(int dev_idx, const void *data, uint32_t len, uint32_t timeout_ms) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    struct audio_stream *s = &dev->streams[AUDIO_DIR_PLAYBACK];
    if (!s->opened) return AUDIO_ERR_PARAM;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t done = 0;
    uint64_t start = audio_uptime_ms();
    uint64_t last_progress = start;

    while (done < len) {
        int n = core_write(dev->index, p + done, len - done);
        if (n < 0) return done ? (int)done : n;
        if (n > 0) {
            done += (uint32_t)n;
            last_progress = audio_uptime_ms();
        }
        if (s->state == AUDIO_STATE_STOPPED && audio_stream_fill(s) * 2 >= s->ring_size)
            core_start(dev->index, AUDIO_DIR_PLAYBACK);
        if (done >= len) break;
        if (!dev->valid) return (int)done;
        if (timeout_ms && audio_uptime_ms() - last_progress > timeout_ms) {
            LOG_WARNING("audio%d: write stalled for %u ms (state %s)", dev->index, (unsigned)timeout_ms,
                        audio_state_name(s->state));
            break;
        }
        audio_wait_tick();
    }
    return (int)done;
}

static int core_read_blocking(int dev_idx, void *data, uint32_t len, uint32_t timeout_ms) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    struct audio_stream *s = &dev->streams[AUDIO_DIR_CAPTURE];
    if (!s->opened) return AUDIO_ERR_PARAM;
    if (s->state == AUDIO_STATE_STOPPED) core_start(dev->index, AUDIO_DIR_CAPTURE);

    uint8_t *p = (uint8_t *)data;
    uint32_t done = 0;
    uint64_t last_progress = audio_uptime_ms();
    while (done < len) {
        int n = core_read(dev->index, p + done, len - done);
        if (n < 0) return done ? (int)done : n;
        if (n > 0) {
            done += (uint32_t)n;
            last_progress = audio_uptime_ms();
        }
        if (done >= len) break;
        if (!dev->valid) break;
        if (timeout_ms && audio_uptime_ms() - last_progress > timeout_ms) break;
        audio_wait_tick();
    }
    return (int)done;
}

static int core_drain(int dev_idx, uint32_t timeout_ms) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    struct audio_stream *s = &dev->streams[AUDIO_DIR_PLAYBACK];
    if (!s->opened) return AUDIO_ERR_PARAM;

    if (s->state == AUDIO_STATE_STOPPED && audio_stream_fill(s) > 0) core_start(dev->index, AUDIO_DIR_PLAYBACK);
    s->draining = 1;

    uint64_t start = audio_uptime_ms();
    uint32_t src_fb = audio_frame_bytes(&s->fmt);
    while (dev->valid && s->opened && s->state == AUDIO_STATE_PLAYING && audio_stream_fill(s) >= src_fb) {
        if (timeout_ms && audio_uptime_ms() - start > timeout_ms) return AUDIO_ERR_TIMEOUT;
        audio_wait_tick();
    }

    uint32_t hw_fb = audio_frame_bytes(&s->hw_fmt);
    uint32_t bytes_per_ms = (hw_fb && s->hw_fmt.sample_rate) ? (hw_fb * s->hw_fmt.sample_rate) / 1000 : 0;
    uint32_t tail_ms = bytes_per_ms ? s->hw_buffer_bytes / bytes_per_ms + 10 : 20;
    uint64_t until = audio_uptime_ms() + tail_ms;
    while (dev->valid && audio_uptime_ms() < until) audio_wait_tick();

    if (dev->valid) core_stop(dev->index, AUDIO_DIR_PLAYBACK);
    return AUDIO_OK;
}

void audio_core_poll(void) {
    for (int i = 0; i < audio_device_slots; i++) {
        struct audio_device *dev = audio_get_device(i);
        if (!dev || !dev->ops || !dev->ops->poll) continue;
        if (!spin_trylock(&dev->lock)) continue;
        if (dev->valid) dev->ops->poll(dev);
        spin_unlock(&dev->lock);
    }

    for (int b = 0; b < audio_backend_count; b++) {
        const struct audio_backend *be = audio_backends[b];
        if (be && be->poll) be->poll();
    }
}

static void core_scan_all(void) {
    for (int b = 0; b < audio_backend_count; b++) {
        const struct audio_backend *be = audio_backends[b];
        if (be && be->scan) be->scan();
    }
}

int audio_generate_tone(int dev_idx, uint32_t freq_hz, uint32_t duration_ms, uint8_t amplitude) {
    struct audio_device *dev = core_dev(dev_idx);
    if (!dev) return AUDIO_ERR_NODEV;
    if (freq_hz == 0 || freq_hz > 20000) return AUDIO_ERR_PARAM;
    if (amplitude > 100) amplitude = 100;

    struct audio_format fmt = {
        .sample_rate = 48000,
        .channels = 2,
        .bits_per_sample = 16,
        .format = AUDIO_FMT_S16_LE,
    };
    int r = core_open(dev->index, AUDIO_DIR_PLAYBACK, &fmt);
    if (r != AUDIO_OK) return r;

    static int16_t tone_buf[1024 * 2];
    uint32_t phase = 0;
    uint32_t phase_inc = (uint32_t)(((uint64_t)freq_hz << 32) / fmt.sample_rate);
    uint32_t total = (uint32_t)(((uint64_t)fmt.sample_rate * duration_ms) / 1000);
    uint32_t fade = fmt.sample_rate / 200;
    int32_t amp = (int32_t)((32767 * (uint32_t)amplitude) / 100);
    uint32_t pos = 0;

    while (pos < total && dev->valid) {
        uint32_t n = total - pos;
        if (n > 1024) n = 1024;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t f = pos + i;
            int32_t env = amp;
            if (f < fade) env = (int32_t)(((int64_t)amp * f) / fade);
            else if (total - f < fade) env = (int32_t)(((int64_t)amp * (total - f)) / fade);
            int32_t v = (int32_t)(((int64_t)audio_sine_table[phase >> 24] * env) >> 15);
            tone_buf[i * 2] = (int16_t)v;
            tone_buf[i * 2 + 1] = (int16_t)v;
            phase += phase_inc;
        }
        int w = core_write_blocking(dev->index, tone_buf, n * 4, 2000);
        if (w < (int)(n * 4)) {
            r = AUDIO_ERR_TIMEOUT;
            break;
        }
        pos += n;
    }

    if (r == AUDIO_OK) r = core_drain(dev->index, duration_ms + 2000);
    core_close(dev->index, AUDIO_DIR_PLAYBACK);
    return r;
}

static struct audio_core_driver audio_core = {
    .scan_all = core_scan_all,
    .open = core_open,
    .close = core_close,
    .write = core_write,
    .read = core_read,
    .start = core_start,
    .stop = core_stop,
    .pause = core_pause,
    .available = core_available,
    .set_volume = core_set_volume,
    .get_volume = core_get_volume,
    .set_mute = core_set_mute,
    .get_mute = core_get_mute,
    .set_default_device = core_set_default_device,
    .get_default_device = core_get_default_device,
    .poll = audio_core_poll,
    .write_blocking = core_write_blocking,
    .read_blocking = core_read_blocking,
    .drain = core_drain,
    .get_state = core_get_state,
};

struct audio_core_driver *return_audio_core_driver(void) {
    if (audio_core_ready) return &audio_core;
    audio_get_tsc();
    audio_quirks_init();
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++) device_table[AUDIO_DEVICE][i] = NULL;
    audio_device_slots = 0;
    audio_default_device = -1;
    audio_core_ready = true;
    LOG_INFO("audio core ready");
    return &audio_core;
}

struct driver *return_meta_audio_core_driver(void) {
    static struct dependency deps[] = {
        MAKE_DEPENDENCY(TIMER_DRIVER, TSC_TIMER),
    };
    static struct driver meta = {
        .name = "Audio Core Driver",
        .type = AUDIO_DRIVER,
        .sub_type = AUDIO_CORE_SLOT,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = { &deps[0] },
        .dependency_count = 1,
        .self = &audio_core,
        .init = (void *)return_audio_core_driver,
    };
    return &meta;
}
