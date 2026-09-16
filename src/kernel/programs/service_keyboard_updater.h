#ifndef SERVICE_KEYBOARD_UPDATER_H
#define SERVICE_KEYBOARD_UPDATER_H

#include "programs.h"

#include <stdint.h>

int keyboard_updater_active_type(void);
uint64_t keyboard_updater_poll_count(void);

service_t *get_keyboard_updater_service(void);

#endif
