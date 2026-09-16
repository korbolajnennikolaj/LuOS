#ifndef SERVICE_MOUSE_UPDATER_H
#define SERVICE_MOUSE_UPDATER_H

#include "programs.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct mouse_sample {
    int32_t dx;
    int32_t dy;
    int32_t wheel;

    bool btn_left;
    bool btn_right;
    bool btn_middle;
} mouse_sample_t;

int32_t mouse_updater_x(void);
int32_t mouse_updater_y(void);

bool mouse_updater_take(mouse_sample_t *out);

int mouse_updater_active_type(void);
uint64_t mouse_updater_event_count(void);
uint64_t mouse_updater_poll_count(void);

service_t *get_mouse_updater_service(void);

#endif
