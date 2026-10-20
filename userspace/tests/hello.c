// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
extern char **environ;
int main(int argc, char **argv) {
    printf("Hello from Linux userspace on LuOS! pid %d\n", getpid());
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    printf("  HOME=%s TERM=%s\n", getenv("HOME"), getenv("TERM"));
    return 7;
}
