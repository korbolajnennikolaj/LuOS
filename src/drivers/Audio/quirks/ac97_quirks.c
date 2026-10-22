// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/Audio/audio_core.h"
#include "drivers/Audio/audio_quirks.h"

#define AC97C_GROUP 1
#define AC97_NAME_GROUP 2
#define AC97_AD198X_GROUP 3

#define AC97_AD_JACK_SPDIF 0x72
#define AC97_AD_HP_LINE 0x76
#define AC97_REG_POWER 0x26
#define AC97_MIXEXT_CLFE 0x36

#define FB_ICH(_id, _model, _set) \
    { .name = "freebsd:ich", .model = (_model), .group = AC97C_GROUP, .match = { .pci_id = (_id) }, .set = (_set) }
#define FB_AC97(_id, _mask, _model) \
    { .model = (_model), .group = AC97_NAME_GROUP, .match = { .codec_id = (_id), .codec_mask = (_mask) } }
#define FB_EAPD_INV(_subsys) \
    { .name = "freebsd:eapd-inv", .match = { .subsys = (_subsys) }, .set = AUDIO_AC97_Q_EAPD_INV }
#define FB_AC97_REGS(_name, _id, _mask, _subsys, ...) \
    { .name = (_name), .match = { .codec_id = (_id), .codec_mask = (_mask), .subsys = (_subsys) }, \
      .regs = AUDIO_REGS(__VA_ARGS__) }
#define FB_AD198X(_id) \
    { .name = "freebsd:ad198x", .group = AC97_AD198X_GROUP, .match = { .codec_id = (_id), .codec_mask = 0xFFFFFFFFu }, \
      .regs = AUDIO_REGS({ AC97_AD_HP_LINE, 0x0420, 0x0420 }) }

static const uint32_t ac97_ad198x_codecs[] = { 0x41445368, 0x41445370, 0x41445375, 0x41445378, 0 };

static const struct audio_quirk ac97_controller_quirks[] = {
    FB_ICH(AUDIO_ID(0x8086, 0x7195), "Intel 440MX", 0),
    FB_ICH(AUDIO_ID(0x8086, 0x2415), "Intel ICH (82801AA)", 0),
    FB_ICH(AUDIO_ID(0x8086, 0x2425), "Intel ICH (82801AB)", 0),
    FB_ICH(AUDIO_ID(0x8086, 0x2445), "Intel ICH2 (82801BA)", 0),
    FB_ICH(AUDIO_ID(0x8086, 0x2485), "Intel ICH3 (82801CA)", 0),
    FB_ICH(AUDIO_ID(0x8086, 0x24c5), "Intel ICH4 (82801DB)", AUDIO_AC97C_Q_ICH4_IOSE | AUDIO_AC97C_Q_IGNORE_PCR),
    FB_ICH(AUDIO_ID(0x8086, 0x24d5), "Intel ICH5 (82801EB)", AUDIO_AC97C_Q_ICH4_IOSE | AUDIO_AC97C_Q_IGNORE_PCR),
    FB_ICH(AUDIO_ID(0x8086, 0x25a6), "Intel 6300ESB", AUDIO_AC97C_Q_ICH4_IOSE | AUDIO_AC97C_Q_IGNORE_PCR),
    FB_ICH(AUDIO_ID(0x8086, 0x266e), "Intel ICH6 (82801FB)", AUDIO_AC97C_Q_ICH4_IOSE | AUDIO_AC97C_Q_IGNORE_PCR),
    FB_ICH(AUDIO_ID(0x8086, 0x27de), "Intel ICH7 (82801GB)", AUDIO_AC97C_Q_ICH4_IOSE | AUDIO_AC97C_Q_IGNORE_PCR),
    FB_ICH(AUDIO_ID(0x1039, 0x7012), "SiS 7012", AUDIO_AC97C_Q_SIS_REGS),
    FB_ICH(AUDIO_ID(0x10de, 0x01b1), "nVidia nForce", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x006a), "nVidia nForce2", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x008a), "nVidia nForce2 400", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x00da), "nVidia nForce3", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x00ea), "nVidia nForce3 250", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x0059), "nVidia nForce4", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x026b), "nVidia nForce 410 MCP", 0),
    FB_ICH(AUDIO_ID(0x10de, 0x003a), "nVidia nForce 4 MCP", 0),
    FB_ICH(AUDIO_ID(0x1022, 0x7445), "AMD-768", 0),
    FB_ICH(AUDIO_ID(0x1022, 0x746d), "AMD-8111", 0),
};

static const struct audio_quirk ac97_codec_quirks[] = {
    FB_AC97(0x41445303, 0xffffffff, "Analog Devices AD1819"),
    FB_AC97(0x41445340, 0xffffffff, "Analog Devices AD1881"),
    FB_AC97(0x41445348, 0xffffffff, "Analog Devices AD1881A"),
    FB_AC97(0x41445360, 0xffffffff, "Analog Devices AD1885"),
    FB_AC97(0x41445361, 0xffffffff, "Analog Devices AD1886"),
    FB_AC97(0x41445362, 0xffffffff, "Analog Devices AD1887"),
    FB_AC97(0x41445363, 0xffffffff, "Analog Devices AD1886A"),
    FB_AC97(0x41445368, 0xffffffff, "Analog Devices AD1888"),
    FB_AC97(0x41445370, 0xffffffff, "Analog Devices AD1980"),
    FB_AC97(0x41445372, 0xffffffff, "Analog Devices AD1981A"),
    FB_AC97(0x41445374, 0xffffffff, "Analog Devices AD1981B"),
    FB_AC97(0x41445375, 0xffffffff, "Analog Devices AD1985"),
    FB_AC97(0x41445378, 0xffffffff, "Analog Devices AD1986"),
    FB_AC97(0x414b4d00, 0xffffffff, "Asahi Kasei AK4540"),
    FB_AC97(0x414b4d01, 0xffffffff, "Asahi Kasei AK4542"),
    FB_AC97(0x414b4d02, 0xffffffff, "Asahi Kasei AK4543"),
    FB_AC97(0x414b4d06, 0xffffffff, "Asahi Kasei AK4544A"),
    FB_AC97(0x454b4d07, 0xffffffff, "AK4545"),
    FB_AC97(0x414c4320, 0xfffffff0, "Realtek ALC100"),
    FB_AC97(0x414c4730, 0xfffffff0, "Avance Logic ALC101"),
    FB_AC97(0x414c4710, 0xfffffff0, "Avance Logic ALC200"),
    FB_AC97(0x414c4740, 0xfffffff0, "Avance Logic ALC202"),
    FB_AC97(0x414c4720, 0xfffffff0, "Avance Logic ALC650"),
    FB_AC97(0x414c4752, 0xfffffff0, "Avance Logic ALC250"),
    FB_AC97(0x414c4760, 0xfffffff0, "Avance Logic ALC655"),
    FB_AC97(0x414c4770, 0xfffffff0, "Avance Logic ALC203"),
    FB_AC97(0x414c4780, 0xfffffff0, "Avance Logic ALC658"),
    FB_AC97(0x414c4790, 0xfffffff0, "Avance Logic ALC850"),
    FB_AC97(0x43525900, 0xfffffff8, "Cirrus Logic CS4297"),
    FB_AC97(0x43525910, 0xfffffff8, "Cirrus Logic CS4297A"),
    FB_AC97(0x43525920, 0xfffffff8, "Cirrus Logic CS4294/98"),
    FB_AC97(0x4352592d, 0xfffffff8, "Cirrus Logic CS4294"),
    FB_AC97(0x43525930, 0xfffffff8, "Cirrus Logic CS4299"),
    FB_AC97(0x43525940, 0xfffffff8, "Cirrus Logic CS4201"),
    FB_AC97(0x43525958, 0xfffffff8, "Cirrus Logic CS4205"),
    FB_AC97(0x43525960, 0xfffffff8, "Cirrus Logic CS4291A"),
    FB_AC97(0x434d4961, 0xffffffff, "C-Media Electronics CMI9739"),
    FB_AC97(0x434d4941, 0xffffffff, "C-Media Electronics CMI9738"),
    FB_AC97(0x434d4978, 0xffffffff, "C-Media Electronics CMI9761"),
    FB_AC97(0x434d4982, 0xffffffff, "C-Media Electronics CMI9761"),
    FB_AC97(0x434d4983, 0xffffffff, "C-Media Electronics CMI9761"),
    FB_AC97(0x43585421, 0xffffffff, "Conexant HSD11246"),
    FB_AC97(0x43585428, 0xfffffff8, "Conexant CX20468"),
    FB_AC97(0x43585430, 0xffffffff, "Conexant CX20468-21"),
    FB_AC97(0x44543000, 0xffffffff, "Diamond Technology DT0398"),
    FB_AC97(0x454d4323, 0xffffffff, "eMicro EM28023"),
    FB_AC97(0x454d4328, 0xffffffff, "eMicro EM28028"),
    FB_AC97(0x45838308, 0xffffffff, "ESS Technology ES1988"),
    FB_AC97(0x48525300, 0xffffffff, "Intersil HMP9701"),
    FB_AC97(0x49434501, 0xffffffff, "ICEnsemble ICE1230"),
    FB_AC97(0x49434511, 0xffffffff, "ICEnsemble ICE1232"),
    FB_AC97(0x49434514, 0xffffffff, "ICEnsemble ICE1232A"),
    FB_AC97(0x49434551, 0xfffffffc, "ICEnsemble VT1616"),
    FB_AC97(0x49544520, 0xffffffff, "ITE, Inc. ITE2226E"),
    FB_AC97(0x49544560, 0xfffffff8, "ITE, Inc. ITE2646E"),
    FB_AC97(0x4e534340, 0xffffffff, "National Semiconductor LM4540"),
    FB_AC97(0x4e534343, 0xffffffff, "National Semiconductor LM4543"),
    FB_AC97(0x4e534346, 0xffffffff, "National Semiconductor LM4546A"),
    FB_AC97(0x4e534348, 0xffffffff, "National Semiconductor LM4548A"),
    FB_AC97(0x4e534331, 0xffffffff, "National Semiconductor LM4549"),
    FB_AC97(0x4e534349, 0xffffffff, "National Semiconductor LM4549A"),
    FB_AC97(0x4e534350, 0xffffffff, "National Semiconductor LM4550"),
    FB_AC97(0x50534301, 0xffffffff, "Philips Semiconductor UCB1510"),
    FB_AC97(0x50534304, 0xffffffff, "Philips Semiconductor UCB1400"),
    FB_AC97(0x83847600, 0xffffffff, "SigmaTel STAC9700/83/84"),
    FB_AC97(0x83847604, 0xffffffff, "SigmaTel STAC9701/03/04/05"),
    FB_AC97(0x83847605, 0xffffffff, "SigmaTel STAC9704"),
    FB_AC97(0x83847608, 0xffffffff, "SigmaTel STAC9708/11"),
    FB_AC97(0x83847609, 0xffffffff, "SigmaTel STAC9721/23"),
    FB_AC97(0x83847644, 0xffffffff, "SigmaTel STAC9744/45"),
    FB_AC97(0x83847650, 0xffffffff, "SigmaTel STAC9750/51"),
    FB_AC97(0x83847652, 0xffffffff, "SigmaTel STAC9752/53"),
    FB_AC97(0x83847656, 0xffffffff, "SigmaTel STAC9756/57"),
    FB_AC97(0x83847658, 0xffffffff, "SigmaTel STAC9758/59"),
    FB_AC97(0x83847660, 0xffffffff, "SigmaTel STAC9760/61"),
    FB_AC97(0x83847662, 0xffffffff, "SigmaTel STAC9762/63"),
    FB_AC97(0x83847666, 0xffffffff, "SigmaTel STAC9766/67"),
    FB_AC97(0x53494c22, 0xffffffff, "Silicon Laboratories Si3036"),
    FB_AC97(0x53494c23, 0xffffffff, "Silicon Laboratories Si3038"),
    FB_AC97(0x54524103, 0xffffffff, "TriTech TR28023"),
    FB_AC97(0x54524106, 0xffffffff, "TriTech TR28026"),
    FB_AC97(0x54524108, 0xffffffff, "TriTech TR28028"),
    FB_AC97(0x54524123, 0xffffffff, "TriTech TR28602"),
    FB_AC97(0x54524e03, 0xfffffff8, "TLV320AIC27"),
    FB_AC97(0x54584e20, 0xffffffff, "Texas Instruments TLC320AD90"),
    FB_AC97(0x56494161, 0xffffffff, "VIA Technologies VIA1612A"),
    FB_AC97(0x56494170, 0xffffffff, "VIA Technologies VIA1617A"),
    FB_AC97(0x574d4c00, 0xffffffff, "Wolfson WM9701A"),
    FB_AC97(0x574d4c03, 0xffffffff, "Wolfson WM9703/4/7/8"),
    FB_AC97(0x574d4c04, 0xffffffff, "Wolfson WM9704Q"),
    FB_AC97(0x574d4c05, 0xffffffff, "Wolfson WM9705/10"),
    FB_AC97(0x574d4d09, 0xffffffff, "WM9709"),
    FB_AC97(0x574d4c12, 0xffffffff, "Wolfson WM9711/12"),
    FB_AC97(0x57454301, 0xffffffff, "Winbond W83971D"),
    FB_AC97(0x594d4800, 0xffffffff, "Yamaha YMF743"),
    FB_AC97(0x594d4802, 0xffffffff, "Yamaha YMF752"),
    FB_AC97(0x594d4803, 0xffffffff, "Yamaha YMF753"),
    FB_AC97(0x01408384, 0xffffffff, "SigmaTel STAC9704"),

    FB_EAPD_INV(AUDIO_ID(0x161f, 0x202f)),
    FB_EAPD_INV(AUDIO_ID(0x161f, 0x203a)),
    FB_EAPD_INV(AUDIO_ID(0x161f, 0x203e)),
    FB_EAPD_INV(AUDIO_ID(0x161f, 0x204c)),
    FB_EAPD_INV(AUDIO_ID(0x104d, 0x8144)),
    FB_EAPD_INV(AUDIO_ID(0x104d, 0x8197)),
    FB_EAPD_INV(AUDIO_ID(0x104d, 0x81c0)),
    FB_EAPD_INV(AUDIO_ID(0x104d, 0x81c5)),
    FB_EAPD_INV(AUDIO_ID(0x103c, 0x3089)),
    FB_EAPD_INV(AUDIO_ID(0x103c, 0x309a)),
    FB_EAPD_INV(AUDIO_ID(0x1033, 0x8213)),
    FB_EAPD_INV(AUDIO_ID(0x1033, 0x82be)),

    FB_AC97_REGS("freebsd:ad1886", 0x41445361, 0xFFFFFFFFu, AUDIO_ID_ANY, { AC97_AD_JACK_SPDIF, 0xFFFF, 0x0010 }),
    { .name = "freebsd:ad198x", .group = AC97_AD198X_GROUP,
      .match = { .subsys = AUDIO_ID(0x1043, 0x1193), .codec_list = ac97_ad198x_codecs } },
    FB_AD198X(0x41445368),
    FB_AD198X(0x41445370),
    FB_AD198X(0x41445375),
    FB_AD198X(0x41445378),
    FB_AC97_REGS("freebsd:ad1981b", 0x41445374, 0xFFFFFFFFu, AUDIO_ID(0x1014, 0x02d9), { AC97_AD_JACK_SPDIF, 0x0800, 0x0800 }),
    FB_AC97_REGS("freebsd:ad1981b", 0x41445374, 0xFFFFFFFFu, AUDIO_ID(0x103c, 0x099c), { AC97_AD_JACK_SPDIF, 0x0800, 0x0800 }),
    FB_AC97_REGS("freebsd:cmi9739", 0x434d4961, 0xFFFFFFFFu, AUDIO_ID(0x1043, 0x1843), { AC97_REG_POWER, 0xFFFF, 0x000f },
                 { AC97_MIXEXT_CLFE, 0xFFFF, 0x0000 }, { 0x64, 0xFFFF, 0x7110 }),
    FB_AC97_REGS("freebsd:alc655", 0x414c4760, 0xFFFFFFF0u, AUDIO_ID(0x1462, 0x0061), { 0x7a, 0x0002, 0x0000 }),
    FB_AC97_REGS("freebsd:alc655", 0x414c4760, 0xFFFFFFF0u, AUDIO_ID(0x1462, 0x0131), { 0x7a, 0x0002, 0x0000 }),
    FB_AC97_REGS("freebsd:alc655", 0x414c4760, 0xFFFFFFF0u, AUDIO_ID(0x1462, 0x0161), { 0x7a, 0x0002, 0x0000 }),
    FB_AC97_REGS("freebsd:alc655", 0x414c4760, 0xFFFFFFF0u, AUDIO_ID(0x1462, 0x0351), { 0x7a, 0x0002, 0x0000 }),
    FB_AC97_REGS("freebsd:alc655", 0x414c4760, 0xFFFFFFF0u, AUDIO_ID(0x1734, 0x10ca), { 0x6a, 0x0001, 0x0001 }),
};

const struct audio_quirk_table audio_quirks_ac97_controllers =
    AUDIO_QUIRK_TABLE("ac97-controllers", AUDIO_QUIRK_AC97_CONTROLLER, ac97_controller_quirks);

const struct audio_quirk_table audio_quirks_ac97_codecs =
    AUDIO_QUIRK_TABLE("ac97-codecs", AUDIO_QUIRK_AC97_CODEC, ac97_codec_quirks);
