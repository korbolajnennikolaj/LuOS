#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

# Builds the userspace test programs with a stock musl toolchain (Debian/Ubuntu: musl-tools, Arch: musl).
# Output goes to bin/; copy bin/* to /bin and lib/ld-musl-x86_64.so.1 to /lib on a LuOS volume.
set -e
cd "$(dirname "$0")"
CC=${CC:-musl-gcc}
mkdir -p bin lib
$CC -static -O2 -o bin/hello_static hello.c
$CC -O2 -o bin/hello_dyn hello.c
$CC -static -O2 -o bin/crash crash.c
$CC -static -O2 -pthread -o bin/utest utest.c
$CC -O2 -pthread -o bin/utest_dyn utest.c
$CC -static -O2 -pthread -o bin/exitgroup exitgroup.c
for ld in /usr/lib/x86_64-linux-musl/libc.so /usr/lib/musl/lib/libc.so /lib/ld-musl-x86_64.so.1; do
    if [ -f "$ld" ]; then cp -L "$ld" lib/ld-musl-x86_64.so.1; break; fi
done
echo "built: $(ls bin)"
