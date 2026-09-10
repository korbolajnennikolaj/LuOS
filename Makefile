PROJECT_NAME = LuOS
KERNEL = kernel.bin
ISO_LIMINE = $(PROJECT_NAME)_limine.iso

CC = gcc
LD = ld
AS = nasm

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

.PHONY: all clean run-uefi run-uefi-smp run-bios iso-limine help \
	run-uhci-bios run-ehci-bios run-ohci-bios \
	run-xhci-bios run-xhci-uefi run-xhci-log \
	run-uhci-log run-ehci-log run-ohci-log \
	run-ata-log run-stress-log

AHCI_LOG       = ahci_trace.log
AHCI_QEMU_LOG  = qemu_debug_ahci.log
DISK_IMG       = disk.img
NVME_DISK_IMG  = nvme_disk.img
DISK_SIZE      = 512M

STRESS_DISK_SIZE = 32M
DISK_AHCI = disk_ahci.img
DISK_XHCI = disk_xhci.img
DISK_EHCI = disk_ehci.img
DISK_UHCI = disk_uhci.img
DISK_OHCI = disk_ohci.img

STRESS_DISKS = $(DISK_AHCI) $(DISK_XHCI) $(DISK_EHCI) $(DISK_UHCI) $(DISK_OHCI)

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
		echo "[WARN] mkfs.exfat not found (exfatprogs/exfat-utils package) - partition 2 will have no filesystem"; \
	fi; \
	sudo mkfs.ext4 -q -F -L LUOSEXT4 "$${LOOPDEV}p3"; \
	sudo losetup -d "$$LOOPDEV"; \
	echo "[IMG] $$IMG ready: p1=FAT32(EFI) p2=exFAT p3=ext4"

$(STRESS_DISKS):
	@echo "[IMG] Creating $@ ($(STRESS_DISK_SIZE) FAT32)..."
	@rm -f $@
	@truncate -s $(STRESS_DISK_SIZE) $@
	@mkfs.vfat -F32 -n STRESS $@ >/dev/null
	@echo "[IMG] $@ ready"

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

SMP_LOG     = qemu_smp_trace.log
SMP_DBG_LOG = qemu_smp_debug.log
SMP_GDB_PORT = 1234

run-uefi-smp: iso-limine
	@echo "[QEMU SMP DEBUG] Running with 4 cores in UEFI mode (TCG, NO KVM)..."
	@echo "[QEMU SMP DEBUG] GDB: gdb your_kernel.elf -ex 'target remote :$(SMP_GDB_PORT)'"
	@if [ ! -f "$(OVMF_PATH)" ]; then \
		echo "[ERR] OVMF not found at $(OVMF_PATH)!"; \
		exit 1; \
	fi
	@rm -f $(SMP_LOG) $(SMP_DBG_LOG)
	qemu-system-x86_64 \
		-machine q35,accel=tcg \
		-bios $(OVMF_PATH) \
		-cdrom $(ISO_LIMINE) \
		-m 2048M \
		-cpu qemu64,+x2apic \
		-smp 4,sockets=1,cores=4,threads=1 \
		-boot d \
		-serial mon:stdio \
		-trace "apic*" \
		-d int,pcall,mmu,cpu_reset,guest_errors,unimp \
		-D $(SMP_DBG_LOG) \
		-no-reboot \
		-gdb tcp::$(SMP_GDB_PORT) \
		-k en-us \
		-nodefaults \
		-vga std \
		2>$(SMP_LOG)
	@echo "[QEMU SMP DEBUG] Logs: $(SMP_DBG_LOG) | $(SMP_LOG)"

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
	@echo "[UHCI BIOS] Machine: pc,usb=on — built-in UHCI (usb-bus.0, 2 ports) + second UHCI for disk"
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

NVME_LOG = nvme_trace.log
NVME_QEMU_LOG = qemu_debug_nvme.log

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

SMP_STRESS_CORES ?= 4
STRESS_LOG     = qemu_stress_trace.log
STRESS_DBG_LOG = qemu_stress_debug.log

ifeq ($(QEMU_HAS_NVME),yes)
STRESS_NVME_ARGS = -drive file=$(NVME_DISK_IMG),if=none,format=raw,id=nvmedisk0 \
	-device nvme,serial=deadbeef,drive=nvmedisk0
else
STRESS_NVME_ARGS =
endif

run-stress-log: iso-limine $(DISK_IMG) $(STRESS_DISKS)
	@echo "[STRESS UEFI] SMP ($(SMP_STRESS_CORES) cores) + AHCI + ATA + xHCI/EHCI/UHCI/OHCI"
	@echo "[STRESS UEFI] Input spread across controllers, not duplicated on each:"
	@echo "[STRESS UEFI]   xHCI  -> usb-mouse + usb-storage"
	@echo "[STRESS UEFI]   EHCI  -> usb-kbd   + usb-storage"
	@echo "[STRESS UEFI]   UHCI  -> usb-storage (no input)"
	@echo "[STRESS UEFI]   OHCI  -> usb-storage (no input)"
	@echo "[STRESS UEFI] So there's exactly one working keyboard and one mouse — you can control the kernel,"
	@echo "[STRESS UEFI] but each controller is still really loaded with its own hardware, not just 'listed' in the command line."
	@echo "[STRESS UEFI] Each controller gets its own dedicated 32 MB FAT32 disk image (no shared-file locking issues)."
	@echo "[STRESS UEFI] The main ATA disk remains the big multi-partition disk.img."
ifneq ($(QEMU_HAS_NVME),yes)
	@echo "[STRESS UEFI] This QEMU build doesn't support -device nvme — NVMe skipped, rest of hardware unchanged."
endif
	@rm -f $(STRESS_LOG) $(STRESS_DBG_LOG)
	@$(MAKE) --no-print-directory $(NVME_DISK_IMG)
	qemu-system-x86_64 \
		-machine q35 \
		-bios $(OVMF_PATH) \
		-cdrom $(ISO_LIMINE) \
		-m 1024M \
		-cpu host \
		-enable-kvm \
		-smp $(SMP_STRESS_CORES) \
		-boot d \
		-serial stdio \
		-nodefaults \
		-vga std \
		-no-reboot \
		-no-shutdown \
		-drive file=$(DISK_IMG),if=ide,format=raw \
		-device ich9-ahci,id=ahci0 \
		-drive file=$(DISK_AHCI),if=none,format=raw,id=ahcidisk0 \
		-device ide-hd,drive=ahcidisk0,bus=ahci0.0 \
		$(STRESS_NVME_ARGS) \
		-device qemu-xhci,id=xhci0 \
		-drive file=$(DISK_XHCI),if=none,format=raw,id=xhcidisk0 \
		-device usb-mouse,bus=xhci0.0 \
		-device usb-storage,drive=xhcidisk0,bus=xhci0.0 \
		-device usb-ehci,id=ehci0 \
		-drive file=$(DISK_EHCI),if=none,format=raw,id=ehcidisk0 \
		-device usb-kbd,bus=ehci0.0 \
		-device usb-storage,drive=ehcidisk0,bus=ehci0.0 \
		-device piix4-usb-uhci,id=uhci2 \
		-drive file=$(DISK_UHCI),if=none,format=raw,id=uhcidisk0 \
		-device usb-storage,drive=uhcidisk0,bus=uhci2.0 \
		-device pci-ohci,id=ohci0 \
		-drive file=$(DISK_OHCI),if=none,format=raw,id=ohcidisk0 \
		-device usb-storage,drive=ohcidisk0,bus=ohci0.0 \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-trace "usb_*" \
		-d guest_errors,unimp,int,cpu_reset \
		-D $(STRESS_DBG_LOG) \
		2>$(STRESS_LOG)
	@echo "[STRESS UEFI] Done. Traces -> $(STRESS_LOG) | QEMU log (incl. int/cpu_reset) -> $(STRESS_DBG_LOG)"

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
		$(NVME_DISK_IMG) \
		$(STRESS_DISKS) \
		$(AHCI_LOG) \
		$(AHCI_QEMU_LOG) \
		$(ATA_LOG) \
		$(ATA_QEMU_LOG) \
		$(NVME_LOG) \
		$(NVME_QEMU_LOG) \
		$(USB_DISK_LOG) \
		$(USB_DISK_QEMU_LOG) \
		$(STRESS_LOG) \
		$(STRESS_DBG_LOG) \
		$(SMP_LOG) \
		$(SMP_DBG_LOG)
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
	@echo "  make run-uefi            - Run in QEMU (UEFI mode, 1 core)"
	@echo "  make run-uefi-smp        - Run in QEMU (UEFI mode, 4 cores, KVM, 1024M RAM)"
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
	@echo "RUN (SMP TEST - 4 cores):"
	@echo "  make run-uefi-smp        - Quick SMP test (UEFI, 4 cores, no extra devices)"
	@echo ""
	@echo "RUN (STRESS - SMP + heavy device load):"
	@echo "  make run-stress-log      - SMP ($(SMP_STRESS_CORES) cores, override with SMP_STRESS_CORES=N)"
	@echo "                             + AHCI + ATA/IDE + NVMe (if supported) + xHCI/EHCI/UHCI/OHCI."
	@echo "                             Input is spread out, not duplicated on every controller:"
	@echo "                             xHCI=mouse, EHCI=keyboard, UHCI/OHCI=storage only —"
	@echo "                             one working kbd+mouse total, still real HW load per controller."
	@echo "                             Each controller gets its own 32 MB FAT32 disk image."
	@echo "                             Stresses the scheduler/SMP bring-up and every driver lock at once."
	@echo "                             Logs -> $(STRESS_LOG) (stderr/trace) and $(STRESS_DBG_LOG) (QEMU -d incl. int/cpu_reset)"
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
	@echo "  disk_ahci/xhci/ehci/uhci/ohci.img - 32 MB FAT32 per-controller images"
	@echo "                             for the stress test (no sudo needed)"
	@echo "  make clean               - removes all disk images and logs"
	@echo ""
	@echo "================================================================="
