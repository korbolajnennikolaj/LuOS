#ifndef MP_LUOS_H
#define MP_LUOS_H

// Public API for MicroPython integration into the LuOS kernel (see
// this port's main.c). The Python counterpart of src/lua/lua.h for
// the Lua interpreter.

// Initializes the MicroPython runtime (heap, GC, compiler). Called
// once at kernel startup, analogous to luos_lua_init().
void mp_luos_init(void);

// Frees the MicroPython runtime. Usually not needed on a bare-metal
// kernel with no shutdown, but present for API completeness.
void mp_luos_deinit(void);

// Compiles and runs one line/block of Python code (e.g. a line typed
// into the kernel shell's interactive REPL). Returns 0 on success,
// -1 if an unhandled exception was raised (the text has already been
// printed).
int mp_luos_exec_str(const char *src);

// Runs MicroPython's own "friendly" REPL (with its own command
// history via readline) on top of mp_hal_stdin_rx_chr/mp_hal_stdout_tx_strn.
void mp_luos_run_repl(void);

// MicroPython version string (e.g. "1.29.0-preview") — for banners
// like fastfetch, the counterpart of LUA_VERSION for Lua.
const char *mp_luos_version_string(void);

#endif // MP_LUOS_H
