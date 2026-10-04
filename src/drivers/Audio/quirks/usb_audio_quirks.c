#include "drivers/Audio/audio_quirks.h"

#define USB_AUDIO_QUIRK(_v, _d, _model, _set) \
    { .name = (_model), .model = (_model), .match = { .usb_id = AUDIO_ID((_v), (_d)) }, .set = (_set) }
#define USB_AUDIO_REV_QUIRK(_v, _d, _rev, _model, _set) \
    { .name = (_model), .model = (_model), .match = { .usb_id = AUDIO_ID((_v), (_d)), .rev_min = (_rev), .rev_max = (_rev) }, .set = (_set) }
#define USB_AUDIO_VENDOR(_v, _d, _model) \
    USB_AUDIO_QUIRK(_v, _d, _model, AUDIO_USB_Q_VENDOR_CLASS)

static const struct audio_quirk usb_audio_quirks[] = {
    { .model = "QEMU USB Audio", .match = { .usb_id = AUDIO_ID(0x46f4, 0x0002) } },

    USB_AUDIO_REV_QUIRK(0x04fa, 0x4201, 0xa2, "Dallas J-6502 speakers", AUDIO_USB_Q_BAD_ADC | AUDIO_USB_Q_NO_XU),
    USB_AUDIO_REV_QUIRK(0x04d2, 0x0070, 0x103, "Altec Lansing ADA70", AUDIO_USB_Q_BAD_ADC),
    USB_AUDIO_QUIRK(0x04d2, 0xff05, "Altec Lansing ASC495", AUDIO_USB_Q_SKIP),
    USB_AUDIO_REV_QUIRK(0x0562, 0x0001, 0x009, "Telex Enhanced USB Microphone", AUDIO_USB_Q_NO_FRAC),
    USB_AUDIO_REV_QUIRK(0x1527, 0x0201, 0x100, "Silicon Portals YAP Phone", AUDIO_USB_Q_INP_ASYNC),
    USB_AUDIO_REV_QUIRK(0x1130, 0xf211, 0x0101, "Ten X USB audio headset", AUDIO_USB_Q_SWAP_LR),

    USB_AUDIO_VENDOR(0x0582, 0x0000, "Roland UA-100"),
    USB_AUDIO_VENDOR(0x0582, 0x0002, "Roland UM-4"),
    USB_AUDIO_VENDOR(0x0582, 0x0003, "Roland SC-8850"),
    USB_AUDIO_VENDOR(0x0582, 0x0004, "Roland U-8"),
    USB_AUDIO_VENDOR(0x0582, 0x0005, "Roland UM-2"),
    USB_AUDIO_VENDOR(0x0582, 0x0007, "Roland SC-8820"),
    USB_AUDIO_VENDOR(0x0582, 0x0008, "Roland PC-300"),
    USB_AUDIO_VENDOR(0x0582, 0x000b, "Roland SK-500"),
    USB_AUDIO_VENDOR(0x0582, 0x000c, "Roland SC-D70"),
    USB_AUDIO_VENDOR(0x0582, 0x0009, "Roland UM-1"),
    USB_AUDIO_VENDOR(0x0582, 0x0014, "EDIROL UM-880"),
    USB_AUDIO_VENDOR(0x0582, 0x0016, "Roland SD-90"),
    USB_AUDIO_VENDOR(0x0582, 0x0023, "Roland UM-550"),
    USB_AUDIO_VENDOR(0x0582, 0x0027, "Roland SD-20"),
    USB_AUDIO_VENDOR(0x0582, 0x0029, "Roland SD-80"),
    USB_AUDIO_VENDOR(0x0582, 0x002b, "Roland UA-700"),
    USB_AUDIO_VENDOR(0x0582, 0x0033, "EDIROL PCR-300"),
    USB_AUDIO_VENDOR(0x0582, 0x00e6, "EDIROL UA-25EX (Advanced Driver)"),
    USB_AUDIO_VENDOR(0x0582, 0x0132, "Roland UA-33"),

    USB_AUDIO_VENDOR(0x0763, 0x2080, "M-Audio Fast Track Ultra"),
    USB_AUDIO_VENDOR(0x0763, 0x2081, "M-Audio Fast Track Ultra 8R"),
};

const struct audio_quirk_table audio_quirks_usb_audio =
    AUDIO_QUIRK_TABLE("usb-audio", AUDIO_QUIRK_USB_AUDIO, usb_audio_quirks);
