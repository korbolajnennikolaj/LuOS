#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#ifndef _UINTPTR_T_DEFINED
typedef uint64_t uintptr_t;
#define _UINTPTR_T_DEFINED
#endif

#ifndef _SIZE_T_DEFINED
typedef uint64_t size_t;
#define _SIZE_T_DEFINED
#endif

#ifdef __cplusplus
extern "C" {
#endif

    void _start(void);
#ifdef __cplusplus
}
#endif

#endif