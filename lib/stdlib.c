#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#include "components/Memory/heap.h"

void *malloc(size_t size)
{
    if (size == 0) return (void*)0;
    void *p = kmalloc(size);
    if (!p) errno = ENOMEM;
    return p;
}

void *calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0) return (void*)0;

    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) {
        errno = ENOMEM;
        return (void*)0;
    }

    void *p = kmalloc(total);
    if (!p) { errno = ENOMEM; return (void*)0; }

    unsigned char *b = (unsigned char*)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;

    return p;
}

void *realloc(void *ptr, size_t size)
{
    void *p = krealloc(ptr, size);
    if (!p && size != 0) errno = ENOMEM;
    return p;
}

void free(void *ptr)
{
    kfree(ptr);
}

void *aligned_alloc(size_t alignment, size_t size)
{

    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        return (void*)0;

    if (alignment <= 8)
        return kmalloc(size);

    size_t overhead = alignment - 1 + sizeof(void*);
    void *raw = kmalloc(size + overhead);
    if (!raw) { errno = ENOMEM; return (void*)0; }

    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    addr = (addr + alignment - 1) & ~(uintptr_t)(alignment - 1);
    ((void**)addr)[-1] = raw;
    return (void*)addr;
}

static uint64_t _rand_state = 0x123456789ABCDEFULL;

int rand(void)
{
    _rand_state ^= _rand_state << 13;
    _rand_state ^= _rand_state >> 7;
    _rand_state ^= _rand_state << 17;
    return (int)(_rand_state & (uint64_t)RAND_MAX);
}

void srand(unsigned int seed)
{
    _rand_state = (uint64_t)seed | ((uint64_t)seed << 32);
    if (_rand_state == 0) _rand_state = 1ULL;
}

__attribute__((noreturn)) void abort(void)
{
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

__attribute__((noreturn)) void exit(int status)
{
    (void)status;
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

__attribute__((noreturn)) void _Exit(int status)
{
    (void)status;
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

#define ATEXIT_MAX 32
static void (*_atexit_tab[ATEXIT_MAX])(void);
static int _atexit_cnt = 0;

int atexit(void (*func)(void))
{
    if (!func || _atexit_cnt >= ATEXIT_MAX) return 1;
    _atexit_tab[_atexit_cnt++] = func;
    return 0;
}

static unsigned long long
_parse_ull(const char *s, char **endptr, int base, int *neg_out)
{
    *neg_out = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    if (*s == '-') { *neg_out = 1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            { base = 16; s += 2; }
        else if (s[0] == '0')
            { base = 8; s++; }
        else
            base = 10;
    } else if (base == 16 &&
               s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    const char *start = s;
    unsigned long long result = 0;
    int overflow = 0;

    while (1) {
        int d;
        unsigned char c = (unsigned char)*s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;

        if (d >= base) break;

        if (!overflow) {
            if (result > (0xFFFFFFFFFFFFFFFFULL - (unsigned)d) / (unsigned)base) {
                overflow = 1;
            } else {
                result = result * (unsigned)base + (unsigned)d;
            }
        }
        s++;
    }

    if (s == start) {

        if (endptr) *endptr = (char*)(s - (*neg_out ? 1 : 0));
        return 0ULL;
    }

    if (overflow) {
        errno = ERANGE;
        result = 0xFFFFFFFFFFFFFFFFULL;
    }

    if (endptr) *endptr = (char*)s;
    return result;
}

long strtol(const char *nptr, char **endptr, int base)
{
    int neg = 0;
    unsigned long long v = _parse_ull(nptr, endptr, base, &neg);
    if (neg) {
        if (v > (unsigned long long)LONG_MAX + 1ULL)
            { errno = ERANGE; return LONG_MIN; }
        return -(long)v;
    }
    if (v > (unsigned long long)LONG_MAX)
        { errno = ERANGE; return LONG_MAX; }
    return (long)v;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    int neg = 0;
    unsigned long long v = _parse_ull(nptr, endptr, base, &neg);
    if (v > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return neg ? -(unsigned long)v : (unsigned long)v;
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    int neg = 0;
    unsigned long long v = _parse_ull(nptr, endptr, base, &neg);
    if (neg) {
        if (v > (unsigned long long)LLONG_MAX + 1ULL)
            { errno = ERANGE; return LLONG_MIN; }
        return -(long long)v;
    }
    if (v > (unsigned long long)LLONG_MAX)
        { errno = ERANGE; return LLONG_MAX; }
    return (long long)v;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    int neg = 0;
    unsigned long long v = _parse_ull(nptr, endptr, base, &neg);
    return neg ? -(unsigned long long)v : v;
}

double strtod(const char *nptr, char **endptr)
{
    const char *s = nptr;
    const char *end = nptr;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    int negative = 0;
    if (*s == '-') { negative = 1; s++; }
    else if (*s == '+') { s++; }

    double result = 0.0;

    if ((s[0]=='i'||s[0]=='I') &&
        (s[1]=='n'||s[1]=='N') &&
        (s[2]=='f'||s[2]=='F'))
    {
        s += 3;
        if ((s[0]=='i'||s[0]=='I') &&
            (s[1]=='n'||s[1]=='N') &&
            (s[2]=='i'||s[2]=='I') &&
            (s[3]=='t'||s[3]=='T') &&
            (s[4]=='y'||s[4]=='Y'))
            s += 5;
        if (endptr) *endptr = (char*)s;
        return negative ? -INFINITY : INFINITY;
    }

    if ((s[0]=='n'||s[0]=='N') &&
        (s[1]=='a'||s[1]=='A') &&
        (s[2]=='n'||s[2]=='N'))
    {
        s += 3;
        if (*s == '(') {
            s++;
            while (*s && *s != ')') s++;
            if (*s == ')') s++;
        }
        if (endptr) *endptr = (char*)s;
        return NAN;
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        unsigned long long mant = 0;
        int exp2 = 0;
        int digits = 0;

        while (1) {
            int d;
            unsigned char c = (unsigned char)*s;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (mant < (1ULL << 60))
                mant = mant * 16 + d;
            else
                exp2 += 4;
            digits++;
            s++;
        }

        if (*s == '.') {
            s++;
            int fbits = 0;
            while (1) {
                int d;
                unsigned char c = (unsigned char)*s;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                if (mant < (1ULL << 60)) {
                    mant = mant * 16 + d;
                    fbits += 4;
                }
                digits++;
                s++;
            }
            exp2 -= fbits;
        }

        if (digits == 0) {
            if (endptr) *endptr = (char*)end;
            return 0.0;
        }

        if (*s == 'p' || *s == 'P') {
            s++;
            int esign = 1;
            if (*s == '-') { esign = -1; s++; }
            else if (*s == '+') { s++; }
            int eexp = 0;
            while (*s >= '0' && *s <= '9')
                eexp = eexp * 10 + (*s++ - '0');
            exp2 += esign * eexp;
        }

        result = (double)mant * pow(2.0, (double)exp2);
        if (endptr) *endptr = (char*)s;
        return negative ? -result : result;
    }

    int any = 0;

    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (double)(*s - '0');
        s++;
        any = 1;
    }

    if (*s == '.') {
        s++;
        double place = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (double)(*s - '0') * place;
            place *= 0.1;
            s++;
            any = 1;
        }
    }

    if (!any) {
        if (endptr) *endptr = (char*)end;
        return 0.0;
    }

    if (*s == 'e' || *s == 'E') {
        const char *e_start = s;
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') { s++; }
        int eexp = 0, edigits = 0;
        while (*s >= '0' && *s <= '9') {
            eexp = eexp * 10 + (*s - '0');
            s++;
            edigits++;
        }
        if (edigits == 0) {
            s = e_start;
        } else {
            result *= pow(10.0, (double)(esign * eexp));
        }
    }

    if (endptr) *endptr = (char*)s;

    if (result > HUGE_VAL) {
        errno = ERANGE;
        return negative ? -HUGE_VAL : HUGE_VAL;
    }

    return negative ? -result : result;
}

float strtof(const char *nptr, char **endptr)
{
    return (float)strtod(nptr, endptr);
}

static void _swap(unsigned char *a, unsigned char *b, size_t sz)
{
    unsigned char tmp;
    while (sz--) {
        tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

#define ISORT_THRESH 16

static void _insertion_sort(unsigned char *base, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{

    unsigned char tmp[512];
    if (sz > sizeof(tmp)) {

        return;
    }

    for (size_t i = 1; i < n; i++) {
        unsigned char *cur = base + i * sz;
        for (size_t k = 0; k < sz; k++) tmp[k] = cur[k];

        size_t j = i;
        while (j > 0 && cmp(base + (j-1)*sz, tmp) > 0) {
            unsigned char *dst = base + j*sz;
            unsigned char *src = base + (j-1)*sz;
            for (size_t k = 0; k < sz; k++) dst[k] = src[k];
            j--;
        }
        unsigned char *dst = base + j*sz;
        for (size_t k = 0; k < sz; k++) dst[k] = tmp[k];
    }
}

static void _sift(unsigned char *base, size_t root, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    while (1) {
        size_t largest = root;
        size_t l = 2*root + 1;
        size_t r = 2*root + 2;
        if (l < n && cmp(base+l*sz, base+largest*sz) > 0) largest = l;
        if (r < n && cmp(base+r*sz, base+largest*sz) > 0) largest = r;
        if (largest == root) break;
        _swap(base+root*sz, base+largest*sz, sz);
        root = largest;
    }
}

static void _heapsort(unsigned char *base, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    if (n < 2) return;
    for (size_t i = n/2; i-- > 0; )
        _sift(base, i, n, sz, cmp);
    for (size_t i = n-1; i > 0; i--) {
        _swap(base, base+i*sz, sz);
        _sift(base, 0, i, sz, cmp);
    }
}

static int _ilog2(size_t n)
{
    int k = 0;
    while (n > 1) { n >>= 1; k++; }
    return k;
}

static void _introsort(unsigned char *base, size_t n, size_t sz, int (*cmp)(const void*, const void*), int depth)
{
    while (n > ISORT_THRESH) {
        if (depth == 0) {
            _heapsort(base, n, sz, cmp);
            return;
        }
        depth--;

        size_t mid = n / 2;
        unsigned char *a = base;
        unsigned char *b = base + mid*sz;
        unsigned char *c = base + (n-1)*sz;

        if (cmp(a, b) > 0) _swap(a, b, sz);
        if (cmp(a, c) > 0) _swap(a, c, sz);
        if (cmp(b, c) > 0) _swap(b, c, sz);

        _swap(b, c - sz, sz);
        unsigned char *pivot = c - sz;

        unsigned char *lo = base;
        unsigned char *hi = pivot - sz;
        while (lo <= hi) {
            while (lo <= hi && cmp(lo, pivot) < 0) lo += sz;
            while (lo <= hi && cmp(hi, pivot) > 0) hi -= sz;
            if (lo <= hi) {
                _swap(lo, hi, sz);
                lo += sz;
                hi -= sz;
            }
        }

        _swap(lo, pivot, sz);
        pivot = lo;

        size_t left_n = (size_t)(pivot - base) / sz;
        size_t right_n = n - left_n - 1;

        if (left_n < right_n) {
            _introsort(base, left_n, sz, cmp, depth);
            base = pivot + sz;
            n = right_n;
        } else {
            _introsort(pivot + sz, right_n, sz, cmp, depth);
            n = left_n;
        }
    }
    _insertion_sort(base, n, sz, cmp);
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void*, const void*))
{
    if (!base || nmemb < 2 || size == 0 || !compar) return;
    _introsort((unsigned char*)base, nmemb, size, compar,
               2 * _ilog2(nmemb));
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void*, const void*))
{
    const unsigned char *lo = (const unsigned char*)base;
    size_t cnt = nmemb;
    while (cnt > 0) {
        size_t half = cnt / 2;
        const unsigned char *mid = lo + half * size;
        int r = compar(key, mid);
        if (r < 0) { cnt = half; }
        else if (r > 0) { lo = mid + size; cnt -= half + 1; }
        else { return (void*)mid; }
    }
    return (void*)0;
}