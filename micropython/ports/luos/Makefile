include ../../py/mkenv.mk

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

# gcc's own "freestanding" headers (stdint.h, stdbool.h, stdarg.h, float.h, ...)
# — не путать с glibc; они нужны даже с -nostdinc/-ffreestanding.
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)

# This port doesn't have a filesystem, and doesn't need the (relatively)
# heavyweight compressed-error-text machinery.
MICROPY_ROM_TEXT_COMPRESSION ?= 0

# Флаги должны строго совпадать с CFLAGS_LUA ядра LuOS (SSE2 нужен для
# двойной точности float — MicroPython собран с MICROPY_FLOAT_IMPL_DOUBLE
# и использует va_arg(double), что требует SSE-регистры в System V ABI).
CFLAGS += -m64 -ffreestanding -O2 -Wall -Wextra \
	-nostdlib -nostdinc -fno-builtin \
	-fno-stack-protector -fno-pie -fno-pic \
	-mno-red-zone -mcmodel=kernel \
	-fno-tree-loop-distribute-patterns \
	-msse -msse2 -mno-mmx \
	-Wno-error \
	-I. -I$(TOP) -I$(BUILD) \
	-I../../../include -I../../../src -I../../../lib \
	-I$(GCC_INCLUDE)

# Отключаем стандартную линковку, нам нужен .o файл для ядра
LDFLAGS += -r -nostdlib

SRC_C = \
	main.c \
	mphalport.c \
	modluos.c \
	modkernel.c \
	modrandom.c \
	modtime.c \
	modos.c \
	modfile.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/interrupt_char.c \

SRC_QSTR += main.c modluos.c modkernel.c modrandom.c modtime.c modos.c modfile.c mphalport.c
SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c

include $(TOP)/py/py.mk

# ВАЖНО: mkrules.mk ставит генерацию qstrdefs/root_pointers/mpversion как
# order-only зависимость именно для переменной $(OBJ) (см. py/mkrules.mk:140),
# поэтому она должна включать ВСЕ объектные файлы — и ядро MicroPython
# (PY_O), и файлы порта — иначе часть файлов начнёт собираться раньше,
# чем сгенерируются genhdr/qstrdefs.generated.h и другие заголовки.
OBJ += $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

# Порт собирает не исполняемый файл, а объект (firmware.o), который затем
# линкуется вместе с остальным ядром LuOS. Явно объявляем это целью 'all',
# иначе mkenv.mk подставит пустую цель 'all' и make ничего не соберёт.
all: $(BUILD)/firmware.o

# Переопределяем линковку: собираем все .o (ядро MPY + порт) в один firmware.o
$(BUILD)/firmware.o: $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $^

include $(TOP)/py/mkrules.mk
