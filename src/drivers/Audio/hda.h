#ifndef HDA_H
#define HDA_H

#include "audio_core.h"
#include "audio_quirks.h"

#include "components/drivers.h"
#include "components/pci.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

#define HDA_MAX_CONTROLLERS 4
#define HDA_MAX_CODECS 15
#define HDA_MAX_DEVICES 8
#define HDA_MAX_CONNS 32
#define HDA_MAX_PATH 10
#define HDA_MAX_OUTPUTS 8
#define HDA_MAX_DACS 8
#define HDA_MMIO_SIZE 0x4000

#define HDA_REG_GCAP 0x00
#define HDA_REG_VMIN 0x02
#define HDA_REG_VMAJ 0x03
#define HDA_REG_OUTPAY 0x04
#define HDA_REG_INPAY 0x06
#define HDA_REG_GCTL 0x08
#define HDA_REG_WAKEEN 0x0C
#define HDA_REG_STATESTS 0x0E
#define HDA_REG_GSTS 0x10
#define HDA_REG_INTCTL 0x20
#define HDA_REG_INTSTS 0x24
#define HDA_REG_WALCLK 0x30
#define HDA_REG_SSYNC 0x38
#define HDA_REG_CORBLBASE 0x40
#define HDA_REG_CORBUBASE 0x44
#define HDA_REG_CORBWP 0x48
#define HDA_REG_CORBRP 0x4A
#define HDA_REG_CORBCTL 0x4C
#define HDA_REG_CORBSTS 0x4D
#define HDA_REG_CORBSIZE 0x4E
#define HDA_REG_RIRBLBASE 0x50
#define HDA_REG_RIRBUBASE 0x54
#define HDA_REG_RIRBWP 0x58
#define HDA_REG_RINTCNT 0x5A
#define HDA_REG_RIRBCTL 0x5C
#define HDA_REG_RIRBSTS 0x5D
#define HDA_REG_RIRBSIZE 0x5E
#define HDA_REG_ICOI 0x60
#define HDA_REG_ICII 0x64
#define HDA_REG_ICIS 0x68
#define HDA_REG_DPLBASE 0x70
#define HDA_REG_DPUBASE 0x74
#define HDA_REG_SD_BASE 0x80
#define HDA_SD_STRIDE 0x20

#define HDA_SD_CTL 0x00
#define HDA_SD_STS 0x03
#define HDA_SD_LPIB 0x04
#define HDA_SD_CBL 0x08
#define HDA_SD_LVI 0x0C
#define HDA_SD_FIFOD 0x10
#define HDA_SD_FMT 0x12
#define HDA_SD_BDPL 0x18
#define HDA_SD_BDPU 0x1C

#define HDA_GCTL_CRST (1u << 0)
#define HDA_GCTL_FCNTRL (1u << 1)
#define HDA_GCTL_UNSOL (1u << 8)

#define HDA_CORBRP_RST (1u << 15)
#define HDA_CORBCTL_RUN (1u << 1)
#define HDA_RIRBWP_RST (1u << 15)
#define HDA_RIRBCTL_RINTCTL (1u << 0)
#define HDA_RIRBCTL_DMAEN (1u << 1)
#define HDA_RIRBSTS_INTFL (1u << 0)
#define HDA_RIRBSTS_OIS (1u << 2)
#define HDA_ICIS_ICB (1u << 0)
#define HDA_ICIS_IRV (1u << 1)

#define HDA_SDCTL_SRST (1u << 0)
#define HDA_SDCTL_RUN (1u << 1)
#define HDA_SDCTL_IOCE (1u << 2)
#define HDA_SDCTL_FEIE (1u << 3)
#define HDA_SDCTL_DEIE (1u << 4)
#define HDA_SDCTL_STRM_SHIFT 20
#define HDA_SDCTL_DIR (1u << 19)
#define HDA_SDSTS_BCIS (1u << 2)
#define HDA_SDSTS_FIFOE (1u << 3)
#define HDA_SDSTS_DESE (1u << 4)
#define HDA_SDSTS_FIFORDY (1u << 5)

#define HDA_VERB_GET_PARAM 0xF00
#define HDA_VERB_GET_CONN_SEL 0xF01
#define HDA_VERB_SET_CONN_SEL 0x701
#define HDA_VERB_GET_CONN_LIST 0xF02
#define HDA_VERB_GET_PROC_STATE 0xF03
#define HDA_VERB_SET_PROC_STATE 0x703
#define HDA_VERB_SET_COEF_INDEX 0x500
#define HDA_VERB_GET_COEF_INDEX 0xD00
#define HDA_VERB_SET_PROC_COEF 0x400
#define HDA_VERB_GET_PROC_COEF 0xC00
#define HDA_VERB_GET_AMP 0xB00
#define HDA_VERB_SET_AMP 0x300
#define HDA_VERB_GET_CONV_FMT 0xA00
#define HDA_VERB_SET_CONV_FMT 0x200
#define HDA_VERB_GET_DIGI_CONV 0xF0D
#define HDA_VERB_SET_DIGI_CONV_1 0x70D
#define HDA_VERB_GET_POWER 0xF05
#define HDA_VERB_SET_POWER 0x705
#define HDA_VERB_GET_CONV_STREAM 0xF06
#define HDA_VERB_SET_CONV_STREAM 0x706
#define HDA_VERB_GET_PIN_CTL 0xF07
#define HDA_VERB_SET_PIN_CTL 0x707
#define HDA_VERB_GET_UNSOL 0xF08
#define HDA_VERB_SET_UNSOL 0x708
#define HDA_VERB_GET_PIN_SENSE 0xF09
#define HDA_VERB_SET_PIN_SENSE 0x709
#define HDA_VERB_GET_BEEP 0xF0A
#define HDA_VERB_SET_BEEP 0x70A
#define HDA_VERB_GET_EAPD 0xF0C
#define HDA_VERB_SET_EAPD 0x70C
#define HDA_VERB_GET_VOLUME_KNOB 0xF0F
#define HDA_VERB_SET_VOLUME_KNOB 0x70F
#define HDA_VERB_GET_GPIO_DATA 0xF15
#define HDA_VERB_SET_GPIO_DATA 0x715
#define HDA_VERB_GET_GPIO_MASK 0xF16
#define HDA_VERB_SET_GPIO_MASK 0x716
#define HDA_VERB_GET_GPIO_DIR 0xF17
#define HDA_VERB_SET_GPIO_DIR 0x717
#define HDA_VERB_GET_CONFIG 0xF1C
#define HDA_VERB_GET_SUBSYSTEM 0xF20
#define HDA_VERB_SET_CHAN_COUNT 0x72D
#define HDA_VERB_FUNC_RESET 0x7FF

#define HDA_PARAM_VENDOR_ID 0x00
#define HDA_PARAM_REVISION_ID 0x02
#define HDA_PARAM_SUBORDINATE 0x04
#define HDA_PARAM_FUNC_GROUP 0x05
#define HDA_PARAM_AFG_CAPS 0x08
#define HDA_PARAM_WIDGET_CAPS 0x09
#define HDA_PARAM_PCM 0x0A
#define HDA_PARAM_STREAM_FMTS 0x0B
#define HDA_PARAM_PIN_CAPS 0x0C
#define HDA_PARAM_AMP_IN_CAPS 0x0D
#define HDA_PARAM_CONN_LEN 0x0E
#define HDA_PARAM_POWER_STATES 0x0F
#define HDA_PARAM_PROC_CAPS 0x10
#define HDA_PARAM_GPIO_COUNT 0x11
#define HDA_PARAM_AMP_OUT_CAPS 0x12
#define HDA_PARAM_VOL_KNOB 0x13

#define HDA_WIDGET_AUDIO_OUT 0x0
#define HDA_WIDGET_AUDIO_IN 0x1
#define HDA_WIDGET_MIXER 0x2
#define HDA_WIDGET_SELECTOR 0x3
#define HDA_WIDGET_PIN 0x4
#define HDA_WIDGET_POWER 0x5
#define HDA_WIDGET_VOLUME_KNOB 0x6
#define HDA_WIDGET_BEEP 0x7
#define HDA_WIDGET_VENDOR 0xF

#define HDA_WCAP_STEREO (1u << 0)
#define HDA_WCAP_IN_AMP (1u << 1)
#define HDA_WCAP_OUT_AMP (1u << 2)
#define HDA_WCAP_AMP_OVRD (1u << 3)
#define HDA_WCAP_FORMAT_OVRD (1u << 4)
#define HDA_WCAP_STRIPE (1u << 5)
#define HDA_WCAP_PROC (1u << 6)
#define HDA_WCAP_UNSOL (1u << 7)
#define HDA_WCAP_CONN_LIST (1u << 8)
#define HDA_WCAP_DIGITAL (1u << 9)
#define HDA_WCAP_POWER (1u << 10)
#define HDA_WCAP_LR_SWAP (1u << 11)
#define HDA_WCAP_TYPE(c) (((c) >> 20) & 0xFu)

#define HDA_PINCAP_IMP_SENSE (1u << 0)
#define HDA_PINCAP_TRIGGER (1u << 1)
#define HDA_PINCAP_PRES_DETECT (1u << 2)
#define HDA_PINCAP_HP_DRIVE (1u << 3)
#define HDA_PINCAP_OUT (1u << 4)
#define HDA_PINCAP_IN (1u << 5)
#define HDA_PINCAP_BALANCED (1u << 6)
#define HDA_PINCAP_HDMI (1u << 7)
#define HDA_PINCAP_VREF_SHIFT 8
#define HDA_PINCAP_EAPD (1u << 16)
#define HDA_PINCAP_DP (1u << 24)

#define HDA_PINCTL_VREF_HIZ 0x00
#define HDA_PINCTL_VREF_50 0x01
#define HDA_PINCTL_VREF_GRD 0x02
#define HDA_PINCTL_VREF_80 0x04
#define HDA_PINCTL_VREF_100 0x05
#define HDA_PINCTL_IN_EN 0x20
#define HDA_PINCTL_OUT_EN 0x40
#define HDA_PINCTL_HP_EN 0x80

#define HDA_AMPCAP_OFFSET(c) ((c) & 0x7Fu)
#define HDA_AMPCAP_STEPS(c) (((c) >> 8) & 0x7Fu)
#define HDA_AMPCAP_STEP_SIZE(c) (((c) >> 16) & 0x7Fu)
#define HDA_AMPCAP_MUTE (1u << 31)

#define HDA_AMP_SET_OUT (1u << 15)
#define HDA_AMP_SET_IN (1u << 14)
#define HDA_AMP_SET_LEFT (1u << 13)
#define HDA_AMP_SET_RIGHT (1u << 12)
#define HDA_AMP_SET_INDEX_SHIFT 8
#define HDA_AMP_MUTE (1u << 7)

#define HDA_CFG_CONN(c) (((c) >> 30) & 0x3u)
#define HDA_CFG_LOCATION(c) (((c) >> 24) & 0x3Fu)
#define HDA_CFG_DEVICE(c) (((c) >> 20) & 0xFu)
#define HDA_CFG_CTYPE(c) (((c) >> 16) & 0xFu)
#define HDA_CFG_COLOR(c) (((c) >> 12) & 0xFu)
#define HDA_CFG_MISC(c) (((c) >> 8) & 0xFu)
#define HDA_CFG_ASSOC(c) (((c) >> 4) & 0xFu)
#define HDA_CFG_SEQ(c) ((c) & 0xFu)

#define HDA_DEV_LINE_OUT 0x0
#define HDA_DEV_SPEAKER 0x1
#define HDA_DEV_HP_OUT 0x2
#define HDA_DEV_CD 0x3
#define HDA_DEV_SPDIF_OUT 0x4
#define HDA_DEV_DIGITAL_OUT 0x5
#define HDA_DEV_LINE_IN 0x8
#define HDA_DEV_AUX 0x9
#define HDA_DEV_MIC_IN 0xA
#define HDA_DEV_SPDIF_IN 0xC
#define HDA_DEV_DIGITAL_IN 0xD

#define HDA_PCM_RATE_MASK 0x0FFFu
#define HDA_PCM_BITS_8 (1u << 16)
#define HDA_PCM_BITS_16 (1u << 17)
#define HDA_PCM_BITS_20 (1u << 18)
#define HDA_PCM_BITS_24 (1u << 19)
#define HDA_PCM_BITS_32 (1u << 20)

typedef struct hda_bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
} __attribute__((packed)) hda_bdl_entry;

typedef struct hda_widget {
    uint8_t nid;
    uint8_t type;
    uint32_t caps;
    uint32_t pincap;
    uint32_t config;
    uint32_t config_bios;
    uint32_t amp_in_caps;
    uint32_t amp_out_caps;
    uint32_t pcm_caps;
    uint32_t stream_fmts;
    uint8_t conn_count;
    uint8_t conns[HDA_MAX_CONNS];
    uint8_t claimed;
    uint8_t claim_idx;
    uint8_t used;
} hda_widget;

typedef struct hda_path {
    uint8_t nodes[HDA_MAX_PATH];
    uint8_t idx[HDA_MAX_PATH];
    uint8_t len;
} hda_path;

typedef struct hda_output {
    uint8_t pin;
    uint8_t device;
    uint8_t assoc;
    uint8_t seq;
    uint8_t jack_sense;
    uint8_t enabled;
    uint8_t vol_nid;
    uint8_t vol_in_amp;
    uint8_t vol_index;
    struct hda_path path;
} hda_output;

typedef struct hda_stream_hw {
    int sd;
    uint8_t tag;
    uint8_t running;
    uint8_t prepared;
    uint8_t *buf;
    uint64_t buf_phys;
    uint32_t buf_pages;
    uint32_t buf_size;
    struct hda_bdl_entry *bdl;
    uint64_t bdl_phys;
    uint32_t bdl_pages;
    uint32_t period_bytes;
    uint32_t periods;
    uint32_t guard_bytes;
    uint32_t last_pos;
    uint64_t played;
    uint64_t written;
    uint32_t sw_off;
    uint16_t fmt_reg;
    uint32_t xruns;
} hda_stream_hw;

struct hda_controller;

typedef struct hda_codec_dev {
    struct audio_device adev;
    struct hda_controller *ctrl;
    uint8_t cad;
    uint8_t afg;
    uint32_t vendor_id;
    uint32_t revision;
    uint32_t subsystem;
    uint32_t afg_pcm_caps;
    uint32_t afg_stream_fmts;
    uint32_t afg_amp_in;
    uint32_t afg_amp_out;
    uint32_t gpio_caps;
    uint8_t first_nid;
    uint8_t widget_count;
    struct hda_widget *widgets;
    struct audio_quirk_set quirks;
    struct hda_output outs[HDA_MAX_OUTPUTS];
    uint8_t out_count;
    uint8_t dacs[HDA_MAX_DACS];
    uint8_t dac_count;
    uint8_t in_pin;
    uint8_t adc;
    uint8_t in_vol_nid;
    uint8_t in_vol_in_amp;
    uint8_t in_vol_index;
    struct hda_path in_path;
    uint8_t has_input;
    struct hda_stream_hw hw[AUDIO_DIR_COUNT];
    uint64_t last_jack_ms;
    int8_t hp_present;
    uint8_t is_digital;
} hda_codec_dev;

typedef struct hda_controller {
    struct pci_device *pci;
    volatile uint8_t *mmio;
    uint64_t mmio_phys;
    uint32_t pci_id;
    uint32_t subsys;
    uint8_t revision;
    char name[48];
    uint16_t gcap;
    uint8_t iss;
    uint8_t oss;
    uint8_t bss;
    uint8_t ok64;
    uint32_t *corb;
    uint64_t corb_phys;
    uint32_t corb_pages;
    uint16_t corb_entries;
    uint16_t corb_wp;
    uint64_t *rirb;
    uint64_t rirb_phys;
    uint32_t rirb_pages;
    uint16_t rirb_entries;
    uint16_t rirb_rp;
    uint8_t single_cmd;
    uint32_t *dmapos;
    uint64_t dmapos_phys;
    uint32_t dmapos_pages;
    uint16_t codec_mask;
    struct audio_quirk_set quirks;
    spinlock_t cmd_lock;
    uint32_t cmd_timeouts;
    uint8_t device_count;
    struct hda_codec_dev *devices[HDA_MAX_DEVICES];
} hda_controller;

typedef struct hda_driver {
    int (*get_controller_count)(void);
    struct hda_controller *(*get_controller)(int idx);
    uint32_t (*codec_command)(struct hda_controller *c, uint8_t cad, uint8_t nid, uint32_t verb_payload);
} hda_driver;

struct hda_driver *return_hda_driver(void);
struct driver *return_meta_hda_driver(void);

#endif
