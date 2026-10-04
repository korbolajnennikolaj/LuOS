#include "shell_audio.h"

#include "audio_player.h"
#include "service_audio.h"
#include "service_root_fs.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "components/Memory/heap.h"
#include "drivers/Audio/audio_core.h"
#include "drivers/Audio/audio_quirks.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Video/limine_video_driver.h"
#include "fs/fs.h"
#include "kernel/rootfs.h"
#include "kernel/scheduler/scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_SHELL_CHUNK 16384
#define AUDIO_REC_MAX_SEC 600
#define AUDIO_REC_MEM_MAX (32u * 1024u * 1024u)

static uint8_t audio_shell_buf[AUDIO_SHELL_CHUNK] __attribute__((aligned(64)));

static struct audio_core_driver *audio_shell_core(void) {
    struct audio_core_driver *core = get_self_driver(AUDIO_DRIVER, AUDIO_CORE_SLOT);
    if (!core) printf_color(LIMINE_COLOR_LIGHT_RED, "Audio core is not loaded.\n");
    return core;
}

static const char *audio_shell_skip(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static const char *audio_shell_word(const char *s, char *out, size_t cap) {
    s = audio_shell_skip(s);
    size_t n = 0;
    while (*s && *s != ' ') {
        if (n + 1 < cap) out[n++] = *s;
        s++;
    }
    out[n] = '\0';
    return s;
}

static bool audio_shell_number(const char *s, long *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (!end || end == s || *end) return false;
    *out = v;
    return true;
}

static bool audio_shell_abort_requested(void) {
    struct keyboard_driver *kbd = get_self_driver(KEYBOARD_DRIVER, VIRTUAL_KEYBOARD);
    if (!kbd || !kbd->get_key_event) return false;
    struct key_event ev;
    bool stop = false;
    while (kbd->get_key_event(&ev)) {
        if (ev.code == KEY_ESC || ev.code == KEY_Q || (ev.ctrl && ev.code == KEY_C)) stop = true;
    }
    return stop;
}

static void audio_shell_rates(const struct audio_device *dev, uint8_t dir, char *buf, size_t cap) {
    static const uint32_t rates[AUDIO_RATE_COUNT] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    size_t pos = 0;
    buf[0] = '\0';
    if (dev->fixed_rate) {
        snprintf(buf, cap, "%u (fixed)", dev->fixed_rate);
        return;
    }
    if (dev->rates[dir] & AUDIO_RATE_CONTINUOUS) {
        int w = snprintf(buf, cap, "%u-%u", dev->rate_min[dir], dev->rate_max[dir]);
        if (w > 0) pos = (size_t)w;
    }
    for (int i = 0; i < AUDIO_RATE_COUNT && pos + 8 < cap; i++) {
        if (!(dev->rates[dir] & (1u << i))) continue;
        if (dev->rates[dir] & AUDIO_RATE_CONTINUOUS) continue;
        int w = snprintf(buf + pos, cap - pos, "%s%u", pos ? "," : "", rates[i]);
        if (w < 0) break;
        pos += (size_t)w;
    }
}

static void audio_shell_formats(uint16_t mask, char *buf, size_t cap) {
    size_t pos = 0;
    buf[0] = '\0';
    for (uint8_t f = 0; f < AUDIO_FMT_COUNT; f++) {
        if (!(mask & AUDIO_FMT_MASK(f))) continue;
        int w = snprintf(buf + pos, cap - pos, "%s%s", pos ? "," : "", audio_format_name(f));
        if (w < 0 || (size_t)w >= cap - pos) break;
        pos += (size_t)w;
    }
}

static void audio_shell_list(void) {
    struct audio_core_driver *core = audio_shell_core();
    if (!core) return;
    int def = core->get_default_device();
    int found = 0;

    printf_color(LIMINE_COLOR_CYAN, "\n=== Audio devices ===\n");
    for (int i = 0; i < audio_get_device_count(); i++) {
        struct audio_device *d = audio_get_device(i);
        if (!d) continue;
        found++;
        printf_color(i == def ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_WHITE, " %c audio%d ", i == def ? '*' : ' ', i);
        printf_color(LIMINE_COLOR_AMBER, "[%s] ", audio_type_name(d->type));
        printf_color(LIMINE_COLOR_WHITE, "%s", d->name);
        printf_color(LIMINE_COLOR_DARK_GRAY, " - %s\n", d->description);
        for (uint8_t dir = 0; dir < AUDIO_DIR_COUNT; dir++) {
            if (!d->has_direction[dir]) continue;
            char rates[96], fmts[48];
            audio_shell_rates(d, dir, rates, sizeof(rates));
            audio_shell_formats(d->formats[dir], fmts, sizeof(fmts));
            struct audio_stream *s = &d->streams[dir];
            printf_color(LIMINE_COLOR_LIGHT_GRAY, "     %-8s %s Hz, %s, up to %u ch, vol %u%%%s%s, %s\n",
                         dir ? "capture" : "playback", rates, fmts, (unsigned)d->max_channels[dir], (unsigned)s->volume,
                         s->muted ? " muted" : "", s->opened && s->sw_volume ? " (soft)" : "",
                         s->opened ? audio_state_name(s->state) : "closed");
        }
        if (d->quirk_names[0]) printf_color(LIMINE_COLOR_DARK_GRAY, "     quirks: %s\n", d->quirk_names);
    }
    if (!found) printf_color(LIMINE_COLOR_LIGHT_RED, " No audio devices found.\n");
    printf_color(LIMINE_COLOR_CYAN, "=====================\n");
}

static void audio_shell_line(const char *line) {
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "  %s\n", line);
}

static int audio_shell_pick_device(const char *arg) {
    struct audio_core_driver *core = get_self_driver(AUDIO_DRIVER, AUDIO_CORE_SLOT);
    long n;
    if (arg && *arg && audio_shell_number(arg, &n)) return (int)n;
    return core ? core->get_default_device() : -1;
}

static void audio_shell_info(const char *arg) {
    int idx = audio_shell_pick_device(audio_shell_skip(arg));
    struct audio_device *d = audio_get_device(idx);
    if (!d) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "No such audio device.\n");
        return;
    }
    printf_color(LIMINE_COLOR_CYAN, "audio%d: %s (%s)\n", idx, d->name, audio_type_name(d->type));
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "  %s\n  codec %s id %08x, device %08x subsys %08x flags 0x%x\n", d->description,
                 d->codec_name, d->codec_id, d->vendor_id, d->subsystem_id, (unsigned)d->flags);
    if (d->quirk_names[0]) printf_color(LIMINE_COLOR_LIGHT_GRAY, "  matched quirks: %s\n", d->quirk_names);
    for (uint8_t dir = 0; dir < AUDIO_DIR_COUNT; dir++) {
        struct audio_stream *s = &d->streams[dir];
        if (!s->opened) continue;
        printf_color(LIMINE_COLOR_LIGHT_GRAY,
                     "  %s: %s, %u Hz %u ch %s -> hw %u Hz %u ch %s, ring %u/%u, frames %llu, underruns %u, overruns %u%s\n",
                     dir ? "capture" : "playback", audio_state_name(s->state), s->fmt.sample_rate, (unsigned)s->fmt.channels,
                     audio_format_name(s->fmt.format), s->hw_fmt.sample_rate, (unsigned)s->hw_fmt.channels,
                     audio_format_name(s->hw_fmt.format), audio_stream_fill(s), s->ring_size,
                     (unsigned long long)s->frames_done, s->underruns, s->overruns, s->sw_volume ? ", soft volume" : "");
    }
    if (d->ops && d->ops->describe) d->ops->describe(d, audio_shell_line);
    printf_color(LIMINE_COLOR_DARK_GRAY, "  audio service: %llu polls, worst gap %llu ms\n",
                 (unsigned long long)audio_service_poll_count(), (unsigned long long)audio_service_max_gap_ms());
}

static void audio_shell_beep(const char *arg) {
    char w1[16], w2[16], w3[16];
    const char *p = audio_shell_word(arg, w1, sizeof(w1));
    p = audio_shell_word(p, w2, sizeof(w2));
    audio_shell_word(p, w3, sizeof(w3));
    long freq = 440, ms = 1000, dev = -1;
    if (w1[0] && !audio_shell_number(w1, &freq)) freq = 440;
    if (w2[0] && !audio_shell_number(w2, &ms)) ms = 1000;
    if (w3[0] && !audio_shell_number(w3, &dev)) dev = -1;
    if (freq < 20 || freq > 20000 || ms <= 0 || ms > 60000) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: audio-beep [FREQ 20-20000] [MS] [DEV]\n");
        return;
    }
    if (!audio_shell_core()) return;
    int idx = (dev >= 0) ? (int)dev : audio_shell_pick_device(NULL);
    printf_color(LIMINE_COLOR_WHITE, "Tone %ld Hz for %ld ms on audio%d...", freq, ms, idx);
    int r = audio_generate_tone(idx, (uint32_t)freq, (uint32_t)ms, 60);
    if (r == AUDIO_OK) printf_color(LIMINE_COLOR_LIGHT_GREEN, " done\n");
    else printf_color(LIMINE_COLOR_LIGHT_RED, " failed (%d)\n", r);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static fs_t *audio_shell_open_path(const char *arg, char *rel, size_t rel_cap, char *abs, size_t abs_cap) {
    if (!rootfs_is_mounted()) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Nothing is mounted. Use 'mount <disk>'.\n");
        return NULL;
    }
    rootfs_normalize_path(arg, abs, abs_cap);
    fs_t *fs = rootfs_resolve(abs, rel, rel_cap);
    if (!fs) printf_color(LIMINE_COLOR_LIGHT_RED, "No filesystem mounted for '%s'.\n", abs);
    return fs;
}

static void audio_shell_time(uint32_t ms, char *buf, size_t cap) {
    uint32_t sec = ms / 1000;
    if (sec >= 3600) snprintf(buf, cap, "%u:%02u:%02u", sec / 3600, (sec / 60) % 60, sec % 60);
    else snprintf(buf, cap, "%u:%02u", sec / 60, sec % 60);
}

static void audio_shell_playing(void) {
    struct audio_player_status st;
    audio_player_get_status(&st);
    if (st.track_count == 0) {
        printf_color(LIMINE_COLOR_DARK_GRAY, "Nothing is playing. Use: play FILE.wav | play DIR\n");
        return;
    }
    const char *state = st.state == AUDIO_PLAYER_PLAYING ? "playing" : st.state == AUDIO_PLAYER_PAUSED ? "paused" : "stopped";
    uint32_t color = st.state == AUDIO_PLAYER_PLAYING ? LIMINE_COLOR_LIGHT_GREEN
                   : st.state == AUDIO_PLAYER_PAUSED ? LIMINE_COLOR_AMBER : LIMINE_COLOR_DARK_GRAY;
    printf_color(color, "%s ", st.state == AUDIO_PLAYER_IDLE ? "Last played:" : "Now playing:");
    printf_color(LIMINE_COLOR_WHITE, "%s", st.file);
    printf_color(color, " [%s]\n", state);
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "  directory: ");
    printf_color(LIMINE_COLOR_CYAN, "%s\n", st.dir);
    char el[16], tot[16];
    audio_shell_time(st.elapsed_ms, el, sizeof(el));
    audio_shell_time(st.total_ms, tot, sizeof(tot));
    struct audio_device *d = audio_get_device(st.device);
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "  track %d/%d  %s / %s  on audio%d (%s)\n", st.track + 1, st.track_count, el, tot,
                 st.device, d ? d->name : "gone");
    if (st.fmt.sample_rate)
        printf_color(LIMINE_COLOR_LIGHT_GRAY, "  %u Hz, %u ch, %s, loop %s, %u done, %u skipped\n", st.fmt.sample_rate,
                     (unsigned)st.fmt.channels, audio_format_name(st.fmt.format), st.loop ? "on" : "off",
                     st.tracks_played, st.errors);
}

static void audio_shell_play(const char *arg) {
    char path_arg[FS_MAX_PATH], devw[16];
    const char *p = audio_shell_word(arg, path_arg, sizeof(path_arg));
    audio_shell_word(p, devw, sizeof(devw));
    if (!path_arg[0]) {
        if (audio_player_active()) {
            audio_player_pause(false);
            audio_shell_playing();
        } else {
            printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: play FILE.wav|DIR [DEV]\n");
        }
        return;
    }
    if (!audio_shell_core()) return;
    long devn = -1;
    if (devw[0] && !audio_shell_number(devw, &devn)) devn = -1;

    int r = audio_player_play(path_arg, (int)devn);
    switch (r) {
        case AUDIO_PLAYER_OK:
            scheduler_sleep_ms(50);
            audio_shell_playing();
            return;
        case AUDIO_PLAYER_ERR_NOENT: printf_color(LIMINE_COLOR_LIGHT_RED, "play: '%s' not found (is a disk mounted?)\n", path_arg); return;
        case AUDIO_PLAYER_ERR_NOFILES: printf_color(LIMINE_COLOR_LIGHT_RED, "play: no .wav files in '%s'\n", path_arg); return;
        case AUDIO_PLAYER_ERR_NODEV: printf_color(LIMINE_COLOR_LIGHT_RED, "play: no audio output device\n"); return;
        default: printf_color(LIMINE_COLOR_LIGHT_RED, "play: failed (%d)\n", r); return;
    }
}

static void audio_shell_player_stop(void) {
    if (audio_player_stop() == AUDIO_PLAYER_OK) printf_color(LIMINE_COLOR_AMBER, "Playback stopped.\n");
    else printf_color(LIMINE_COLOR_DARK_GRAY, "Nothing is playing.\n");
}

static void audio_shell_player_pause(bool paused) {
    if (audio_player_pause(paused) != AUDIO_PLAYER_OK) {
        printf_color(LIMINE_COLOR_DARK_GRAY, "Nothing is playing.\n");
        return;
    }
    printf_color(LIMINE_COLOR_AMBER, "%s\n", paused ? "Paused." : "Resumed.");
}

static void audio_shell_player_skip(bool forward) {
    int r = forward ? audio_player_next() : audio_player_prev();
    if (r != AUDIO_PLAYER_OK) {
        printf_color(LIMINE_COLOR_DARK_GRAY, "Nothing is playing.\n");
        return;
    }
    scheduler_sleep_ms(150);
    audio_shell_playing();
}

static void audio_shell_loop(const char *arg) {
    arg = audio_shell_skip(arg);
    struct audio_player_status st;
    audio_player_get_status(&st);
    bool want = !st.loop;
    if (strcmp(arg, "on") == 0) want = true;
    else if (strcmp(arg, "off") == 0) want = false;
    audio_player_set_loop(want);
    printf_color(LIMINE_COLOR_WHITE, "Playlist loop %s\n", want ? "on" : "off");
}

static void audio_shell_record(const char *arg) {
    char path_arg[FS_MAX_PATH], secw[16], devw[16];
    const char *p = audio_shell_word(arg, path_arg, sizeof(path_arg));
    p = audio_shell_word(p, secw, sizeof(secw));
    audio_shell_word(p, devw, sizeof(devw));
    long sec = 5;
    if (!path_arg[0] || (secw[0] && (!audio_shell_number(secw, &sec) || sec <= 0 || sec > AUDIO_REC_MAX_SEC))) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: audio-rec FILE.wav [SECONDS] [DEV]\n");
        return;
    }
    struct audio_core_driver *core = audio_shell_core();
    if (!core) return;
    int idx = audio_shell_pick_device(devw);
    struct audio_device *d = audio_get_device(idx);
    if (!d || !d->has_direction[AUDIO_DIR_CAPTURE]) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "audio%d cannot record.\n", idx);
        return;
    }

    char abs[FS_MAX_PATH], rel[FS_MAX_PATH];
    fs_t *fs = audio_shell_open_path(path_arg, rel, sizeof(rel), abs, sizeof(abs));
    if (!fs) return;

    struct audio_format fmt = { .sample_rate = 48000, .channels = 2, .bits_per_sample = 16, .format = AUDIO_FMT_S16_LE };
    int r = core->open(idx, AUDIO_DIR_CAPTURE, &fmt);
    if (r != AUDIO_OK) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "audio-rec: cannot open capture on audio%d (%d)\n", idx, r);
        return;
    }

    fs_file_t f;
    if (fs_open(fs, rel, true, true, &f) != FS_OK) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "audio-rec: cannot create '%s'\n", abs);
        core->close(idx, AUDIO_DIR_CAPTURE);
        return;
    }

    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    put32(hdr + 4, 36);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    put32(hdr + 16, 16);
    put16(hdr + 20, 1);
    put16(hdr + 22, fmt.channels);
    put32(hdr + 24, fmt.sample_rate);
    put32(hdr + 28, fmt.sample_rate * audio_frame_bytes(&fmt));
    put16(hdr + 32, (uint16_t)audio_frame_bytes(&fmt));
    put16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, 0);
    fs_write(&f, hdr, 44);

    uint32_t total = (uint32_t)sec * fmt.sample_rate * audio_frame_bytes(&fmt);
    uint32_t done = 0;
    uint8_t *mem = (total <= AUDIO_REC_MEM_MAX) ? (uint8_t *)kmalloc(total) : NULL;
    printf_color(LIMINE_COLOR_WHITE, "Recording %ld s from audio%d into %s (q/Esc stops)...\n", sec, idx, abs);
    core->start(idx, AUDIO_DIR_CAPTURE);
    while (done < total) {
        uint32_t want = total - done;
        if (want > 4096) want = 4096;
        int n = core->read_blocking(idx, mem ? mem + done : audio_shell_buf, want, 2000);
        if (n <= 0) {
            printf_color(LIMINE_COLOR_LIGHT_RED, "audio-rec: no data from device\n");
            break;
        }
        if (!mem) fs_write(&f, audio_shell_buf, (uint64_t)n);
        done += (uint32_t)n;
        if (audio_shell_abort_requested()) break;
    }
    core->stop(idx, AUDIO_DIR_CAPTURE);
    core->close(idx, AUDIO_DIR_CAPTURE);
    if (mem) {
        printf_color(LIMINE_COLOR_WHITE, "Writing %u bytes...\n", done);
        for (uint32_t off = 0; off < done;) {
            uint32_t chunk = done - off > 65536 ? 65536 : done - off;
            fs_write(&f, mem + off, chunk);
            off += chunk;
        }
        kfree(mem);
    }

    put32(hdr + 4, 36 + done);
    put32(hdr + 40, done);
    fs_seek(&f, 0);
    fs_write(&f, hdr, 44);
    fs_close(&f);
    printf_color(LIMINE_COLOR_LIGHT_GREEN, "Saved %u bytes (%u.%u s)\n", done, done / (fmt.sample_rate * 4),
                 (done % (fmt.sample_rate * 4)) * 10 / (fmt.sample_rate * 4));
}

static void audio_shell_volume(const char *arg) {
    struct audio_core_driver *core = audio_shell_core();
    if (!core) return;
    char w1[16], w2[16];
    const char *p = audio_shell_word(arg, w1, sizeof(w1));
    audio_shell_word(p, w2, sizeof(w2));
    long a = -1, b = -1;
    bool h1 = w1[0] && audio_shell_number(w1, &a);
    bool h2 = w2[0] && audio_shell_number(w2, &b);
    int idx = h2 ? (int)a : core->get_default_device();
    long vol = h2 ? b : a;
    if (!audio_get_device(idx)) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "No such audio device.\n");
        return;
    }
    if (h1 && (vol < 0 || vol > 100)) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "Usage: audio-vol [DEV] [0-100]\n");
        return;
    }
    if (h1) core->set_volume(idx, AUDIO_DIR_PLAYBACK, (uint8_t)vol);
    printf_color(LIMINE_COLOR_WHITE, "audio%d playback volume: %d%%%s\n", idx, core->get_volume(idx, AUDIO_DIR_PLAYBACK),
                 core->get_mute(idx, AUDIO_DIR_PLAYBACK) == 1 ? " (muted)" : "");
}

static void audio_shell_mute(const char *arg) {
    struct audio_core_driver *core = audio_shell_core();
    if (!core) return;
    int idx = core->get_default_device();
    arg = audio_shell_skip(arg);
    int cur = core->get_mute(idx, AUDIO_DIR_PLAYBACK);
    if (cur < 0) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "No default audio device.\n");
        return;
    }
    bool want = !cur;
    if (strcmp(arg, "on") == 0) want = true;
    else if (strcmp(arg, "off") == 0) want = false;
    core->set_mute(idx, AUDIO_DIR_PLAYBACK, want);
    printf_color(LIMINE_COLOR_WHITE, "audio%d %s\n", idx, want ? "muted" : "unmuted");
}

static void audio_shell_default(const char *arg) {
    struct audio_core_driver *core = audio_shell_core();
    if (!core) return;
    long n;
    if (!audio_shell_number(audio_shell_skip(arg), &n)) {
        printf_color(LIMINE_COLOR_WHITE, "Default audio device: audio%d\n", core->get_default_device());
        return;
    }
    if (core->set_default_device((int)n) != AUDIO_OK) printf_color(LIMINE_COLOR_LIGHT_RED, "No such audio device.\n");
    else printf_color(LIMINE_COLOR_LIGHT_GREEN, "audio%ld is now the default device\n", n);
}

static void audio_shell_quirks(void) {
    printf_color(LIMINE_COLOR_CYAN, "=== Audio quirk tables ===\n");
    for (int i = 0; i < audio_quirk_table_count(); i++) {
        const struct audio_quirk_table *t = audio_quirk_get_table(i);
        if (!t) continue;
        printf_color(LIMINE_COLOR_LIGHT_GRAY, " #%d %-20s %-16s %u entries\n", i, t->name ? t->name : "?",
                     audio_quirk_domain_name(t->domain), (unsigned)t->count);
    }
    printf_color(LIMINE_COLOR_CYAN, "=== Matched per device ===\n");
    for (int i = 0; i < audio_get_device_count(); i++) {
        struct audio_device *d = audio_get_device(i);
        if (!d) continue;
        printf_color(LIMINE_COLOR_WHITE, " audio%d %s: ", i, d->name);
        printf_color(LIMINE_COLOR_LIGHT_GRAY, "%s\n", d->quirk_names[0] ? d->quirk_names : "(none)");
    }
}

static void audio_shell_stop(const char *arg) {
    struct audio_core_driver *core = audio_shell_core();
    if (!core) return;
    audio_player_stop();
    int idx = audio_shell_pick_device(audio_shell_skip(arg));
    core->stop(idx, AUDIO_DIR_PLAYBACK);
    core->close(idx, AUDIO_DIR_PLAYBACK);
    printf_color(LIMINE_COLOR_AMBER, "audio%d playback stopped\n", idx);
}

void shell_audio_help(void) {
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "\n-- Audio --\n");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "audio         - list audio devices", "audio-info [DEV] - device/codec details");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "play FILE|DIR [DEV] - background playback", "playing (np) - what plays, from which dir");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "stop / pause / resume - player control", "next / prev   - skip in the directory");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "loop [on|off] - repeat the playlist", "audio-beep [HZ] [MS] [DEV] - test tone");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "audio-rec FILE [SEC] [DEV] - record WAV", "audio-stop [DEV] - stop playback");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "audio-vol [DEV] [0-100] - volume", "audio-mute [on|off] - toggle mute");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "audio-default [DEV] - default device", "audio-quirks  - quirk tables / matches");
    printf_color(LIMINE_COLOR_CYAN, "%-46s %-40s\n", "audio-rescan  - probe for new devices", "");
}

bool shell_audio_command(const char *cmd) {
    if (strcmp(cmd, "audio") == 0 || strcmp(cmd, "audio-list") == 0) { audio_shell_list(); return true; }
    if (strcmp(cmd, "audio-info") == 0) { audio_shell_info(""); return true; }
    if (strncmp(cmd, "audio-info ", 11) == 0) { audio_shell_info(cmd + 11); return true; }
    if (strcmp(cmd, "audio-beep") == 0) { audio_shell_beep(""); return true; }
    if (strncmp(cmd, "audio-beep ", 11) == 0) { audio_shell_beep(cmd + 11); return true; }
    if (strncmp(cmd, "audio-play ", 11) == 0) { audio_shell_play(cmd + 11); return true; }
    if (strcmp(cmd, "audio-play") == 0) { audio_shell_play(""); return true; }
    if (strncmp(cmd, "play ", 5) == 0) { audio_shell_play(cmd + 5); return true; }
    if (strcmp(cmd, "play") == 0) { audio_shell_play(""); return true; }
    if (strcmp(cmd, "stop") == 0) { audio_shell_player_stop(); return true; }
    if (strcmp(cmd, "pause") == 0) { audio_shell_player_pause(true); return true; }
    if (strcmp(cmd, "resume") == 0) { audio_shell_player_pause(false); return true; }
    if (strcmp(cmd, "next") == 0) { audio_shell_player_skip(true); return true; }
    if (strcmp(cmd, "prev") == 0) { audio_shell_player_skip(false); return true; }
    if (strcmp(cmd, "playing") == 0 || strcmp(cmd, "np") == 0) { audio_shell_playing(); return true; }
    if (strcmp(cmd, "loop") == 0) { audio_shell_loop(""); return true; }
    if (strncmp(cmd, "loop ", 5) == 0) { audio_shell_loop(cmd + 5); return true; }
    if (strncmp(cmd, "audio-rec ", 10) == 0) { audio_shell_record(cmd + 10); return true; }
    if (strcmp(cmd, "audio-rec") == 0) { audio_shell_record(""); return true; }
    if (strcmp(cmd, "audio-vol") == 0) { audio_shell_volume(""); return true; }
    if (strncmp(cmd, "audio-vol ", 10) == 0) { audio_shell_volume(cmd + 10); return true; }
    if (strcmp(cmd, "audio-mute") == 0) { audio_shell_mute(""); return true; }
    if (strncmp(cmd, "audio-mute ", 11) == 0) { audio_shell_mute(cmd + 11); return true; }
    if (strcmp(cmd, "audio-default") == 0) { audio_shell_default(""); return true; }
    if (strncmp(cmd, "audio-default ", 14) == 0) { audio_shell_default(cmd + 14); return true; }
    if (strcmp(cmd, "audio-quirks") == 0) { audio_shell_quirks(); return true; }
    if (strcmp(cmd, "audio-stop") == 0) { audio_shell_stop(""); return true; }
    if (strncmp(cmd, "audio-stop ", 11) == 0) { audio_shell_stop(cmd + 11); return true; }
    if (strcmp(cmd, "audio-rescan") == 0) {
        struct audio_core_driver *core = audio_shell_core();
        if (core && core->scan_all) core->scan_all();
        audio_shell_list();
        return true;
    }
    return false;
}
