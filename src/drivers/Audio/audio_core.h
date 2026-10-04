#ifndef AUDIO_CORE_H
#define AUDIO_CORE_H

#include "components/drivers.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

enum AUDIO_TYPE
{
    USB_AUDIO = 0,
    HDA_AUDIO = 1,
    AC97_AUDIO = 2,
};

#define AUDIO_TYPE_COUNT 3

#define AUDIO_CORE_SLOT 3
#define AUDIO_DEVICE_FIRST_SLOT 4
#define MAX_AUDIO_DEVICES (MAX_DRIVERS_PER_TYPE - AUDIO_DEVICE_FIRST_SLOT)

#define AUDIO_MAX_BACKENDS 8

#define AUDIO_DIR_PLAYBACK 0
#define AUDIO_DIR_CAPTURE 1
#define AUDIO_DIR_COUNT 2

#define AUDIO_STATE_STOPPED 0
#define AUDIO_STATE_PLAYING 1
#define AUDIO_STATE_PAUSED 2

#define AUDIO_FMT_U8 0x00
#define AUDIO_FMT_S16_LE 0x01
#define AUDIO_FMT_S24_LE 0x02
#define AUDIO_FMT_S32_LE 0x03
#define AUDIO_FMT_S24_3LE 0x04
#define AUDIO_FMT_COUNT 5

#define AUDIO_FMT_MASK(f) (1u << (f))

#define AUDIO_RATE_8000 (1u << 0)
#define AUDIO_RATE_11025 (1u << 1)
#define AUDIO_RATE_16000 (1u << 2)
#define AUDIO_RATE_22050 (1u << 3)
#define AUDIO_RATE_32000 (1u << 4)
#define AUDIO_RATE_44100 (1u << 5)
#define AUDIO_RATE_48000 (1u << 6)
#define AUDIO_RATE_88200 (1u << 7)
#define AUDIO_RATE_96000 (1u << 8)
#define AUDIO_RATE_176400 (1u << 9)
#define AUDIO_RATE_192000 (1u << 10)
#define AUDIO_RATE_COUNT 11

#define AUDIO_RATE_CONTINUOUS (1u << 31)

#define AUDIO_RATE_MIN 4000
#define AUDIO_RATE_MAX 192000

#define AUDIO_MAX_CHANNELS 8

#define AUDIO_VOLUME_MAX 100
#define AUDIO_VOLUME_DEFAULT 75

#define AUDIO_DEFAULT_RATE 48000
#define AUDIO_DEFAULT_CHANNELS 2
#define AUDIO_DEFAULT_FORMAT AUDIO_FMT_S16_LE

#define AUDIO_RING_SIZE (64 * 1024)
#define AUDIO_PERIOD_SIZE 4096

#define AUDIO_OK 0
#define AUDIO_ERR_TIMEOUT -1
#define AUDIO_ERR_IO -2
#define AUDIO_ERR_PARAM -3
#define AUDIO_ERR_BUSY -4
#define AUDIO_ERR_NODEV -5
#define AUDIO_ERR_UNSUPPORTED -6
#define AUDIO_ERR_NOMEM -7

#define PCI_SUBCLASS_AC97 0x01
#define PCI_SUBCLASS_HDA 0x03

#define USB_CLASS_AUDIO 0x01
#define USB_SUBCLASS_AUDIOCONTROL 0x01
#define USB_SUBCLASS_AUDIOSTREAMING 0x02

#define UAC_SET_CUR 0x01
#define UAC_GET_CUR 0x81
#define UAC_GET_MIN 0x82
#define UAC_GET_MAX 0x83
#define UAC_GET_RES 0x84

#define UAC_FU_MUTE_CONTROL 0x01
#define UAC_FU_VOLUME_CONTROL 0x02
#define UAC_EP_SAMPLING_FREQ_CONTROL 0x01

#define AUDIO_DEV_FLAG_HW_VOLUME (1u << 0)
#define AUDIO_DEV_FLAG_HW_MUTE (1u << 1)
#define AUDIO_DEV_FLAG_HOTPLUG (1u << 2)
#define AUDIO_DEV_FLAG_DIGITAL (1u << 3)
#define AUDIO_DEV_FLAG_REMOVED (1u << 4)

struct pci_device;
struct usb_device;
struct audio_device;
struct audio_quirk_set;

typedef struct audio_format {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t format;
} audio_format;

typedef struct audio_convert_state {
    uint32_t step;
    uint32_t frac;
    int32_t prev[AUDIO_MAX_CHANNELS];
    int32_t cur[AUDIO_MAX_CHANNELS];
    uint8_t primed;
} audio_convert_state;

typedef struct audio_stream {
    uint8_t direction;
    uint8_t state;
    struct audio_format fmt;

    uint8_t *ring;
    uint32_t ring_size;
    volatile uint32_t read_pos;
    volatile uint32_t write_pos;

    uint32_t period_bytes;

    uint64_t frames_done;
    uint32_t underruns;

    uint8_t volume;
    uint8_t muted;

    uint8_t opened;

    struct audio_format hw_fmt;
    struct audio_convert_state conv;

    uint8_t sw_volume;
    uint8_t sw_mute;
    uint8_t draining;
    uint8_t passthrough;
    uint8_t swap_lr;
    uint8_t force_sw_volume;

    uint32_t hw_buffer_bytes;
    uint32_t overruns;
    uint64_t hw_bytes;
    uint64_t silence_bytes;
} audio_stream;

typedef struct audio_device_ops {
    int (*open)(struct audio_device *dev, uint8_t direction,
                const struct audio_format *fmt);

    int (*close)(struct audio_device *dev, uint8_t direction);

    int (*start)(struct audio_device *dev, uint8_t direction);

    int (*stop)(struct audio_device *dev, uint8_t direction);

    int (*pause)(struct audio_device *dev, uint8_t direction, bool paused);

    uint32_t (*get_position)(struct audio_device *dev, uint8_t direction);

    int (*set_volume)(struct audio_device *dev, uint8_t direction, uint8_t volume);

    int (*set_mute)(struct audio_device *dev, uint8_t direction, bool muted);

    void (*poll)(struct audio_device *dev);

    void (*describe)(struct audio_device *dev, void (*out)(const char *line));

    void (*remove)(struct audio_device *dev);
} audio_device_ops;

typedef struct audio_device {
    enum AUDIO_TYPE type;
    char name[32];

    struct pci_device *pci;
    struct usb_device *usb;

    const struct audio_device_ops *ops;
    void *priv;

    uint32_t rates[AUDIO_DIR_COUNT];
    uint16_t formats[AUDIO_DIR_COUNT];
    uint8_t max_channels[AUDIO_DIR_COUNT];
    uint8_t has_direction[AUDIO_DIR_COUNT];

    struct audio_stream streams[AUDIO_DIR_COUNT];

    uint8_t valid;

    int index;
    uint32_t flags;
    char description[64];
    char codec_name[48];
    uint32_t vendor_id;
    uint32_t subsystem_id;
    uint32_t codec_id;
    uint64_t quirks;
    char quirk_names[96];

    uint32_t fixed_rate;
    uint32_t rate_min[AUDIO_DIR_COUNT];
    uint32_t rate_max[AUDIO_DIR_COUNT];

    spinlock_t lock;
} audio_device;

typedef struct audio_backend {
    const char *name;
    enum AUDIO_TYPE type;
    void (*scan)(void);
    void (*poll)(void);
} audio_backend;

void audio_core_poll(void);

typedef struct audio_core_driver {
    void (*scan_all)(void);

    int (*open)(int dev_idx, uint8_t direction,
                const struct audio_format *fmt);

    int (*close)(int dev_idx, uint8_t direction);

    int (*write)(int dev_idx, const void *data, uint32_t len);

    int (*read)(int dev_idx, void *data, uint32_t len);

    int (*start)(int dev_idx, uint8_t direction);

    int (*stop)(int dev_idx, uint8_t direction);

    int (*pause)(int dev_idx, uint8_t direction, bool paused);

    uint32_t (*available)(int dev_idx, uint8_t direction);

    int (*set_volume)(int dev_idx, uint8_t direction, uint8_t volume);
    int (*get_volume)(int dev_idx, uint8_t direction);

    int (*set_mute)(int dev_idx, uint8_t direction, bool muted);
    int (*get_mute)(int dev_idx, uint8_t direction);

    int (*set_default_device)(int dev_idx);
    int (*get_default_device)(void);

    void (*poll)(void);

    int (*write_blocking)(int dev_idx, const void *data, uint32_t len, uint32_t timeout_ms);
    int (*read_blocking)(int dev_idx, void *data, uint32_t len, uint32_t timeout_ms);
    int (*drain)(int dev_idx, uint32_t timeout_ms);
    int (*get_state)(int dev_idx, uint8_t direction);
} audio_core_driver;

struct audio_core_driver *return_audio_core_driver(void);
struct driver *return_meta_audio_core_driver(void);

int audio_get_device_count(void);
struct audio_device *audio_get_device(int idx);

int audio_core_add_device(struct audio_device *dev);
void audio_core_remove_device(struct audio_device *dev);

int audio_core_register_backend(const struct audio_backend *backend);
int audio_core_backend_count(void);
const struct audio_backend *audio_core_get_backend(int idx);

uint32_t audio_frame_bytes(const struct audio_format *fmt);
uint32_t audio_rate_to_mask(uint32_t sample_rate);
uint32_t audio_mask_to_rate(uint32_t mask_bit);
uint32_t audio_sample_bytes(uint8_t format);
const char *audio_format_name(uint8_t format);
const char *audio_type_name(enum AUDIO_TYPE type);
const char *audio_state_name(uint8_t state);

bool audio_device_supports_rate(const struct audio_device *dev, uint8_t direction, uint32_t rate);
int audio_negotiate_format(const struct audio_device *dev, uint8_t direction,
                           const struct audio_format *want, struct audio_format *out);

uint32_t audio_stream_fill(const struct audio_stream *s);
uint32_t audio_stream_space(const struct audio_stream *s);

uint32_t audio_stream_render(struct audio_device *dev, struct audio_stream *s, uint8_t *dst, uint32_t dst_bytes);
uint32_t audio_stream_capture(struct audio_device *dev, struct audio_stream *s, const uint8_t *src, uint32_t src_bytes);

void audio_stream_reset_converter(struct audio_stream *s);
uint32_t audio_volume_gain_q15(uint8_t volume);

void *audio_dma_alloc(uint32_t size, bool below_4g, uint64_t *phys_out, uint32_t *pages_out);
void audio_dma_free(void *virt, uint64_t phys, uint32_t pages);

void audio_delay_us(uint64_t us);
void audio_delay_ms(uint64_t ms);
uint64_t audio_uptime_ms(void);

int audio_generate_tone(int dev_idx, uint32_t freq_hz, uint32_t duration_ms, uint8_t amplitude);

#endif
