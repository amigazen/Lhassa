/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_amiga_time.c - Amiga DateStamp <-> lh_datetime (library shell).
 *
 * Kept in lib_source so SAS/C always links these symbols into lh.library.
 * Core lh_datetime.c still owns pack/unpack and Unix level-2 stamps.
 */

#include "lh_native_guard.h"

#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "/include/lh.h"

#ifndef TICKS_PER_SECOND
#define LH_TPS 50
#else
#define LH_TPS TICKS_PER_SECOND
#endif

static const unsigned char lh_days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int lh_is_leap_year(unsigned short year)
{
    if ((year % 4) != 0) {
        return 0;
    }
    if ((year % 100) != 0) {
        return 1;
    }
    return (year % 400) == 0;
}

void lh_datetime_from_datestamp(const struct DateStamp *ds, lh_datetime *dt)
{
    unsigned long days;
    unsigned short year;
    unsigned char month;
    unsigned char day;
    unsigned long minute;
    unsigned long tick;
    int m;
    unsigned long ydays;
    unsigned long mdays;

    if (!ds || !dt) {
        return;
    }
    days = (unsigned long)ds->ds_Days;
    minute = (unsigned long)ds->ds_Minute;
    tick = (unsigned long)ds->ds_Tick;
    year = 1978;
    for (;;) {
        ydays = lh_is_leap_year(year) ? 366UL : 365UL;
        if (days < ydays) {
            break;
        }
        days -= ydays;
        year++;
    }
    month = 1;
    day = 1;
    for (m = 0; m < 12; m++) {
        mdays = lh_days_in_month[m];
        if (m == 1 && lh_is_leap_year(year)) {
            mdays++;
        }
        if (days < mdays) {
            month = (unsigned char)(m + 1);
            day = (unsigned char)(days + 1);
            break;
        }
        days -= mdays;
    }
    dt->year = year;
    dt->month = month;
    dt->day = day;
    dt->hour = (unsigned char)(minute / 60UL);
    dt->minute = (unsigned char)(minute % 60UL);
    dt->second = (unsigned char)(tick / (unsigned long)LH_TPS);
}

void lh_datetime_to_datestamp(const lh_datetime *dt, struct DateStamp *ds)
{
    unsigned long days;
    unsigned short y;
    int m;
    unsigned long mdays;

    if (!dt || !ds) {
        return;
    }
    memset(ds, 0, sizeof(*ds));
    days = 0;
    for (y = 1978; y < dt->year; y++) {
        days += lh_is_leap_year(y) ? 366UL : 365UL;
    }
    for (m = 1; m < (int)dt->month; m++) {
        mdays = lh_days_in_month[m - 1];
        if (m == 2 && lh_is_leap_year(dt->year)) {
            mdays++;
        }
        days += mdays;
    }
    if (dt->day > 0) {
        days += (unsigned long)(dt->day - 1);
    }
    ds->ds_Days = (LONG)days;
    ds->ds_Minute = (LONG)((unsigned long)dt->hour * 60UL
        + (unsigned long)dt->minute);
    ds->ds_Tick = (LONG)((unsigned long)dt->second * (unsigned long)LH_TPS);
}

void lh_datetime_now(lh_datetime *dt)
{
    struct DateStamp ds;

    if (!dt) {
        return;
    }
    DateStamp(&ds);
    lh_datetime_from_datestamp(&ds, dt);
}
