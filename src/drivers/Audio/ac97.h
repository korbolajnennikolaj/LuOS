#ifndef AC97_H
#define AC97_H

#include "audio_core.h"
#include "audio_quirks.h"

#include "components/drivers.h"
#include "components/pci.h"

#include <stdbool.h>
#include <stdint.h>

#define AC97_MAX_CONTROLLERS 4
#define AC97_BDL_ENTRIES 32
#define AC97_PERIODS 4
#define AC97_PERIOD_MS 16

#define AC97_BOX_PCM_IN 0x00
#define AC97_BOX_PCM_OUT 0x10
#define AC97_BOX_MIC_IN 0x20

#define AC97_BOX_BDBAR 0x00
#define AC97_BOX_CIV 0x04
#define AC97_BOX_LVI 0x05
#define AC97_BOX_SR 0x06
#define AC97_BOX_PICB 0x08
#define AC97_BOX_PIV 0x0A
#define AC97_BOX_CR 0x0B
#define AC97_SIS_BOX_SR 0x08
#define AC97_SIS_BOX_PICB 0x06

#define AC97_SR_DCH 0x01
#define AC97_SR_CELV 0x02
#define AC97_SR_LVBCI 0x04
#define AC97_SR_BCIS 0x08
#define AC97_SR_FIFOE 0x10
#define AC97_SR_CLEAR (AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE)

#define AC97_CR_RPBM 0x01
#define AC97_CR_RR 0x02
#define AC97_CR_LVBIE 0x04
#define AC97_CR_FEIE 0x08
#define AC97_CR_IOCE 0x10

#define AC97_REG_GLOB_CNT 0x2C
#define AC97_REG_GLOB_STA 0x30
#define AC97_REG_CAS 0x34
#define AC97_REG_SIS_UNMUTE 0x4C

#define AC97_GLOB_CNT_GIE 0x00000001u
#define AC97_GLOB_CNT_COLD 0x00000002u
#define AC97_GLOB_CNT_WARM 0x00000004u
#define AC97_GLOB_CNT_SHUT 0x00000008u
#define AC97_GLOB_CNT_PCM_MASK 0x00300000u

#define AC97_GLOB_STA_PCR 0x00000100u
#define AC97_GLOB_STA_SCR 0x00000200u
#define AC97_GLOB_STA_RCS 0x00008000u

#define AC97_MIX_RESET 0x00
#define AC97_MIX_MASTER 0x02
#define AC97_MIX_HEADPHONE 0x04
#define AC97_MIX_MONO 0x06
#define AC97_MIX_BEEP 0x0A
#define AC97_MIX_PHONE 0x0C
#define AC97_MIX_MIC 0x0E
#define AC97_MIX_LINE_IN 0x10
#define AC97_MIX_CD 0x12
#define AC97_MIX_VIDEO 0x14
#define AC97_MIX_AUX 0x16
#define AC97_MIX_PCM_OUT 0x18
#define AC97_MIX_REC_SEL 0x1A
#define AC97_MIX_REC_GAIN 0x1C
#define AC97_MIX_GENERAL 0x20
#define AC97_MIX_POWERDOWN 0x26
#define AC97_MIX_EXT_ID 0x28
#define AC97_MIX_EXT_STAT 0x2A
#define AC97_MIX_FRONT_RATE 0x2C
#define AC97_MIX_SURR_RATE 0x2E
#define AC97_MIX_LFE_RATE 0x30
#define AC97_MIX_ADC_RATE 0x32
#define AC97_MIX_MIC_RATE 0x34
#define AC97_MIX_VENDOR_ID1 0x7C
#define AC97_MIX_VENDOR_ID2 0x7E

#define AC97_MUTE 0x8000u
#define AC97_EXT_VRA 0x0001u
#define AC97_EXT_DRA 0x0002u
#define AC97_EXT_SPDIF 0x0004u
#define AC97_EXT_VRM 0x0008u
#define AC97_PD_READY 0x000Fu
#define AC97_PD_EAPD 0x8000u

typedef struct ac97_bdl_entry {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry;

typedef struct ac97_stream_hw {
    uint8_t box;
    uint8_t running;
    uint8_t prepared;
    uint8_t *buf;
    uint64_t buf_phys;
    uint32_t buf_pages;
    uint32_t buf_size;
    struct ac97_bdl_entry *bdl;
    uint64_t bdl_phys;
    uint32_t bdl_pages;
    uint32_t period_bytes;
    uint32_t last_pos;
    uint64_t played;
    uint64_t written;
    uint32_t sw_off;
    uint32_t rate;
    uint32_t xruns;
    uint32_t restarts;
} ac97_stream_hw;

typedef struct ac97_controller {
    struct audio_device adev;
    struct pci_device *pci;
    uint32_t pci_id;
    uint32_t subsys;
    uint8_t revision;
    char name[48];
    uint8_t mmio;
    uint16_t nam_io;
    uint16_t nabm_io;
    volatile uint8_t *nam_mem;
    volatile uint8_t *nabm_mem;
    uint8_t reg_sr;
    uint8_t reg_picb;
    uint8_t picb_bytes;
    uint32_t codec_id;
    uint16_t ext_id;
    uint16_t caps;
    uint8_t vra;
    uint8_t master_bits;
    uint8_t has_adc;
    struct audio_quirk_set ctrl_quirks;
    struct audio_quirk_set codec_quirks;
    struct ac97_stream_hw hw[AUDIO_DIR_COUNT];
    uint32_t read_timeouts;
} ac97_controller;

typedef struct ac97_driver {
    int (*get_controller_count)(void);
    struct ac97_controller *(*get_controller)(int idx);
    uint16_t (*codec_read)(struct ac97_controller *c, uint8_t reg);
    void (*codec_write)(struct ac97_controller *c, uint8_t reg, uint16_t value);
} ac97_driver;

struct ac97_driver *return_ac97_driver(void);
struct driver *return_meta_ac97_driver(void);

#endif
