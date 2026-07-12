/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_icon.c - Cache default Workbench icons via icon.library.
 *
 * Only change from the working handler: Disk/Drawer defaults come from
 * GetDefDiskObject (not HappyENV blobs).  icon.library has no API to
 * emit .info bytes in memory, so we PutDiskObject once to RAM: during
 * init (pr_MsgPort idle), dos-read the bytes into AllocMem, DeleteFile.
 * The cache lives only in working memory after that.
 *
 * Must run before the startup ReplyDosPacket - GetDef/Put/Open Wait on
 * pr_MsgPort and would steal client packets after the device is live.
 *
 * SAS/C: AllocMem/Open need GD gd in scope (SysBase/DOSBase macros).
 * Never put star-slash inside block comments.  DB2 takes two value args.
 */

#include "lh-handler.h"
#include "lh_drawer_info.h"

#include <workbench/workbench.h>
#include <proto/icon.h>

#include <string.h>

struct Library *IconBase = NULL;

struct lhh_icon_cache {
    UBYTE *data;
    ULONG len;
};

static struct lhh_icon_cache lhh_icons[4];

const UBYTE *lhh_icon_data(int kind)
{
    if (kind < 0 || kind > LHH_VINFO_PROJECT) {
        return NULL;
    }
    return lhh_icons[kind].data;
}

ULONG lhh_icon_len(int kind)
{
    if (kind < 0 || kind > LHH_VINFO_PROJECT) {
        return 0;
    }
    return lhh_icons[kind].len;
}

static void lhh_icon_slot_clear(GD gd, int kind)
{
    if (lhh_icons[kind].data != NULL && lhh_icons[kind].len > 0) {
        FreeMem(lhh_icons[kind].data, lhh_icons[kind].len);
    }
    lhh_icons[kind].data = NULL;
    lhh_icons[kind].len = 0;
    (void)gd;
}

/*
 * Init-only dos read (pr_MsgPort idle).  gd required for Open/AllocMem.
 */
static int lhh_icon_read_dos(GD gd, STRPTR path, UBYTE **out, ULONG *outlen)
{
    BPTR fh;
    UBYTE *buf;
    UBYTE *nbuf;
    ULONG cap;
    ULONG len;
    LONG n;

    *out = NULL;
    *outlen = 0;
    fh = Open(path, MODE_OLDFILE);
    if (fh == ZERO) {
        return 0;
    }
    cap = 2048UL;
    buf = (UBYTE *)AllocMem(cap, MEMF_PUBLIC);
    if (buf == NULL) {
        Close(fh);
        return 0;
    }
    len = 0;
    for (;;) {
        if (len + 512UL > cap) {
            nbuf = (UBYTE *)AllocMem(cap * 2UL, MEMF_PUBLIC);
            if (nbuf == NULL) {
                FreeMem(buf, cap);
                Close(fh);
                return 0;
            }
            CopyMem(buf, nbuf, len);
            FreeMem(buf, cap);
            buf = nbuf;
            cap *= 2UL;
        }
        n = Read(fh, buf + len, 512);
        if (n < 0) {
            FreeMem(buf, cap);
            Close(fh);
            return 0;
        }
        if (n == 0) {
            break;
        }
        len += (ULONG)n;
    }
    Close(fh);
    if (len == 0) {
        FreeMem(buf, cap);
        return 0;
    }
    if (len < cap) {
        nbuf = (UBYTE *)AllocMem(len, MEMF_PUBLIC);
        if (nbuf != NULL) {
            CopyMem(buf, nbuf, len);
            FreeMem(buf, cap);
            buf = nbuf;
        } else {
            /* Keep oversized block; FreeMem must use cap - fail safe. */
            FreeMem(buf, cap);
            return 0;
        }
    }
    *out = buf;
    *outlen = len;
    (void)gd;
    return 1;
}

static int lhh_icon_seed(GD gd, int kind, const UBYTE *src, ULONG len)
{
    UBYTE *buf;

    if (src == NULL || len == 0UL) {
        return 0;
    }
    lhh_icon_slot_clear(gd, kind);
    buf = (UBYTE *)AllocMem(len, MEMF_PUBLIC);
    if (buf == NULL) {
        return 0;
    }
    CopyMem((APTR)src, buf, len);
    lhh_icons[kind].data = buf;
    lhh_icons[kind].len = len;
    return 1;
}

/*
 * GetDefDiskObject + PutDiskObject (icon.library serializer) + read to RAM.
 */
static int lhh_icon_cache_one(GD gd, int kind, LONG wbtype, STRPTR ramname)
{
    struct DiskObject *dobj;
    struct DiskObject *check;
    static char infopath[64];
    LONG i;
    LONG ok;
    UBYTE *data;
    ULONG len;

    dobj = GetDefDiskObject(wbtype);
    if (dobj == NULL) {
        DB1("icon: GetDefDiskObject(%ld) failed\n", wbtype);
        return 0;
    }
    dobj->do_Type = (UBYTE)wbtype;
    ok = PutDiskObject(ramname, dobj);
    FreeDiskObject(dobj);
    if (!ok) {
        DB1("icon: PutDiskObject(%s) failed\n", ramname);
        return 0;
    }

    check = GetDiskObject(ramname);
    if (check == NULL || check->do_Type != (UBYTE)wbtype) {
        if (check != NULL) {
            DB3("icon: %s type=%ld want=%ld\n", ramname,
                (LONG)check->do_Type, wbtype);
            FreeDiskObject(check);
        } else {
            DB1("icon: GetDiskObject(%s) failed after Put\n", ramname);
        }
        for (i = 0; ramname[i] != '\0' && i < 58; i++) {
            infopath[i] = ramname[i];
        }
        infopath[i++] = '.';
        infopath[i++] = 'i';
        infopath[i++] = 'n';
        infopath[i++] = 'f';
        infopath[i++] = 'o';
        infopath[i] = '\0';
        DeleteFile((STRPTR)infopath);
        return 0;
    }
    FreeDiskObject(check);

    for (i = 0; ramname[i] != '\0' && i < 58; i++) {
        infopath[i] = ramname[i];
    }
    infopath[i++] = '.';
    infopath[i++] = 'i';
    infopath[i++] = 'n';
    infopath[i++] = 'f';
    infopath[i++] = 'o';
    infopath[i] = '\0';

    data = NULL;
    len = 0;
    if (!lhh_icon_read_dos(gd, (STRPTR)infopath, &data, &len)) {
        DB1("icon: read %s failed\n", infopath);
        DeleteFile((STRPTR)infopath);
        return 0;
    }
    DeleteFile((STRPTR)infopath);

    lhh_icon_slot_clear(gd, kind);
    lhh_icons[kind].data = data;
    lhh_icons[kind].len = len;
    DB2("icon: cached kind=%ld len=%ld\n", (LONG)kind, len);
    return 1;
}

int lhh_icon_init(APTR gdarg)
{
    GD gd;
    LONG i;

    gd = (GD)gdarg;
    for (i = 0; i < 4; i++) {
        lhh_icons[i].data = NULL;
        lhh_icons[i].len = 0;
    }
    if (gd == NULL || gd->gd_We == NULL) {
        return 0;
    }

    IconBase = OpenLibrary((STRPTR)"icon.library", 37);
    if (IconBase == NULL) {
        DB("icon: OpenLibrary icon.library failed\n");
        return 0;
    }

    if (!lhh_icon_cache_one(gd, LHH_VINFO_DISK, WBDISK,
            (STRPTR)"RAM:lhh_def_disk")) {
        DB("icon: WBDISK default unavailable\n");
        return 0;
    }

    if (!lhh_icon_cache_one(gd, LHH_VINFO_DRAWER, WBDRAWER,
            (STRPTR)"RAM:lhh_def_drawer")) {
        (void)lhh_icon_seed(gd, LHH_VINFO_DRAWER, lhh_drawer_info_fallback,
            lhh_drawer_info_fallback_len);
    }

    if (lhh_icons[LHH_VINFO_DISK].data == NULL
        || lhh_icons[LHH_VINFO_DRAWER].data == NULL) {
        return 0;
    }
    return 1;
}

void lhh_icon_cleanup(APTR gdarg)
{
    GD gd;
    LONG i;

    gd = (GD)gdarg;
    for (i = 0; i < 4; i++) {
        lhh_icon_slot_clear(gd, i);
    }
    if (IconBase != NULL) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
}
