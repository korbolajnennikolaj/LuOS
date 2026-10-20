# LuOS

A hobby x86-64 kernel built from scratch, booted via [Limine](https://limine-bootloader.org), with BIOS and UEFI support. It includes its own drivers, filesystems, and embedded Lua and MicroPython interpreters capable of running scripts directly from attached disks and USB drives.

## Kernel Features

| Subsystem | What's implemented |
|---|---|
| Boot | Limine, BIOS and UEFI |
| Memory | Heap allocator based on a red-black tree (fast block lookup/insert/delete) that grows on demand; PAT with WB/WT/UC-/UC/WP/WC page types; ioremap/vmap windows with 2 MiB pages; physically contiguous DMA buffers; cross-CPU TLB shootdown |
| PCI | PCIe ECAM (MCFG) with 4 KiB config space, BAR sizing, capability and extended-capability walks, bridge topology, MSI and MSI-X with any vector on any CPU, dynamic interrupt vectors |
| Storage | AHCI, ATA/IDE, NVMe, RAM disk (Limine modules / initrd), MBR and GPT partitions |
| USB | xHCI (USB 3.0), EHCI (USB 2.0), OHCI/UHCI (USB 1.1), external hubs (incl. USB 3.0 and Transaction Translators), USB Mass Storage, USB hotplug, composite devices split into functions (IAD, UAC collections, CDC unions — e.g. a webcam is *Video* + *Audio*), isochronous transfers on all four controllers (EHCI iTD/siTD) |
| Audio | Intel HD Audio (codec graph parsing, automatic output/input paths, jack-sense automute), AC'97 (ICH and compatibles, variable rate), USB Audio Class 1 and 2 playback and capture (webcam/headset microphones, asynchronous feedback, full and high speed) with hotplug; any PCM format/rate/channel count is converted on the fly; FreeBSD-compatible quirk tables; simultaneous output to several devices at once (multi-output routing: jack + speakers + a second controller) |
| GPU | GPU stack on the linuxkpi subsystem, which runs GPU drivers through Mesa and the Linux DRM code; kernel-space GL/Vulkan batches, KMS multi-monitor management with resolution / refresh-rate / brightness controls and built-in kernel-space cube demos. External cards are not initialized by default — `gpu-init PATH` starts them |
| Filesystems | FAT12/16/32 and exFAT (read/write, `mkfs`), ramfs, ext2/ext3 (read/write, `mkfs`), ext4, NTFS, ISO9660 with Rock Ridge and Joliet, tar/cpio initrd (read-only), per-volume locking for parallel I/O and a shared sector cache — see [Filesystems](#filesystems) |
| Userspace | Unmodified Linux x86-64 programs (glibc or musl, static or dynamically linked) in ring 3: Linux system call ABI, processes and threads, demand-paged and shared mmap, memfd, futex, TLS, AVX state, pipes, files on every mounted volume and in the built-in filesystem — see [userspace/tests](userspace/tests) |
| Scripting | Embedded Lua and MicroPython — `.lua` and `.py` scripts can be run directly from disk or a USB drive; full `gl` (OpenGL) and `vk` (Vulkan) libraries with constants, immediate mode, matrices, shaders, textures and buffers, writing graphics scripts on Mesa (llvmpipe CPU rendering works out of the box) |
| Scheduler | Preemptive SMP scheduler: per-core run queues, 16-level multi-level feedback queue (dynamic priority, level-dependent quantum), aging against starvation, wake-up placement, work stealing and load balancing, per-task FPU/SSE state, stack overflow canary |
| Services | Service manager with priorities, dependencies, health checks and restarts (root fs, keyboard updater, mouse updater, USB hotplug, audio, shell) |
| Logging | Kernel logger with a 256 KiB ring buffer, `[timestamp] [level] [caller] message` format, per-output levels for buffer/UART (COM1)/screen, interrupt-driven COM1 output through a 128 KiB TX queue (IRQ4, synchronous fallback), `dmesg` and `log-dump` to a `.txt` file, last messages shown on the panic screen |
| Console | Interactive shell with its own console subsystem |
| Extras | A few built-in terminal games (3D cube, Tetris-like, 2048-like) |

## Component Stability

How reliable and stable each component is right now (scale: Bad < Normal < Good < Best):

| Component | Rating |
|---|---|
| TSC | Best |
| PIT | Best |
| RTC | Best |
| APIC (timer) | Best |
| HPET | Good/Best |
| xHCI | Good/Best |
| UHCI | Good |
| EHCI | Good |
| OHCI | Bad/Normal |
| ACPI | Normal/Good |
| NVMe | Good/Best |
| AHCI | Good |
| USB MSC | Good |
| ATA | Good (multi-sector PIO with SET MULTIPLE MODE, string I/O, per-device sector cache) |
| HDA | Good |
| AC97 | Good (real hardware: Sony VAIO) |
| USB Audio | Normal (UAC1 playback confirmed on real hardware; UAC2, capture and EHCI tested in QEMU) |
| GPU | Normal/Good |

> The kernel has been tested and runs well on real hardware, not just in emulators. That said, use on your own hardware is entirely at your own risk.

## Requirements

- `gcc`, `nasm`, `binutils`, `make`
- `xorriso`
- [`limine`](https://limine-bootloader.org) (expected by default at `/usr/share/limine`; version 10.8.5 is recommended)
- `qemu-system-x86_64` (to run without real hardware)
- OVMF firmware for UEFI testing (expected by default at `/usr/share/edk2/x64/OVMF.4m.fd`)
- `parted`, `dosfstools`, `exfatprogs`, `e2fsprogs` (only needed for building test disk images)

Paths to `limine` and OVMF are set at the top of the `Makefile` (`LIMINE_PATH`, `OVMF_PATH`) — adjust them for your distro if they live elsewhere.

## Building

```bash
make all           # build the kernel -> kernel.bin
make iso-limine    # bootable ISO -> LuOS_limine.iso
```

## Running in QEMU

```bash
make run-uefi      # QEMU, UEFI mode (OVMF)
make run-bios      # QEMU, BIOS mode
```

Targets for testing individual controllers:

```bash
make run-ahci-log     # AHCI + disk.img
make run-ata-log      # legacy PATA/IDE
make run-xhci-bios    # xHCI (USB 3.0) + virtual keyboard/mouse/disk
make run-xhci-uefi    # same, but UEFI
make run-uhci-bios    # UHCI (USB 1.1)
make run-ehci-bios    # EHCI (USB 2.0)
make run-ohci-bios    # OHCI (USB 1.1)
make run-audio-hda    # Intel HDA + codec, host sound through PulseAudio (AUDIO_BACKEND=pa)
make run-audio-ac97   # AC97
make run-audio-usb    # USB Audio Class 1 device on xHCI
make run-audio-all    # all three; AUDIO_BACKEND=wav,path=out.wav records to a file
```

## Filesystems

| Filesystem | Read | Write | Format (`mkfs`) | Notes |
|---|---|---|---|---|
| FAT12 / FAT16 / FAT32 | Yes | Yes | Yes | Long names in UTF-8, Windows lowercase flags for short names |
| exFAT | Yes | Yes | Yes | Allocation bitmap, FAT chains and contiguous files, VolumeDirty flag while mounted |
| ramfs | Yes | Yes | — | In-memory, contents are lost on unmount or reboot |
| ext2 / ext3 | Yes | Yes | Yes | Direct/indirect (1/2/3-level) block mapping, bitmap allocators with group placement, journal on ext3 volumes is ignored (volumes needing journal replay are rejected) |
| ext4 | Yes | No | No | |
| NTFS | Yes | No | No | Compressed and encrypted files are not supported |
| ISO9660 | Yes | No | No | Rock Ridge and Joliet names |
| tar / cpio (initrd) | Yes | No | No | ustar, GNU long names, pax, cpio newc |

Filesystem shell commands:

```
mount NAME [POINT]          # mount a disk or partition (auto-detects the filesystem)
mount ramfs POINT           # mount an empty in-memory filesystem
umount [POINT]
mkfs TYPE NAME [LABEL]      # format a disk or partition: fat, fat12, fat16, fat32, exfat, ext2, ext3
fs-test [DIR]               # write/read self-test, leaves a check set in DIR/fskeep
fs-sum [DIR]                # CRC32 and size of every file, written to the log
```

`mkfs fat` picks FAT12, FAT16 or FAT32 by size. A device cannot be formatted while it or any of its partitions is mounted. Formatting a partition also updates its MBR type; formatting a whole disk replaces its partition table.

### initrd

Any file passed as a Limine module becomes a RAM disk (`rd0`, `rd1`, ...), and an `rd*` device is preferred when the root filesystem is mounted automatically. The image can be a tar or cpio archive or an image of any supported filesystem (a FAT or exFAT image stays writable in RAM).

```bash
make iso-limine INITRD=initrd.tar
```

## Audio

```
play FILE.wav|DIR [DEV]     # background playback (a directory plays all its .wav files in order)
playing                     # (np) what is playing, from which directory, track N/M, time
stop / pause / resume       # player control, the shell stays usable while music plays
next / prev / loop [on|off] # skip in the directory, repeat the playlist
audio                       # list devices (* = default)
audio-beep [HZ] [MS] [DEV]  # test tone
audio-rec FILE.wav [SEC]    # record 48 kHz stereo
audio-vol [DEV] [0-100]     # volume, audio-mute [on|off], audio-default DEV
audio-info [DEV]            # codec widgets, pin configs, chosen paths, stream stats
audio-quirks                # quirk tables and what matched each device
```

Hardware workarounds are data in `src/drivers/Audio/quirks/` (pin config patches in FreeBSD syntax, verbs and coefficients in Linux syntax, GPIO, EAPD/VRef/sense flags, hooks).

## GPU

External cards are **not initialized by default**. To initialize one, run `gpu-init PATH`, where `PATH` is a firmware directory or a `.zip` (for example `/lib/firmware.zip`) — the path allows the Linux DRM driver to load its firmware and probe the card. Until a real card is bound, the monitor commands return an error instead of touching hardware, and `kmscube` / `vkkmv` render on the **CPU** — directly into the visible framebuffer (the boot Limine one, or the DRM console buffer when simpledrm has claimed it), with the console restored afterwards and ESC aborting mid-demo. The firmware console driver (simpledrm) does **not** count as an initialized stack: with only simpledrm bound the demos still take the CPU renderer.

Driver probes never freeze the shell: `gpu-init`, `gpu-reinit`, `gpu-bind`, `gpu-unbind` and the enumeration behind `gpu-devs` run in helper tasks with timeouts (probe 20 s, enumeration 3 s). A driver stuck on odd hardware is reported in the log and the shell stays alive.

### Scripting the GPU

Both interpreters expose the same graphics library on top of the kernel Mesa batches — `gl` (classic
OpenGL: immediate mode with `begin`/`vertex`/`color`, fixed-function matrices and lighting, shaders,
textures, VBOs) and `vk` (Vulkan instance/device/queue setup with property queries). Every GL/VK
constant is reachable by name (`gl.TRIANGLES`, `vk.QUEUE_GRAPHICS_BIT`, or `gl.const("GL_BLEND")`
for the full 5000+ enum table). Frames render into an off-screen FBO, come back with
`gl.readPixels`, and can be shown on screen with `gl.present` — on the CPU renderer (llvmpipe)
without any GPU stack running.

```lua
gl.start()
gl.open(256, 256)
gl.matrixMode(gl.PROJECTION); gl.perspective(45, 1, 0.1, 100)
gl.matrixMode(gl.MODELVIEW); gl.loadIdentity(); gl.translatef(0, 0, -4)
gl.clearColor(0.1, 0.2, 0.3, 1); gl.clear(gl.COLOR_BUFFER_BIT)
gl.begin(gl.TRIANGLES)
gl.color3f(1, 0, 0); gl.vertex2f(-0.6, -0.6)
gl.color3f(0, 1, 0); gl.vertex2f( 0.6, -0.6)
gl.color3f(0, 0, 1); gl.vertex2f( 0.0,  0.6)
gl.end()
gl.present()
gl.close()
```

The same calls exist in MicroPython (`import gl`, `gl.start()` ...). Self-tests exercising CPU Mesa
rendering ship in the built-in filesystem: `run /gfx_test.lua`, `run /gfx_vk_test.lua`,
`run /gfx_test.py`, `run /gfx_vk_test.py`.

```
gpu-init PATH              # start the GPU stack (PATH = firmware dir or .zip)
gpu-devs / gpu-info [N]    # cards, bound drivers, DRM nodes
gpu-mons                   # monitors: connectors, modes, layout
gpu-mon-on N [MODE]        # enable monitor N (MODE index optional) - shows the mode screen
gpu-mon-mode N 1920x1080@60    # resolution and refresh rate (WxH[@HZ] or mode index)
gpu-mon-modes N            # list the modes of monitor N
gpu-mon-move N X Y         # position; gpu-mon-clone DST SRC; gpu-mon-layout
gpu-mon-bright N 0-100     # per-monitor brightness (software dim)
gpu-mon-off N              # power monitor N off - it stays off (the console cannot re-light it)
gpu-bright [N VALUE]       # hardware panel backlight, if the driver registers one
kmscube / vkkmv [SEC DEV MON WxH]   # kernel-space cube demos; vblank-paced page flips on a GPU card, uncapped software rendering on the CPU framebuffer without the stack; after a demo the console reclaims its connector, like fbcon after kmscube on Linux
```

## Running on Real Hardware

Build the ISO with `make iso-limine`, then flash `LuOS_limine.iso` onto a USB drive. [USBImager](https://bztsrc.gitlab.io/usbimager/) is recommended — it writes the image byte-for-byte with no extra magic, and works the same way on Linux, Windows, and macOS.

## Cleaning Up

```bash
make clean
```

## Project Layout

```
src/
├── components/   # ACPI, PCI, memory management, interrupts, logger, panic
├── drivers/      # Storage, USB, Audio, Video, Input, Timer, Serial drivers
├── fs/           # FAT12/16/32, exFAT, ext4, NTFS, ISO9660, ramfs, tar/cpio archive implementations
├── kernel/       # core kernel, console, games
│   ├── programs/ # service manager and system services
│   ├── scheduler/# task scheduler
│   └── smp/      # multiprocessing
└── lua/          # embedded Lua interpreter + LuOS bindings
micropython/      # embedded MicroPython (bare-metal port)
lib/              # freestanding libc replacement (stdio, string, math, etc.)
include/          # freestanding headers (stdint, stddef, etc.)
userspace/        # test programs and the built-in filesystem builder (sysroot)
scripts/          # linker script and build helpers
```

## Future Plans

| Plan | Description |
|---|---|
| Network stack | Basic networking support |
| Audio stack | Interrupt-driven streams, HDA multichannel/HDMI, USB MIDI and implicit feedback, a UVC driver for the video half of webcams, filling the quirk tables from FreeBSD |
| OHCI hardware testing | Buy a real OHCI device and test the driver against real hardware, not just emulation |

## License

Mixed: the code written for LuOS is MIT, the Linux code under `linux/upstream/` is GPL-2.0, and the bundled third-party components keep their licenses — see [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Note that compiling the project turns the result into a derivative of the GPL Linux code: while the source files keep their own licenses, the compiled kernel as a whole is covered by GPL-2.0.
