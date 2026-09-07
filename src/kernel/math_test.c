#include "math_test.h"

#include "drivers/Video/limine_video_driver.h"

#include <math.h>
#include <stdint.h>

#ifndef LIMINE_COLOR_WHITE
#define LIMINE_COLOR_WHITE 0xFFFFFFFF
#define LIMINE_COLOR_LIGHT_GREEN 0xFF55FF55
#define LIMINE_COLOR_LIGHT_RED 0xFFFF5555
#define LIMINE_COLOR_LIGHT_BLUE 0xFF5555FF
#define LIMINE_COLOR_LIGHT_GRAY 0xFFAAAAAA
#define LIMINE_COLOR_CYAN 0xFF55FFFF
#define LIMINE_COLOR_YELLOW 0xFFFFFF55
#endif

static void print_u64_math(struct limine_video_driver* video, uint64_t val, uint32_t color) {
    if (val == 0) { video->printf("0", color); return; }
    char buf[21];
    int i = 0;
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    char out[21];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = '\0';
    video->printf(out, color);
}

static void print_double(struct limine_video_driver* video, double val, uint32_t color) {
    if (isnan(val)) { video->printf("NaN", LIMINE_COLOR_LIGHT_RED); return; }
    if (isinf(val)) {
        if (val < 0) video->printf("-inf", LIMINE_COLOR_LIGHT_RED);
        else video->printf("inf", LIMINE_COLOR_LIGHT_RED);
        return;
    }
    if (val < 0.0) { video->printf("-", color); val = -val; }

    uint64_t int_part = (uint64_t)val;
    double frac_part = val - (double)int_part;

    print_u64_math(video, int_part, color);
    video->printf(".", color);

    uint64_t frac_int = (uint64_t)(frac_part * 10000.0);
    if (frac_int < 1000) video->printf("0", color);
    if (frac_int < 100) video->printf("0", color);
    if (frac_int < 10) video->printf("0", color);
    print_u64_math(video, frac_int, color);
}

static void test_math_op(struct limine_video_driver* video, const char* name, double result, double expected) {
    video->printf("  ", LIMINE_COLOR_LIGHT_GRAY);
    video->printf(name, LIMINE_COLOR_WHITE);
    video->printf(": ", LIMINE_COLOR_LIGHT_GRAY);
    print_double(video, result, LIMINE_COLOR_CYAN);

    double diff = result - expected;
    if (diff < 0.0) diff = -diff;

    if (diff < 0.0001)
        video->printf(" [PASS]\n", LIMINE_COLOR_LIGHT_GREEN);
    else {
        video->printf(" [FAIL] (expected ", LIMINE_COLOR_LIGHT_RED);
        print_double(video, expected, LIMINE_COLOR_YELLOW);
        video->printf(")\n", LIMINE_COLOR_LIGHT_RED);
    }
}

void cmd_math_test(struct limine_video_driver* video) {
    video->printf("\n=== Advanced Math Library Diagnostics ===\n", LIMINE_COLOR_LIGHT_BLUE);

    video->printf("--- Trigonometry ---\n", LIMINE_COLOR_WHITE);
    test_math_op(video, "sin(M_PI_2)   ", sin(M_PI_2), 1.0);
    test_math_op(video, "cos(M_PI)     ", cos(M_PI), -1.0);
    test_math_op(video, "tan(M_PI_4)   ", tan(M_PI / 4.0), 1.0);
    test_math_op(video, "atan2(1, 1)   ", atan2(1.0, 1.0), M_PI_4);
    test_math_op(video, "asin(1.0)     ", asin(1.0), M_PI_2);

    video->printf("\n--- Log & Exp ---\n", LIMINE_COLOR_WHITE);
    test_math_op(video, "exp(1.0)      ", exp(1.0), M_E);
    test_math_op(video, "log(M_E)      ", log(M_E), 1.0);
    test_math_op(video, "log10(100.0)  ", log10(100.0), 2.0);
    test_math_op(video, "pow(2.0, -2.0)", pow(2.0, -2.0), 0.25);
    test_math_op(video, "cbrt(-27.0)   ", cbrt(-27.0), -3.0);

    video->printf("\n--- Modulo & Precision ---\n", LIMINE_COLOR_WHITE);
    test_math_op(video, "fmod(5.5, 2.0)", fmod(5.5, 2.0), 1.5);
    test_math_op(video, "fmod(-5.5, 2.0)", fmod(-5.5, 2.0), -1.5);
    test_math_op(video, "rem(5.5, 2.0) ", remainder(5.5, 2.0), -0.5);
    test_math_op(video, "trunc(-3.14)  ", trunc(-3.14), -3.0);
    test_math_op(video, "round(-3.5)   ", round(-3.5), -4.0);

    video->printf("\n--- FP Exceptions & Edge Cases ---\n", LIMINE_COLOR_LIGHT_BLUE);

    video->printf("  -0.0 check    : ", LIMINE_COLOR_WHITE);
    double nz = -0.0;
    _db v_nz; v_nz.d = nz;
    if (v_nz.u & (1ULL << 63)) video->printf("PASS (-0)\n", LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("FAIL (no sign)\n", LIMINE_COLOR_LIGHT_RED);

    video->printf("  copysign check: ", LIMINE_COLOR_WHITE);
    if (copysign(1.0, -1.0) == -1.0) video->printf("PASS\n", LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("FAIL\n", LIMINE_COLOR_LIGHT_RED);

    video->printf("  fpclassify NAN: ", LIMINE_COLOR_WHITE);
    if (fpclassify(NAN) == FP_NAN) video->printf("PASS\n", LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("FAIL\n", LIMINE_COLOR_LIGHT_RED);

    video->printf("  inf - inf =   : ", LIMINE_COLOR_WHITE);
    double inf_sub = INFINITY - INFINITY;
    if (isnan(inf_sub)) video->printf("PASS (NAN)\n", LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("FAIL\n", LIMINE_COLOR_LIGHT_RED);

    video->printf("  log(-1.0)     : ", LIMINE_COLOR_WHITE);
    if (isnan(log(-1.0))) video->printf("PASS (NAN)\n", LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("FAIL\n", LIMINE_COLOR_LIGHT_RED);

    video->printf("========================================\n\n", LIMINE_COLOR_LIGHT_BLUE);
}
