#pragma GCC target("sse2")

#include <math.h>
#include <stdint.h>

#define _D_SIGN 0x8000000000000000ULL
#define _D_EXP 0x7FF0000000000000ULL
#define _D_MANT 0x000FFFFFFFFFFFFFULL
#define _D_BIAS 1023

#define _ISNAN(x) ({ _db _v; _v.d=(x); \
    ((_v.u&_D_EXP)==_D_EXP)&&((_v.u&_D_MANT)!=0ULL); })
#define _ISINF(x) ({ _db _v; _v.d=(x); \
    (_v.u&0x7FFFFFFFFFFFFFFFULL)==_D_EXP; })

int fpclassify(double x) {
    _db v; v.d = x;
    unsigned long long e = v.u & _D_EXP;
    unsigned long long m = v.u & _D_MANT;
    if (e == _D_EXP) return m ? FP_NAN : FP_INFINITE;
    if (e == 0ULL) return m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

double fabs(double x) {
    _db v; v.d = x; v.u &= ~_D_SIGN; return v.d;
}

double copysign(double x, double y) {
    _db vx, vy; vx.d = x; vy.d = y;
    vx.u = (vx.u & ~_D_SIGN) | (vy.u & _D_SIGN);
    return vx.d;
}

static inline void _mask_fpu_exceptions(uint16_t *saved_cw) {
    uint16_t cw;
    __asm__ volatile ("fstcw %0" : "=m"(cw));
    *saved_cw = cw;
    uint16_t masked = cw | 0x3F;
    __asm__ volatile ("fldcw %0" :: "m"(masked));
}

static inline void _restore_fpu_cw(uint16_t saved_cw) {

    __asm__ volatile ("fclex");
    __asm__ volatile ("fldcw %0" :: "m"(saved_cw));
}

static double _fpu_round(double x, unsigned short rc) {
    double r;
    unsigned short cw_save, cw_new;
    __asm__ volatile (
        "fstcw  %[cs]\n\t"
        "movw   %[cs], %%ax\n\t"
        "andw   $0xF3FF, %%ax\n\t"
        "orw    %[rc], %%ax\n\t"
        "orw    $0x003F, %%ax\n\t"
        "movw   %%ax, %[cn]\n\t"
        "fldcw  %[cn]\n\t"
        "fldl   %[x]\n\t"
        "frndint\n\t"
        "fstpl  %[r]\n\t"
        "fldcw  %[cs]"
        : [r]"=m"(r), [cs]"=m"(cw_save), [cn]"=m"(cw_new)
        : [x]"m"(x), [rc]"r"(rc)
        : "ax"
    );
    return r;
}

double floor(double x) { return _fpu_round(x, 0x0400); }
double ceil(double x) { return _fpu_round(x, 0x0800); }
double trunc(double x) { return _fpu_round(x, 0x0C00); }
double rint(double x) { return _fpu_round(x, 0x0000); }
double nearbyint(double x) { return _fpu_round(x, 0x0000); }

double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

double modf(double x, double *iptr) {
    double t = trunc(x);
    *iptr = t;
    return (x == t) ? copysign(0.0, x) : (x - t);
}

double frexp(double x, int *exp_out) {
    _db v; v.d = x;
    int cls = fpclassify(x);
    if (cls == FP_ZERO || cls == FP_NAN || cls == FP_INFINITE) {
        *exp_out = 0;
        return x;
    }
    int e = (int)((v.u >> 52) & 0x7FF);
    if (e == 0) {
        _db s; s.u = (unsigned long long)(_D_BIAS + 52) << 52;
        v.d *= s.d;
        e = (int)((v.u >> 52) & 0x7FF) - 52;
    }
    *exp_out = e - (_D_BIAS - 1);
    v.u = (v.u & ~_D_EXP) | ((unsigned long long)(_D_BIAS - 1) << 52);
    return v.d;
}

double ldexp(double x, int n) {
    _db v; v.d = x;
    int cls = fpclassify(x);
    if (cls == FP_ZERO || cls == FP_NAN || cls == FP_INFINITE) return x;
    int e = (int)((v.u >> 52) & 0x7FF) + n;
    if (e >= 0x7FF) return copysign(HUGE_VAL, x);
    if (e <= 0) return copysign(0.0, x);
    v.u = (v.u & ~_D_EXP) | ((unsigned long long)e << 52);
    return v.d;
}
double scalbn(double x, int n) { return ldexp(x, n); }

double fmod(double x, double y) {
    if (_ISNAN(x) || _ISNAN(y)) return NAN;
    if (y == 0.0 || _ISINF(x)) return NAN;
    if (_ISINF(y)) return x;
    if (x == 0.0) return x;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl   %[y]\n\t"
        "fldl   %[x]\n\t"
        "1:\n\t"
        "fprem\n\t"
        "fnstsw %%ax\n\t"
        "testb  $4, %%ah\n\t"
        "jnz    1b\n\t"
        "fstpl  %[r]\n\t"
        "fstp   %%st(0)"
        : [r]"=m"(r)
        : [x]"m"(x), [y]"m"(y)
        : "ax"
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double remainder(double x, double y) {
    if (_ISNAN(x) || _ISNAN(y)) return NAN;
    if (y == 0.0 || _ISINF(x)) return NAN;
    if (_ISINF(y)) return x;
    if (x == 0.0) return x;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl   %[y]\n\t"
        "fldl   %[x]\n\t"
        "1:\n\t"
        "fprem1\n\t"
        "fnstsw %%ax\n\t"
        "testb  $4, %%ah\n\t"
        "jnz    1b\n\t"
        "fstpl  %[r]\n\t"
        "fstp   %%st(0)"
        : [r]"=m"(r)
        : [x]"m"(x), [y]"m"(y)
        : "ax"
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double sqrt(double x) {
    if (_ISNAN(x)) return NAN;
    if (x < 0.0) return NAN;
    if (x == 0.0) return x;
    double r;
    __asm__ volatile (
        "fldl  %1\n\t"
        "fsqrt\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    return r;
}

double cbrt(double x) {
    if (x == 0.0 || _ISNAN(x) || _ISINF(x)) return x;
    double s = (x < 0.0) ? -1.0 : 1.0;
    double ax = fabs(x);
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double l;
    __asm__ volatile ("fldln2\n\t" "fldl %1\n\t" "fyl2x\n\t" "fstpl %0"
                      : "=m"(l) : "m"(ax));
    double l3 = l / 3.0;
    double r;
    __asm__ volatile (
        "fldl2e\n\t"
        "fmull  %[x]\n\t"
        "fld    %%st(0)\n\t"
        "frndint\n\t"
        "fxch   %%st(1)\n\t"
        "fsub   %%st(1),%%st\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp  %%st,%%st(1)\n\t"
        "fscale\n\t"
        "fstp   %%st(1)\n\t"
        "fstpl  %[r]"
        : [r]"=m"(r)
        : [x]"m"(l3)
    );
    _restore_fpu_cw(saved_cw);
    return s * r;
}

double hypot(double x, double y) {
    if (_ISINF(x) || _ISINF(y)) return HUGE_VAL;
    if (_ISNAN(x) || _ISNAN(y)) return NAN;
    double ax = fabs(x), ay = fabs(y);
    double hi = (ax > ay) ? ax : ay;
    double lo = (ax > ay) ? ay : ax;
    if (hi == 0.0) return 0.0;
    double t = lo / hi;
    return hi * sqrt(1.0 + t * t);
}

static double _log2r(double x) {

    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fld1\n\t"
        "fldl  %1\n\t"
        "fyl2x\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double log(double x) {
    if (_ISNAN(x)) return NAN;
    if (_ISINF(x)) {
        _db v; v.d = x;
        return (v.u & _D_SIGN) ? NAN : HUGE_VAL;
    }
    if (x == 0.0) return -HUGE_VAL;
    if (x < 0.0) return NAN;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile ("fldln2\n\t" "fldl %1\n\t" "fyl2x\n\t" "fstpl %0"
                      : "=m"(r) : "m"(x));
    _restore_fpu_cw(saved_cw);
    return r;
}

double log2(double x) {
    if (_ISNAN(x)) return NAN;
    if (_ISINF(x)) {
        _db v; v.d = x;
        return (v.u & _D_SIGN) ? NAN : HUGE_VAL;
    }
    if (x == 0.0) return -HUGE_VAL;
    if (x < 0.0) return NAN;
    return _log2r(x);
}

double log10(double x) {
    if (_ISNAN(x)) return NAN;
    if (_ISINF(x)) {
        _db v; v.d = x;
        return (v.u & _D_SIGN) ? NAN : HUGE_VAL;
    }
    if (x == 0.0) return -HUGE_VAL;
    if (x < 0.0) return NAN;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile ("fldlg2\n\t" "fldl %1\n\t" "fyl2x\n\t" "fstpl %0"
                      : "=m"(r) : "m"(x));
    _restore_fpu_cw(saved_cw);
    return r;
}

double log1p(double x) {
    if (_ISNAN(x)) return NAN;
    if (x == -1.0) return -HUGE_VAL;
    if (x < -1.0) return NAN;
    if (fabs(x) < 2.2e-16) return x;
    return log(1.0 + x);
}

double exp(double x) {
    if (_ISNAN(x)) return NAN;
    if (x == 0.0) return 1.0;
    if (x > 709.782) return HUGE_VAL;
    if (x < -745.133) return 0.0;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl2e\n\t"
        "fmull  %[x]\n\t"
        "fld    %%st(0)\n\t"
        "frndint\n\t"
        "fxch   %%st(1)\n\t"
        "fsub   %%st(1), %%st\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp  %%st, %%st(1)\n\t"
        "fscale\n\t"
        "fstp   %%st(1)\n\t"
        "fstpl  %[r]"
        : [r]"=m"(r)
        : [x]"m"(x)
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double exp2(double x) {
    if (_ISNAN(x)) return NAN;
    if (x > 1023.0) return HUGE_VAL;
    if (x < -1074.0) return 0.0;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl   %[x]\n\t"
        "fld    %%st(0)\n\t"
        "frndint\n\t"
        "fxch   %%st(1)\n\t"
        "fsub   %%st(1), %%st\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp  %%st, %%st(1)\n\t"
        "fscale\n\t"
        "fstp   %%st(1)\n\t"
        "fstpl  %[r]"
        : [r]"=m"(r)
        : [x]"m"(x)
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double expm1(double x) {
    if (_ISNAN(x)) return NAN;
    if (fabs(x) < 1e-8) return x + x * x * 0.5;
    return exp(x) - 1.0;
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (_ISNAN(x) || _ISNAN(y)) return NAN;
    if (x == -1.0 && _ISINF(y)) return 1.0;
    if (x < 0.0) {
        double yi = trunc(y);
        if (y != yi) return NAN;
        double mag = exp2(y * _log2r(-x));
        return ((long long)yi & 1LL) ? -mag : mag;
    }
    if (x == 0.0) return (y < 0.0) ? HUGE_VAL : 0.0;
    if (_ISINF(x)) return (y < 0.0) ? 0.0 : HUGE_VAL;
    return exp2(y * _log2r(x));
}

double sin(double x) {
    if (_ISNAN(x) || _ISINF(x)) return NAN;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile ("fldl %1\n\t" "fsin\n\t" "fstpl %0"
                      : "=m"(r) : "m"(x));
    _restore_fpu_cw(saved_cw);
    return r;
}

double cos(double x) {
    if (_ISNAN(x) || _ISINF(x)) return NAN;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile ("fldl %1\n\t" "fcos\n\t" "fstpl %0"
                      : "=m"(r) : "m"(x));
    _restore_fpu_cw(saved_cw);
    return r;
}

double tan(double x) {
    if (_ISNAN(x) || _ISINF(x)) return NAN;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl  %1\n\t"
        "fptan\n\t"
        "fstp  %%st(0)\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double atan2(double y, double x) {
    if (_ISNAN(x) || _ISNAN(y)) return NAN;
    if (x == 0.0 && y == 0.0) return 0.0;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl  %2\n\t"
        "fldl  %1\n\t"
        "fpatan\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(y), "m"(x)
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double atan(double x) {
    if (_ISNAN(x)) return NAN;
    uint16_t saved_cw;
    _mask_fpu_exceptions(&saved_cw);
    double r;
    __asm__ volatile (
        "fldl  %1\n\t"
        "fld1\n\t"
        "fpatan\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    _restore_fpu_cw(saved_cw);
    return r;
}

double asin(double x) {
    if (_ISNAN(x)) return NAN;
    if (x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0) return M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (_ISNAN(x)) return NAN;
    if (x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0) return 0.0;
    if (x == -1.0) return M_PI;
    return atan2(sqrt(1.0 - x * x), x);
}

double sinh(double x) {
    if (_ISNAN(x) || _ISINF(x)) return x;
    if (fabs(x) < 1e-8) return x;
    if (fabs(x) > 709.0) return copysign(HUGE_VAL, x);
    double ex = exp(x);
    return (ex - 1.0 / ex) * 0.5;
}

double cosh(double x) {
    if (_ISNAN(x)) return NAN;
    if (_ISINF(x)) return HUGE_VAL;
    if (fabs(x) > 709.0) return HUGE_VAL;
    double ex = exp(x);
    return (ex + 1.0 / ex) * 0.5;
}

double tanh(double x) {
    if (_ISNAN(x)) return NAN;
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2x = exp(2.0 * x);
    return (e2x - 1.0) / (e2x + 1.0);
}

double asinh(double x) {
    if (_ISNAN(x) || _ISINF(x)) return x;
    if (fabs(x) < 1e-8) return x;
    return log(x + sqrt(x * x + 1.0));
}

double acosh(double x) {
    if (_ISNAN(x)) return NAN;
    if (x < 1.0) return NAN;
    if (_ISINF(x)) return HUGE_VAL;
    if (x == 1.0) return 0.0;
    return log(x + sqrt(x * x - 1.0));
}

double atanh(double x) {
    if (_ISNAN(x)) return NAN;
    if (fabs(x) > 1.0) return NAN;
    if (fabs(x) == 1.0) return copysign(HUGE_VAL, x);
    if (fabs(x) < 1e-8) return x;
    return log1p(2.0 * x / (1.0 - x)) * 0.5;
}

double fmax(double x, double y) {
    if (_ISNAN(x)) return y;
    if (_ISNAN(y)) return x;
    return (x > y) ? x : y;
}

double fmin(double x, double y) {
    if (_ISNAN(x)) return y;
    if (_ISNAN(y)) return x;
    return (x < y) ? x : y;
}

double fdim(double x, double y) {
    if (_ISNAN(x) || _ISNAN(y)) return NAN;
    return (x > y) ? (x - y) : 0.0;
}

double fma(double x, double y, double z) {
    if (_ISNAN(x) || _ISNAN(y) || _ISNAN(z)) return NAN;
    return x * y + z;
}

// POSIX double nan(const char *tagp) — the NaN payload tag isn't
// supported here (not needed by MicroPython's number parser, which
// calls nan("")); we just return a "quiet" NaN.
double nan(const char *tagp) {
    (void)tagp;
    return NAN;
}

void fpu_init(void) {
    unsigned long long cr0, cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 &= ~(1ULL << 3);
    cr0 |= (1ULL << 1);
    cr0 |= (1ULL << 5);
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    __asm__ volatile ("finit");

    uint16_t cw = 0x37F;
    __asm__ volatile ("fldcw %0" :: "m"(cw));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));

    uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr));
}