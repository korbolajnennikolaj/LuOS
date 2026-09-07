#ifndef LUOS_CONSOLE_H
#define LUOS_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUOS_CONSOLE_EOF (-1)

#define LUOS_CONSOLE_INTR (-2)

int luos_console_readline(const char *prompt, char *buf, size_t cap);

int luos_console_getc(void);

void luos_console_ungetc(int c);

void luos_console_flush_input(void);

int luos_console_at_eof(void);

int luos_console_has_key(void);

int luos_console_getkey(void);

void luos_console_write(const char *s);

#ifdef __cplusplus
}
#endif

#endif
