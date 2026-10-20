#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

# Builds libdrm (core + KMS) and the DRM test programs as static musl binaries.
# usage: ./build.sh /path/to/libdrm-2.4.125   (the release tarball from dri.freedesktop.org, or `apt-get source libdrm`)
# Output goes to bin/; copy bin/* to /bin on a LuOS volume.
set -e
cd "$(dirname "$0")"
SRC=${1:?usage: build.sh /path/to/libdrm-source}
CC=${CC:-musl-gcc}
OUT=$(pwd)/bin
GEN=$(pwd)/gen
mkdir -p "$OUT" "$GEN"

cat > "$GEN/config.h" <<'H'
#define HAVE_LIBDRM_ATOMIC_PRIMITIVES 1
#define HAVE_LIB_ATOMIC_OPS 0
#define HAVE_SYS_SYSCTL_H 0
#define HAVE_SYS_SELECT_H 1
#define HAVE_ALLOCA_H 1
#define MAJOR_IN_SYSMACROS 1
#define HAVE_OPEN_MEMSTREAM 1
#define HAVE_SECURE_GETENV 1
#define HAVE_VISIBILITY 1
#define HAVE_CAIRO 0
#define HAVE_VALGRIND 0
#define UDEV 0
#define _GNU_SOURCE 1
H
python3 "$SRC/gen_table_fourcc.py" "$SRC/include/drm/drm_fourcc.h" "$GEN/generated_static_table_fourcc.h"

# kernel uapi headers (linux/, asm/) are taken from the build host after musl's own headers
KHDR="-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu"
CFLAGS="-O2 -static -include $GEN/config.h -I$GEN -I$SRC -I$SRC/include/drm -I$SRC/tests -I$SRC/tests/util $KHDR"
CFLAGS="$CFLAGS -Wno-unused-result -Wno-deprecated-declarations"

LIBDRM="$SRC/xf86drm.c $SRC/xf86drmHash.c $SRC/xf86drmRandom.c $SRC/xf86drmSL.c $SRC/xf86drmMode.c"
UTIL="$SRC/tests/util/format.c $SRC/tests/util/kms.c $SRC/tests/util/pattern.c"

$CC $CFLAGS -o "$OUT/modetest" $SRC/tests/modetest/modetest.c $SRC/tests/modetest/buffers.c \
	$SRC/tests/modetest/cursor.c $UTIL $LIBDRM -lm -lpthread
$CC $CFLAGS -o "$OUT/kmstest" kmstest.c $LIBDRM
echo "built: $(ls "$OUT")"
