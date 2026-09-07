#include "libc_test.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LIMINE_COLOR_WHITE
#define LIMINE_COLOR_WHITE 0xFFFFFFFF
#define LIMINE_COLOR_LIGHT_GREEN 0xFF55FF55
#define LIMINE_COLOR_LIGHT_RED 0xFFFF5555
#define LIMINE_COLOR_LIGHT_BLUE 0xFF5555FF
#define LIMINE_COLOR_LIGHT_GRAY 0xFFAAAAAA
#define LIMINE_COLOR_CYAN 0xFF55FFFF
#define LIMINE_COLOR_YELLOW 0xFFFFFF55
#endif

static struct limine_video_driver *g_video;
static int g_pass;
static int g_fail;

static void _print_int(int64_t val) {
    char buf[22];
    int i = 21;
    buf[i] = '\0';
    int neg = val < 0;
    uint64_t u = neg ? (uint64_t)(-(val + 1)) + 1u : (uint64_t)val;
    if (u == 0) { buf[--i] = '0'; }
    while (u) { buf[--i] = '0' + (u % 10); u /= 10; }
    if (neg) buf[--i] = '-';
    g_video->printf(&buf[i], LIMINE_COLOR_CYAN);
}

static void _check(const char *name, int pass) {
    g_video->printf("  ", LIMINE_COLOR_LIGHT_GRAY);
    g_video->printf(name, LIMINE_COLOR_WHITE);
    g_video->printf(": ", LIMINE_COLOR_LIGHT_GRAY);
    if (pass) {
        g_video->printf("PASS\n", LIMINE_COLOR_LIGHT_GREEN);
        g_pass++;
    } else {
        g_video->printf("FAIL\n", LIMINE_COLOR_LIGHT_RED);
        g_fail++;
    }
}

#define CHECK(name, expr) _check((name), !!(expr))

static void _section(const char *title) {
    g_video->printf("\n--- ", LIMINE_COLOR_LIGHT_GRAY);
    g_video->printf(title, LIMINE_COLOR_YELLOW);
    g_video->printf(" ---\n", LIMINE_COLOR_LIGHT_GRAY);
}

static void test_ctype(void) {
    _section("ctype.h");

    CHECK("isdigit('0')", isdigit('0'));
    CHECK("isdigit('9')", isdigit('9'));
    CHECK("!isdigit('a')", !isdigit('a'));

    CHECK("isalpha('A')", isalpha('A'));
    CHECK("isalpha('z')", isalpha('z'));
    CHECK("!isalpha('1')", !isalpha('1'));

    CHECK("isalnum('0')", isalnum('0'));
    CHECK("isalnum('Z')", isalnum('Z'));
    CHECK("!isalnum('!')", !isalnum('!'));

    CHECK("isspace(' ')", isspace(' '));
    CHECK("isspace('\\t')", isspace('\t'));
    CHECK("isspace('\\n')", isspace('\n'));
    CHECK("!isspace('a')", !isspace('a'));

    CHECK("isxdigit('f')", isxdigit('f'));
    CHECK("isxdigit('F')", isxdigit('F'));
    CHECK("isxdigit('9')", isxdigit('9'));
    CHECK("!isxdigit('g')", !isxdigit('g'));

    CHECK("isupper('A')", isupper('A'));
    CHECK("islower('a')", islower('a'));
    CHECK("!isupper('a')", !isupper('a'));

    CHECK("isprint(' ')", isprint(' '));
    CHECK("!isgraph(' ')", !isgraph(' '));
    CHECK("iscntrl('\\0')", iscntrl('\0'));
    CHECK("iscntrl(127)", iscntrl(127));

    CHECK("ispunct('!')", ispunct('!'));
    CHECK("!ispunct('a')", !ispunct('a'));

    CHECK("isblank(' ')", isblank(' '));
    CHECK("isblank('\\t')", isblank('\t'));

    CHECK("tolower('A')='a'", tolower('A') == 'a');
    CHECK("toupper('z')='Z'", toupper('z') == 'Z');
    CHECK("tolower('a')='a'", tolower('a') == 'a');
    CHECK("toupper('1')='1'", toupper('1') == '1');
}

static void test_string(void) {
    _section("string.h");

    char buf[64];

    memset(buf, 0xAB, 8);
    CHECK("memset", (unsigned char)buf[0] == 0xAB && (unsigned char)buf[7] == 0xAB);

    memcpy(buf, "Hello", 6);
    CHECK("memcpy", buf[0] == 'H' && buf[4] == 'o' && buf[5] == '\0');

    CHECK("memcmp eq", memcmp("abc", "abc", 3) == 0);
    CHECK("memcmp lt", memcmp("abc", "abd", 3) < 0);
    CHECK("memcmp gt", memcmp("abd", "abc", 3) > 0);

    CHECK("memchr", memchr("hello", 'l', 5) == &"hello"[2]);
    CHECK("memchr nil",memchr("hello", 'z', 5) == 0);

    char mv[8] = "ABCDE";
    memmove(mv + 1, mv, 4);
    CHECK("memmove overlap", mv[1]=='A' && mv[4]=='D');

    CHECK("strlen", strlen("hello") == 5);
    CHECK("strlen 0", strlen("") == 0);
    CHECK("strnlen", strnlen("hello", 3) == 3);
    CHECK("strnlen full", strnlen("hi", 10) == 2);

    strcpy(buf, "world");
    CHECK("strcpy", strcmp(buf, "world") == 0);

    strncpy(buf, "abcde", 3); buf[3] = '\0';
    CHECK("strncpy", strcmp(buf, "abc") == 0);

    strcpy(buf, "foo");
    strcat(buf, "bar");
    CHECK("strcat", strcmp(buf, "foobar") == 0);

    strcpy(buf, "foo");
    strncat(buf, "barbaz", 3);
    CHECK("strncat", strcmp(buf, "foobar") == 0);

    CHECK("strcmp eq", strcmp("abc", "abc") == 0);
    CHECK("strcmp lt", strcmp("abc", "abd") < 0);
    CHECK("strncmp", strncmp("abcX", "abcY", 3) == 0);

    const char *s = "abcabc";
    CHECK("strchr", strchr(s, 'b') == s + 1);
    CHECK("strrchr", strrchr(s, 'b') == s + 4);
    CHECK("strchr nil",strchr(s, 'z') == 0);

    CHECK("strstr", strstr("hello world", "world") == &"hello world"[6]);
    CHECK("strstr nil",strstr("hello", "xyz") == 0);

    CHECK("strspn", strspn("abcdef", "abc") == 3);
    CHECK("strcspn", strcspn("abcdef", "de") == 3);
    CHECK("strpbrk", strpbrk("abcdef", "de") == &"abcdef"[3]);
}

static int _int_cmp(const void *a, const void *b) {
    return *(const int*)a - *(const int*)b;
}

static void test_stdlib(void) {
    _section("stdlib.h");

    char *end;
    CHECK("strtol dec", strtol("42", &end, 10) == 42);
    CHECK("strtol neg", strtol("-17", &end, 10) == -17);
    CHECK("strtol hex", strtol("0x1F", &end, 16) == 31);
    CHECK("strtol oct", strtol("010", &end, 8) == 8);
    CHECK("strtol auto", strtol("0xFF", &end, 0) == 255);
    CHECK("strtol end", (end == &"0xFF"[4]));

    CHECK("strtoul", strtoul("4294967295", &end, 10) == 0xFFFFFFFFUL);

    CHECK("strtoll", strtoll("-9223372036854775807", &end, 10) == LLONG_MIN + 1);

    double d = strtod("3.14", &end);
    {
        double diff = d - 3.14; if (diff < 0) diff = -diff;
        CHECK("strtod 3.14", diff < 1e-9);
    }
    d = strtod("-0.5", &end);
    CHECK("strtod neg", d == -0.5);
    d = strtod("1e3", &end);
    CHECK("strtod exp", d == 1000.0);

    CHECK("atoi", atoi("123") == 123);
    CHECK("atof", atof("2.5") == 2.5);

    CHECK("abs", abs(-7) == 7);
    CHECK("labs", labs(-1000000L) == 1000000L);
    CHECK("llabs", llabs(-1LL) == 1LL);

    div_t dv = div(17, 5);
    ldiv_t ldv = ldiv(17L, 5L);
    CHECK("div quot", dv.quot == 3);
    CHECK("div rem", dv.rem == 2);
    CHECK("ldiv", ldv.quot == 3 && ldv.rem == 2);

    int arr[6] = {5, 3, 1, 4, 2, 6};
    qsort(arr, 6, sizeof(int), _int_cmp);
    int sorted = 1;
    for (int i = 0; i < 5; i++) if (arr[i] > arr[i+1]) { sorted = 0; break; }
    CHECK("qsort", sorted && arr[0] == 1 && arr[5] == 6);

    int key = 4;
    int *found = (int*)bsearch(&key, arr, 6, sizeof(int), _int_cmp);
    CHECK("bsearch found", found && *found == 4);
    key = 99;
    CHECK("bsearch nil", bsearch(&key, arr, 6, sizeof(int), _int_cmp) == 0);

    srand(42);
    int r1 = rand();
    srand(42);
    int r2 = rand();
    CHECK("rand deterministic", r1 == r2);
    CHECK("rand range", (unsigned)r1 <= (unsigned)RAND_MAX);
}

static void test_stdio(void) {
    _section("stdio.h (snprintf)");

    char buf[128];

    snprintf(buf, sizeof(buf), "%d", 42);
    CHECK("%%d  42", strcmp(buf, "42") == 0);

    snprintf(buf, sizeof(buf), "%d", -1);
    CHECK("%%d -1", strcmp(buf, "-1") == 0);

    snprintf(buf, sizeof(buf), "%u", 4294967295U);
    CHECK("%%u UINT_MAX", strcmp(buf, "4294967295") == 0);

    snprintf(buf, sizeof(buf), "%ld", -9223372036854775807L - 1L);
    CHECK("%%ld LONG_MIN", strcmp(buf, "-9223372036854775808") == 0);

    snprintf(buf, sizeof(buf), "%x", 0xDEAD);
    CHECK("%%x 0xdead", strcmp(buf, "dead") == 0);

    snprintf(buf, sizeof(buf), "%X", 0xBEEF);
    CHECK("%%X 0xBEEF", strcmp(buf, "BEEF") == 0);

    snprintf(buf, sizeof(buf), "%lx", 0xDEADBEEFUL);
    CHECK("%%lx", strcmp(buf, "deadbeef") == 0);

    snprintf(buf, sizeof(buf), "%s", "hello");
    CHECK("%%s", strcmp(buf, "hello") == 0);

    snprintf(buf, sizeof(buf), "%c", 'Z');
    CHECK("%%c", strcmp(buf, "Z") == 0);

    snprintf(buf, sizeof(buf), "%%");
    CHECK("%%%%", strcmp(buf, "%") == 0);

    int n = snprintf(buf, 4, "hello");
    CHECK("snprintf trunc content", strcmp(buf, "hel") == 0);
    CHECK("snprintf trunc retval", n == 5);

    sprintf(buf, "%d+%d=%d", 1, 2, 3);
    CHECK("sprintf calc", strcmp(buf, "1+2=3") == 0);
}

#define CLOSE(a,b) ({ double _d = (a)-(b); if (_d<0) _d=-_d; _d < 1e-9; })
#define CLOSE4(a,b) ({ double _d = (a)-(b); if (_d<0) _d=-_d; _d < 1e-4; })

static void test_math(void) {
    _section("math.h — special values");

    CHECK("isnan(NAN)", isnan(NAN));
    CHECK("!isnan(1.0)", !isnan(1.0));
    CHECK("isinf(INF)", isinf(INFINITY));
    CHECK("isinf(-INF)", isinf(-INFINITY));
    CHECK("!isinf(1.0)", !isinf(1.0));
    CHECK("isfinite(1.0)", isfinite(1.0));
    CHECK("!isfinite(INF)", !isfinite(INFINITY));
    CHECK("signbit(-1.0)", signbit(-1.0));
    CHECK("!signbit(1.0)", !signbit(1.0));
    CHECK("signbit(-0.0)", signbit(-0.0));
    CHECK("fpclass NAN", fpclassify(NAN) == FP_NAN);
    CHECK("fpclass INF", fpclassify(INFINITY) == FP_INFINITE);
    CHECK("fpclass ZERO", fpclassify(0.0) == FP_ZERO);
    CHECK("fpclass NORMAL", fpclassify(1.0) == FP_NORMAL);

    _section("math.h — rounding");
    CHECK("floor(3.7)", floor(3.7) == 3.0);
    CHECK("floor(-3.7)", floor(-3.7) == -4.0);
    CHECK("ceil(3.2)", ceil(3.2) == 4.0);
    CHECK("ceil(-3.2)", ceil(-3.2) == -3.0);
    CHECK("trunc(3.9)", trunc(3.9) == 3.0);
    CHECK("trunc(-3.9)", trunc(-3.9) == -3.0);
    CHECK("round(0.5)", round(0.5) == 1.0);
    CHECK("round(-0.5)", round(-0.5) == -1.0);

    _section("math.h — fabs / copysign / fma");
    CHECK("fabs(-5.0)", fabs(-5.0) == 5.0);
    CHECK("copysign", copysign(3.0, -1.0) == -3.0);
    CHECK("fmax", fmax(2.0, 5.0) == 5.0);
    CHECK("fmin", fmin(2.0, 5.0) == 2.0);
    CHECK("fdim(5,3)", fdim(5.0, 3.0) == 2.0);
    CHECK("fdim(3,5)", fdim(3.0, 5.0) == 0.0);
    CHECK("fma(2,3,4)", fma(2.0, 3.0, 4.0) == 10.0);

    _section("math.h — sqrt / cbrt / hypot / pow");
    CHECK("sqrt(4)", CLOSE(sqrt(4.0), 2.0));
    CHECK("sqrt(2)", CLOSE(sqrt(2.0), M_SQRT2));
    CHECK("cbrt(27)", CLOSE(cbrt(27.0), 3.0));
    CHECK("cbrt(-8)", CLOSE(cbrt(-8.0), -2.0));
    CHECK("hypot(3,4)", CLOSE(hypot(3.0, 4.0), 5.0));
    CHECK("pow(2,10)", CLOSE(pow(2.0, 10.0), 1024.0));
    CHECK("pow(2,-1)", CLOSE(pow(2.0, -1.0), 0.5));
    CHECK("pow(0,0)=1", pow(0.0, 0.0) == 1.0);

    _section("math.h — exp / log");
    CHECK("exp(0)=1", CLOSE(exp(0.0), 1.0));
    CHECK("exp(1)=e", CLOSE(exp(1.0), M_E));
    CHECK("exp2(8)=256", CLOSE(exp2(8.0), 256.0));
    CHECK("expm1(0)", CLOSE(expm1(0.0), 0.0));
    CHECK("log(e)=1", CLOSE(log(M_E), 1.0));
    CHECK("log(1)=0", CLOSE(log(1.0), 0.0));
    CHECK("log2(8)=3", CLOSE(log2(8.0), 3.0));
    CHECK("log10(100)=2", CLOSE(log10(100.0), 2.0));
    CHECK("log1p(0)=0", CLOSE(log1p(0.0), 0.0));
    CHECK("log(-1)=NaN", isnan(log(-1.0)));

    _section("math.h — trig");
    CHECK("sin(0)=0", CLOSE(sin(0.0), 0.0));
    CHECK("sin(pi/2)=1", CLOSE(sin(M_PI_2), 1.0));
    CHECK("cos(0)=1", CLOSE(cos(0.0), 1.0));
    CHECK("cos(pi)=-1", CLOSE(cos(M_PI), -1.0));
    CHECK("tan(pi/4)=1", CLOSE4(tan(M_PI_4), 1.0));
    CHECK("asin(1)=pi/2", CLOSE(asin(1.0), M_PI_2));
    CHECK("acos(1)=0", CLOSE(acos(1.0), 0.0));
    CHECK("atan(1)=pi/4", CLOSE(atan(1.0), M_PI_4));
    CHECK("atan2(1,1)", CLOSE(atan2(1.0, 1.0), M_PI_4));
    CHECK("atan2(-1,-1)", CLOSE4(atan2(-1.0,-1.0), -3.0*M_PI_4));

    _section("math.h — hyperbolic");
    CHECK("sinh(0)=0", CLOSE(sinh(0.0), 0.0));
    CHECK("cosh(0)=1", CLOSE(cosh(0.0), 1.0));
    CHECK("tanh(0)=0", CLOSE(tanh(0.0), 0.0));
    CHECK("asinh(0)=0", CLOSE(asinh(0.0), 0.0));
    CHECK("acosh(1)=0", CLOSE(acosh(1.0), 0.0));
    CHECK("atanh(0)=0", CLOSE(atanh(0.0), 0.0));

    _section("math.h — fmod / modf / frexp / ldexp / scalbn");
    CHECK("fmod(5.5,2)=1.5", CLOSE(fmod(5.5, 2.0), 1.5));
    CHECK("fmod(-5.5,2)=-1.5", CLOSE(fmod(-5.5, 2.0), -1.5));
    CHECK("remainder(5.5,2)", CLOSE(remainder(5.5, 2.0), -0.5));

    double ip;
    double fp2 = modf(3.75, &ip);
    CHECK("modf intpart", ip == 3.0);
    CHECK("modf fracpart", CLOSE(fp2, 0.75));

    int exp2i;
    double mant = frexp(8.0, &exp2i);
    CHECK("frexp mant", CLOSE(mant, 0.5));
    CHECK("frexp exp", exp2i == 4);
    CHECK("ldexp", CLOSE(ldexp(0.5, 4), 8.0));
    CHECK("scalbn", CLOSE(scalbn(1.0, 10), 1024.0));

    _section("math.h — inf/nan arithmetic");
    CHECK("INF+1=INF", isinf(INFINITY + 1.0));
    CHECK("INF-INF=NaN", isnan(INFINITY - INFINITY));
    CHECK("0/0=NaN", isnan(0.0 / 0.0));
    CHECK("1/0=INF", isinf(1.0 / 0.0));
    CHECK("sqrt(-1)=NaN", isnan(sqrt(-1.0)));
}

static jmp_buf g_jmpbuf;
static int g_jmp_landed;

static void _do_longjmp(int v) {
    longjmp(g_jmpbuf, v);
}

static void test_setjmp(void) {
    _section("setjmp.h");

    g_jmp_landed = 0;

    volatile int v = setjmp(g_jmpbuf);
    if (v == 0) {
        _do_longjmp(42);
    } else {
        g_jmp_landed = v;
    }
    CHECK("setjmp returns 42 via longjmp", g_jmp_landed == 42);

    g_jmp_landed = 0;
    v = setjmp(g_jmpbuf);
    if (v == 0) {
        longjmp(g_jmpbuf, 0);
    } else {
        g_jmp_landed = v;
    }
    CHECK("longjmp(0) returns 1", g_jmp_landed == 1);

    sigjmp_buf sjb;
    g_jmp_landed = 0;

    volatile int sv = sigsetjmp(sjb, 0);
    if (sv == 0) {
        siglongjmp(sjb, 7);
    } else {
        g_jmp_landed = sv;
    }
    CHECK("sigsetjmp/siglongjmp", g_jmp_landed == 7);
}

static void test_errno(void) {
    _section("errno.h");

    CHECK("EPERM  == 1", EPERM == 1);
    CHECK("ENOENT == 2", ENOENT == 2);
    CHECK("ENOMEM == 12", ENOMEM == 12);
    CHECK("ERANGE == 34", ERANGE == 34);
    CHECK("EINVAL == 22", EINVAL == 22);
    CHECK("EDOM   == 33", EDOM == 33);
    CHECK("EILSEQ == 84", EILSEQ == 84);
    CHECK("EWOULDBLOCK==EAGAIN", EWOULDBLOCK == EAGAIN);

    const char *s1 = strerror(EPERM);
    CHECK("strerror(EPERM) != NULL", s1 != 0);
    CHECK("strerror(EPERM) non-empty", s1 && s1[0] != '\0');

    const char *s2 = strerror(ENOENT);
    CHECK("strerror(ENOENT) != NULL", s2 != 0);

    const char *su = strerror(9999);
    CHECK("strerror unknown != NULL", su != 0);

    char errbuf[64];
    int rc = strerror_r(ENOMEM, errbuf, sizeof(errbuf));
    CHECK("strerror_r rc==0", rc == 0);
    CHECK("strerror_r non-empty",errbuf[0] != '\0');

    errno = EINVAL;
    CHECK("errno read/write", errno == EINVAL);
    errno = 0;
    CHECK("errno clear", errno == 0);
}

static void test_locale(void) {
    _section("locale.h");

    char *l = setlocale(LC_ALL, "C");
    CHECK("setlocale C != NULL", l != 0);

    l = setlocale(LC_ALL, "");
    CHECK("setlocale '' != NULL", l != 0);

    l = setlocale(LC_ALL, 0);
    CHECK("setlocale query != NULL", l != 0);

    l = setlocale(LC_ALL, "ru_RU.UTF-8");
    CHECK("setlocale unknown == NULL", l == 0);

    struct lconv *lc = localeconv();
    CHECK("localeconv != NULL", lc != 0);
    CHECK("decimal_point == '.'", lc && lc->decimal_point && lc->decimal_point[0] == '.');
    CHECK("thousands_sep == ''", lc && lc->thousands_sep && lc->thousands_sep[0] == '\0');

    CHECK("LC_ALL    == 0", LC_ALL == 0);
    CHECK("LC_NUMERIC== 4", LC_NUMERIC == 4);
}

static void test_time(void) {
    _section("time.h — mktime / gmtime_r");

    struct tm t = {0};

    t.tm_year = 100;
    t.tm_mon = 0;
    t.tm_mday = 1;
    t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
    time_t ep = mktime(&t);
    CHECK("mktime 2000-01-01", ep == 946684800LL);

    struct tm tr;
    gmtime_r(&ep, &tr);
    CHECK("gmtime_r year", tr.tm_year == 100);
    CHECK("gmtime_r mon", tr.tm_mon == 0);
    CHECK("gmtime_r mday", tr.tm_mday == 1);
    CHECK("gmtime_r hour", tr.tm_hour == 0);
    CHECK("gmtime_r wday", tr.tm_wday == 6);

    time_t zero = 0;
    gmtime_r(&zero, &tr);
    CHECK("epoch 0 year", tr.tm_year == 70);
    CHECK("epoch 0 mon", tr.tm_mon == 0);
    CHECK("epoch 0 mday", tr.tm_mday == 1);
    CHECK("epoch 0 wday", tr.tm_wday == 4);

    CHECK("difftime", difftime(100, 40) == 60.0);

    _section("time.h — strftime");
    char sbuf[64];
    struct tm tf = {0};
    tf.tm_year = 124;
    tf.tm_mon = 5;
    tf.tm_mday = 15;
    tf.tm_wday = 6;
    tf.tm_hour = 9; tf.tm_min = 5; tf.tm_sec = 3;

    strftime(sbuf, sizeof(sbuf), "%Y", &tf);
    CHECK("strftime %%Y", strcmp(sbuf, "2024") == 0);

    strftime(sbuf, sizeof(sbuf), "%m", &tf);
    CHECK("strftime %%m", strcmp(sbuf, "06") == 0);

    strftime(sbuf, sizeof(sbuf), "%d", &tf);
    CHECK("strftime %%d", strcmp(sbuf, "15") == 0);

    strftime(sbuf, sizeof(sbuf), "%H:%M:%S", &tf);
    CHECK("strftime H:M:S", strcmp(sbuf, "09:05:03") == 0);

    CHECK("CLOCKS_PER_SEC == 1e6", CLOCKS_PER_SEC == 1000000LL);
}

static void test_limits(void) {
    _section("limits.h");

    CHECK("CHAR_BIT == 8", CHAR_BIT == 8);

    CHECK("SCHAR_MAX == 127", SCHAR_MAX == 127);
    CHECK("SCHAR_MIN == -128", SCHAR_MIN == -128);
    CHECK("UCHAR_MAX == 255", UCHAR_MAX == 255U);

    CHECK("SHRT_MAX == 32767", SHRT_MAX == 32767);
    CHECK("SHRT_MIN == -32768", SHRT_MIN == -32768);

    CHECK("INT_MAX == 2^31-1", INT_MAX == 2147483647);
    CHECK("INT_MIN == -2^31", INT_MIN == (-2147483647 - 1));
    CHECK("UINT_MAX == 2^32-1", UINT_MAX == 4294967295U);

    CHECK("LONG_MAX == 2^63-1", LONG_MAX == 9223372036854775807L);
    CHECK("LONG_MIN == -2^63", LONG_MIN == (-9223372036854775807L - 1L));
    CHECK("ULONG_MAX", ULONG_MAX == 18446744073709551615UL);

    CHECK("LLONG_MAX == 2^63-1", LLONG_MAX == 9223372036854775807LL);
    CHECK("LLONG_MIN == -2^63", LLONG_MIN == (-9223372036854775807LL - 1LL));
    CHECK("ULLONG_MAX", ULLONG_MAX == 18446744073709551615ULL);

    CHECK("PATH_MAX == 4096", PATH_MAX == 4096);
    CHECK("NAME_MAX == 255", NAME_MAX == 255);
    CHECK("MB_LEN_MAX == 4", MB_LEN_MAX == 4);

    CHECK("SIZE_MAX >= UINT_MAX", SIZE_MAX >= UINT_MAX);
}

static void print_summary(void) {
    char buf[64];
    int total = g_pass + g_fail;

    g_video->printf("\n========================================\n",
                    LIMINE_COLOR_LIGHT_BLUE);
    g_video->printf("  libc TEST SUMMARY: ", LIMINE_COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%d", g_pass);
    g_video->printf(buf, LIMINE_COLOR_LIGHT_GREEN);
    g_video->printf(" passed, ", LIMINE_COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%d", g_fail);
    g_video->printf(buf, g_fail ? LIMINE_COLOR_LIGHT_RED : LIMINE_COLOR_LIGHT_GREEN);
    g_video->printf(" failed, ", LIMINE_COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%d", total);
    g_video->printf(buf, LIMINE_COLOR_CYAN);
    g_video->printf(" total\n", LIMINE_COLOR_WHITE);

    if (g_fail == 0)
        g_video->printf("  ALL TESTS PASSED\n", LIMINE_COLOR_LIGHT_GREEN);
    else
        g_video->printf("  SOME TESTS FAILED\n", LIMINE_COLOR_LIGHT_RED);

    g_video->printf("========================================\n\n",
                    LIMINE_COLOR_LIGHT_BLUE);
}

void cmd_libc_test(struct limine_video_driver *video) {
    g_video = video;
    g_pass = 0;
    g_fail = 0;

    video->printf("\n=== FREESTANDING LIBC TEST SUITE ===\n",
                  LIMINE_COLOR_LIGHT_BLUE);

    test_ctype();
    test_string();
    test_stdlib();
    test_stdio();
    test_math();
    test_setjmp();
    test_errno();
    test_locale();
    test_time();
    test_limits();

    print_summary();
}
