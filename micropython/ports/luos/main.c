// main.c — MicroPython integration into the LuOS kernel.
// The embedding model mirrors the official micropython/ports/embed port
// (see ports/embed/port/embed_util.c): initialize the runtime once
// (mp_luos_init), then either run the "friendly" REPL (mp_luos_run_repl,
// its own input/line history via shared/readline), or execute a single
// line/script from the kernel's existing shell (mp_luos_exec_str) —
// the second option is exactly what the "python" command in
// src/kernel/kernel.c uses, mirroring how the "lua" command is set up
// there.

#include <string.h>
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/builtin.h"
#include "py/mperrno.h"
#include "shared/runtime/pyexec.h"
#include "stdio.h"
#include "components/Memory/heap.h" // For kmalloc

#define MP_LUOS_HEAP_SIZE (64 * 1024)

static char *mp_heap = NULL;
static char *mp_stack_top_ptr = NULL;

// ----------------------------------------------------------------------
// Required MicroPython runtime hooks
// ----------------------------------------------------------------------

void nlr_jump_fail(void *val) {
    (void)val;
    printf("[MPY] FATAL: Uncaught NLR exception!\n");
    while (1) asm volatile("hlt");
}

void gc_collect(void) {
    gc_collect_start();
    // Conservatively scan the kernel stack between the current SP and
    // the saved stack top (mp_stack_top_ptr, see mp_luos_init).
    void *dummy;
    gc_collect_root((void **)&dummy, ((mp_uint_t)mp_stack_top_ptr - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}

// No filesystem — there are no imports from disk.
mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

// builtin open() is implemented in modfile.c, backed by the kernel's
// rootfs mount (see kernel/rootfs.h) — real disk I/O, not a stub. Only
// module *imports* from disk stay unsupported (mp_import_stat() above).

#if !(MICROPY_READER_POSIX || MICROPY_READER_VFS)
// py/builtinimport.c and py/builtinevex.c reference mp_lexer_new_from_file()
// in the "load a .py file from disk" code path — on LuOS this path is
// unreachable (mp_import_stat always returns MP_IMPORT_STAT_NO_EXIST),
// but the symbol still has to exist, or the final kernel link would
// fail with "undefined reference". We give an honest error if it's
// ever reached anyway.
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    (void)filename;
    mp_raise_OSError(MP_ENOENT);
}
#endif

// ----------------------------------------------------------------------
// Initialization / deinitialization
// ----------------------------------------------------------------------

void mp_luos_init(void) {
    int stack_dummy;
    mp_stack_top_ptr = (char *)&stack_dummy;
    mp_stack_set_top(&stack_dummy);
    mp_stack_set_limit(32 * 1024);

    // Allocate the MicroPython heap out of the kernel heap.
    mp_heap = (char *)kmalloc(MP_LUOS_HEAP_SIZE);
    gc_init(mp_heap, mp_heap + MP_LUOS_HEAP_SIZE);

    mp_init();
}

void mp_luos_deinit(void) {
    mp_deinit();
}

// ----------------------------------------------------------------------
// Code execution
// ----------------------------------------------------------------------

// Compiles and runs a single line/block of Python code. Returns 0 on
// success and -1 if an unhandled exception was raised (the exception
// text is printed via printf/mp_hal_stdout in that case).
// Used by the "python"/"py" command of the kernel's interactive shell —
// the same way luos_lua_dostring() is used by the "lua" command.
int mp_luos_exec_str(const char *src) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT); // <-- FIXED
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
        return 0;
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        return -1;
    }
}

// MicroPython version string (for the fastfetch banner etc.), see mp_luos.h.
const char *mp_luos_version_string(void) {
    return MICROPY_VERSION_STRING;
}

// MicroPython's own "friendly" REPL (with its own command history via
// shared/readline). Used when a classic REPL style is needed on top of
// mp_hal_stdin_rx_chr/mp_hal_stdout_tx_strn (see mphalport.c).
void mp_luos_run_repl(void) {
    printf("\nMicroPython REPL on LuOS\n");
    printf("Type \"import luos; luos.reboot()\" to test bridges.\n\n");

    for (;;) {
        if (pyexec_friendly_repl() != 0) break;
    }
}
