#ifndef STDIO_H
#define STDIO_H

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#define STDIO_MAX_STREAMS 8
#define STDIO_STREAM_STDOUT 0
#define STDIO_STREAM_STDERR 1

typedef struct {

    void (*write)(const char *str, uint32_t color);

    uint32_t default_color;

    uint32_t error_color;

    const char *name;

    int active;
} stdio_stream_t;

extern stdio_stream_t stdio_streams[STDIO_MAX_STREAMS];

int stdio_register_stream(int index, void (*write_fn)(const char*, uint32_t), uint32_t default_color, uint32_t error_color, const char *name);

void stdio_register_video_stream(void);

void stdio_unregister_stream(int index);

int printf(const char *fmt, ...);

int fprintf_stream(int stream_idx, uint32_t color, const char *fmt, ...);

int vprintf_stream(int stream_idx, uint32_t color, const char *fmt, va_list ap);

int puts(const char *s);
int putchar(int c);

int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#define printf_color(color, fmt, ...) \
    fprintf_stream(STDIO_STREAM_STDOUT, (color), (fmt), ##__VA_ARGS__)

#define eprintf(fmt, ...) \
    fprintf_stream(STDIO_STREAM_STDERR, 0xFFFF5555, (fmt), ##__VA_ARGS__)

#endif