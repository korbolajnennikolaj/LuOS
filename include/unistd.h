#ifndef UNISTD_H
#define UNISTD_H

// A minimal <unistd.h> stub for the freestanding LuOS kernel build.
// There are no real syscalls (read/write/...) in the kernel — this
// header only exists because a few MicroPython core files (py/parse.c,
// py/runtime.c, py/objdeque.c, py/stream.c, py/emitbc.c) include
// <unistd.h> purely for the ssize_t type.

#include <stddef.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#endif // UNISTD_H
