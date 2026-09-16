# LuOS

A hobby x86-64 kernel built from scratch, booted via [Limine](https://limine-bootloader.org), with BIOS and UEFI support. It includes its own drivers, filesystems, and embedded Lua and MicroPython interpreters capable of running scripts directly from attached disks and USB drives.

## Kernel Features

| Subsystem | What's implemented |
|---|---|
| Boot | Limine, BIOS and UEFI |
| Memory | Heap allocator based on a red-black tree (fast block lookup/insert/delete) |
| Storage | AHCI, ATA/IDE, NVMe |
| USB | xHCI (USB 3.0), EHCI (USB 2.0), OHCI/UHCI (USB 1.1), USB Mass Storage, USB hotplug |
| Filesystems | FAT32, exFAT, ext4, ISO9660 |
| Scripting | Embedded Lua and MicroPython — `.lua` and `.py` scripts can be run directly from disk or a USB drive |
| Services | Service manager with priorities, dependencies, health checks and restarts (root fs, keyboard updater, mouse updater, USB hotplug, shell) |
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
| ATA | Good |

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
├── components/   # ACPI, PCI, memory management, interrupts
├── drivers/      # Storage, USB, Video, Input, Timer drivers
├── fs/           # FAT32, exFAT, ext4, ISO9660 implementations
├── kernel/       # core kernel, console, games
│   ├── programs/ # service manager and system services
│   ├── scheduler/# task scheduler
│   └── smp/      # multiprocessing
└── lua/          # embedded Lua interpreter + LuOS bindings
micropython/      # embedded MicroPython (bare-metal port)
lib/              # freestanding libc replacement (stdio, string, math, etc.)
include/          # freestanding headers (stdint, stddef, etc.)
scripts/          # linker script and build helpers
```

## Future Plans

| Plan | Description |
|---|---|
| SMP | Multiprocessing support |
| Network stack | Basic networking support |
| Audio stack | Sound output support |
| OHCI hardware testing | Buy a real OHCI device and test the driver against real hardware, not just emulation |

## License

See [LICENSE](LICENSE) (MIT). This project uses Lua and MicroPython, both MIT-licensed — see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
