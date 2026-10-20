# Third-Party Licenses

LuOS's own code is MIT and the Linux code under `linux/upstream/` is GPL-2.0 — see
[LICENSE](LICENSE) for the exact split and the compilation warning: once the project is
compiled into one kernel, the resulting binary as a whole is covered by GPL-2.0. The
components bundled with the project keep their own licenses, listed below.

## Linux (linux/)

The Linux 6.18 code under `linux/upstream/` is taken verbatim from the Linux kernel and
keeps its own license (GPL-2.0, with the individual files' copyright notices and SPDX
identifiers intact). The LinuxKPI runtime under `linux/kpi/` is written for LuOS and is
MIT, like the rest of LuOS's own code (`linux/kpi/include/asm/agp.h` is a verbatim
Linux header and keeps GPL-2.0).

## Lua

Path in this repository: `src/lua/`

```
Copyright (C) 1994-2026 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## MicroPython

Path in this repository: `micropython/`

```
The MIT License (MIT)

Copyright (c) 2013-2026 Damien P. George

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

> Individual ports and files within MicroPython may include code with additional copyright holders or different licensing terms — see the original `micropython/LICENSE` file for details.

## musl libc (userspace/tests/lib/ld-musl-x86_64.so.1, statically linked into userspace/tests/bin)

musl 1.2.4, https://musl.libc.org — MIT License, Copyright © 2005-2020 Rich Felker, et al.
Only used by the userspace test programs; nothing from musl is linked into the kernel.
