#ifndef SERVICE_SHELL_H
#define SERVICE_SHELL_H

#include "programs.h"

#include <stdbool.h>

bool shell_is_ready(void);

service_t *get_shell_service(void);

#endif
