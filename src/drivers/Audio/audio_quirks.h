#ifndef AUDIO_QUIRKS_H
#define AUDIO_QUIRKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct audio_device;

enum audio_quirk_domain {
    AUDIO_QUIRK_HDA_CONTROLLER = 0,
    AUDIO_QUIRK_HDA_CODEC = 1,
    AUDIO_QUIRK_AC97_CONTROLLER = 2,
    AUDIO_QUIRK_AC97_CODEC = 3,
    AUDIO_QUIRK_USB_AUDIO = 4,
};

#define AUDIO_QUIRK_DOMAIN_COUNT 5

#define AUDIO_ID(vendor, device) ((((uint32_t)(vendor)) << 16) | ((uint32_t)(device) & 0xFFFFu))
#define AUDIO_ID_ANY 0x00000000u
#define AUDIO_ID_ALL 0xFFFFFFFFu
#define AUDIO_ID_VENDOR(vendor) AUDIO_ID((vendor), 0xFFFFu)
#define AUDIO_ID_DEVICE(device) AUDIO_ID(0xFFFFu, (device))

#define AUDIO_HDAC_Q_64BIT (1ull << 0)
#define AUDIO_HDAC_Q_DMAPOS (1ull << 1)
#define AUDIO_HDAC_Q_MSI (1ull << 2)
#define AUDIO_HDAC_Q_SINGLE_CMD (1ull << 3)
#define AUDIO_HDAC_Q_INTEL_TCSEL (1ull << 4)
#define AUDIO_HDAC_Q_SNOOP_ATI (1ull << 5)
#define AUDIO_HDAC_Q_SNOOP_NVIDIA (1ull << 6)
#define AUDIO_HDAC_Q_SNOOP_INTEL_SCH (1ull << 7)
#define AUDIO_HDAC_Q_CORB_RP_NO_ACK (1ull << 8)
#define AUDIO_HDAC_Q_RIRB_DELAY (1ull << 9)
#define AUDIO_HDAC_Q_LONG_RESET (1ull << 10)
#define AUDIO_HDAC_Q_SKIP (1ull << 11)
#define AUDIO_HDAC_Q_NO_STREAM_RESET_WAIT (1ull << 12)
#define AUDIO_HDAC_Q_FLAG_COUNT 13

#define AUDIO_HDA_Q_GPIO0 (1ull << 0)
#define AUDIO_HDA_Q_GPIO1 (1ull << 1)
#define AUDIO_HDA_Q_GPIO2 (1ull << 2)
#define AUDIO_HDA_Q_GPIO3 (1ull << 3)
#define AUDIO_HDA_Q_GPIO4 (1ull << 4)
#define AUDIO_HDA_Q_GPIO5 (1ull << 5)
#define AUDIO_HDA_Q_GPIO6 (1ull << 6)
#define AUDIO_HDA_Q_GPIO7 (1ull << 7)
#define AUDIO_HDA_Q_GPIOFLUSH (1ull << 8)
#define AUDIO_HDA_Q_SOFTPCMVOL (1ull << 9)
#define AUDIO_HDA_Q_FIXEDRATE (1ull << 10)
#define AUDIO_HDA_Q_FORCESTEREO (1ull << 11)
#define AUDIO_HDA_Q_EAPDINV (1ull << 12)
#define AUDIO_HDA_Q_SENSEINV (1ull << 14)
#define AUDIO_HDA_Q_IVREF50 (1ull << 16)
#define AUDIO_HDA_Q_IVREF80 (1ull << 17)
#define AUDIO_HDA_Q_IVREF100 (1ull << 18)
#define AUDIO_HDA_Q_OVREF50 (1ull << 19)
#define AUDIO_HDA_Q_OVREF80 (1ull << 20)
#define AUDIO_HDA_Q_OVREF100 (1ull << 21)
#define AUDIO_HDA_Q_IVREF (AUDIO_HDA_Q_IVREF50 | AUDIO_HDA_Q_IVREF80 | AUDIO_HDA_Q_IVREF100)
#define AUDIO_HDA_Q_OVREF (AUDIO_HDA_Q_OVREF50 | AUDIO_HDA_Q_OVREF80 | AUDIO_HDA_Q_OVREF100)
#define AUDIO_HDA_Q_VREF (AUDIO_HDA_Q_IVREF | AUDIO_HDA_Q_OVREF)
#define AUDIO_HDA_Q_GPIO_MASK 0xFFull
#define AUDIO_HDA_Q_NO_AUTOMUTE (1ull << 32)
#define AUDIO_HDA_Q_NO_EAPD (1ull << 33)
#define AUDIO_HDA_Q_FUNC_RESET (1ull << 34)
#define AUDIO_HDA_Q_IGNORE_PIN_CONFIG (1ull << 35)
#define AUDIO_HDA_Q_SPEAKER_ALWAYS_ON (1ull << 36)
#define AUDIO_HDA_Q_DIGITAL_OUT (1ull << 37)
#define AUDIO_HDA_Q_SKIP (1ull << 38)
#define AUDIO_HDA_Q_NO_CAPTURE (1ull << 39)
#define AUDIO_HDA_Q_EAPD_ALL_PINS (1ull << 40)
#define AUDIO_HDA_Q_AMP_PARAM_AFG (1ull << 41)
#define AUDIO_HDA_Q_FLAG_COUNT 42

#define AUDIO_AC97C_Q_IGNORE_PCR (1ull << 0)
#define AUDIO_AC97C_Q_IGNORE_RESET (1ull << 1)
#define AUDIO_AC97C_Q_SIS_REGS (1ull << 2)
#define AUDIO_AC97C_Q_ICH4_IOSE (1ull << 3)
#define AUDIO_AC97C_Q_PREFER_MMIO (1ull << 4)
#define AUDIO_AC97C_Q_FIXED_48K (1ull << 5)
#define AUDIO_AC97C_Q_SKIP (1ull << 6)
#define AUDIO_AC97C_Q_NO_SEMAPHORE (1ull << 7)
#define AUDIO_AC97C_Q_FLAG_COUNT 8

#define AUDIO_AC97_Q_EAPD_INV (1ull << 0)
#define AUDIO_AC97_Q_RDCD_BUG (1ull << 1)
#define AUDIO_AC97_Q_NO_VRA (1ull << 2)
#define AUDIO_AC97_Q_MASTER_5BIT (1ull << 3)
#define AUDIO_AC97_Q_NO_HEADPHONE (1ull << 4)
#define AUDIO_AC97_Q_NO_EAPD (1ull << 5)
#define AUDIO_AC97_Q_SWAP_LR (1ull << 6)
#define AUDIO_AC97_Q_SOFT_VOLUME (1ull << 7)
#define AUDIO_AC97_Q_MIC_BOOST (1ull << 8)
#define AUDIO_AC97_Q_FLAG_COUNT 9

#define AUDIO_USB_Q_NO_FRAC (1ull << 0)
#define AUDIO_USB_Q_INP_ASYNC (1ull << 1)
#define AUDIO_USB_Q_NO_XU (1ull << 2)
#define AUDIO_USB_Q_BAD_ADC (1ull << 3)
#define AUDIO_USB_Q_VENDOR_CLASS (1ull << 4)
#define AUDIO_USB_Q_SWAP_LR (1ull << 5)
#define AUDIO_USB_Q_NO_SET_RATE (1ull << 6)
#define AUDIO_USB_Q_NO_VOLUME (1ull << 7)
#define AUDIO_USB_Q_NO_MUTE (1ull << 8)
#define AUDIO_USB_Q_SKIP (1ull << 9)
#define AUDIO_USB_Q_SET_RATE_ALWAYS (1ull << 10)
#define AUDIO_USB_Q_VOLUME_LINEAR (1ull << 11)
#define AUDIO_USB_Q_NO_FEEDBACK (1ull << 12)
#define AUDIO_USB_Q_NO_CAPTURE (1ull << 13)
#define AUDIO_USB_Q_NO_PLAYBACK (1ull << 14)
#define AUDIO_USB_Q_FB_FULL_SPEED_FMT (1ull << 15)
#define AUDIO_USB_Q_CLOCK_NO_VALIDATE (1ull << 16)
#define AUDIO_USB_Q_FLAG_COUNT 17

#define AUDIO_HDA_PIN_CONN_JACK 0
#define AUDIO_HDA_PIN_CONN_NONE 1
#define AUDIO_HDA_PIN_CONN_FIXED 2
#define AUDIO_HDA_PIN_CONN_BOTH 3

#define AUDIO_HDA_PIN_CONFIG_NONE 0x411111F0u

#define AUDIO_HDA_PIN_CFG(conn, loc, dev, ctype, color, misc, as, seq) \
    ((((uint32_t)(conn) & 0x3u) << 30) | (((uint32_t)(loc) & 0x3Fu) << 24) | \
     (((uint32_t)(dev) & 0xFu) << 20) | (((uint32_t)(ctype) & 0xFu) << 16) | \
     (((uint32_t)(color) & 0xFu) << 12) | (((uint32_t)(misc) & 0xFu) << 8) | \
     (((uint32_t)(as) & 0xFu) << 4) | ((uint32_t)(seq) & 0xFu))

enum audio_pin_patch_type {
    AUDIO_PIN_PATCH_OVERRIDE = 1,
    AUDIO_PIN_PATCH_MASK = 2,
    AUDIO_PIN_PATCH_STRING = 3,
    AUDIO_PIN_PATCH_DISABLE = 4,
};

typedef struct audio_pin_patch {
    uint8_t nid;
    uint8_t type;
    uint32_t value;
    uint32_t mask;
    const char *str;
} audio_pin_patch;

typedef struct audio_verb {
    uint8_t nid;
    uint16_t verb;
    uint16_t param;
} audio_verb;

typedef struct audio_coef {
    uint8_t nid;
    uint16_t index;
    uint16_t mask;
    uint16_t value;
} audio_coef;

typedef struct audio_reg_patch {
    uint8_t reg;
    uint16_t mask;
    uint16_t value;
} audio_reg_patch;

#define AUDIO_PIN_OVERRIDE(_nid, _cfg) { .nid = (_nid), .type = AUDIO_PIN_PATCH_OVERRIDE, .value = (_cfg) }
#define AUDIO_PIN_MASK(_nid, _mask, _val) { .nid = (_nid), .type = AUDIO_PIN_PATCH_MASK, .value = (_val), .mask = (_mask) }
#define AUDIO_PIN_STRING(_nid, _str) { .nid = (_nid), .type = AUDIO_PIN_PATCH_STRING, .str = (_str) }
#define AUDIO_PIN_DISABLE(_nid) { .nid = (_nid), .type = AUDIO_PIN_PATCH_DISABLE }

#define AUDIO_PINS(...) ((const struct audio_pin_patch[]){ __VA_ARGS__, { 0 } })
#define AUDIO_VERBS(...) ((const struct audio_verb[]){ __VA_ARGS__, { 0 } })
#define AUDIO_COEFS(...) ((const struct audio_coef[]){ __VA_ARGS__, { 0 } })
#define AUDIO_REGS(...) ((const struct audio_reg_patch[]){ __VA_ARGS__, { 0 } })

enum audio_quirk_stage {
    AUDIO_QUIRK_STAGE_ATTACH = 0,
    AUDIO_QUIRK_STAGE_INIT = 1,
    AUDIO_QUIRK_STAGE_CONFIGURED = 2,
    AUDIO_QUIRK_STAGE_OPEN = 3,
    AUDIO_QUIRK_STAGE_CLOSE = 4,
};

struct audio_quirk;

typedef struct audio_quirk_ctx {
    enum audio_quirk_domain domain;
    enum audio_quirk_stage stage;
    struct audio_device *dev;
    void *backend;
    uint8_t codec_addr;
    uint8_t afg_nid;
    uint32_t (*verb)(void *backend, uint8_t nid, uint32_t verb_payload);
    uint16_t (*reg_read)(void *backend, uint8_t reg);
    void (*reg_write)(void *backend, uint8_t reg, uint16_t value);
    uint32_t *pin_config;
    uint8_t pin_first;
    uint8_t pin_count;
} audio_quirk_ctx;

typedef int (*audio_quirk_hook_t)(const struct audio_quirk *q, struct audio_quirk_ctx *ctx);

typedef struct audio_quirk_match {
    uint32_t pci_id;
    uint32_t subsys;
    uint32_t codec_id;
    uint32_t codec_mask;
    uint32_t codec_subsys;
    uint32_t usb_id;
    uint16_t rev_min;
    uint16_t rev_max;
} audio_quirk_match;

typedef struct audio_quirk {
    const char *name;
    const char *model;
    struct audio_quirk_match match;
    uint64_t set;
    uint64_t clear;
    uint32_t gpio_mask;
    uint32_t gpio_dir;
    uint32_t gpio_data;
    uint32_t fixed_rate;
    uint32_t codec_probe_mask;
    uint16_t delay_ms;
    uint8_t usb_iface;
    uint8_t usb_alt;
    const struct audio_pin_patch *pins;
    const struct audio_verb *verbs;
    const struct audio_coef *coefs;
    const struct audio_reg_patch *regs;
    audio_quirk_hook_t hook;
} audio_quirk;

typedef struct audio_quirk_table {
    const char *name;
    enum audio_quirk_domain domain;
    const struct audio_quirk *entries;
    uint32_t count;
} audio_quirk_table;

#define AUDIO_QUIRK_TABLE(_name, _domain, _array) \
    { .name = (_name), .domain = (_domain), .entries = (_array), .count = (uint32_t)(sizeof(_array) / sizeof((_array)[0])) }

typedef struct audio_quirk_ids {
    uint32_t pci_id;
    uint32_t subsys;
    uint32_t codec_id;
    uint32_t codec_subsys;
    uint32_t usb_id;
    uint16_t revision;
    uint8_t has_revision;
} audio_quirk_ids;

#define AUDIO_QUIRK_MAX_MATCHES 24
#define AUDIO_QUIRK_MAX_TABLES 32

typedef struct audio_quirk_set {
    enum audio_quirk_domain domain;
    uint64_t flags;
    uint32_t gpio_mask;
    uint32_t gpio_dir;
    uint32_t gpio_data;
    uint32_t fixed_rate;
    uint32_t codec_probe_mask;
    uint16_t delay_ms;
    int16_t usb_iface;
    int16_t usb_alt;
    const char *model;
    const struct audio_quirk *matches[AUDIO_QUIRK_MAX_MATCHES];
    uint8_t match_count;
} audio_quirk_set;

void audio_quirks_init(void);

int audio_quirk_register_table(const struct audio_quirk_table *table);
int audio_quirk_table_count(void);
const struct audio_quirk_table *audio_quirk_get_table(int idx);

bool audio_quirk_id_match(uint32_t pattern, uint32_t value);
bool audio_quirk_matches(const struct audio_quirk_match *m, const struct audio_quirk_ids *ids);

void audio_quirk_resolve(enum audio_quirk_domain domain, const struct audio_quirk_ids *ids,
                         uint64_t defaults, struct audio_quirk_set *out);

int audio_quirk_parse_pin_string(uint32_t config, const char *str, uint32_t *out);
uint32_t audio_quirk_pin_apply(uint32_t config, const struct audio_pin_patch *patch);
int audio_quirk_apply_pins(const struct audio_quirk_set *set, uint8_t nid, uint32_t *config);

int audio_quirk_run_verbs(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx);
int audio_quirk_run_coefs(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx);
int audio_quirk_run_regs(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx);
int audio_quirk_run_hooks(const struct audio_quirk_set *set, struct audio_quirk_ctx *ctx);

void audio_quirk_set_names(const struct audio_quirk_set *set, char *buf, size_t cap);
void audio_quirk_flags_string(enum audio_quirk_domain domain, uint64_t flags, char *buf, size_t cap);

const char *audio_quirk_domain_name(enum audio_quirk_domain domain);
const char *audio_quirk_flag_name(enum audio_quirk_domain domain, int bit);
int audio_quirk_flag_count(enum audio_quirk_domain domain);

const char *audio_hda_pin_device_name(uint8_t dev);
const char *audio_hda_pin_conn_name(uint8_t conn);
const char *audio_hda_pin_color_name(uint8_t color);
const char *audio_hda_pin_ctype_name(uint8_t ctype);
const char *audio_hda_pin_location_name(uint8_t loc);

extern const struct audio_quirk_table *const audio_builtin_quirk_tables[];
extern const uint32_t audio_builtin_quirk_table_count;

#endif
