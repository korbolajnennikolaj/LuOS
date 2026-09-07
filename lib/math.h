#ifndef MATH_H
#define MATH_H

#define M_E 2.71828182845904523536
#define M_LOG2E 1.44269504088896340736
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

typedef union {
    double d;
    unsigned long long u;
} _db;

static inline double __math_inf(void) {
    union { unsigned long long u; double d; } v = { 0x7FF0000000000000ULL };
    return v.d;
}
static inline double __math_nan(void) {
    union { unsigned long long u; double d; } v = { 0x7FF8000000000000ULL };
    return v.d;
}
#define HUGE_VAL (__math_inf())
#define INFINITY (__math_inf())
#define NAN (__math_nan())

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

static inline int __isnan_d(double x) {
    union { unsigned long long u; double d; } v; v.d = x;
    return ((v.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
        && ((v.u & 0x000FFFFFFFFFFFFFULL) != 0ULL);
}
static inline int __isinf_d(double x) {
    union { unsigned long long u; double d; } v; v.d = x;
    return (v.u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}
static inline int __isfinite_d(double x) {
    union { unsigned long long u; double d; } v; v.d = x;
    return (v.u & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}
static inline int __isnormal_d(double x) {
    union { unsigned long long u; double d; } v; v.d = x;
    unsigned long long e = v.u & 0x7FF0000000000000ULL;
    return (e != 0ULL) && (e != 0x7FF0000000000000ULL);
}

#define isnan(x) __isnan_d((double)(x))
#define isinf(x) __isinf_d((double)(x))
#define isfinite(x) __isfinite_d((double)(x))
#define isnormal(x) __isnormal_d((double)(x))
#define signbit(x) ({ union{double _d;unsigned long long _u;}_v;_v._d=(double)(x);(_v._u>>63)&1; })

int fpclassify(double x);

double fabs(double x);
double copysign(double x, double y);

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);

double fmod(double x, double y);
double modf(double x, double *iptr);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double scalbn(double x, int n);

double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);

double pow(double x, double y);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

double fmax(double x, double y);
double fmin(double x, double y);
double fdim(double x, double y);
double fma(double x, double y, double z);
double nan(const char *tagp);

double remainder(double x, double y);

void fpu_init(void);

#endif