// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/Audio/audio_quirks.h"

#define RTK_COEF_NID 0x20
#define RTK_SET_COEF_INDEX 0x500u
#define RTK_GET_PROC_COEF 0xC00u
#define RTK_SET_PROC_COEF 0x400u

#define RTK_COEF(_idx, _mask, _val) { .nid = RTK_COEF_NID, .index = (_idx), .mask = (_mask), .value = (_val) }
#define RTK_COEFEX(_nid, _idx, _mask, _val) { .nid = (_nid), .index = (_idx), .mask = (_mask), .value = (_val) }
#define RTK_CLEAR(_idx, _bits) RTK_COEF((_idx), (_bits), 0)
#define RTK_SET(_idx, _bits) RTK_COEF((_idx), (_bits), (_bits))
#define RTK_WRITE(_idx, _val) RTK_COEF((_idx), 0xFFFFu, (_val))

#define RTK_EAPD(_v, _d, ...) \
    { .name = "realtek-eapd", .match = { .codec_id = AUDIO_ID((_v), (_d)) }, .coefs = AUDIO_COEFS(__VA_ARGS__) }
#define RTK_INIT(_v, _d, ...) \
    { .name = "realtek-init", .match = { .codec_id = AUDIO_ID((_v), (_d)) }, .coefs = AUDIO_COEFS(__VA_ARGS__) }
#define RTK_HOOK(_d) \
    { .name = "realtek-eapd", .match = { .codec_id = AUDIO_ID(0x10ec, (_d)) }, .hook = rtk_eapd_coef0 }

#define RTK_EAPD_225 RTK_COEF(0x67, 0xF000, 0x3000), RTK_CLEAR(0x36, 1u << 13), RTK_CLEAR(0x10, 1u << 9)
#define RTK_EAPD_256 RTK_CLEAR(0x36, 1u << 13), RTK_CLEAR(0x10, 1u << 9)
#define RTK_EAPD_255 RTK_CLEAR(0x10, 1u << 9)
#define RTK_EAPD_294 RTK_CLEAR(0x10, 1u << 15)
#define RTK_EAPD_274 RTK_WRITE(0x6e, 0x0c25), RTK_CLEAR(0x10, 1u << 15)
#define RTK_EAPD_EAPD_CTRL RTK_SET(0x0d, 1u << 14)
#define RTK_EAPD_1150 RTK_CLEAR(0x07, 1u << 1)

#define RTK_INIT_256 \
    RTK_CLEAR(0x46, 3u << 12), RTK_COEFEX(0x53, 0x02, 0x8000, 0x8000), RTK_COEFEX(0x53, 0x02, 0x8000, 0x0000), \
    RTK_WRITE(0x36, 0x5757)

static uint32_t rtk_coef_read(struct audio_quirk_ctx *ctx, uint16_t idx) {
    ctx->verb(ctx->backend, RTK_COEF_NID, (RTK_SET_COEF_INDEX << 8) | idx);
    return ctx->verb(ctx->backend, RTK_COEF_NID, RTK_GET_PROC_COEF << 8) & 0xFFFFu;
}

static void rtk_coef_update(struct audio_quirk_ctx *ctx, uint16_t idx, uint16_t mask, uint16_t bits) {
    uint32_t val = (rtk_coef_read(ctx, idx) & ~(uint32_t)mask) | bits;
    ctx->verb(ctx->backend, RTK_COEF_NID, (RTK_SET_COEF_INDEX << 8) | idx);
    ctx->verb(ctx->backend, RTK_COEF_NID, (RTK_SET_PROC_COEF << 8) | (val & 0xFFFFu));
}

static int rtk_eapd_coef0(const struct audio_quirk *q, struct audio_quirk_ctx *ctx) {
    if (ctx->stage != AUDIO_QUIRK_STAGE_INIT || !ctx->verb) return 0;
    uint32_t rev = rtk_coef_read(ctx, 0) & 0x00F0u;
    switch (q->match.codec_id & 0xFFFFu) {
        case 0x0269:
            if (rev == 0x0010) rtk_coef_update(ctx, 0x0d, 0, 1u << 14);
            if (rev == 0x0020) rtk_coef_update(ctx, 0x04, 1u << 15, 0);
            if (rev == 0x0030) rtk_coef_update(ctx, 0x10, 1u << 9, 0);
            break;
        case 0x0662:
            if (rev == 0x0030) rtk_coef_update(ctx, 0x04, 1u << 10, 0);
            break;
        case 0x0888:
            if (rev == 0x0020 || rev == 0x0030) rtk_coef_update(ctx, 0x07, 1u << 5, 0);
            break;
    }
    return 0;
}

static const struct audio_quirk hda_realtek_quirks[] = {
    RTK_EAPD(0x10ec, 0x0262, RTK_SET(0x07, 1u << 5)),
    RTK_EAPD(0x10ec, 0x0267, RTK_SET(0x07, 1u << 13)),
    RTK_EAPD(0x10ec, 0x0268, RTK_SET(0x07, 1u << 13)),
    RTK_HOOK(0x0269),
    RTK_EAPD(0x10ec, 0x0280, RTK_CLEAR(0x04, 1u << 15)),
    RTK_EAPD(0x10ec, 0x0284, RTK_CLEAR(0x04, 1u << 15)),
    RTK_EAPD(0x10ec, 0x0290, RTK_CLEAR(0x04, 1u << 15)),
    RTK_EAPD(0x10ec, 0x0292, RTK_CLEAR(0x04, 1u << 15)),
    RTK_EAPD(0x10ec, 0x0225, RTK_EAPD_225),
    RTK_EAPD(0x10ec, 0x0295, RTK_EAPD_225),
    RTK_EAPD(0x10ec, 0x0299, RTK_EAPD_225),
    RTK_EAPD(0x10ec, 0x0215, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0236, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0245, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0256, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0257, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0285, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0289, RTK_EAPD_256),
    RTK_EAPD(0x10ec, 0x0230, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0233, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0235, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0255, RTK_EAPD_255),
    RTK_EAPD(0x19e5, 0x8326, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0282, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0283, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0286, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0288, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0298, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0300, RTK_EAPD_255),
    RTK_EAPD(0x10ec, 0x0275, RTK_SET(0x0e, 1u << 0)),
    RTK_EAPD(0x10ec, 0x0287, RTK_CLEAR(0x10, 1u << 9), RTK_WRITE(0x08, 0x4ab7)),
    RTK_EAPD(0x10ec, 0x0293, RTK_CLEAR(0x0a, 1u << 13)),
    RTK_EAPD(0x10ec, 0x0234, RTK_EAPD_274),
    RTK_EAPD(0x10ec, 0x0274, RTK_EAPD_274),
    RTK_EAPD(0x10ec, 0x0294, RTK_EAPD_294),
    RTK_EAPD(0x10ec, 0x0700, RTK_EAPD_294),
    RTK_EAPD(0x10ec, 0x0701, RTK_EAPD_294),
    RTK_EAPD(0x10ec, 0x0703, RTK_EAPD_294),
    RTK_EAPD(0x10ec, 0x0711, RTK_EAPD_294),
    RTK_HOOK(0x0662),
    RTK_EAPD(0x10ec, 0x0272, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0273, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0663, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0665, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0670, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0671, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0672, RTK_EAPD_EAPD_CTRL),
    RTK_EAPD(0x10ec, 0x0222, RTK_CLEAR(0x19, 1u << 13)),
    RTK_EAPD(0x10ec, 0x0623, RTK_CLEAR(0x19, 1u << 13)),
    RTK_EAPD(0x10ec, 0x0668, RTK_CLEAR(0x07, 3u << 13)),
    RTK_EAPD(0x10ec, 0x0867, RTK_CLEAR(0x04, 1u << 10)),
    RTK_HOOK(0x0888),
    RTK_EAPD(0x10ec, 0x0892, RTK_CLEAR(0x07, 1u << 5)),
    RTK_EAPD(0x10ec, 0x0897, RTK_CLEAR(0x07, 1u << 5)),
    RTK_EAPD(0x10ec, 0x0899, RTK_EAPD_1150),
    RTK_EAPD(0x10ec, 0x0900, RTK_EAPD_1150),
    RTK_EAPD(0x10ec, 0x0b00, RTK_EAPD_1150),
    RTK_EAPD(0x10ec, 0x1168, RTK_EAPD_1150),
    RTK_EAPD(0x10ec, 0x1220, RTK_EAPD_1150),

    RTK_INIT(0x10ec, 0x0230, RTK_INIT_256),
    RTK_INIT(0x10ec, 0x0235, RTK_INIT_256),
    RTK_INIT(0x10ec, 0x0236, RTK_INIT_256),
    RTK_INIT(0x10ec, 0x0255, RTK_INIT_256),
    RTK_INIT(0x10ec, 0x0256, RTK_INIT_256),
    RTK_INIT(0x10ec, 0x0257, RTK_INIT_256),
    RTK_INIT(0x19e5, 0x8326, RTK_INIT_256),
    RTK_INIT(0x10ec, 0x0234, RTK_COEF(0x6b, 0x0018, 0x0018)),
    RTK_INIT(0x10ec, 0x0274, RTK_COEF(0x6b, 0x0018, 0x0018)),
    RTK_INIT(0x10ec, 0x0294, RTK_COEF(0x6b, 0x0018, 0x0018)),
};

const struct audio_quirk_table audio_quirks_hda_realtek =
    AUDIO_QUIRK_TABLE("hda-realtek", AUDIO_QUIRK_HDA_CODEC, hda_realtek_quirks);
