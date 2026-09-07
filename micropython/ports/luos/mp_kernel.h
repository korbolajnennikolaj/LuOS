#ifndef MP_KERNEL_H
#define MP_KERNEL_H

#include <stdint.h>

// Passes driver pointers into MicroPython's "kernel" module
// (micropython/ports/luos/modkernel.c). Called once at kernel startup,
// in the same place where g_bind_* is filled in for Lua (see kernel.c).

struct limine_video_driver;
struct rtc_driver;
struct pit_driver;
struct tsc_driver;

void mp_kernel_bind(struct limine_video_driver *video, struct rtc_driver *rtc,
                     struct pit_driver *pit, struct tsc_driver *tsc);

// Common "ticks" source (TSC) for the built-in random/time modules.
uint64_t mp_kernel_uptime_ms(void);

#endif // MP_KERNEL_H
