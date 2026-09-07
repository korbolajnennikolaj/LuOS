// mpconfigport.h — MicroPython port configuration for the LuOS kernel
// (bare-metal x86_64, no filesystem, no libc)

#include <stdint.h>

// Use the basic feature set (compiler + REPL), nothing extra.
#define MICROPY_CONFIG_ROM_LEVEL          (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER           (1)
#define MICROPY_ENABLE_GC                 (1)
#define MICROPY_HELPER_REPL               (1)
#define MICROPY_REPL_EMACS_KEYS           (1)
#define MICROPY_REPL_AUTO_INDENT          (1)

// No FS and no os module — only what's compiled into the firmware is loaded.
#define MICROPY_ENABLE_EXTERNAL_IMPORT    (0)
#define MICROPY_READER_POSIX              (0)
#define MICROPY_READER_VFS                (0)
#define MICROPY_VFS                       (0)

#define MICROPY_PY_BUILTINS_FLOAT         (1)
#define MICROPY_FLOAT_IMPL                (MICROPY_FLOAT_IMPL_DOUBLE)
// Both enabled: the "os" module (modos.c) and the global open()
// builtin (modfile.c) are wired to the kernel's real rootfs mount (see
// kernel/rootfs.h), not stubs - see the file-level comments in each
// for how "no filesystem mounted" is reported.
#define MICROPY_PY_IO                     (1)
#define MICROPY_PY_OS                     (1)
#define MICROPY_PY_SYS                    (1)
#define MICROPY_PY_SYS_STDFILES           (0)
#define MICROPY_PY_SYS_STDIO_BUFFER       (0)
#define MICROPY_PY_SYS_MODULES            (0)
#define MICROPY_PY_SYS_EXIT               (0)
#define MICROPY_PY_SYS_PATH               (0)
#define MICROPY_PY_SYS_ARGV               (0)

// No ANSI C / libc in the kernel environment — don't use the nonstandard alloca().
#define MICROPY_NO_ALLOCA                 (1)

#define MICROPY_ALLOC_PATH_MAX            (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT    (16)

#define MICROPY_ERROR_REPORTING           (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_KBD_EXCEPTION             (1)

// The input() builtin. Off by default at CORE_FEATURES ROM level, and
// py/mpconfig.h notes it "can only be enabled if the port uses this
// readline" - meaning shared/readline. This port does not: it supplies
// its own mp_hal_readline (see mphalport.h/.c), which is the documented
// override point in py/modbuiltins.c, and routes the line through the
// kernel's console editor so input() behaves exactly like Lua's
// io.read() - same echo, same Backspace, same Ctrl+D/Ctrl+C.
#define MICROPY_PY_BUILTINS_INPUT         (1)

// REPL command history (readline)
#define MICROPY_READLINE_HISTORY_SIZE     (8)

// Map the "port state" directly onto the common VM state so we don't
// need a separate MICROPY_PORT_ROOT_POINTERS struct — the "minimal"
// port does the same.
#define MP_STATE_PORT MP_STATE_VM

#define MICROPY_HW_BOARD_NAME             "LuOS"
#define MICROPY_HW_MCU_NAME               "x86_64"

// LuOS data types (64-bit platform)
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

// The freestanding kernel environment has no <alloca.h>, but
// MICROPY_NO_ALLOCA(1) above guarantees that py/mpconfig.h replaces
// alloca() with m_malloc(), so the header isn't needed.
