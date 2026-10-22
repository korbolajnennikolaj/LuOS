// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/Audio/audio_quirks.h"

extern const struct audio_quirk_table audio_quirks_hda_controllers;
extern const struct audio_quirk_table audio_quirks_hda_realtek;
extern const struct audio_quirk_table audio_quirks_hda_freebsd;
extern const struct audio_quirk_table audio_quirks_hda_linux;
extern const struct audio_quirk_table audio_quirks_ac97_controllers;
extern const struct audio_quirk_table audio_quirks_ac97_codecs;
extern const struct audio_quirk_table audio_quirks_usb_audio;

const struct audio_quirk_table *const audio_builtin_quirk_tables[] = {
    &audio_quirks_hda_controllers,
    &audio_quirks_hda_realtek,
    &audio_quirks_hda_freebsd,
    &audio_quirks_hda_linux,
    &audio_quirks_ac97_controllers,
    &audio_quirks_ac97_codecs,
    &audio_quirks_usb_audio,
};

const uint32_t audio_builtin_quirk_table_count =
    (uint32_t)(sizeof(audio_builtin_quirk_tables) / sizeof(audio_builtin_quirk_tables[0]));
