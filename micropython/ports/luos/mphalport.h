#include <stdint.h>
#include "py/mpconfig.h"

// The signatures must match the default prototypes from py/mphal.h
// letter for letter (otherwise -Wall catches "conflicting types" when
// building MicroPython core files that include py/mphal.h directly).
int mp_hal_stdin_rx_chr(void);
void mp_hal_stdout_tx_str(const char *str);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len);
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);

// mp_hal_set_interrupt_char() itself is implemented (when
// MICROPY_KBD_EXCEPTION is on, which it is for this port - see
// mpconfigport.h) by shared/runtime/interrupt_char.c, already listed
// in this port's Makefile SRC_C. That file compiles and links fine on
// its own, but nothing in py/ ever declares the function in a header
// core files see - py/modmicropython.c and shared/runtime/pyexec.c
// both call it without any prototype in scope, which is an
// implicit-declaration warning on most compilers and a hard error on
// newer GCC (-Werror=implicit-function-declaration is on by default
// starting with GCC 14, so what's a warning here today can fail the
// build outright on a newer host toolchain). Every other port that
// enables MICROPY_KBD_EXCEPTION declares this prototype itself, in
// its own mphalport.h (see e.g. ports/stm32, ports/rp2, ports/esp32,
// ports/nrf) - shared/runtime/interrupt_char.h isn't included by any
// py/ core file automatically. Doing the same here.
void mp_hal_set_interrupt_char(int c); // -1 to disable

// input() override. py/modbuiltins.c does:
//
//     #ifndef mp_hal_readline
//     #define mp_hal_readline readline
//     #endif
//
// i.e. a port may substitute its own line reader, which is what we do
// here - shared/readline expects raw character-at-a-time terminal input,
// whereas on LuOS the line editing already lives in the kernel
// (src/kernel/console.c) and is shared with Lua's io library.
//
// vstr_t is declared in py/misc.h, which py/mphal.h does not pull in;
// forward-declaring the struct tag is enough for a pointer parameter and
// keeps this header free of extra includes. vstr_t is a typedef of
// struct _vstr_t, so the types match at the call site.
struct _vstr_t;
int mp_luos_readline(struct _vstr_t *line, const char *prompt);
#define mp_hal_readline mp_luos_readline
