PROJECT_NAME = LuOS
KERNEL = kernel.bin
ISO_LIMINE = $(PROJECT_NAME)_limine.iso

CC = gcc
LD = ld
AS = nasm

# === Добавь эти переменные в начало Makefile ===
MPY_DIR = micropython
MPY_PORT = luos
MPY_BUILD_DIR = $(MPY_DIR)/ports/$(MPY_PORT)/build
MPY_FIRMWARE = $(MPY_BUILD_DIR)/firmware.o

LIMINE_PATH = /usr/share/limine
OVMF_PATH = /usr/share/edk2/x64/OVMF.4m.fd
GCC_INCLUDE = $(shell $(CC) -print-file-name=include)

CFLAGS_BASE = -m64 -ffreestanding -O2 -Wall -Wextra \
	-nostdlib -nostdinc -fno-builtin \
	-fno-stack-protector -fno-pie -fno-pic \
	-mno-red-zone -mcmodel=kernel \
	-fno-tree-loop-distribute-patterns \
	-I./include -I./src -I./lib \
	-I$(GCC_INCLUDE) \
	-I./src/lua/include \
	-I./src/lua/core \
	-I./src/lua \
	-I./micropython/ports/luos

CFLAGS = $(CFLAGS_BASE) \
	-mno-sse -mno-sse2 -mno-mmx -mno-80387

CFLAGS_MATH = $(CFLAGS_BASE) \
	-msse -msse2 -mno-mmx

CFLAGS_LUA = $(CFLAGS_BASE) \
	-msse -msse2 -mno-mmx \
	-DLUA_USE_LUOS \
	-Wno-empty-body

ASFLAGS = -f elf64

LDFLAGS = -m elf_x86_64 -T scripts/linker.ld -nostdlib -z max-page-size=0x1000 -static

# libgcc.a — компиляторные хелперы (мягкая арифметика: 64-битное деление,
# конверсии float16 и т.п.), которые gcc может подставлять в сгенерированный
# код даже в freestanding-режиме. Нужен на финальной линковке ядра, иначе
# такие символы (например __extendhfdf2/__truncsfhf2 из MicroPython/py/binary.c)
# останутся неразрешёнными.
LIBGCC = $(shell $(CC) $(CFLAGS_BASE) -print-libgcc-file-name)

SRC_DIR = src
LIB_DIR = lib
BUILD_DIR = build
ISO_DIR_LIMINE = isodir_limine

C_SOURCES   = $(shell find src lib -name '*.c')
C_OBJECTS   = $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SOURCES))

ASM_SOURCES = $(shell find src lib -name '*.asm')
ASM_OBJECTS = $(patsubst %.asm, $(BUILD_DIR)/%.o, $(ASM_SOURCES))

LUA_SOURCES = $(shell find src/lua -name '*.c')

MATH_SOURCES = lib/math.c src/kernel/math_test.c \
	src/drivers/Video/limine_video_driver.c \
	lib/stdlib.c lib/time.c src/kernel/libc_test.c \
	lib/dtoa.c lib/stdio.c src/kernel/games.c

LUA_OBJECTS    = $(patsubst %.c, $(BUILD_DIR)/%.o, $(LUA_SOURCES))
MATH_OBJECTS   = $(patsubst %.c, $(BUILD_DIR)/%.o, $(MATH_SOURCES))
NORMAL_OBJECTS = $(filter-out $(MATH_OBJECTS) $(LUA_OBJECTS), $(C_OBJECTS))

OBJECTS = $(NORMAL_OBJECTS) $(MATH_OBJECTS) $(LUA_OBJECTS) $(ASM_OBJECTS)

.PHONY: all clean run-uefi run-bios iso-limine help \
	run-uhci-bios run-ehci-bios run-ohci-bios \
	run-xhci-bios run-xhci-uefi run-xhci-log \
	run-uhci-log run-ehci-log run-ohci-log \
	run-ata-log

AHCI_LOG       = ahci_trace.log
AHCI_QEMU_LOG  = qemu_debug_ahci.log
DISK_IMG       = disk.img
NVME_DISK_IMG  = nvme_disk.img
DISK_SIZE      = 512M

# =================================================================
# Общий образ диска для всех тестов (AHCI/ATA/USB/xHCI/NVMe и т.д.)
#
# Вместо простого нулевого файла (dd if=/dev/zero) здесь создаётся
# настоящий диск с GPT-таблицей и тремя разделами:
#   p1 - FAT32  (помечен как загрузочный/EFI)
#   p2 - exFAT
#   p3 - ext4
#
# Требуются: parted, util-linux (losetup/partprobe), dosfstools (mkfs.vfat),
# exfatprogs/exfat-utils (mkfs.exfat), e2fsprogs (mkfs.ext4) и sudo для
# работы с loop-устройствами (см. цель install).
#
# Правило общее для $(DISK_IMG) и $(NVME_DISK_IMG) — раз это обычные
# файловые цели без иных предпосылок, make пересоздаст образ только
# если файла ещё нет на диске (обычное поведение make для файлов).
# Чтобы пересоздать образ заново — удалите его (или `make clean`).
# =================================================================
$(DISK_IMG) $(NVME_DISK_IMG):
	@set -e; \
	IMG="$@"; \
	SIZE="$(DISK_SIZE)"; \
	echo "[IMG] Creating $$IMG ($$SIZE) with GPT: p1=FAT32 p2=exFAT p3=ext4..."; \
	rm -f "$$IMG"; \
	truncate -s "$$SIZE" "$$IMG"; \
	parted -s "$$IMG" -- \
		mklabel gpt \
		mkpart primary fat32 1MiB 35% \
		mkpart primary 35% 68% \
		mkpart primary ext4 68% 100% \
		set 1 boot on; \
	LOOPDEV=$$(sudo losetup -fP --show "$$IMG"); \
	echo "[IMG] loop device: $$LOOPDEV"; \
	sudo partprobe "$$LOOPDEV" >/dev/null 2>&1 || true; \
	udevadm settle 2>/dev/null || sleep 0.5; \
	sudo mkfs.vfat -F32 -n LUOSFAT "$${LOOPDEV}p1" >/dev/null; \
	if command -v mkfs.exfat >/dev/null 2>&1; then \
		sudo mkfs.exfat -n LUOSEXFAT "$${LOOPDEV}p2" >/dev/null; \
	else \
		echo "[WARN] mkfs.exfat не найден (пакет exfatprogs/exfat-utils) - раздел 2 останется без ФС"; \
	fi; \
	sudo mkfs.ext4 -q -F -L LUOSEXT4 "$${LOOPDEV}p3"; \
	sudo losetup -d "$$LOOPDEV"; \
	echo "[IMG] $$IMG готов: p1=FAT32(EFI) p2=exFAT p3=ext4"

all: $(BUILD_DIR) $(KERNEL)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "[AS]  Assembling $<..."
	@$(AS) $(ASFLAGS) $< -o $@

$(LUA_OBJECTS): $(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  Compiling (Lua) $<..."
	@$(CC) $(CFLAGS_LUA) -c $< -o $@

$(MATH_OBJECTS): $(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  Compiling (SSE2) $<..."
	@$(CC) $(CFLAGS_MATH) -c $< -o $@

# === Добавь правило сборки MicroPython ===
$(MPY_FIRMWARE):
	@echo "[MPY] Building MicroPython bare-metal port..."
	@$(MAKE) -C $(MPY_DIR)/ports/$(MPY_PORT)

$(NORMAL_OBJECTS): $(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  Compiling $<..."
	@$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJECTS) $(MPY_FIRMWARE)
	@echo "[LD]  Linking kernel with MicroPython..."
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(MPY_FIRMWARE) $(LIBGCC)
	@echo "Kernel built: $(KERNEL)"

run-uefi: iso-limine
	@echo "[QEMU] Running with graphics + QEMU monitor + CPU debug output (PS/2 input)..."
	@echo "[HOTKEYS] Ctrl+Alt+2 = switch to monitor"
	@echo "[HOTKEYS] Ctrl+Alt+1 = switch back to display"
	@echo "[HOTKEYS] Ctrl+Alt+G = release mouse/keyboard"
	@echo "[DEBUG] CPU debug output in console, system display in window"
	@echo "[INFO] Full debug log saved to qemu_debug.log"
	@if [ ! -f "$(OVMF_PATH)" ]; then \
		echo "[ERR] OVMF not found at $(OVMF_PATH)! Install edk2-ovmf."; \
		exit 1; \
	fi
	qemu-system-x86_64 \
		-machine q35 \
		-bios $(OVMF_PATH) \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-boot d \
		-serial stdio \
		-d guest_errors,unimp,int \
		-D qemu_debug.log \
		-no-reboot \
		-k en-us \
		-nodefaults \
		-vga std

run-bios: iso-limine
	@echo "[QEMU] Running in BIOS mode (CPU: 2GHz, PS/2 input)..."
	qemu-system-x86_64 \
		-rtc base=localtime,clock=host \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu qemu64,tsc-frequency=2000000000 \
		-smp 1 \
		-serial stdio \
		-k en-us \
		-nodefaults \
		-vga std

XHCI_UEFI_LOG    = xhci_uefi_trace.log
XHCI_UEFI_DBG    = qemu_debug_xhci_uefi.log
XHCI_BIOS_LOG    = xhci_bios_trace.log
XHCI_BIOS_DBG    = qemu_debug_xhci_bios.log

run-xhci-uefi: iso-limine $(DISK_IMG)
	@echo "[xHCI UEFI] NEC xHCI + virtual usb-kbd + usb-mouse + usb-storage (UEFI/OVMF)"
	@echo "[xHCI UEFI] NOTE: OVMF maps xHCI BAR above 4GB!"
	@if [ ! -f "$(OVMF_PATH)" ]; then echo "[ERR] OVMF not found!"; exit 1; fi
	@rm -f $(XHCI_UEFI_LOG) $(XHCI_UEFI_DBG); \
	qemu-system-x86_64 \
		-machine q35 \
		-bios $(OVMF_PATH) \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu host \
		-enable-kvm \
		-device qemu-xhci,id=xhci \
		-drive file=$(DISK_IMG),if=none,format=raw,id=usbdisk0 \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-device usb-storage,drive=usbdisk0,bus=xhci.0 \
		-boot d \
		-serial stdio \
		-trace "usb_xhci_*" \
		-d guest_errors,unimp \
		-D $(XHCI_UEFI_DBG) \
		-no-reboot \
		2>$(XHCI_UEFI_LOG)
	@echo "[xHCI UEFI] Done. Traces -> $(XHCI_UEFI_LOG) | QEMU log -> $(XHCI_UEFI_DBG)"

run-xhci-bios: iso-limine $(DISK_IMG)
	@echo "[xHCI BIOS] NEC xHCI + virtual usb-kbd + usb-mouse + usb-storage (BIOS/SeaBIOS)"
	@rm -f $(XHCI_BIOS_LOG) $(XHCI_BIOS_DBG)
	qemu-system-x86_64 \
		-machine q35 \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu host \
		-enable-kvm \
		-device nec-usb-xhci,id=xhci,streams=on \
		-drive file=$(DISK_IMG),if=none,format=raw,id=usbdisk0 \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0 \
		-device usb-storage,drive=usbdisk0,bus=xhci.0 \
		-boot d \
		-serial stdio \
		-vga std \
		-trace "usb_xhci_*" \
		-d guest_errors,unimp \
		-D $(XHCI_BIOS_DBG) \
		-no-reboot 2>$(XHCI_BIOS_LOG)
	@echo "[xHCI BIOS] Done. Traces -> $(XHCI_BIOS_LOG) | QEMU log -> $(XHCI_BIOS_DBG)"

run-xhci-log: run-xhci-bios

run-bios-2ghz: iso-limine
	@echo "[QEMU] Running in BIOS mode (CPU: 2GHz, TCG)..."
	qemu-system-x86_64 \
		-rtc base=localtime,clock=host \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu qemu64,tsc-frequency=2000000000 \
		-accel tcg \
		-smp 1 \
		-serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04

UHCI_LOG       = uhci_trace.log
UHCI_DBG_LOG   = qemu_debug_uhci.log
EHCI_LOG       = ehci_trace.log
EHCI_DBG_LOG   = qemu_debug_ehci.log
OHCI_LOG       = ohci_trace.log
OHCI_DBG_LOG   = qemu_debug_ohci.log

run-uhci-bios: iso-limine $(DISK_IMG)
	@echo "[UHCI BIOS] USB 1.1 controller (PIIX4 built-in), emulated usb-kbd + usb-mouse + usb-storage"
	@echo "[UHCI BIOS] Machine: pc,usb=on — встроенный UHCI (usb-bus.0, 2端口) + второй UHCI для диска"
	@rm -f $(UHCI_LOG) $(UHCI_DBG_LOG); \
	qemu-system-x86_64 \
		-machine pc,usb=on \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu host \
		-enable-kvm \
		-device piix4-usb-uhci,id=uhci2 \
		-drive file=$(DISK_IMG),if=none,format=raw,id=usbdisk0 \
		-device usb-kbd,bus=usb-bus.0,port=1 \
		-device usb-mouse,bus=usb-bus.0,port=2 \
		-device usb-storage,drive=usbdisk0,bus=uhci2.0,port=1 \
		-boot d \
		-serial stdio \
		-trace "usb_uhci_*" \
		-d guest_errors,unimp \
		-D $(UHCI_DBG_LOG) \
		-no-reboot \
		2>$(UHCI_LOG)
	@echo "[UHCI BIOS] Done. Traces -> $(UHCI_LOG) | QEMU log -> $(UHCI_DBG_LOG)"

run-ehci-bios: iso-limine $(DISK_IMG)
	@echo "[EHCI BIOS] USB 2.0 controller (EHCI + companion UHCI), emulated usb-kbd + usb-mouse + usb-storage"
	@echo "[EHCI BIOS] EHCI MMIO BAR mapped below 4GB by SeaBIOS"
	@rm -f $(EHCI_LOG) $(EHCI_DBG_LOG); \
	qemu-system-x86_64 \
		-machine pc \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu host \
		-enable-kvm \
		-device usb-ehci,id=ehci \
		-drive file=$(DISK_IMG),if=none,format=raw,id=usbdisk0 \
		-device usb-kbd \
		-device usb-mouse \
		-device usb-storage,drive=usbdisk0,bus=ehci.0 \
		-boot d \
		-serial stdio \
		-trace "usb_ehci_*" \
		-d guest_errors,unimp \
		-D $(EHCI_DBG_LOG) \
		-no-reboot \
		2>$(EHCI_LOG)
	@echo "[EHCI BIOS] Done. Traces -> $(EHCI_LOG) | QEMU log -> $(EHCI_DBG_LOG)"

run-ohci-bios: iso-limine $(DISK_IMG)
	@echo "[OHCI BIOS] USB 1.1 controller (PCI OHCI), emulated usb-kbd + usb-mouse + usb-storage"
	@echo "[OHCI BIOS] OHCI PCI MMIO BAR mapped below 4GB by SeaBIOS"
	@rm -f $(OHCI_LOG) $(OHCI_DBG_LOG); \
	qemu-system-x86_64 \
		-machine pc \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-cpu host \
		-enable-kvm \
		-device pci-ohci,id=ohci \
		-drive file=$(DISK_IMG),if=none,format=raw,id=usbdisk0 \
		-device usb-kbd,bus=ohci.0 \
		-device usb-mouse,bus=ohci.0 \
		-device usb-storage,drive=usbdisk0,bus=ohci.0 \
		-boot d \
		-serial stdio \
		-trace "usb_ohci_*" \
		-d guest_errors,unimp \
		-D $(OHCI_DBG_LOG) \
		-no-reboot \
		2>$(OHCI_LOG)
	@echo "[OHCI BIOS] Done. Traces -> $(OHCI_LOG) | QEMU log -> $(OHCI_DBG_LOG)"

run-uhci-log: run-uhci-bios
run-ehci-log: run-ehci-bios
run-ohci-log: run-ohci-bios

# =================================================================

ATA_LOG = ata_trace.log
ATA_QEMU_LOG = qemu_debug_ata.log

run-ata-log: iso-limine $(DISK_IMG)
	@echo "[ATA] Preparing disk image + starting QEMU in PATA mode..."

	@rm -f $(ATA_LOG) $(ATA_QEMU_LOG)

	qemu-system-x86_64 \
		-machine pc \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-smp 1 \
		-serial stdio \
		-nodefaults \
		-vga std \
		-drive file=$(DISK_IMG),if=ide,format=raw \
		-d guest_errors,unimp \
		-D $(ATA_QEMU_LOG) \
		-no-reboot \
		2>$(ATA_LOG)

	@echo "[ATA] Logs saved:"
	@echo "  QEMU : $(ATA_QEMU_LOG)"
	@echo "  STDERR: $(ATA_LOG)"

iso-limine: $(KERNEL)
	@echo "[ISO] Building Limine ISO..."
	@if [ ! -d "$(LIMINE_PATH)" ]; then \
		echo "[ERR] Limine files not found at $(LIMINE_PATH)!"; \
		exit 1; \
	fi

	@rm -rf $(ISO_DIR_LIMINE)
	@mkdir -p $(ISO_DIR_LIMINE)

	@cp $(KERNEL) $(ISO_DIR_LIMINE)/

	@echo "TIMEOUT:5" > $(ISO_DIR_LIMINE)/limine.conf
	@echo "" >> $(ISO_DIR_LIMINE)/limine.conf
	@echo "/LuOS" >> $(ISO_DIR_LIMINE)/limine.conf
	@echo "    PROTOCOL:limine" >> $(ISO_DIR_LIMINE)/limine.conf
	@echo "    KERNEL_PATH:boot():/kernel.bin" >> $(ISO_DIR_LIMINE)/limine.conf

	@cp $(LIMINE_PATH)/limine-bios.sys $(ISO_DIR_LIMINE)/
	@cp $(LIMINE_PATH)/limine-bios-cd.bin $(ISO_DIR_LIMINE)/
	@cp $(LIMINE_PATH)/limine-uefi-cd.bin $(ISO_DIR_LIMINE)/

	@xorriso -as mkisofs -quiet \
		-b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_DIR_LIMINE) -o $(ISO_LIMINE)

	@echo "[ISO] Installing BIOS bootloader..."
	@limine bios-install $(ISO_LIMINE)

	@echo "ISO created successfully: $(ISO_LIMINE)"


USB_DISK_LOG = usb_flash_trace.log
USB_DISK_QEMU_LOG = qemu_debug_usb_flash.log

fullsource:
	@echo "[EXPORT] Gathering all source files into fullsource.txt..."
	@rm -f fullsource.txt
	@find . include -type f \( -name "*.c" -o -name "*.h" -o -name "*.asm" \) | sort | while read file; do \
		echo "========================================" >> fullsource.txt; \
		echo " FILE: $$file" >> fullsource.txt; \
		echo "========================================" >> fullsource.txt; \
		cat "$$file" >> fullsource.txt; \
		echo -e "\n\n" >> fullsource.txt; \
	done
	@echo "Done!"

unpack:
	@echo "[UNPACK] Extracting files from fullsource.txt..."
	@python3 unpack.py

run-ahci-log: iso-limine $(DISK_IMG)
	@echo "[AHCI] Preparing disk image + starting QEMU..."

	@rm -f $(AHCI_LOG) $(AHCI_QEMU_LOG)

	qemu-system-x86_64 \
		-machine q35 \
		-bios $(OVMF_PATH) \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-smp 1 \
		-serial stdio \
		-nodefaults \
		-vga std \
		-drive file=$(DISK_IMG),if=none,format=raw,id=disk0 \
		-device ich9-ahci,id=ahci \
		-device ide-hd,drive=disk0,bus=ahci.0 \
		-d guest_errors,unimp \
		-D $(AHCI_QEMU_LOG) \
		-no-reboot \
		2>$(AHCI_LOG)

	@echo "[AHCI] Logs saved:"
	@echo "  QEMU : $(AHCI_QEMU_LOG)"
	@echo "  STDERR: $(AHCI_LOG)"

# =================================================================
# NVMe test target
#
# QEMU's "nvme" device (qemu-system-x86_64 -device nvme) is only
# available if that build of QEMU was compiled with NVMe emulation
# support - true for essentially every mainstream distro package, but
# not guaranteed everywhere (some minimal/custom QEMU builds omit it).
# Rather than fail with a cryptic "'nvme' is not a valid device model
# name" from QEMU itself, we check `-device help` up front and skip
# with a clear message if it's missing.
# =================================================================

NVME_LOG = nvme_trace.log
NVME_QEMU_LOG = qemu_debug_nvme.log
# NVME_DISK_IMG объявлен выше, рядом с DISK_IMG

# Evaluated once, at Makefile-parse time (not inside the recipe), so a
# plain Make conditional can gate the whole target below. Doing this
# check *inside* the recipe with a shell "if ... exit 0 ... fi" doesn't
# actually work here: each recipe line runs in its own subshell, so an
# "exit 0" from one line's "if" block only ends that line - Make just
# moves on and runs the next recipe line regardless. Evaluating the
# check with $(shell ...) up front and branching with ifeq avoids that
# trap entirely.
QEMU_HAS_NVME := $(shell qemu-system-x86_64 -device help 2>/dev/null | grep -q '"nvme"' && echo yes)

run-nvme-log: iso-limine
ifneq ($(QEMU_HAS_NVME),yes)
	@echo "[NVME] This build of QEMU ($(shell command -v qemu-system-x86_64)) does not support -device nvme."
	@echo "[NVME] Skipping NVMe test. Install a QEMU build with NVMe emulation to run this target."
else
	@echo "[NVME] QEMU supports NVMe, preparing disk image + starting QEMU..."

	@rm -f $(NVME_LOG) $(NVME_QEMU_LOG)

	@$(MAKE) --no-print-directory $(NVME_DISK_IMG)

	qemu-system-x86_64 \
		-machine q35 \
		-bios $(OVMF_PATH) \
		-cdrom $(ISO_LIMINE) \
		-m 512M \
		-smp 1 \
		-serial stdio \
		-nodefaults \
		-vga std \
		-drive file=$(NVME_DISK_IMG),if=none,format=raw,id=nvmedisk0 \
		-device nvme,serial=deadbeef,drive=nvmedisk0 \
		-d guest_errors,unimp \
		-D $(NVME_QEMU_LOG) \
		-no-reboot \
		2>$(NVME_LOG)

	@echo "[NVME] Logs saved:"
	@echo "  QEMU : $(NVME_QEMU_LOG)"
	@echo "  STDERR: $(NVME_LOG)"
endif

clean:
	@echo "[CLEAN] Cleaning all build artifacts and logs..."
	@rm -rf $(BUILD_DIR) $(KERNEL) $(ISO_LIMINE) $(ISO_DIR_LIMINE)
	@$(MAKE) -C $(MPY_DIR)/ports/$(MPY_PORT) clean
	@rm -rf $(BUILD_DIR) $(KERNEL) $(ISO_LIMINE) $(ISO_DIR_LIMINE)
	@rm -f qemu_cpu_debug.log serial.log fullsource.txt qemu_debug.log \
		$(XHCI_UEFI_LOG) $(XHCI_UEFI_DBG) \
		$(XHCI_BIOS_LOG) $(XHCI_BIOS_DBG) \
		$(UHCI_LOG) $(UHCI_DBG_LOG) \
		$(EHCI_LOG) $(EHCI_DBG_LOG) \
		$(OHCI_LOG) $(OHCI_DBG_LOG) \
		$(DISK_IMG) \
		$(AHCI_LOG) \
		$(AHCI_QEMU_LOG) \
		$(ATA_LOG) \
		$(ATA_QEMU_LOG) \
		$(NVME_LOG) \
		$(NVME_QEMU_LOG) \
		$(NVME_DISK_IMG) \
		$(USB_DISK_LOG) \
		$(USB_DISK_QEMU_LOG)
	@echo "Cleanup complete."

help:
	@echo "================================================================="
	@echo "LuOS Build System - Help"
	@echo "================================================================="
	@echo "BUILD:"
	@echo "  make all                 - Compile the kernel (kernel.bin)"
	@echo "  make iso-limine          - Create a bootable ISO"
	@echo "  make clean               - Remove build artifacts and logs"
	@echo ""
	@echo "RUN (QEMU):"
	@echo "  make run-uefi            - Run in QEMU (UEFI mode)"
	@echo "  make run-bios            - Run in QEMU (BIOS mode)"
	@echo "  make run-bios-2ghz       - Run in BIOS mode (TCG, 2GHz, no KVM)"
	@echo ""
	@echo "RUN (STORAGE):"
	@echo "  make run-ahci-log        - Run QEMU with AHCI + disk.img + logging"
	@echo ""
	@echo "RUN (ATA - classic IDE):"
	@echo "  make run-ata-log         - Run QEMU with PATA/IDE (legacy ATA mode)"
	@echo ""
	@echo "RUN (xHCI - USB 3.0):"
	@echo "  make run-xhci-bios       - NEC xHCI + virtual kbd/mouse/disk (BIOS/SeaBIOS)"
	@echo "  make run-xhci-uefi       - NEC xHCI + virtual kbd/mouse/disk (UEFI/OVMF)"
	@echo "  make run-xhci-log        - alias for run-xhci-bios"
	@echo ""
	@echo "RUN (Legacy USB - with virtual kbd/mouse/disk):"
	@echo "  make run-uhci-bios       - PIIX4 UHCI (USB 1.1)"
	@echo "  make run-ehci-bios       - EHCI (USB 2.0) + companion UHCI"
	@echo "  make run-ohci-bios       - PCI OHCI (USB 1.1)"
	@echo ""
	@echo "DEBUG AND SOURCE:"
	@echo "  make fullsource          - Gather all source files into one file (.c, .h, .asm)"
	@echo "  make unpack              - Extract files from fullsource.txt"
	@echo ""
	@echo "DISKS:"
	@echo "  disk.img / nvme_disk.img - automatically created (GPT, 3 partitions:"
	@echo "                             p1=FAT32, p2=exFAT, p3=ext4) for AHCI,"
	@echo "                             ATA, and USB (xHCI/EHCI/OHCI/UHCI) tests"
	@echo "                             (requires sudo for losetup)"
	@echo "  make clean               - removes disk.img, nvme_disk.img, and all logs"
	@echo ""
	@echo "================================================================="
