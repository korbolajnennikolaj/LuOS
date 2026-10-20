// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include <stdio.h>
int main(void) {
    printf("about to dereference NULL...\n");
    fflush(stdout);
    volatile int *p = 0;
    *p = 42;
    printf("still alive?!\n");
    return 0;
}
