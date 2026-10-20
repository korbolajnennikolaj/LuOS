#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import re
import sys

HDR = """// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef MESA_CONST_TABLES_H
#define MESA_CONST_TABLES_H

#include <stdint.h>

struct mesa_const {
    const char *name;
    uint32_t value;
};

extern const struct mesa_const mesa_gl_consts[];
extern const uint32_t mesa_gl_const_count;
extern const struct mesa_const mesa_vk_consts[];
extern const uint32_t mesa_vk_const_count;

const struct mesa_const *mesa_const_find(const struct mesa_const *tab, uint32_t n, const char *name);

#endif
"""

C_TOP = """// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "drivers/GPU/mesa_const_tables.h"

#include <string.h>

"""

FIND = """const struct mesa_const *mesa_const_find(const struct mesa_const *tab, uint32_t n, const char *name) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = strcmp(tab[mid].name, name);
        if (c == 0) return &tab[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
"""

RX = re.compile(r'^#define\s+([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$')


def parse(path, prefix):
    found = {}
    dups = 0
    with open(path) as f:
        for line in f:
            m = RX.match(line)
            if not m:
                continue
            name = m.group(1)
            if not name.startswith(prefix) or name == prefix:
                continue
            short = name[len(prefix):]
            if short in found:
                dups += 1
                continue
            found[short] = int(m.group(2), 0)
    return found, dups


def emit(f, table, cname):
    f.write("const struct mesa_const %s_consts[] = {\n" % cname)
    for name in sorted(table):
        f.write('    { "%s", %s },\n' % (name, hex(table[name])))
    f.write("};\n")
    f.write("const uint32_t %s_const_count = %d;\n\n" % (cname, len(table)))


def main():
    gl, gl_dups = parse("src/drivers/GPU/mesa_gl_defs.h", "GL_")
    vk, vk_dups = parse("src/drivers/GPU/mesa_vk_defs.h", "VK_")
    with open("src/drivers/GPU/mesa_const_tables.h", "w") as f:
        f.write(HDR)
    with open("src/drivers/GPU/mesa_const_tables.c", "w") as f:
        f.write(C_TOP)
        emit(f, gl, "mesa_gl")
        emit(f, vk, "mesa_vk")
        f.write(FIND)
    print("GL consts: %d (%d dup dropped), VK consts: %d (%d dup dropped)" % (len(gl), gl_dups, len(vk), vk_dups))


if __name__ == "__main__":
    sys.exit(main())
