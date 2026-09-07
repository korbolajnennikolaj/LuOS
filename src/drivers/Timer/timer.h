#ifndef TIMER_H
#define TIMER_H

#include "apic_driver.h"
#include "hpet_driver.h"
#include "pit_driver.h"
#include "rtc_driver.h"
#include "tsc_driver.h"

enum TIMER_TYPE{
    PIT_TIMER = 0,
    HPET_TIMER = 1,
    RTC_TIMER = 2,
    TSC_TIMER = 3,
    APIC_TIMER = 4
};

#endif
