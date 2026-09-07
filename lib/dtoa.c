#include "dtoa.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

static int _is_nan(double x) {
    union { double d; uint64_t u; } v;
    v.d = x;
    return ((v.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
        && ((v.u & 0x000FFFFFFFFFFFFFULL) != 0ULL);
}

static int _is_inf(double x) {
    union { double d; uint64_t u; } v;
    v.d = x;
    return (v.u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}

static int _is_neg(double x) {
    union { double d; uint64_t u; } v;
    v.d = x;
    return (v.u >> 63) & 1;
}

static int _uint_to_buf(char *buf, size_t sz, uint64_t val) {
    char tmp[24];
    int len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    else {
        while (val > 0) { tmp[len++] = '0' + (val % 10); val /= 10; }
    }
    if ((size_t)len >= sz) len = (int)sz - 1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

static int _fmt_fixed(double value, int prec, char *buf, size_t sz) {
    int pos = 0;

    if (_is_neg(value) && value != 0.0) {
        if ((size_t)pos < sz) buf[pos++] = '-';
        value = -value;
    }

    double int_part;
    double frac_part = modf(value, &int_part);

    if (int_part == 0.0) {
        if ((size_t)pos < sz) buf[pos++] = '0';
    } else {
        char tmp[32];
        int tlen = 0;

        double ip = int_part;
        if (ip >= 1e18) {

            int tpos = _uint_to_buf(tmp, sizeof(tmp), (uint64_t)ip);
            for (int i = 0; i < tpos && (size_t)pos < sz; i++)
                buf[pos++] = tmp[i];
        } else {
            uint64_t ival = (uint64_t)ip;
            tlen = _uint_to_buf(tmp, sizeof(tmp), ival);
            for (int i = 0; i < tlen && (size_t)pos < sz; i++)
                buf[pos++] = tmp[i];
        }
    }

    if (prec > 0) {
        if ((size_t)pos < sz) buf[pos++] = '.';

        if (frac_part < 0.0) frac_part = -frac_part;

        for (int i = 0; i < prec && (size_t)pos < sz; i++) {
            frac_part *= 10.0;
            int digit = (int)frac_part;
            if (digit > 9) digit = 9;
            if (digit < 0) digit = 0;
            buf[pos++] = '0' + digit;
            frac_part -= (double)digit;
        }

        if (frac_part >= 0.5 && pos > 0) {
            int carry = 1;
            for (int i = pos - 1; i >= 0 && carry; i--) {
                if (buf[i] == '.') continue;
                if (buf[i] == '-') break;
                int d = buf[i] - '0' + carry;
                carry = d / 10;
                buf[i] = '0' + (d % 10);
            }
            if (carry) {

                if ((size_t)(pos + 1) < sz) {
                    int start = (_is_neg(-value)) ? 1 : 0;
                    memmove(buf + start + 1, buf + start, pos - start);
                    buf[start] = '1';
                    pos++;
                }
            }
        }
    } else if (prec == 0) {

        if (frac_part >= 0.5) {

            int carry = 1;
            for (int i = pos - 1; i >= 0 && carry; i--) {
                if (buf[i] == '-') break;
                int d = buf[i] - '0' + carry;
                carry = d / 10;
                buf[i] = '0' + (d % 10);
            }
        }
    }

    if ((size_t)pos < sz) buf[pos] = '\0';
    else if (sz > 0) buf[sz - 1] = '\0';

    return pos;
}

static int _fmt_scientific(double value, int prec, int upper, char *buf, size_t sz) {
    int pos = 0;

    if (_is_neg(value) && value != 0.0) {
        if ((size_t)pos < sz) buf[pos++] = '-';
        value = -value;
    }

    int exponent = 0;

    if (value == 0.0) {

    } else {

        exponent = (int)floor(log10(value));
        value = value / pow(10.0, (double)exponent);

        if (value >= 10.0) { value /= 10.0; exponent++; }
        if (value < 1.0 && value > 0.0) { value *= 10.0; exponent--; }
    }

    char mantissa[64];
    _fmt_fixed(value, prec, mantissa, sizeof(mantissa));

    for (int i = 0; mantissa[i] && (size_t)pos < sz; i++)
        buf[pos++] = mantissa[i];

    if ((size_t)pos < sz) buf[pos++] = upper ? 'E' : 'e';

    if (exponent < 0) {
        if ((size_t)pos < sz) buf[pos++] = '-';
        exponent = -exponent;
    } else {
        if ((size_t)pos < sz) buf[pos++] = '+';
    }

    if (exponent < 10) {
        if ((size_t)pos < sz) buf[pos++] = '0';
        if ((size_t)pos < sz) buf[pos++] = '0' + exponent;
    } else if (exponent < 100) {
        if ((size_t)pos < sz) buf[pos++] = '0' + (exponent / 10);
        if ((size_t)pos < sz) buf[pos++] = '0' + (exponent % 10);
    } else {
        char tmp[8];
        int tlen = _uint_to_buf(tmp, sizeof(tmp), (uint64_t)exponent);
        for (int i = 0; i < tlen && (size_t)pos < sz; i++)
            buf[pos++] = tmp[i];
    }

    if ((size_t)pos < sz) buf[pos] = '\0';
    else if (sz > 0) buf[sz - 1] = '\0';

    return pos;
}

static int _fmt_general(double value, int prec, int upper, char *buf, size_t sz) {
    if (prec <= 0) prec = 1;

    double abs_val = value < 0 ? -value : value;
    int exponent = 0;

    if (abs_val != 0.0 && !_is_nan(abs_val) && !_is_inf(abs_val)) {
        exponent = (int)floor(log10(abs_val));
    }

    int len;

    if (exponent < -4 || exponent >= prec) {
        len = _fmt_scientific(value, prec - 1, upper, buf, sz);
    } else {
        int fprec = prec - 1 - exponent;
        if (fprec < 0) fprec = 0;
        len = _fmt_fixed(value, fprec, buf, sz);
    }

    int has_dot = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '.') { has_dot = 1; break; }
        if (buf[i] == 'e' || buf[i] == 'E') break;
    }

    if (has_dot) {

        int exp_start = len;
        for (int i = 0; i < len; i++) {
            if (buf[i] == 'e' || buf[i] == 'E') { exp_start = i; break; }
        }

        int end = exp_start;
        while (end > 0 && buf[end - 1] == '0') end--;
        if (end > 0 && buf[end - 1] == '.') end--;

        if (end < exp_start) {

            memmove(buf + end, buf + exp_start, len - exp_start);
            len = end + (len - exp_start);
            buf[len] = '\0';
        }
    }

    return len;
}

int dtoa_format(double value, char fmt_char, int precision, char *buf, size_t bufsize) {
    if (bufsize == 0) return 0;

    if (_is_nan(value)) {
        const char *s = (fmt_char >= 'A' && fmt_char <= 'Z') ? "NAN" : "nan";
        int i = 0;
        while (s[i] && (size_t)(i + 1) < bufsize) { buf[i] = s[i]; i++; }
        buf[i] = '\0';
        return i;
    }

    if (_is_inf(value)) {
        int pos = 0;
        if (_is_neg(value)) {
            if ((size_t)(pos + 1) < bufsize) buf[pos++] = '-';
        }
        const char *s = (fmt_char >= 'A' && fmt_char <= 'Z') ? "INF" : "inf";
        int i = 0;
        while (s[i] && (size_t)(pos + 1) < bufsize) { buf[pos++] = s[i++]; }
        buf[pos] = '\0';
        return pos;
    }

    switch (fmt_char) {
        case 'f': case 'F':
            if (precision < 0) precision = 6;
            return _fmt_fixed(value, precision, buf, bufsize);

        case 'e': case 'E':
            if (precision < 0) precision = 6;
            return _fmt_scientific(value, precision, fmt_char == 'E', buf, bufsize);

        case 'g': case 'G':
            if (precision < 0) precision = 6;
            return _fmt_general(value, precision, fmt_char == 'G', buf, bufsize);

        default:
            if (precision < 0) precision = 6;
            return _fmt_general(value, precision, 0, buf, bufsize);
    }
}