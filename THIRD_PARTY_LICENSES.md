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

## Linux audio quirk data (src/drivers/Audio/quirks/)

All audio quirk tables are generated from Linux and FreeBSD; LuOS keeps no quirk data of its own.
From Linux (GPL-2.0): the Realtek fixup and quirk tables of `sound/hda/codecs/realtek/`
(`hda_linux_quirks.c`), the Realtek vendor init `alc_fill_eapd_coef()` and the ALC255/ALC256/ALC294
init hooks (`hda_realtek_quirks.c`), the `azx_ids` snoop/TCSEL/64-bit/MSI/CORB capabilities of
`sound/hda/controllers/intel.c` (`hda_controller_quirks.c`) and the clock-source flags of
`sound/usb/quirks.c` (`usb_audio_quirks.c`).

## FreeBSD audio quirk data (src/drivers/Audio/quirks/)

Generated from FreeBSD (BSD-2-Clause): `sys/dev/sound/pci/hda/` (`hdaa_patches.c`, `pin_patch.h`,
`pin_patch_realtek.h`, the `hdac_devices` controller table of `hdac.c` and the `hdacc_codecs` codec
names of `hdacc.c`), `sys/dev/sound/pci/ich.c` and `sys/dev/sound/pcm/ac97.c` (`ac97_quirks.c`), and
`sys/dev/usb/quirk/usb_quirk.c` with names from `sys/dev/usb/usbdevs` (`usb_audio_quirks.c`). The
HD-audio files carry this notice:

```
Copyright (c) 2006 Stephane E. Potvin <sepotvin@videotron.ca>
Copyright (c) 2006 Ariff Abdullah <ariff@FreeBSD.org>
Copyright (c) 2008-2012 Alexander Motin <mav@FreeBSD.org>
Copyright (c) 2018 Khamba Staring <k.staring@quickdecay.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

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
