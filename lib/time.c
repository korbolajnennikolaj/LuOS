#include <time.h>
#include <stdint.h>
#include <stddef.h>

#include "components/drivers.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/rtc_driver.h"
#include "drivers/Timer/tsc_driver.h"
#include <ports.h>

static struct rtc_driver *_rtc = NULL;
static struct tsc_driver *_tsc = NULL;
static int _drv_ok = 0;

static void _acquire_drivers(void)
{
    if (_drv_ok) return;
    _rtc = (struct rtc_driver *)get_self_driver(TIMER_DRIVER, RTC_TIMER);
    _tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);
    _drv_ok = 1;
}

static volatile time_t _boot_epoch = 0;
static volatile int _epoch_set = 0;

static volatile uint64_t _boot_uptime_ms = 0;

static volatile uint64_t _resync_tsc_ms = 0;

static volatile uint64_t _tick_ms = 0;

static struct tm _tm_static;

static int _is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int _month_days(int mon, int year)
{
    static const int days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mon == 1 && _is_leap(year)) return 29;
    return days[mon];
}

static void _epoch_to_tm(time_t t, struct tm *out)
{
    time_t days = t / (time_t)SECS_PER_DAY;
    int secs = (int)(t % (time_t)SECS_PER_DAY);
    if (secs < 0) { secs += SECS_PER_DAY; days--; }

    out->tm_sec = secs % 60;
    out->tm_min = (secs / 60) % 60;
    out->tm_hour = secs / 3600;

    int wday = (int)((days + 4) % 7);
    if (wday < 0) wday += 7;
    out->tm_wday = wday;

    int year = 1970;
    while (1) {
        int dy = _is_leap(year) ? 366 : 365;
        if (days < (time_t)dy) break;
        days -= dy; year++;
    }
    out->tm_year = year - 1900;
    out->tm_yday = (int)days;

    int mon = 0;
    while (mon < 11) {
        int dm = _month_days(mon, year);
        if (days < dm) break;
        days -= dm; mon++;
    }
    out->tm_mon = mon;
    out->tm_mday = (int)days + 1;
    out->tm_isdst = 0;
}

time_t rtc_system_time_to_epoch(const struct system_time *st)
{
    int year = (int)st->year;
    int mon = (int)st->month - 1;
    int day = (int)st->day - 1;
    time_t days = 0;

    for (int y = 1970; y < year; y++)
        days += _is_leap(y) ? 366 : 365;
    for (int m = 0; m < mon; m++)
        days += _month_days(m, year);
    days += day;

    return days * (time_t)SECS_PER_DAY
         + (time_t)st->hours * 3600
         + (time_t)st->minutes * 60
         + (time_t)st->seconds;
}

#define _CMOS_ADDR 0x70
#define _CMOS_DATA 0x71
#define _BCD2BIN(v) (((v) >> 4) * 10 + ((v) & 0xF))

static uint8_t _cmos_read(uint8_t reg)
{
    outb(_CMOS_ADDR, reg);
    return inb(_CMOS_DATA);
}

time_t rtc_read_epoch_direct(void)
{
    int tries = 0;
    while ((_cmos_read(0x0A) & 0x80) && tries++ < 100000)
        __asm__ volatile("pause");

    uint8_t sec = _cmos_read(0x00), min = _cmos_read(0x02);
    uint8_t hr = _cmos_read(0x04), day = _cmos_read(0x07);
    uint8_t mon = _cmos_read(0x08), yr = _cmos_read(0x09);
    uint8_t rb = _cmos_read(0x0B);

    if (!(rb & 0x04)) {
        sec=_BCD2BIN(sec); min=_BCD2BIN(min);
        hr =_BCD2BIN(hr); day=_BCD2BIN(day);
        mon=_BCD2BIN(mon); yr =_BCD2BIN(yr);
    }
    if (!(rb & 0x02) && (hr & 0x80))
        hr = ((hr & 0x7F) + 12) % 24;

    struct system_time st = {
        .seconds=sec, .minutes=min, .hours=hr,
        .day=day, .month=mon, .year=(uint16_t)(2000+yr)
    };
    return rtc_system_time_to_epoch(&st);
}

static time_t _rtc_read_epoch(void)
{
    if (_rtc && _rtc->get_rtc_time) {
        struct system_time *st = _rtc->get_rtc_time();
        if (st && st->year >= 2000 && st->year <= 2100)
            return rtc_system_time_to_epoch(st);
    }

    return rtc_read_epoch_direct();
}

static uint64_t _uptime_ms(void)
{
    if (_tsc && _tsc->get_tsc_uptime_ms)
        return _tsc->get_tsc_uptime_ms();
    return _tick_ms;
}

static void _anchor_epoch(time_t epoch)
{
    _boot_epoch = epoch;
    _boot_uptime_ms = _uptime_ms();
    _epoch_set = 1;

    if (_tsc && _tsc->get_tsc_ms)
        _resync_tsc_ms = _tsc->get_tsc_ms();
}

void time_set_boot_epoch(time_t unix_seconds)
{
    _acquire_drivers();
    _anchor_epoch(unix_seconds);
}

void time_set_tsc_freq_hz(uint64_t hz) { (void)hz; }

void time_tick_ms(uint64_t ms) { _tick_ms += ms; }

void time_init_from_drivers(void)
{
    _acquire_drivers();

    time_t t = _rtc_read_epoch();
    if (t > 0)
        _anchor_epoch(t);
}

#define RESYNC_INTERVAL_MS (3600ULL * 1000ULL)

static void _maybe_resync(void)
{
    if (!(_tsc && _tsc->get_tsc_ms)) return;

    uint64_t now_ms = _tsc->get_tsc_ms();
    if (now_ms - _resync_tsc_ms < RESYNC_INTERVAL_MS) return;

    time_t fresh = _rtc_read_epoch();
    if (fresh > 0) _anchor_epoch(fresh);
}

time_t time(time_t *t)
{
    _acquire_drivers();
    time_t result;

    if (_epoch_set) {
        uint64_t elapsed_ms = _uptime_ms() - _boot_uptime_ms;
        result = _boot_epoch + (time_t)(elapsed_ms / 1000ULL);
        _maybe_resync();
    } else {
        result = _rtc_read_epoch();
        if (result > 0)
            _anchor_epoch(result);
        else
            result = (time_t)-1;
    }

    if (t) *t = result;
    return result;
}

clock_t clock(void)
{
    _acquire_drivers();
    if (_tsc && _tsc->get_tsc_uptime_ms)
        return (clock_t)(_tsc->get_tsc_uptime_ms() * 1000ULL);
    return (clock_t)(_tick_ms * 1000ULL);
}

double difftime(time_t t2, time_t t1)
{
    return (double)(t2 - t1);
}

time_t mktime(struct tm *tm)
{
    if (!tm) return (time_t)-1;

    while (tm->tm_sec >= 60) { tm->tm_min++; tm->tm_sec -= 60; }
    while (tm->tm_sec < 0) { tm->tm_min--; tm->tm_sec += 60; }
    while (tm->tm_min >= 60) { tm->tm_hour++; tm->tm_min -= 60; }
    while (tm->tm_min < 0) { tm->tm_hour--; tm->tm_min += 60; }
    while (tm->tm_hour >= 24) { tm->tm_mday++; tm->tm_hour -= 24; }
    while (tm->tm_hour < 0) { tm->tm_mday--; tm->tm_hour += 24; }
    while (tm->tm_mon >= 12) { tm->tm_year++; tm->tm_mon -= 12; }
    while (tm->tm_mon < 0) { tm->tm_year--; tm->tm_mon += 12; }

    int year = tm->tm_year + 1900;

    while (tm->tm_mday <= 0) {
        if (--tm->tm_mon < 0) { tm->tm_mon = 11; tm->tm_year--; year--; }
        tm->tm_mday += _month_days(tm->tm_mon, year);
    }
    {
        int dm;
        while (tm->tm_mday > (dm = _month_days(tm->tm_mon, year))) {
            tm->tm_mday -= dm;
            if (++tm->tm_mon >= 12) { tm->tm_mon = 0; tm->tm_year++; year++; }
        }
    }

    time_t days = 0;
    for (int y = 1970; y < year; y++)
        days += _is_leap(y) ? 366 : 365;
    for (int m = 0; m < tm->tm_mon; m++)
        days += _month_days(m, year);
    days += tm->tm_mday - 1;

    int wday = (int)((days + 4) % 7);
    if (wday < 0) wday += 7;
    tm->tm_wday = wday;

    {
        time_t year_start = 0;
        for (int y = 1970; y < year; y++)
            year_start += _is_leap(y) ? 366 : 365;
        tm->tm_yday = (int)(days - year_start);
    }

    return days * (time_t)SECS_PER_DAY
         + (time_t)tm->tm_hour * 3600
         + (time_t)tm->tm_min * 60
         + (time_t)tm->tm_sec;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    if (!timep || !result) return (struct tm *)0;
    _epoch_to_tm(*timep, result);
    return result;
}

struct tm *gmtime(const time_t *timep)
{
    return gmtime_r(timep, &_tm_static);
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    return gmtime_r(timep, result);
}

struct tm *localtime(const time_t *timep)
{
    return gmtime(timep);
}

int timespec_get(struct timespec *ts, int base)
{
    if (!ts || base != TIME_UTC) return 0;
    _acquire_drivers();

    ts->tv_sec = time((time_t *)0);
    ts->tv_nsec = 0;

    if (_tsc && _tsc->get_tsc_uptime_ms) {
        uint64_t subsec_ms = (_tsc->get_tsc_uptime_ms() - _boot_uptime_ms)
                             % 1000ULL;
        ts->tv_nsec = (long)(subsec_ms * 1000000ULL);
    }
    return base;
}

static const char * const _wday_long[7] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};
static const char * const _wday_short[7] = {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
};
static const char * const _mon_long[12] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};
static const char * const _mon_short[12] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

static size_t _puts_buf(char *buf, size_t pos, size_t max, const char *s)
{
    while (*s && pos+1 < max) buf[pos++] = *s++;
    return pos;
}

static size_t _puti_buf(char *buf, size_t pos, size_t max, int val, int width)
{
    char tmp[16]; int i = 0;
    if (val < 0) val = 0;
    if (val == 0) { tmp[i++]='0'; }
    else { int v=val; while(v>0){tmp[i++]=(char)('0'+v%10);v/=10;} }
    while (i < width && pos+1 < max) buf[pos++]='0', width--;
    for (int j=i-1; j>=0 && pos+1<max; j--) buf[pos++]=tmp[j];
    return pos;
}

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm)
{
    if (!s || maxsize==0 || !format || !tm) return 0;
    size_t pos = 0;

    while (*format && pos+1 < maxsize) {
        if (*format != '%') { s[pos++] = *format++; continue; }
        format++;
        char spec = *format++;

        switch (spec) {
        case 'Y': pos=_puti_buf(s,pos,maxsize,tm->tm_year+1900,4); break;
        case 'y': pos=_puti_buf(s,pos,maxsize,(tm->tm_year+1900)%100,2); break;
        case 'C': pos=_puti_buf(s,pos,maxsize,(tm->tm_year+1900)/100,2); break;
        case 'm': pos=_puti_buf(s,pos,maxsize,tm->tm_mon+1,2); break;
        case 'B':
            if(tm->tm_mon>=0&&tm->tm_mon<12)
                pos=_puts_buf(s,pos,maxsize,_mon_long[tm->tm_mon]); break;
        case 'b': case 'h':
            if(tm->tm_mon>=0&&tm->tm_mon<12)
                pos=_puts_buf(s,pos,maxsize,_mon_short[tm->tm_mon]); break;
        case 'd': pos=_puti_buf(s,pos,maxsize,tm->tm_mday,2); break;
        case 'e':
            if(tm->tm_mday<10&&pos+1<maxsize) s[pos++]=' ';
            pos=_puti_buf(s,pos,maxsize,tm->tm_mday,1); break;
        case 'j': pos=_puti_buf(s,pos,maxsize,tm->tm_yday+1,3); break;
        case 'w': pos=_puti_buf(s,pos,maxsize,tm->tm_wday,1); break;
        case 'u': pos=_puti_buf(s,pos,maxsize,
                            tm->tm_wday==0?7:tm->tm_wday,1); break;
        case 'A':
            if(tm->tm_wday>=0&&tm->tm_wday<7)
                pos=_puts_buf(s,pos,maxsize,_wday_long[tm->tm_wday]); break;
        case 'a':
            if(tm->tm_wday>=0&&tm->tm_wday<7)
                pos=_puts_buf(s,pos,maxsize,_wday_short[tm->tm_wday]); break;
        case 'H': pos=_puti_buf(s,pos,maxsize,tm->tm_hour,2); break;
        case 'I': { int h=tm->tm_hour%12; if(!h)h=12;
                    pos=_puti_buf(s,pos,maxsize,h,2); } break;
        case 'M': pos=_puti_buf(s,pos,maxsize,tm->tm_min,2); break;
        case 'S': pos=_puti_buf(s,pos,maxsize,tm->tm_sec,2); break;
        case 'p': pos=_puts_buf(s,pos,maxsize,tm->tm_hour<12?"AM":"PM"); break;
        case 'P': pos=_puts_buf(s,pos,maxsize,tm->tm_hour<12?"am":"pm"); break;
        case 'X':
            pos=_puti_buf(s,pos,maxsize,tm->tm_hour,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_min,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_sec,2); break;
        case 'x':
            pos=_puti_buf(s,pos,maxsize,tm->tm_mon+1,2);
            if(pos+1<maxsize)s[pos++]='/';
            pos=_puti_buf(s,pos,maxsize,tm->tm_mday,2);
            if(pos+1<maxsize)s[pos++]='/';
            pos=_puti_buf(s,pos,maxsize,(tm->tm_year+1900)%100,2); break;
        case 'c':
            if(tm->tm_wday>=0&&tm->tm_wday<7)
                pos=_puts_buf(s,pos,maxsize,_wday_short[tm->tm_wday]);
            if(pos+1<maxsize)s[pos++]=' ';
            if(tm->tm_mon>=0&&tm->tm_mon<12)
                pos=_puts_buf(s,pos,maxsize,_mon_short[tm->tm_mon]);
            if(pos+1<maxsize)s[pos++]=' ';
            if(tm->tm_mday<10&&pos+1<maxsize)s[pos++]=' ';
            pos=_puti_buf(s,pos,maxsize,tm->tm_mday,1);
            if(pos+1<maxsize)s[pos++]=' ';
            pos=_puti_buf(s,pos,maxsize,tm->tm_hour,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_min,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_sec,2);
            if(pos+1<maxsize)s[pos++]=' ';
            pos=_puti_buf(s,pos,maxsize,tm->tm_year+1900,4); break;
        case 'D':
            pos=_puti_buf(s,pos,maxsize,tm->tm_mon+1,2);
            if(pos+1<maxsize)s[pos++]='/';
            pos=_puti_buf(s,pos,maxsize,tm->tm_mday,2);
            if(pos+1<maxsize)s[pos++]='/';
            pos=_puti_buf(s,pos,maxsize,(tm->tm_year+1900)%100,2); break;
        case 'F':
            pos=_puti_buf(s,pos,maxsize,tm->tm_year+1900,4);
            if(pos+1<maxsize)s[pos++]='-';
            pos=_puti_buf(s,pos,maxsize,tm->tm_mon+1,2);
            if(pos+1<maxsize)s[pos++]='-';
            pos=_puti_buf(s,pos,maxsize,tm->tm_mday,2); break;
        case 'R':
            pos=_puti_buf(s,pos,maxsize,tm->tm_hour,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_min,2); break;
        case 'T':
            pos=_puti_buf(s,pos,maxsize,tm->tm_hour,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_min,2);
            if(pos+1<maxsize)s[pos++]=':';
            pos=_puti_buf(s,pos,maxsize,tm->tm_sec,2); break;
        case 'Z': pos=_puts_buf(s,pos,maxsize,"UTC"); break;
        case 'z': pos=_puts_buf(s,pos,maxsize,"+0000"); break;
        case 'n': if(pos+1<maxsize)s[pos++]='\n'; break;
        case 't': if(pos+1<maxsize)s[pos++]='\t'; break;
        case '%': if(pos+1<maxsize)s[pos++]='%'; break;
        default: break;
        }
    }
    s[pos] = '\0';
    return pos;
}