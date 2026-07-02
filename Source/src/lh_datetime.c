/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_datetime.c - LHA DOS timestamp pack/unpack.
 */

#include "lh_internal.h"

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

void lh_datetime_from_time_t(lh_datetime *dt, long t)
{
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
}
