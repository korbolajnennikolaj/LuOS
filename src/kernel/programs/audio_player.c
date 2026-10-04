#include "audio_player.h"

#include "service_root_fs.h"

#include "components/drivers.h"
#include "components/logger.h"
#include "kernel/rootfs.h"
#include "kernel/scheduler/scheduler.h"
#include "kernel/scheduler/spinlock.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PLAYER_CHUNK 8192
#define PLAYER_IDLE_SLEEP_MS 5
#define PLAYER_STOP_TIMEOUT_MS 3000
#define PLAYER_PREV_RESTART_MS 3000
#define PLAYER_TASK_PRIORITY 4

enum player_track_result {
    TRACK_DONE,
    TRACK_NEXT,
    TRACK_PREV,
    TRACK_STOP,
    TRACK_SKIP,
    TRACK_FATAL,
};

static char player_dir[FS_MAX_PATH];
static char player_names[AUDIO_PLAYER_MAX_TRACKS][FS_MAX_NAME + 1];
static int player_count = 0;
static volatile int player_index = 0;
static volatile int player_device = -1;
static volatile int player_state = AUDIO_PLAYER_IDLE;
static volatile bool player_running = false;
static volatile bool player_stop_req = false;
static volatile bool player_pause_req = false;
static volatile bool player_next_req = false;
static volatile bool player_prev_req = false;
static volatile bool player_loop = false;
static struct audio_format player_fmt;
static volatile uint64_t player_fed = 0;
static volatile uint32_t player_data_bytes = 0;
static volatile uint32_t player_tracks_played = 0;
static volatile uint32_t player_errors = 0;
static spinlock_t player_lock = SPINLOCK_INIT;
static uint8_t player_buf[PLAYER_CHUNK] __attribute__((aligned(64)));

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int audio_wav_parse_header(fs_file_t *f, struct audio_format *fmt, uint32_t *data_bytes, uint64_t *data_offset) {
    uint8_t hdr[12];
    if (fs_read(f, hdr, 12) != 12) return -1;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return -2;

    bool have_fmt = false;
    uint64_t pos = 12;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t ch[8];
        if (fs_read(f, ch, 8) != 8) return -3;
        pos += 8;
        uint32_t size = le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fb[40];
            uint32_t take = size < sizeof(fb) ? size : (uint32_t)sizeof(fb);
            if (take < 16 || fs_read(f, fb, take) != (int64_t)take) return -4;
            uint16_t tag = le16(fb);
            if (tag == 0xFFFE && take >= 26) tag = le16(fb + 24);
            if (tag != 1) return -5;
            fmt->channels = (uint8_t)le16(fb + 2);
            fmt->sample_rate = le32(fb + 4);
            fmt->bits_per_sample = (uint8_t)le16(fb + 14);
            switch (fmt->bits_per_sample) {
                case 8: fmt->format = AUDIO_FMT_U8; break;
                case 16: fmt->format = AUDIO_FMT_S16_LE; break;
                case 24: fmt->format = AUDIO_FMT_S24_3LE; break;
                case 32: fmt->format = AUDIO_FMT_S32_LE; break;
                default: return -6;
            }
            if (fmt->channels == 0 || fmt->channels > AUDIO_MAX_CHANNELS) return -6;
            have_fmt = true;
            pos += take;
            uint64_t skip = size - take + (size & 1);
            if (skip) {
                pos += skip;
                if (fs_seek(f, pos) != FS_OK) return -8;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) return -7;
            *data_bytes = size;
            if (data_offset) *data_offset = pos;
            return 0;
        } else {
            pos += size + (size & 1);
            if (fs_seek(f, pos) != FS_OK) return -8;
        }
    }
    return -9;
}

static struct audio_core_driver *player_core(void) {
    return (struct audio_core_driver *)get_self_driver(AUDIO_DRIVER, AUDIO_CORE_SLOT);
}

static bool player_has_wav_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *e = name + n - 4;
    return e[0] == '.' && tolower((unsigned char)e[1]) == 'w' && tolower((unsigned char)e[2]) == 'a' &&
           tolower((unsigned char)e[3]) == 'v';
}

static int player_name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void player_track_path(int idx, char *out, size_t cap) {
    if (strcmp(player_dir, "/") == 0) snprintf(out, cap, "/%s", player_names[idx]);
    else snprintf(out, cap, "%s/%s", player_dir, player_names[idx]);
}

static void player_split(const char *abs, char *dir, size_t dir_cap, char *name, size_t name_cap) {
    const char *slash = strrchr(abs, '/');
    if (!slash || slash == abs) {
        snprintf(dir, dir_cap, "/");
        snprintf(name, name_cap, "%s", slash ? slash + 1 : abs);
        return;
    }
    size_t dl = (size_t)(slash - abs);
    if (dl >= dir_cap) dl = dir_cap - 1;
    memcpy(dir, abs, dl);
    dir[dl] = '\0';
    snprintf(name, name_cap, "%s", slash + 1);
}

static bool player_interrupt(enum player_track_result *out) {
    if (player_stop_req) { *out = TRACK_STOP; return true; }
    if (player_next_req) { player_next_req = false; *out = TRACK_NEXT; return true; }
    if (player_prev_req) { player_prev_req = false; *out = TRACK_PREV; return true; }
    return false;
}

static void player_apply_pause(struct audio_core_driver *core, int dev) {
    if (player_pause_req && player_state == AUDIO_PLAYER_PLAYING) {
        if (core->get_state(dev, AUDIO_DIR_PLAYBACK) == AUDIO_STATE_PLAYING) core->pause(dev, AUDIO_DIR_PLAYBACK, true);
        player_state = AUDIO_PLAYER_PAUSED;
    } else if (!player_pause_req && player_state == AUDIO_PLAYER_PAUSED) {
        if (core->get_state(dev, AUDIO_DIR_PLAYBACK) == AUDIO_STATE_PAUSED) core->pause(dev, AUDIO_DIR_PLAYBACK, false);
        player_state = AUDIO_PLAYER_PLAYING;
    }
}

static enum player_track_result player_play_track(struct audio_core_driver *core, int idx) {
    char path[FS_MAX_PATH], rel[FS_MAX_PATH], rel_check[FS_MAX_PATH];
    player_track_path(idx, path, sizeof(path));
    int dev = player_device;

    fs_file_t f;
    struct audio_format fmt = { 0 };
    uint32_t data_bytes = 0;

    fs_lock();
    fs_t *fs = rootfs_resolve(path, rel, sizeof(rel));
    if (!fs || fs_open(fs, rel, false, false, &f) != FS_OK) {
        fs_unlock();
        LOG_WARNING("player: cannot open %s", path);
        return TRACK_SKIP;
    }
    int hr = audio_wav_parse_header(&f, &fmt, &data_bytes, NULL);
    if (hr != 0) {
        fs_close(&f);
        fs_unlock();
        LOG_WARNING("player: %s is not a PCM WAV file (%d), skipped", path, hr);
        return TRACK_SKIP;
    }
    fs_unlock();

    uint32_t fb = audio_frame_bytes(&fmt);
    int r = core->open(dev, AUDIO_DIR_PLAYBACK, &fmt);
    if (r != AUDIO_OK) {
        fs_lock();
        if (rootfs_resolve(path, rel_check, sizeof(rel_check)) == fs) fs_close(&f);
        fs_unlock();
        LOG_WARNING("player: audio%d refused %s (%d)", dev, path, r);
        return (r == AUDIO_ERR_NODEV || r == AUDIO_ERR_BUSY) ? TRACK_FATAL : TRACK_SKIP;
    }

    spin_lock(&player_lock);
    player_fmt = fmt;
    player_data_bytes = data_bytes;
    player_fed = 0;
    player_index = idx;
    spin_unlock(&player_lock);
    if (player_state == AUDIO_PLAYER_IDLE) player_state = AUDIO_PLAYER_PLAYING;
    LOG_DEBUG("player: audio%d <- %s (%u Hz %u ch %s)", dev, path, fmt.sample_rate, (unsigned)fmt.channels,
              audio_format_name(fmt.format));

    uint32_t chunk_max = PLAYER_CHUNK - (PLAYER_CHUNK % fb);
    uint32_t left = data_bytes;
    uint32_t pending = 0;
    uint32_t pending_off = 0;
    bool file_ok = true;
    enum player_track_result result = TRACK_DONE;
    struct audio_device *adev = audio_get_device(dev);

    while (true) {
        if (player_interrupt(&result)) break;
        player_apply_pause(core, dev);
        if (player_state == AUDIO_PLAYER_PAUSED) {
            scheduler_sleep_ms(20);
            continue;
        }

        if (pending == 0 && left > 0 && file_ok) {
            uint32_t want = left < chunk_max ? left : chunk_max;
            fs_lock();
            if (rootfs_resolve(path, rel_check, sizeof(rel_check)) != fs) {
                fs_unlock();
                LOG_WARNING("player: %s disappeared (unmounted)", path);
                result = TRACK_FATAL;
                break;
            }
            int64_t n = fs_read(&f, player_buf, want);
            fs_unlock();
            if (n <= 0) {
                file_ok = false;
                left = 0;
            } else {
                pending = (uint32_t)n;
                pending_off = 0;
                left -= (uint32_t)n;
            }
        }

        if (pending > 0) {
            int w = core->write(dev, player_buf + pending_off, pending);
            if (w < 0) {
                LOG_WARNING("player: audio%d went away (%d)", dev, w);
                result = TRACK_FATAL;
                break;
            }
            pending -= (uint32_t)w;
            pending_off += (uint32_t)w;
            player_fed += (uint32_t)w;
        }

        struct audio_stream *s = adev ? &adev->streams[AUDIO_DIR_PLAYBACK] : NULL;
        bool eof = (left == 0 && pending == 0);
        if (core->get_state(dev, AUDIO_DIR_PLAYBACK) == AUDIO_STATE_STOPPED && s &&
            (eof || audio_stream_fill(s) * 2 >= s->ring_size))
            core->start(dev, AUDIO_DIR_PLAYBACK);

        if (eof) {
            if (!s || audio_stream_fill(s) < fb) break;
            scheduler_sleep_ms(PLAYER_IDLE_SLEEP_MS);
            continue;
        }
        if (pending > 0) scheduler_sleep_ms(PLAYER_IDLE_SLEEP_MS);
    }

    fs_lock();
    if (rootfs_resolve(path, rel_check, sizeof(rel_check)) == fs) fs_close(&f);
    fs_unlock();

    if (result == TRACK_DONE) {
        core->drain(dev, 2000);
        player_tracks_played++;
    } else {
        core->stop(dev, AUDIO_DIR_PLAYBACK);
    }
    core->close(dev, AUDIO_DIR_PLAYBACK);
    if (!file_ok && result == TRACK_DONE) LOG_DEBUG("player: %s ended early (short read)", path);
    return result;
}

static void player_entry(void *arg) {
    (void)arg;
    struct audio_core_driver *core = player_core();
    int idx = player_index;

    while (core && !player_stop_req && idx >= 0 && idx < player_count) {
        enum player_track_result r = player_play_track(core, idx);
        if (r == TRACK_STOP || r == TRACK_FATAL) break;
        if (r == TRACK_SKIP) player_errors++;
        if (r == TRACK_PREV) {
            struct audio_player_status st;
            audio_player_get_status(&st);
            if (st.elapsed_ms < PLAYER_PREV_RESTART_MS && idx > 0) idx--;
        } else {
            idx++;
        }
        if (idx >= player_count && player_loop && (r != TRACK_SKIP || player_errors < (uint32_t)player_count)) idx = 0;
        if (idx >= player_count) break;
        spin_lock(&player_lock);
        player_index = idx;
        player_fed = 0;
        player_data_bytes = 0;
        spin_unlock(&player_lock);
    }

    player_state = AUDIO_PLAYER_IDLE;
    player_pause_req = false;
    player_running = false;
}

static int player_build_list(const char *abs) {
    char rel[FS_MAX_PATH];
    fs_t *fs = rootfs_resolve(abs, rel, sizeof(rel));
    if (!fs) return AUDIO_PLAYER_ERR_NOENT;

    fs_dirent_t st;
    bool is_root = strcmp(rel, "/") == 0;
    if (!is_root && fs_stat(fs, rel, &st) != FS_OK) return AUDIO_PLAYER_ERR_NOENT;

    if (is_root || st.type == FS_ENTRY_DIR) {
        fs_dir_t d;
        if (fs_opendir(fs, rel, &d) != FS_OK) return AUDIO_PLAYER_ERR_NOENT;
        player_count = 0;
        fs_dirent_t e;
        while (player_count < AUDIO_PLAYER_MAX_TRACKS && fs_readdir(&d, &e) == FS_OK) {
            if (e.type == FS_ENTRY_DIR || !player_has_wav_ext(e.name)) continue;
            snprintf(player_names[player_count], sizeof(player_names[0]), "%s", e.name);
            player_count++;
        }
        fs_closedir(&d);
        for (int i = 1; i < player_count; i++) {
            char tmp[FS_MAX_NAME + 1];
            memcpy(tmp, player_names[i], sizeof(tmp));
            int j = i - 1;
            while (j >= 0 && player_name_cmp(player_names[j], tmp) > 0) {
                memcpy(player_names[j + 1], player_names[j], sizeof(tmp));
                j--;
            }
            memcpy(player_names[j + 1], tmp, sizeof(tmp));
        }
        snprintf(player_dir, sizeof(player_dir), "%s", abs);
        size_t dl = strlen(player_dir);
        if (dl > 1 && player_dir[dl - 1] == '/') player_dir[dl - 1] = '\0';
        return player_count ? AUDIO_PLAYER_OK : AUDIO_PLAYER_ERR_NOFILES;
    }

    player_split(abs, player_dir, sizeof(player_dir), player_names[0], sizeof(player_names[0]));
    player_count = 1;
    return AUDIO_PLAYER_OK;
}

int audio_player_play(const char *path, int dev_idx) {
    if (!path || !path[0]) return AUDIO_PLAYER_ERR_PARAM;
    struct audio_core_driver *core = player_core();
    if (!core) return AUDIO_PLAYER_ERR_NODEV;
    int dev = dev_idx >= 0 ? dev_idx : core->get_default_device();
    struct audio_device *adev = audio_get_device(dev);
    if (!adev || !adev->has_direction[AUDIO_DIR_PLAYBACK]) return AUDIO_PLAYER_ERR_NODEV;
    if (!rootfs_is_mounted()) return AUDIO_PLAYER_ERR_NOENT;

    audio_player_stop();

    char abs[FS_MAX_PATH];
    rootfs_normalize_path(path, abs, sizeof(abs));
    fs_lock();
    int r = player_build_list(abs);
    fs_unlock();
    if (r != AUDIO_PLAYER_OK) return r;

    player_device = dev;
    player_index = 0;
    player_fed = 0;
    player_data_bytes = 0;
    player_tracks_played = 0;
    player_errors = 0;
    memset(&player_fmt, 0, sizeof(player_fmt));
    player_stop_req = false;
    player_pause_req = false;
    player_next_req = false;
    player_prev_req = false;
    player_state = AUDIO_PLAYER_PLAYING;
    player_running = true;

    if (!task_create_ex("audio_player", player_entry, NULL, PLAYER_TASK_PRIORITY, TASK_ANY_CORE, 0)) {
        player_running = false;
        player_state = AUDIO_PLAYER_IDLE;
        return AUDIO_PLAYER_ERR_TASK;
    }
    return AUDIO_PLAYER_OK;
}

int audio_player_stop(void) {
    if (!player_running) return AUDIO_PLAYER_ERR_IDLE;
    player_stop_req = true;
    for (uint32_t waited = 0; player_running && waited < PLAYER_STOP_TIMEOUT_MS; waited += 5)
        scheduler_sleep_ms(5);
    if (player_running) LOG_WARNING("player: task did not stop in %u ms", PLAYER_STOP_TIMEOUT_MS);
    return AUDIO_PLAYER_OK;
}

int audio_player_pause(bool paused) {
    if (!player_running) return AUDIO_PLAYER_ERR_IDLE;
    player_pause_req = paused;
    return AUDIO_PLAYER_OK;
}

int audio_player_next(void) {
    if (!player_running) return AUDIO_PLAYER_ERR_IDLE;
    player_pause_req = false;
    player_next_req = true;
    return AUDIO_PLAYER_OK;
}

int audio_player_prev(void) {
    if (!player_running) return AUDIO_PLAYER_ERR_IDLE;
    player_pause_req = false;
    player_prev_req = true;
    return AUDIO_PLAYER_OK;
}

void audio_player_set_loop(bool loop) {
    player_loop = loop;
}

bool audio_player_active(void) {
    return player_running;
}

bool audio_player_uses_path(const char *mount_point) {
    if (!player_running || !mount_point) return false;
    if (strcmp(mount_point, "/") == 0) return true;
    size_t n = strlen(mount_point);
    return strncmp(player_dir, mount_point, n) == 0 && (player_dir[n] == '\0' || player_dir[n] == '/');
}

void audio_player_get_status(struct audio_player_status *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    spin_lock(&player_lock);
    out->state = player_running ? player_state : AUDIO_PLAYER_IDLE;
    out->device = player_device;
    out->track = player_index;
    out->track_count = player_count;
    out->fmt = player_fmt;
    out->loop = player_loop;
    out->tracks_played = player_tracks_played;
    out->errors = player_errors;
    snprintf(out->dir, sizeof(out->dir), "%s", player_dir);
    if (player_count > 0 && player_index >= 0 && player_index < player_count)
        snprintf(out->file, sizeof(out->file), "%s", player_names[player_index]);
    uint32_t bps = audio_frame_bytes(&player_fmt) * player_fmt.sample_rate;
    uint64_t fed = player_fed;
    uint32_t total = player_data_bytes;
    spin_unlock(&player_lock);

    struct audio_device *d = audio_get_device(out->device);
    uint32_t queued = (d && d->streams[AUDIO_DIR_PLAYBACK].opened) ? audio_stream_fill(&d->streams[AUDIO_DIR_PLAYBACK]) : 0;
    uint64_t played = fed > queued ? fed - queued : 0;
    if (bps) {
        out->elapsed_ms = (uint32_t)((played * 1000) / bps);
        out->total_ms = (uint32_t)(((uint64_t)total * 1000) / bps);
    }
}
