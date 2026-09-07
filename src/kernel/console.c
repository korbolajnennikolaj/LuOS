#include "kernel/console.h"

#include "components/drivers.h"
#include "drivers/Input/keyboard_driver.h"
#include "drivers/Timer/pit_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Video/limine_video_driver.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CONSOLE_INPUT_COLOR LIMINE_COLOR_LIGHT_GREEN

#define CONSOLE_REPEAT_DELAY_MS 400u
#define CONSOLE_REPEAT_RATE_MS 35u

static struct limine_video_driver *console_video(void) {
    return (struct limine_video_driver *)get_self_driver(LIMINE_VIDEO_DRIVER, 0);
}

static uint64_t console_uptime_ms(void) {
    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    if (tsc && tsc->get_tsc_uptime_ms) return tsc->get_tsc_uptime_ms();

    struct pit_driver *pit = (struct pit_driver *)get_self_driver(TIMER_DRIVER, PIT_TIMER);
    if (pit && pit->get_pit_ms) return pit->get_pit_ms();

    return 0;
}

static void console_echo(const char *s) {
    struct limine_video_driver *v = console_video();
    if (v && v->printf) v->printf(s, CONSOLE_INPUT_COLOR);
    else printf("%s", s);
}

void luos_console_write(const char *s) {
    if (!s || !s[0]) return;
    printf("%s", s);
}

int luos_console_readline(const char *prompt, char *buf, size_t cap) {
    if (!buf || cap == 0) return LUOS_CONSOLE_EOF;
    buf[0] = '\0';

    struct keyboard_driver *kbd = return_keyboard_driver();
    if (!kbd || !kbd->has_key || !kbd->get_key_event) {

        return LUOS_CONSOLE_EOF;
    }

    if (prompt && prompt[0]) luos_console_write(prompt);

    size_t idx = 0;

    int hold_valid = 0;
    uint8_t hold_sc = 0;
    char hold_ch = 0;
    enum KEYBOARD_TYPE hold_src = PS2_KEYBOARD;
    uint64_t hold_start_ms = 0;
    uint64_t last_repeat_ms = 0;

    for (;;) {

        if (kbd->keyboard_handler) kbd->keyboard_handler();

        if (hold_valid && kbd->is_key_held_from) {
            if (!kbd->is_key_held_from(hold_src, hold_sc)) {
                hold_valid = 0;
            } else {
                uint64_t now_ms = console_uptime_ms();
                if (now_ms != 0 && now_ms - hold_start_ms >= CONSOLE_REPEAT_DELAY_MS) {
                    if (!last_repeat_ms) last_repeat_ms = hold_start_ms + CONSOLE_REPEAT_DELAY_MS;
                    if (now_ms - last_repeat_ms >= CONSOLE_REPEAT_RATE_MS) {
                        last_repeat_ms = now_ms;

                        if (hold_ch == '\b') {
                            if (idx > 0) { idx--; console_echo("\b \b"); }
                        } else if (hold_ch >= 32 && hold_ch <= 126 && idx + 1 < cap) {
                            buf[idx++] = hold_ch;
                            char s[2] = { hold_ch, '\0' };
                            console_echo(s);
                        }
                    }
                }
            }
        }

        struct key_event ev;
        if (!kbd->get_key_event(&ev)) {

            if (kbd->get_active_type && kbd->get_active_type() == USB_KEYBOARD)
                asm volatile("pause");
            else
                asm volatile("hlt");
            continue;
        }

        if (ev.ctrl) {
            if (ev.code == KEY_D) {
                if (idx == 0) {
                    console_echo("\n");
                    return LUOS_CONSOLE_EOF;
                }

                console_echo("\n");
                buf[idx] = '\0';
                return (int)idx;
            }
            if (ev.code == KEY_C) {
                console_echo("^C\n");
                buf[0] = '\0';
                return LUOS_CONSOLE_INTR;
            }

            continue;
        }

        if (ev.is_enter || ev.ch == '\n' || ev.ch == '\r') {
            console_echo("\n");
            buf[idx] = '\0';
            return (int)idx;
        }

        if (ev.is_backspace || ev.ch == '\b') {
            if (idx > 0) { idx--; console_echo("\b \b"); }
            hold_valid = 1; hold_sc = ev.scancode; hold_src = ev.source; hold_ch = '\b';
            hold_start_ms = console_uptime_ms();
            last_repeat_ms = 0;
            continue;
        }

        if (ev.ch >= 32 && ev.ch <= 126) {
            if (idx + 1 < cap) {
                buf[idx++] = ev.ch;
                char s[2] = { ev.ch, '\0' };
                console_echo(s);
            }
            hold_valid = 1; hold_sc = ev.scancode; hold_src = ev.source; hold_ch = ev.ch;
            hold_start_ms = console_uptime_ms();
            last_repeat_ms = 0;
        }
    }
}

#define CONSOLE_LINE_MAX 512

static char line_buf[CONSOLE_LINE_MAX + 2];
static size_t line_pos = 0;
static size_t line_len = 0;

static int pushback = LUOS_CONSOLE_EOF;
static int eof_seen = 0;

int luos_console_getc(void) {
    if (pushback != LUOS_CONSOLE_EOF) {
        int c = pushback;
        pushback = LUOS_CONSOLE_EOF;
        return c;
    }

    if (line_pos < line_len) {
        eof_seen = 0;
        return (unsigned char)line_buf[line_pos++];
    }

    int n = luos_console_readline("", line_buf, CONSOLE_LINE_MAX);
    if (n < 0) {
        line_pos = line_len = 0;
        eof_seen = 1;
        return LUOS_CONSOLE_EOF;
    }

    line_buf[n] = '\n';
    line_buf[n + 1] = '\0';
    line_pos = 0;
    line_len = (size_t)n + 1;
    eof_seen = 0;

    return (unsigned char)line_buf[line_pos++];
}

void luos_console_ungetc(int c) {
    if (c == LUOS_CONSOLE_EOF) return;
    pushback = c;
    eof_seen = 0;
}

void luos_console_flush_input(void) {
    line_pos = line_len = 0;
    pushback = LUOS_CONSOLE_EOF;
    eof_seen = 0;
}

int luos_console_at_eof(void) {
    return eof_seen;
}

int luos_console_has_key(void) {
    struct keyboard_driver *kbd = return_keyboard_driver();
    if (!kbd || !kbd->has_key) return 0;
    if (kbd->keyboard_handler) kbd->keyboard_handler();
    if (pushback != LUOS_CONSOLE_EOF || line_pos < line_len) return 1;
    return kbd->has_key() ? 1 : 0;
}

int luos_console_getkey(void) {
    struct keyboard_driver *kbd = return_keyboard_driver();
    if (!kbd || !kbd->get_key_event) return LUOS_CONSOLE_EOF;

    for (;;) {
        if (kbd->keyboard_handler) kbd->keyboard_handler();

        struct key_event ev;
        if (!kbd->get_key_event(&ev)) {
            if (kbd->get_active_type && kbd->get_active_type() == USB_KEYBOARD)
                asm volatile("pause");
            else
                asm volatile("hlt");
            continue;
        }

        if (ev.ctrl) {
            if (ev.code == KEY_D) return LUOS_CONSOLE_EOF;
            if (ev.code == KEY_C) return LUOS_CONSOLE_INTR;
            continue;
        }

        if (ev.is_enter || ev.ch == '\n' || ev.ch == '\r') return '\n';
        if (ev.is_backspace || ev.ch == '\b') return '\b';
        if (ev.ch >= 32 && ev.ch <= 126) return (int)(unsigned char)ev.ch;
        if (ev.ch == '\t') return '\t';
    }
}
