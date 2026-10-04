#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include "drivers/Audio/audio_core.h"
#include "drivers/Audio/audio_quirks.h"
#include "drivers/USB/usb_core.h"

#include <stdbool.h>
#include <stdint.h>

#define UA_MAX_DEVICES 4
#define UA_MAX_ALTS 16
#define UA_MAX_RATES 16
#define UA_MAX_UNITS 48
#define UA_MAX_INPUTS 8
#define UA_REQS 6
#define UA_REQ_MS 8
#define UA_REQ_BYTES 16384
#define UA_FB_BYTES 64
#define UA_FB_PKTS 8
#define UA_CFG_MAX USB_CONFIG_MAX
#define UA_CTL_MAX 512

#define UA_REQ_SET_INTERFACE 0x0B

#define UA_CS_INTERFACE 0x24
#define UA_CS_ENDPOINT 0x25

#define UA_AC_HEADER 0x01
#define UA_AC_INPUT_TERMINAL 0x02
#define UA_AC_OUTPUT_TERMINAL 0x03
#define UA_AC_MIXER_UNIT 0x04
#define UA_AC_SELECTOR_UNIT 0x05
#define UA_AC_FEATURE_UNIT 0x06
#define UA_AC_PROCESSING_UNIT 0x07
#define UA_AC_EXTENSION_UNIT 0x08

#define UA2_AC_EFFECT_UNIT 0x07
#define UA2_AC_PROCESSING_UNIT 0x08
#define UA2_AC_EXTENSION_UNIT 0x09
#define UA2_AC_CLOCK_SOURCE 0x0A
#define UA2_AC_CLOCK_SELECTOR 0x0B
#define UA2_AC_CLOCK_MULTIPLIER 0x0C
#define UA2_AC_SAMPLE_RATE_CONVERTER 0x0D

#define UA_AS_GENERAL 0x01
#define UA_AS_FORMAT_TYPE 0x02

#define UA_TERMINAL_USB_STREAMING 0x0101

#define UA_FORMAT_PCM 0x0001
#define UA_FORMAT_PCM8 0x0002

#define UA2_CUR 0x01
#define UA2_RANGE 0x02
#define UA2_CS_SAM_FREQ_CONTROL 0x01
#define UA2_CS_CLOCK_VALID_CONTROL 0x02
#define UA2_CX_CLOCK_SELECTOR_CONTROL 0x01

#define UA_EP_SYNC_NONE 0
#define UA_EP_SYNC_ASYNC 1
#define UA_EP_SYNC_ADAPTIVE 2
#define UA_EP_SYNC_SYNC 3

enum ua_unit_kind {
    UA_U_NONE = 0,
    UA_U_IT,
    UA_U_OT,
    UA_U_MIXER,
    UA_U_SELECTOR,
    UA_U_FEATURE,
    UA_U_PROCESSING,
    UA_U_EXTENSION,
    UA_U_EFFECT,
    UA_U_CLK_SRC,
    UA_U_CLK_SEL,
    UA_U_CLK_MUL,
    UA_U_SRC,
};

typedef struct ua_alt {
    uint8_t iface;
    uint8_t alt;
    uint8_t ep_addr;
    uint8_t ep_attr;
    uint16_t ep_mps;
    uint8_t ep_interval;
    uint8_t ep_refresh;
    uint8_t sync_ep;
    uint8_t fb_addr;
    uint8_t fb_attr;
    uint16_t fb_mps;
    uint8_t fb_interval;
    uint8_t freq_ctl;
    uint8_t terminal_link;
    uint8_t format_type;
    uint16_t format_tag;
    uint32_t formats;
    uint8_t channels;
    uint8_t subframe;
    uint8_t bits;
    uint8_t continuous;
    uint8_t rate_count;
    uint32_t rates[UA_MAX_RATES];
    uint32_t rate_min;
    uint32_t rate_max;
    uint8_t clock_id;
    uint8_t direction;
    uint8_t valid;
} ua_alt;

typedef struct ua_unit {
    uint8_t id;
    uint8_t kind;
    uint8_t source;
    uint8_t nr_inputs;
    uint8_t inputs[UA_MAX_INPUTS];
    uint8_t clock;
    uint16_t terminal_type;
    uint8_t mute_master;
    uint8_t vol_master;
    uint8_t mute_ch;
    uint8_t vol_ch;
    uint8_t clk_attr;
    uint8_t clk_ctrl;
} ua_unit;

typedef struct ua_fu {
    uint8_t id;
    uint8_t channel;
    uint8_t has_volume;
    uint8_t has_mute;
    uint8_t channels;
    int16_t vol_min;
    int16_t vol_max;
    int16_t vol_res;
} ua_fu;

typedef struct ua_stream {
    int8_t cur_alt;
    uint8_t opened;
    uint8_t active;
    uint8_t paused;
    uint8_t fb_opened;
    uint8_t fb_inflight;
    uint8_t next_sub;
    uint8_t next_done;
    uint8_t inflight[UA_REQS];
    uint8_t prepared[UA_REQS];
    uint8_t interval_uf;
    uint16_t pkt_max;
    uint16_t pkts_per_req;
    uint32_t rate;
    uint32_t frame_bytes;
    uint32_t pps;
    uint32_t acc;
    uint32_t nominal_q16;
    uint32_t fb_q16;
    uint64_t submit_ms[UA_REQS];
    uint64_t last_progress_ms;
    struct usb_iso_request req[UA_REQS];
    uint8_t *buf[UA_REQS];
    uint64_t buf_phys[UA_REQS];
    uint32_t buf_pages[UA_REQS];
    struct usb_iso_request fb_req;
    uint8_t *fb_buf;
    uint64_t fb_phys;
    uint32_t fb_pages;
    uint32_t submits;
    uint32_t completed;
    uint32_t errors;
    uint32_t busy;
    uint32_t restarts;
    uint32_t fb_updates;
    uint32_t consecutive_errors;
    uint64_t bytes;
} ua_stream;

typedef struct ua_device {
    struct audio_device adev;
    struct usb_device *usb;
    struct usb_controller *ctrl;
    struct usb_function_info *fn;
    int8_t fn_index;
    uint8_t address;
    uint32_t generation;
    uint16_t vid;
    uint16_t pid;
    uint8_t slot;
    uint8_t ac_iface;
    uint8_t uac_protocol;
    uint8_t uac2;
    uint8_t hs;
    uint32_t iface_mask;
    struct ua_alt alts[UA_MAX_ALTS];
    uint8_t alt_count;
    struct ua_unit units[UA_MAX_UNITS];
    uint8_t unit_count;
    struct ua_fu fu[AUDIO_DIR_COUNT];
    struct audio_quirk_set quirks;
    struct ua_stream st[AUDIO_DIR_COUNT];
    uint8_t gone;
} ua_device;

typedef struct usb_audio_driver {
    int (*get_device_count)(void);
    struct ua_device *(*get_device)(int idx);
} usb_audio_driver;

struct usb_audio_driver *return_usb_audio_driver(void);
struct driver *return_meta_usb_audio_driver(void);

#endif
