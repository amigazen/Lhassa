/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_datetime.c - LHA DOS/Unix timestamp pack/unpack.
 *
 * Amiga DateStamp conversion is in lib_source/lh_amiga_time.c.
 */

#include "lh_internal.h"

/* Seconds per calendar day (Unix / level-2 LHA stamps). */
#define LH_SECS_PER_DAY       86400UL

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

/*
 * Amiga DateStamp bridges (lh_datetime_now / from_datestamp / to_datestamp)
 * live in lib_source/lh_amiga_time.c so lh.library always links them.
 */
#ifdef LH_HOST
void lh_datetime_now(lh_datetime *dt)
{
    if (!dt) {
        return;
    }
    lh_datetime_from_time_t(dt, (long)time(NULL));
}
#endif

lh_status lh_datetime_validate(const lh_datetime *dt)
{
    if (!dt) {
        return LH_ERR_INVALID_ARG;
    }
    if (dt->year < 1980 || dt->year > 2107) {
        return LH_ERR_INVALID_ARG;
    }
    if (dt->month < 1 || dt->month > 12) {
        return LH_ERR_INVALID_ARG;
    }
    if (dt->day < 1 || dt->day > 31) {
        return LH_ERR_INVALID_ARG;
    }
    if (dt->hour > 23 || dt->minute > 59 || dt->second > 59) {
        return LH_ERR_INVALID_ARG;
    }
    return LH_OK;
}

unsigned long lh_dos_timestamp_pack(const lh_datetime *dt)
{
    unsigned long ts;

    ts = (unsigned long)(((dt->year - 1980) & 0x7f) << 25);
    ts |= (unsigned long)((dt->month & 0x0f) << 21);
    ts |= (unsigned long)((dt->day & 0x1f) << 16);
    ts |= (unsigned long)((dt->hour & 0x1f) << 11);
    ts |= (unsigned long)((dt->minute & 0x3f) << 5);
    ts |= (unsigned long)((dt->second / 2) & 0x1f);
    return ts;
}

lh_status lh_datetime_pack(const lh_datetime *dt, unsigned long *out)
{
    lh_status st;

    if (!dt || !out) {
        return LH_ERR_INVALID_ARG;
    }
    st = lh_datetime_validate(dt);
    if (st != LH_OK) {
        return st;
    }
    *out = lh_dos_timestamp_pack(dt);
    return LH_OK;
}

void lh_datetime_unpack(unsigned long packed, lh_datetime *dt)
{
    if (!dt) {
        return;
    }
    dt->year = (unsigned short)(1980 + ((packed >> 25) & 0x7f));
    dt->month = (unsigned char)((packed >> 21) & 0x0f);
    dt->day = (unsigned char)((packed >> 16) & 0x1f);
    dt->hour = (unsigned char)((packed >> 11) & 0x1f);
    dt->minute = (unsigned char)((packed >> 5) & 0x3f);
    dt->second = (unsigned char)((packed & 0x1f) * 2);
}

/*
 * Convert calendar fields to Unix seconds without relying on libc timegm.
 * Used for LHA level-2 timestamps on both Amiga and host builds.
 */
unsigned long lh_datetime_to_unix(const lh_datetime *dt)
{
    unsigned long days;
    unsigned short y;
    int m;
    unsigned long mdays;

    if (!dt) {
        return 0;
    }
    days = 0;
    for (y = 1970; y < dt->year; y++) {
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
    return days * LH_SECS_PER_DAY
        + (unsigned long)dt->hour * 3600UL
        + (unsigned long)dt->minute * 60UL
        + (unsigned long)dt->second;
}

void lh_datetime_from_unix(lh_datetime *dt, unsigned long unix_secs)
{
    unsigned long days;
    unsigned long rem;
    unsigned short year;
    unsigned char month;
    unsigned char day;
    int m;

    if (!dt) {
        return;
    }
    days = unix_secs / LH_SECS_PER_DAY;
    rem = unix_secs % LH_SECS_PER_DAY;
    year = 1970;
    while (1) {
        unsigned long ydays;

        ydays = lh_is_leap_year(year) ? 366UL : 365UL;
        if (days < ydays) {
            break;
        }
        days -= ydays;
        year++;
        if (year > 2107) {
            break;
        }
    }
    month = 1;
    day = 1;
    for (m = 0; m < 12; m++) {
        unsigned long mdays;

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
    dt->hour = (unsigned char)(rem / 3600UL);
    rem %= 3600UL;
    dt->minute = (unsigned char)(rem / 60UL);
    dt->second = (unsigned char)(rem % 60UL);
}

void lh_datetime_from_time_t(lh_datetime *dt, long t)
{
#ifdef LH_HOST
    struct tm *tmv;
    time_t tv;

    if (!dt) {
        return;
    }
    tv = (time_t)t;
    tmv = localtime(&tv);
    if (!tmv) {
        memset(dt, 0, sizeof(*dt));
        dt->year = 1980;
        dt->month = 1;
        dt->day = 1;
        return;
    }
    dt->year = (unsigned short)(tmv->tm_year + 1900);
    dt->month = (unsigned char)(tmv->tm_mon + 1);
    dt->day = (unsigned char)tmv->tm_mday;
    dt->hour = (unsigned char)tmv->tm_hour;
    dt->minute = (unsigned char)tmv->tm_min;
    dt->second = (unsigned char)tmv->tm_sec;
#else
    /* Amiga: treat t as Unix seconds (level-2 LHA stamp). */
    if (!dt) {
        return;
    }
    if (t <= 0) {
        lh_datetime_now(dt);
        return;
    }
    lh_datetime_from_unix(dt, (unsigned long)t);
#endif
}
