#ifndef STDINT_H
#define STDINT_H

#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint64_t size_t;

#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__)
    typedef int64_t intptr_t;
    typedef uint64_t uintptr_t;
#else
    typedef int32_t intptr_t;
    typedef uint32_t uintptr_t;
#endif

#ifndef INTPTR_MAX
#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__)
#define INTPTR_MAX 9223372036854775807LL
#define INTPTR_MIN (-INTPTR_MAX - 1LL)
#else
#define INTPTR_MAX 2147483647
#define INTPTR_MIN (-INTPTR_MAX - 1)
#endif
#endif
#ifndef UINTPTR_MAX
#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__)
#define UINTPTR_MAX 18446744073709551615ULL
#else
#define UINTPTR_MAX 4294967295U
#endif
#endif

#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807LL
#define INT64_MIN (-INT64_MAX - 1LL)
#define UINT64_MAX 18446744073709551615ULL
#endif

#ifndef INT32_MAX
#define INT32_MAX 2147483647
#define INT32_MIN (-INT32_MAX - 1)
#define UINT32_MAX 4294967295U
#endif

#ifndef INT8_MAX
#define INT8_MAX 127
#define INT8_MIN (-INT8_MAX - 1)
#define UINT8_MAX 255
#endif

#ifndef INT16_MAX
#define INT16_MAX 32767
#define INT16_MIN (-INT16_MAX - 1)
#define UINT16_MAX 65535
#endif

#ifndef SIZE_MAX
#define SIZE_MAX UINT64_MAX
#endif

#endif