/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_amiga.c - Amiga dos.library hooks for LhA extract (dates, paths, I/O).
 * ANSI builds compile empty stubs; no stdio here.
 */

#include "lha_platform.h"
#include "/include/lh.h"

#ifdef LHA_AMIGA

#define __USE_SYSBASE
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include <string.h>

/*
 * lh_archive.c consults LhBase->lhb_PendingMem when built into lh.library.
 * Standalone LhA has no library base; keep the symbol so the shared core
 * links, with PendingMem handoff permanently inactive.
 */
struct LHBase;
struct LHBase *LhBase = NULL;

static void lha_dt_to_datestamp(const lh_datetime *dt, struct DateStamp *ds)
{
    unsigned long unix_secs;
    unsigned long amiga_secs;

    if (!dt || !ds) {
        return;
    }
    unix_secs = (unsigned long)dt->second
        + (unsigned long)dt->minute * 60UL
        + (unsigned long)dt->hour * 3600UL
        + (unsigned long)(dt->day - 1) * 86400UL
        + (unsigned long)(dt->month - 1) * 30UL * 86400UL
        + (unsigned long)(dt->year - 1970) * 365UL * 86400UL;
    amiga_secs = unix_secs - ((8UL * 365UL + 2UL) * 86400UL);
    ds->ds_Days = (LONG)(amiga_secs / 86400UL);
    amiga_secs %= 86400UL;
    ds->ds_Minute = (LONG)(amiga_secs / 60UL);
    ds->ds_Tick = (LONG)((amiga_secs % 60UL) * 50UL);
}

static int lha_mkdir_chain(STRPTR dirpath)
{
    char buf[512];
    char *p;
    BPTR lock;

    if (!dirpath || !dirpath[0]) {
        return 1;
    }
    strncpy(buf, (const char *)dirpath, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            lock = CreateDir((STRPTR)buf);
            if (lock == (BPTR)NULL && IoErr() != ERROR_OBJECT_EXISTS) {
                return 0;
            }
            if (lock != (BPTR)NULL) {
                UnLock(lock);
            }
            *p = '/';
        }
    }
    lock = CreateDir((STRPTR)buf);
    if (lock == (BPTR)NULL && IoErr() != ERROR_OBJECT_EXISTS) {
        return 0;
    }
    if (lock != (BPTR)NULL) {
        UnLock(lock);
    }
    return 1;
}

int lha_mkpath_amiga(const char *path)
{
    char buf[512];
    STRPTR pp;
    STRPTR p;

    if (!path || !path[0]) {
        return 0;
    }
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pp = PathPart((STRPTR)buf);
    if (!pp || pp == (STRPTR)buf) {
        return 1;
    }
    *pp = '\0';
    if (buf[0] == '\0') {
        return 1;
    }
    p = (STRPTR)buf;
    while (*p) {
        p++;
    }
    if (p > (STRPTR)buf && *(p - 1) == ':') {
        return 1;
    }
    return lha_mkdir_chain((STRPTR)buf);
}

int lha_write_file_amiga(const char *path, const unsigned char *data, size_t len)
{
    BPTR fh;
    LONG wrote;
    char parent[512];
    STRPTR pp;

    if (!path || !path[0]) {
        return 0;
    }
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    pp = PathPart((STRPTR)parent);
    if (pp && pp != (STRPTR)parent) {
        *pp = '\0';
        if (!lha_mkpath_amiga(parent)) {
            return 0;
        }
    }
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    if (len == 0) {
        Close(fh);
        return 1;
    }
    if (!data) {
        Close(fh);
        DeleteFile((STRPTR)path);
        return 0;
    }
    wrote = Write(fh, (APTR)data, (LONG)len);
    Close(fh);
    if (wrote != (LONG)len) {
        DeleteFile((STRPTR)path);
        return 0;
    }
    return 1;
}

void lha_apply_file_metadata(const char *path, const lh_datetime *dt, lh_attrs attrs)
{
    struct DateStamp ds;

    if (!path || !path[0]) {
        return;
    }
    if (dt) {
        lha_dt_to_datestamp(dt, &ds);
        SetFileDate((STRPTR)path, &ds);
    }
    SetProtection((STRPTR)path, (LONG)attrs);
}

#else /* !LHA_AMIGA */

void lha_apply_file_metadata(const char *path, const lh_datetime *dt, lh_attrs attrs)
{
    (void)path;
    (void)dt;
    (void)attrs;
}

#endif
