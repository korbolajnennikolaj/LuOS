#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "dtoa.h"

#include "drivers/Video/limine_video_driver.h"
#include "components/drivers.h"
#include "kernel/scheduler/spinlock.h"

static spinlock_t stdio_out_lock = SPINLOCK_INIT;

stdio_stream_t stdio_streams[STDIO_MAX_STREAMS] = {0};

int stdio_register_stream(int index, void (*write_fn)(const char*, uint32_t), uint32_t default_color, uint32_t error_color, const char *name)
{
    if (index < 0 || index >= STDIO_MAX_STREAMS) return -1;
    if (!write_fn) return -1;

    stdio_streams[index].write = write_fn;
    stdio_streams[index].default_color = default_color;
    stdio_streams[index].error_color = error_color;
    stdio_streams[index].name = name;
    stdio_streams[index].active = 1;
    return index;
}

void stdio_unregister_stream(int index) {
    if (index < 0 || index >= STDIO_MAX_STREAMS) return;
    stdio_streams[index].active = 0;
    stdio_streams[index].write = 0;
}

static void _video_write(const char *str, uint32_t color) {
    struct limine_video_driver *v =
        (struct limine_video_driver *)get_self_driver(LIMINE_VIDEO_DRIVER, 0);
    if (v && v->printf) {
        v->printf(str, color);
    }
}

void stdio_register_video_stream(void) {

    stdio_register_stream(
        STDIO_STREAM_STDOUT,
        _video_write,
        LIMINE_COLOR_WHITE,
        LIMINE_COLOR_LIGHT_RED,
        "video_stdout"
    );

    stdio_register_stream(
        STDIO_STREAM_STDERR,
        _video_write,
        LIMINE_COLOR_LIGHT_RED,
        LIMINE_COLOR_LIGHT_RED,
        "video_stderr"
    );
}

static void _str_reverse(char *s, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char t = s[i]; s[i] = s[j]; s[j] = t;
        i++; j--;
    }
}

static int _u64_to_str(uint64_t val, char *buf, int base, const char *digits) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    int len = 0;
    while (val) {
        buf[len++] = digits[val % base];
        val /= base;
    }
    buf[len] = '\0';
    _str_reverse(buf, len);
    return len;
}

static int _uint_to_str_r(uint64_t val, char *buf, size_t sz, int base, const char *digits) {
    char tmp[24];
    int len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    else {
        while (val > 0 && len < 23) {
            tmp[len++] = digits[val % base];
            val /= base;
        }
    }
    if ((size_t)len >= sz) len = (int)sz - 1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    int stream;
    uint32_t color;
} _fmt_ctx;

static void _ctx_putc(_fmt_ctx *ctx, char c) {
    if (ctx->buf) {
        if (ctx->cap == 0 || ctx->pos + 1 < ctx->cap) {
            ctx->buf[ctx->pos] = c;
        }
    } else {

        char tmp[2] = { c, '\0' };
        if (ctx->stream >= 0 && ctx->stream < STDIO_MAX_STREAMS) {
            stdio_stream_t *st = &stdio_streams[ctx->stream];
            if (st->active && st->write) {
                st->write(tmp, ctx->color);
            }
        }
    }
    ctx->pos++;
}

static void _ctx_puts(_fmt_ctx *ctx, const char *s) {
    if (!s) s = "(null)";

    if (ctx->buf) {
        while (*s) _ctx_putc(ctx, *s++);
    } else {
        if (ctx->stream >= 0 && ctx->stream < STDIO_MAX_STREAMS) {
            stdio_stream_t *st = &stdio_streams[ctx->stream];
            if (st->active && st->write) {
                st->write(s, ctx->color);

                while (*s++) ctx->pos++;
            }
        }
    }
}

static void _ctx_padded(_fmt_ctx *ctx, const char *s, int len, int width, int left_align, char pad_char) {
    if (width > len && !left_align) {
        for (int i = 0; i < width - len; i++) _ctx_putc(ctx, pad_char);
    }
    for (int i = 0; i < len; i++) _ctx_putc(ctx, s[i]);
    if (width > len && left_align) {
        for (int i = 0; i < width - len; i++) _ctx_putc(ctx, ' ');
    }
}

static int _vformat(_fmt_ctx *ctx, const char *fmt, va_list ap) {
    char num_buf[80];

    while (*fmt) {
        if (*fmt != '%') {
            _ctx_putc(ctx, *fmt++);
            continue;
        }
        fmt++;

        int left_align = 0;
        int zero_pad = 0;
        int plus_sign = 0;
        int space_sign = 0;
        int hash_flag = 0;

        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '+') { plus_sign = 1; fmt++; }
            else if (*fmt == ' ') { space_sign = 1; fmt++; }
            else if (*fmt == '#') { hash_flag = 1; fmt++; }
            else break;
        }

        if (left_align) zero_pad = 0;

        int width = 0;
        int has_width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left_align = 1; width = -width; }
            has_width = 1;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                has_width = 1;
                fmt++;
            }
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                if (precision < 0) precision = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int is_long = 0;
        int is_short = 0;
        int is_size_t = 0;
        int is_ptrdiff = 0;
        int is_intmax = 0;

        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }
        else if (*fmt == 'h') { is_short = 1; fmt++; if (*fmt == 'h') { is_short = 2; fmt++; } }
        else if (*fmt == 'z') { is_size_t = 1; fmt++; }
        else if (*fmt == 't') { is_ptrdiff = 1; fmt++; }
        else if (*fmt == 'j') { is_intmax = 1; fmt++; }
        (void)is_short; (void)is_size_t; (void)is_ptrdiff; (void)is_intmax;
        (void)has_width; (void)hash_flag; (void)plus_sign; (void)space_sign;

        char spec = *fmt++;

        switch (spec) {
        case 'd':
        case 'i': {
            int64_t v;
            if (is_long >= 2) v = va_arg(ap, long long);
            else if (is_long == 1) v = va_arg(ap, long);
            else v = (int64_t)va_arg(ap, int);

            char sign = 0;
            uint64_t uv;
            if (v < 0) { sign = '-'; uv = (uint64_t)(-v); }
            else { uv = (uint64_t)v; if (plus_sign) sign = '+'; else if (space_sign) sign = ' '; }

            char tmp[24];
            int tlen = _uint_to_str_r(uv, tmp, sizeof(tmp), 10, "0123456789");
            int total = tlen + (sign ? 1 : 0);

            if (zero_pad && width > total) {
                if (sign) _ctx_putc(ctx, sign);
                for (int i = 0; i < width - total; i++) _ctx_putc(ctx, '0');
                for (int i = 0; i < tlen; i++) _ctx_putc(ctx, tmp[i]);
            } else {
                char full[32];
                int flen = 0;
                if (sign) full[flen++] = sign;
                for (int i = 0; i < tlen && flen < 31; i++) full[flen++] = tmp[i];
                full[flen] = '\0';
                _ctx_padded(ctx, full, flen, width, left_align, ' ');
            }
            break;
        }

        case 'u': {
            uint64_t v;
            if (is_long >= 2) v = va_arg(ap, unsigned long long);
            else if (is_long == 1) v = va_arg(ap, unsigned long);
            else v = (uint64_t)va_arg(ap, unsigned int);

            char tmp[24];
            int tlen = _uint_to_str_r(v, tmp, sizeof(tmp), 10, "0123456789");
            _ctx_padded(ctx, tmp, tlen, width, left_align, zero_pad ? '0' : ' ');
            break;
        }

        case 'x': case 'X': {
            uint64_t v;
            if (is_long >= 2) v = va_arg(ap, unsigned long long);
            else if (is_long == 1) v = va_arg(ap, unsigned long);
            else v = (uint64_t)va_arg(ap, unsigned int);

            const char *digits = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[20];
            int tlen = _uint_to_str_r(v, tmp, sizeof(tmp), 16, digits);
            _ctx_padded(ctx, tmp, tlen, width, left_align, zero_pad ? '0' : ' ');
            break;
        }

        case 'o': {
            uint64_t v;
            if (is_long >= 2) v = va_arg(ap, unsigned long long);
            else if (is_long == 1) v = va_arg(ap, unsigned long);
            else v = (uint64_t)va_arg(ap, unsigned int);

            char tmp[24];
            int tlen = _uint_to_str_r(v, tmp, sizeof(tmp), 8, "01234567");
            _ctx_padded(ctx, tmp, tlen, width, left_align, zero_pad ? '0' : ' ');
            break;
        }

        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void*);
            char tmp[20];
            tmp[0] = '0'; tmp[1] = 'x';
            for (int i = 0; i < 16; i++)
                tmp[2 + i] = "0123456789abcdef"[(v >> (60 - i*4)) & 0xF];
            tmp[18] = '\0';
            _ctx_padded(ctx, tmp, 18, width, left_align, ' ');
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int slen = (int)strlen(s);
            if (precision >= 0 && precision < slen) slen = precision;
            _ctx_padded(ctx, s, slen, width, left_align, ' ');
            break;
        }

        case 'c': {
            char c = (char)va_arg(ap, int);
            _ctx_padded(ctx, &c, 1, width, left_align, ' ');
            break;
        }

        case 'f': case 'F':
        case 'e': case 'E':
        case 'g': case 'G': {
            double fval = va_arg(ap, double);
            int prec = (precision >= 0) ? precision : -1;
            int len = dtoa_format(fval, spec, prec, num_buf, sizeof(num_buf));
            _ctx_padded(ctx, num_buf, len, width, left_align, zero_pad ? '0' : ' ');
            break;
        }

        case 'a': case 'A': {

            double fval = va_arg(ap, double);
            int prec = (precision >= 0) ? precision : 14;
            int len = dtoa_format(fval, 'g', prec, num_buf, sizeof(num_buf));
            _ctx_padded(ctx, num_buf, len, width, left_align, ' ');
            break;
        }

        case 'n':

            (void)va_arg(ap, int*);
            break;

        case '%':
            _ctx_putc(ctx, '%');
            break;

        case '\0':
            fmt--;
            break;

        default:

            _ctx_putc(ctx, '%');
            _ctx_putc(ctx, spec);
            break;
        }
    }

    if (ctx->buf && ctx->cap > 0) {
        size_t term = ctx->pos < ctx->cap ? ctx->pos : ctx->cap - 1;
        ctx->buf[term] = '\0';
    }

    return (int)ctx->pos;
}

int vprintf_stream(int stream_idx, uint32_t color, const char *fmt, va_list ap)
{
    _fmt_ctx ctx = {
        .buf = NULL,
        .cap = 0,
        .pos = 0,
        .stream = stream_idx,
        .color = color
    };
    uint64_t flags = spin_lock_irqsave(&stdio_out_lock);
    int n = _vformat(&ctx, fmt, ap);
    spin_unlock_irqrestore(&stdio_out_lock, flags);
    return n;
}

int fprintf_stream(int stream_idx, uint32_t color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf_stream(stream_idx, color, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    stdio_stream_t *st = &stdio_streams[STDIO_STREAM_STDOUT];
    uint32_t color = st->active ? st->default_color : (uint32_t)LIMINE_COLOR_WHITE;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf_stream(STDIO_STREAM_STDOUT, color, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    int n = fprintf_stream(STDIO_STREAM_STDOUT,
                           stdio_streams[STDIO_STREAM_STDOUT].default_color,
                           "%s\n", s);
    return n;
}

int putchar(int c) {
    char buf[2] = { (char)c, '\0' };
    stdio_stream_t *st = &stdio_streams[STDIO_STREAM_STDOUT];
    if (st->active && st->write) {
        uint64_t flags = spin_lock_irqsave(&stdio_out_lock);
        st->write(buf, st->default_color);
        spin_unlock_irqrestore(&stdio_out_lock, flags);
    }
    return c;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    _fmt_ctx ctx = {
        .buf = buf,
        .cap = (size_t)-1,
        .pos = 0,
        .stream = -1,
        .color = 0
    };
    return _vformat(&ctx, fmt, ap);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    _fmt_ctx ctx = {
        .buf = buf,
        .cap = n,
        .pos = 0,
        .stream = -1,
        .color = 0
    };
    return _vformat(&ctx, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}