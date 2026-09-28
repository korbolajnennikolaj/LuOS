#include "py/mphal.h"
#include "stdio.h"
#include "string.h"
#include "drivers/Input/keyboard_driver.h" // LuOS keyboard driver
#include "kernel/console.h"          // shared line/key input (see src/kernel/console.c)
#include "py/misc.h"                  // vstr_t / vstr_add_strn
#include "shared/readline/readline.h" // CHAR_CTRL_C, CHAR_CTRL_D
#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "kernel/scheduler/scheduler.h"

// ----------------------------------------------------------------------
// Output
// ----------------------------------------------------------------------

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    char buf[128];
    size_t total = len;
    while (len > 0) {
        size_t chunk = len > sizeof(buf) - 1 ? sizeof(buf) - 1 : len;
        memcpy(buf, str, chunk);
        buf[chunk] = '\0';
        printf("%s", buf); // Use printf from lib/stdio.c
        str += chunk;
        len -= chunk;
    }
    return total;
}

void mp_hal_stdout_tx_str(const char *str) {
    mp_hal_stdout_tx_strn(str, strlen(str));
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    // "Cooked" output: translate a lone '\n' into "\r\n", the way
    // other MicroPython ports do it in a terminal.
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            mp_hal_stdout_tx_strn("\r\n", 2);
        } else {
            mp_hal_stdout_tx_strn(&str[i], 1);
        }
    }
}

// ----------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------

// ----------------------------------------------------------------------
// The input() builtin
//
// Wired in via the mp_hal_readline macro in mphalport.h. Contract, from
// mp_builtin_input() in py/modbuiltins.c: append the typed line to
// `line` (no trailing newline) and return 0; return CHAR_CTRL_C to raise
// KeyboardInterrupt, or CHAR_CTRL_D with nothing appended to raise
// EOFError. The prompt has already been printed by the caller, so the
// empty prompt passed down here is correct - we must not print it twice.
// ----------------------------------------------------------------------

int mp_luos_readline(struct _vstr_t *line, const char *prompt) {
    char buf[512];

    int n = luos_console_readline(prompt, buf, sizeof(buf));
    if (n == LUOS_CONSOLE_INTR) return CHAR_CTRL_C;
    if (n == LUOS_CONSOLE_EOF) return CHAR_CTRL_D;

    if (n > 0) vstr_add_strn((vstr_t *)line, buf, (size_t)n);
    return 0;
}

int mp_hal_stdin_rx_chr(void) {
    // Routed through the shared console helper (kernel/console.h) rather
    // than polling kbd->has_key() directly. has_key() does NOT pump the
    // keyboard backends by itself - vkbd_input()/vkbd_keyboard_handler()
    // are what drive backend_poll() and usb_core->poll_transfers(), so
    // the old loop here could sit forever on a USB keyboard whose
    // reports nobody was fetching. luos_console_getkey() pumps on every
    // iteration, exactly like the shell prompt does.
    int c = luos_console_getkey();
    if (c == LUOS_CONSOLE_EOF) return CHAR_CTRL_D;
    if (c == LUOS_CONSOLE_INTR) return CHAR_CTRL_C;
    return c;
}

// ----------------------------------------------------------------------
// Time / delays
// ----------------------------------------------------------------------

static struct tsc_driver *mp_hal_tsc(void) {
    static struct tsc_driver *tsc = NULL;
    if (!tsc) tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    return tsc;
}

mp_uint_t mp_hal_ticks_ms(void) {
    struct tsc_driver *t = mp_hal_tsc();
    return t ? (mp_uint_t)t->get_tsc_uptime_ms() : 0;
}

mp_uint_t mp_hal_ticks_us(void) {
    struct tsc_driver *t = mp_hal_tsc();
    return (t && t->get_tsc_uptime_us) ? (mp_uint_t)t->get_tsc_uptime_us() : 0;
}

mp_uint_t mp_hal_ticks_cpu(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((mp_uint_t)hi << 32) | lo;
}

void mp_hal_delay_ms(mp_uint_t ms) {
    if (current_task()) {
        scheduler_sleep_ms(ms);
        return;
    }
    struct tsc_driver *t = mp_hal_tsc();
    if (t) { t->sleep_tsc_ms(ms); return; }
    for (volatile mp_uint_t i = 0; i < ms * 100000u; i++) asm volatile("nop");
}

void mp_hal_delay_us(mp_uint_t us) {
    if (us >= 2000 && current_task()) {
        scheduler_sleep_ms(us / 1000);
        return;
    }
    struct tsc_driver *t = mp_hal_tsc();
    if (t) { t->sleep_tsc_us(us); return; }
    for (volatile mp_uint_t i = 0; i < us * 100u; i++) asm volatile("nop");
}
