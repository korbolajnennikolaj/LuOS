#ifndef STDDEF_H
#define STDDEF_H

typedef long ptrdiff_t;
typedef long long max_align_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#ifndef offsetof
#define offsetof(type, member) ((size_t)&((type*)0)->member)
#endif

#endif