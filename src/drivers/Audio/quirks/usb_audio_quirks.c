// SPDX-License-Identifier: GPL-2.0-only AND BSD-2-Clause
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/Audio/audio_quirks.h"

#define FB_USB(_v, _d, _model, _set) \
    { .name = "freebsd:" _model, .model = (_model), .match = { .usb_id = AUDIO_ID((_v), (_d)) }, .set = (_set) }
#define FB_USB_REV(_v, _d, _lo, _hi, _model, _set) \
    { .name = "freebsd:" _model, .model = (_model), \
      .match = { .usb_id = AUDIO_ID((_v), (_d)), .rev_min = (_lo), .rev_max = (_hi) }, .set = (_set) }
#define LX_USB(_id, _set) \
    { .name = "linux:usb-audio", .match = { .usb_id = (_id) }, .set = (_set) }

static const struct audio_quirk usb_audio_quirks[] = {
    FB_USB_REV(0x04fa, 0x4201, 0x00a2, 0x00a2, "Dallas Semiconductor J-6502 speakers", AUDIO_USB_Q_BAD_ADC),
    FB_USB_REV(0x04fa, 0x4201, 0x00a2, 0x00a2, "Dallas Semiconductor J-6502 speakers", AUDIO_USB_Q_NO_XU),
    FB_USB_REV(0x04d2, 0x0070, 0x0103, 0x0103, "Altec Lansing ADA70 Speakers", AUDIO_USB_Q_BAD_ADC),
    FB_USB(0x04d2, 0xff05, "Altec Lansing ASC495 Speakers", AUDIO_USB_Q_SKIP),
    FB_USB_REV(0x0562, 0x0001, 0x0009, 0x0009, "Telex Communications Enhanced USB Microphone", AUDIO_USB_Q_NO_FRAC),
    FB_USB_REV(0x1527, 0x0201, 0x0100, 0x0100, "Silicon Portals YAP Phone", AUDIO_USB_Q_INP_ASYNC),
    FB_USB_REV(0x1130, 0xf211, 0x0101, 0x0101, "Ten X Technology, Inc. USB audio headset", AUDIO_USB_Q_SWAP_LR),
    FB_USB(0x0582, 0x0009, "Roland UM-1 MIDI I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0003, "Roland SC-8850 MIDI Synth", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0016, "Roland SD-90 MIDI Synth", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0014, "Roland EDIROL UM-880 MIDI I/F (native)", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0000, "Roland UA-100 Audio I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0002, "Roland UM-4 MIDI I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0004, "Roland U-8 Audio I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0005, "Roland UM-2 MIDI I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0007, "Roland SC-8820 MIDI Synth", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0008, "Roland PC-300 MIDI Keyboard", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x000b, "Roland SK-500 MIDI Keyboard", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x000c, "Roland SC-D70 MIDI Synth", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0023, "Roland UM-550 MIDI I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0027, "Roland SD-20 MIDI Synth", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0029, "Roland SD-80 MIDI Synth", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x002b, "Roland UA-700 Audio I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0033, "Roland EDIROL PCR-300 MIDI I/F", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0763, 0x2080, "M-Audio Fast Track Ultra", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0763, 0x2081, "M-Audio Fast Track Ultra 8R", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x00e6, "Roland EDIROL UA-25EX (Advanced Driver)", AUDIO_USB_Q_VENDOR_CLASS),
    FB_USB(0x0582, 0x0132, "Roland UA-33 Audio I/F", AUDIO_USB_Q_VENDOR_CLASS),

    LX_USB(AUDIO_ID(0x154e, 0x500e), AUDIO_USB_Q_CLOCK_NO_VALIDATE),
};

const struct audio_quirk_table audio_quirks_usb_audio =
    AUDIO_QUIRK_TABLE("usb-audio", AUDIO_QUIRK_USB_AUDIO, usb_audio_quirks);
