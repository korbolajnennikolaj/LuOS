#ifndef ASSERT_H
#define ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : \
        (printf("ASSERT FAILED: %s at %s:%d\n", #expr, __FILE__, __LINE__), \
         abort()))
#endif

#ifndef static_assert
#define static_assert _Static_assert
#endif

#endif