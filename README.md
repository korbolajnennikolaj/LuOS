# LuOS

A hobby x86-64 kernel built from scratch, booted via [Limine](https://limine-bootloader.org), with BIOS and UEFI support. It includes its own drivers, filesystems, and embedded Lua and MicroPython interpreters capable of running scripts directly from attached disks and USB drives.

## Kernel Features

| Subsystem | What's implemented |
|---|---|
| Boot | Limine, BIOS and UEFI |
| Memory | Heap allocator based on a red-black tree (fast block lookup/insert/delete) |
| Storage | AHCI, ATA/IDE, NVMe, RAM disk (Limine modules / initrd), MBR and GPT partitions |
| USB | xHCI (USB 3.0), EHCI (USB 2.0), OHCI/UHCI (USB 1.1), external hubs (incl. USB 3.0 and Transaction Translators), USB Mass Storage, USB hotplug, composite devices split into functions (IAD, UAC collections, CDC unions — e.g. a webcam is *Video* + *Audio*), isochronous transfers on all four controllers (EHCI iTD/siTD) — see [docs/USB.md](docs/USB.md) |
| Audio | Intel HD Audio (codec graph parsing, automatic output/input paths, jack-sense automute), AC'97 (ICH and compatibles, variable rate), USB Audio Class 1 and 2 playback and capture (webcam/headset microphones, asynchronous feedback, full and high speed) with hotplug; any PCM format/rate/channel count is converted on the fly; FreeBSD-compatible quirk tables — see [docs/AUDIO.md](docs/AUDIO.md) |
| Filesystems | FAT12/16/32 and exFAT (read/write, `mkfs`), ramfs, ext2/ext3 (read/write, `mkfs`), ext4, NTFS, ISO9660 with Rock Ridge and Joliet, tar/cpio initrd (read-only), per-volume locking for parallel I/O and a shared sector cache — see [Filesystems](#filesystems) |
| Scripting | Embedded Lua and MicroPython — `.lua` and `.py` scripts can be run directly from disk or a USB drive |
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
| HDA | Normal (QEMU; the >4 GiB DMA fix of 0.10.13 is not yet confirmed on real hardware) |
| AC97 | Good (real hardware: Sony VAIO) |
| USB Audio | Normal (UAC1 playback confirmed on real hardware; UAC2, capture and EHCI tested in QEMU) |

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
make iso-limine    # build a bootable ISO -> LuOS_limine.iso
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

Hardware workarounds are data in `src/drivers/Audio/quirks/` (pin config patches in FreeBSD syntax, verbs and coefficients in Linux syntax, GPIO, EAPD/VRef/sense flags, hooks). [docs/AUDIO.md](docs/AUDIO.md) describes the core API, the backends and how to port FreeBSD/Linux quirk tables.

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
scripts/          # linker script and build helpers
docs/             # subsystem documentation (audio)
```

## Future Plans

| Plan | Description |
|---|---|
| Network stack | Basic networking support |
| Audio stack | Interrupt-driven streams, HDA multichannel/HDMI, USB MIDI and implicit feedback, a UVC driver for the video half of webcams, filling the quirk tables from FreeBSD |
| OHCI hardware testing | Buy a real OHCI device and test the driver against real hardware, not just emulation |

## License

See [LICENSE](LICENSE) (MIT). This project uses Lua and MicroPython, both MIT-licensed — see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
