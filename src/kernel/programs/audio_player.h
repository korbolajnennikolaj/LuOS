#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "drivers/Audio/audio_core.h"
#include "fs/fs.h"

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_PLAYER_MAX_TRACKS 128

enum AUDIO_PLAYER_STATE {
    AUDIO_PLAYER_IDLE = 0,
    AUDIO_PLAYER_PLAYING = 1,
    AUDIO_PLAYER_PAUSED = 2,
};

#define AUDIO_PLAYER_OK 0
#define AUDIO_PLAYER_ERR_PARAM -1
#define AUDIO_PLAYER_ERR_NOENT -2
#define AUDIO_PLAYER_ERR_NOFILES -3
#define AUDIO_PLAYER_ERR_NODEV -4
#define AUDIO_PLAYER_ERR_TASK -5
#define AUDIO_PLAYER_ERR_IDLE -6

typedef struct audio_player_status {
    int state;
    int device;
    int track;
    int track_count;
    char dir[FS_MAX_PATH];
    char file[FS_MAX_NAME + 1];
    struct audio_format fmt;
    uint32_t elapsed_ms;
    uint32_t total_ms;
    uint8_t loop;
    uint32_t tracks_played;
    uint32_t errors;
} audio_player_status;

int audio_wav_parse_header(fs_file_t *f, struct audio_format *fmt, uint32_t *data_bytes, uint64_t *data_offset);

int audio_player_play(const char *path, int dev_idx);
int audio_player_stop(void);
int audio_player_pause(bool paused);
int audio_player_next(void);
int audio_player_prev(void);
void audio_player_set_loop(bool loop);
bool audio_player_active(void);
bool audio_player_uses_path(const char *mount_point);
void audio_player_get_status(struct audio_player_status *out);

#endif
