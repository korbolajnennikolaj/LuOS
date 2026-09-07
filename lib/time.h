#ifndef TIME_H
#define TIME_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t time_t;
typedef int64_t clock_t;
typedef int64_t suseconds_t;

#define CLOCKS_PER_SEC 1000000LL

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

#define TIME_UTC 1

#define SECS_PER_MIN 60
#define SECS_PER_HOUR 3600
#define SECS_PER_DAY 86400

void time_init_from_drivers(void);

void time_set_boot_epoch(time_t unix_seconds);

void time_set_tsc_freq_hz(uint64_t hz);

void time_tick_ms(uint64_t ms);

struct system_time;
time_t rtc_system_time_to_epoch(const struct system_time *st);

time_t time(time_t *t);

clock_t clock(void);

double difftime(time_t t2, time_t t1);

time_t mktime(struct tm *tm);

struct tm *gmtime_r (const time_t *timep, struct tm *result);
struct tm *localtime_r(const time_t *timep, struct tm *result);
struct tm *gmtime (const time_t *timep);
struct tm *localtime(const time_t *timep);

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm);

int timespec_get(struct timespec *ts, int base);

static inline uint64_t time_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

time_t rtc_read_epoch_direct(void);

#endif