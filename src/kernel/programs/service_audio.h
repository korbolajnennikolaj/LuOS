#ifndef SERVICE_AUDIO_H
#define SERVICE_AUDIO_H

#include "programs.h"

#include <stdint.h>

uint64_t audio_service_poll_count(void);
uint64_t audio_service_max_gap_ms(void);
void audio_service_reset_stats(void);

service_t *get_audio_service(void);

#endif
