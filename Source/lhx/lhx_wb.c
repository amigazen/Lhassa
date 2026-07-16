/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx_wb.c - Workbench startup for LhX.
 *
 * When LhX is the default tool of a .lha project icon:
 *   1) If LHA: is mounted (lh-handler), remap the path to LHA: and
 *      OpenWorkbenchObject() so the archive opens as a virtual drawer.
 *   2) Otherwise extract into T:LhX/<archive>/ with Olsen's Gauge
 *      progress requester.  On success, OpenWorkbenchObject(SHOWALL)
 *      on that drawer; on failure or Stop, delete the T: tree.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <graphics/gfxbase.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/wb.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "lhx_internal.h"
#include "lhx_gauge.h"

#ifndef ZERO
#define ZERO ((BPTR)0L)
#endif

#ifndef WBOPENA_Show
#define WBOPENA_Show (TAG_USER + 0x4B)
#endif
#ifndef DDFLAGS_SHOWALL
#define DDFLAGS_SHOWALL 2
#endif

/* Why a T: unpack failed (for EasyRequest after Gauge closes). */
#define LHX_WB_OK           0
#define LHX_WB_ERR_LIBS     1
#define LHX_WB_ERR_DEST     2
#define LHX_WB_ERR_GAUGE    3
#define LHX_WB_ERR_OPEN     4
#define LHX_WB_ERR_EXTRACT  5
#define LHX_WB_ERR_STOP     6

/* AmigaDOS codes lh.library uses in LhErr (see SDK/Autodocs/lh.doc). */
#ifndef ERROR_READ_PROTECTED
#define ERROR_READ_PROTECTED 224
#endif
#ifndef ERROR_WRITE_PROTECTED
#define ERROR_WRITE_PROTECTED 223
#endif
#ifndef ERROR_OBJECT_NOT_FOUND
#define ERROR_OBJECT_NOT_FOUND 205
#endif
#ifndef ERROR_NO_FREE_STORE
#define ERROR_NO_FREE_STORE 103
#endif
#ifndef ERROR_INVALID_COMPONENT_NAME
#define ERROR_INVALID_COMPONENT_NAME 210
#endif
#ifndef ERROR_OBJECT_WRONG_TYPE
#define ERROR_OBJECT_WRONG_TYPE 212
#endif
#ifndef ERROR_OBJECT_IN_USE
#define ERROR_OBJECT_IN_USE 202
#endif

#ifndef TICKS_PER_SECOND
#define TICKS_PER_SECOND 50
#endif

/* Bases required by Workbench open and by Gauge (see lhx_gauge.c). */
struct Library *WorkbenchBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

extern struct Library *LhBase;
extern struct Library *UtilityBase;

/* Temporary WB debug log — read with: type RAM:LhX.log */
#define LHX_WB_LOGNAME "RAM:LhX.log"

static BPTR lhx_wb_logfh = ZERO;

static void lhx_wb_log_open(void)
{
    if (lhx_wb_logfh != ZERO) {
        return;
    }
    lhx_wb_logfh = Open((STRPTR)LHX_WB_LOGNAME, MODE_NEWFILE);
    if (lhx_wb_logfh != ZERO) {
        FPrintf(lhx_wb_logfh, "=== LhX WB log ===\n", 0);
    }
}

static void lhx_wb_log_close(void)
{
    if (lhx_wb_logfh != ZERO) {
        FPrintf(lhx_wb_logfh, "=== end ===\n", 0);
        Close(lhx_wb_logfh);
        lhx_wb_logfh = ZERO;
    }
}

static void lhx_wb_log_s(STRPTR label, STRPTR value)
{
    if (lhx_wb_logfh == ZERO) {
        return;
    }
    if (label == NULL) {
        label = (STRPTR)"?";
    }
    if (value == NULL) {
        FPrintf(lhx_wb_logfh, "%s: (null)\n", (LONG)label);
    } else {
        FPrintf(lhx_wb_logfh, "%s: %s\n", (LONG)label, (LONG)value);
    }
    Flush(lhx_wb_logfh);
}

static void lhx_wb_log_l(STRPTR label, LONG value)
{
    if (lhx_wb_logfh == ZERO) {
        return;
    }
    if (label == NULL) {
        label = (STRPTR)"?";
    }
    FPrintf(lhx_wb_logfh, "%s: %ld\n", (LONG)label, value);
    Flush(lhx_wb_logfh);
}

static void lhx_wb_log_ll(STRPTR label, LONG a, LONG b)
{
    if (lhx_wb_logfh == ZERO) {
        return;
    }
    if (label == NULL) {
        label = (STRPTR)"?";
    }
    FPrintf(lhx_wb_logfh, "%s: %ld %ld\n", (LONG)label, a, b);
    Flush(lhx_wb_logfh);
}

static void lhx_copy_str(STRPTR dst, const char *src, LONG lim)
{
    LONG i;

    if (dst == NULL || lim <= 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; src[i] != '\0' && i + 1 < lim; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * Human-readable note for lh.library LhErr / AmigaDOS codes.
 * ERROR_READ_PROTECTED (224) is reused for catalog/header parse failure.
 */
static STRPTR lhx_wb_dos_err_note(LONG code)
{
    switch (code) {
    case ERROR_READ_PROTECTED:
        return (STRPTR)"archive header/catalog parse failed";
    case ERROR_WRITE_PROTECTED:
        return (STRPTR)"archive write failed";
    case ERROR_OBJECT_NOT_FOUND:
        return (STRPTR)"object not found";
    case ERROR_NO_FREE_STORE:
        return (STRPTR)"out of memory";
    case ERROR_INVALID_COMPONENT_NAME:
        return (STRPTR)"invalid path or name";
    case ERROR_OBJECT_WRONG_TYPE:
        return (STRPTR)"wrong object type";
    case ERROR_OBJECT_IN_USE:
        return (STRPTR)"object in use";
    case 0:
        return (STRPTR)"unknown error";
    default:
        return (STRPTR)"see LhErr code";
    }
}

/*
 * Gauge has no error body - EasyRequest after it closes.
 * errcode is LhErr() when available (0 if none).
 */
static void lhx_wb_append_long(char *msg, LONG *n, LONG lim, LONG value)
{
    char num[12];
    LONG nlen;
    LONG v;

    v = value;
    if (v < 0) {
        if (*n + 1 < lim) {
            msg[(*n)++] = '-';
        }
        v = -v;
    }
    nlen = 0;
    if (v == 0) {
        num[nlen++] = '0';
    } else {
        while (v > 0 && nlen < (LONG)sizeof(num) - 1) {
            num[nlen++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (nlen > 0 && *n + 1 < lim) {
        nlen--;
        msg[(*n)++] = num[nlen];
    }
}

static void lhx_wb_append_str(char *msg, LONG *n, LONG lim, const char *s)
{
    LONG i;

    if (s == NULL) {
        return;
    }
    for (i = 0; s[i] != '\0' && *n + 1 < lim; i++) {
        msg[(*n)++] = s[i];
    }
}

static void lhx_wb_alert(LONG why, STRPTR archive, LONG errcode)
{
    struct EasyStruct es;
    STRPTR name;
    STRPTR note;
    char msg[180];
    LONG n;
    const char *prefix;

    if (IntuitionBase == NULL || why == LHX_WB_OK) {
        return;
    }

    if (why == LHX_WB_ERR_STOP) {
        es.es_StructSize = sizeof(struct EasyStruct);
        es.es_Flags = 0;
        es.es_Title = (UBYTE *)"LhX";
        es.es_TextFormat = (UBYTE *)"Extraction cancelled.\nPartial files were removed.";
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequest(NULL, &es, NULL);
        return;
    }

    name = archive;
    if (name == NULL || name[0] == '\0') {
        name = (STRPTR)"archive";
    } else {
        name = FilePart(name);
        if (name == NULL || name[0] == '\0') {
            name = archive;
        }
    }
    note = lhx_wb_dos_err_note(errcode);

    switch (why) {
    case LHX_WB_ERR_OPEN:
        prefix = "Cannot open \"";
        break;
    case LHX_WB_ERR_EXTRACT:
        prefix = "Cannot extract \"";
        break;
    case LHX_WB_ERR_DEST:
        prefix = "Cannot create temp drawer for \"";
        break;
    case LHX_WB_ERR_GAUGE:
        prefix = "Cannot open progress window for \"";
        break;
    case LHX_WB_ERR_LIBS:
        prefix = "Cannot open required libraries\nto extract \"";
        break;
    default:
        prefix = "Failed on \"";
        break;
    }

    n = 0;
    lhx_wb_append_str(msg, &n, (LONG)sizeof(msg), prefix);
    lhx_wb_append_str(msg, &n, (LONG)sizeof(msg), (const char *)name);
    lhx_wb_append_str(msg, &n, (LONG)sizeof(msg), "\".");
    if (errcode != 0) {
        lhx_wb_append_str(msg, &n, (LONG)sizeof(msg), "\nLhErr ");
        lhx_wb_append_long(msg, &n, (LONG)sizeof(msg), errcode);
        lhx_wb_append_str(msg, &n, (LONG)sizeof(msg), " - ");
        lhx_wb_append_str(msg, &n, (LONG)sizeof(msg), (const char *)note);
    }
    if (why == LHX_WB_ERR_EXTRACT) {
        lhx_wb_append_str(msg, &n, (LONG)sizeof(msg),
            "\nPartial files were removed.");
    }
    msg[n] = '\0';

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"LhX";
    es.es_TextFormat = (UBYTE *)msg;
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequest(NULL, &es, NULL);
}

/*
 * True if path is already under LHA: (case-insensitive device name).
 */
static int lhx_path_is_lha(STRPTR path)
{
    if (path == NULL) {
        return 0;
    }
    if (path[0] != 'L' && path[0] != 'l') {
        return 0;
    }
    if (path[1] != 'H' && path[1] != 'h') {
        return 0;
    }
    if (path[2] != 'A' && path[2] != 'a') {
        return 0;
    }
    return path[3] == ':';
}

/*
 * LHA: is usable only when the lh-handler device is in the DosList with a
 * live task.  Do not Lock("LHA:") — a missing volume prompts "Insert disk",
 * and an Assign named LHA: would falsely look like the overlay.
 */
static int lhx_lha_device_available(void)
{
    struct DosList *dl;
    struct DosList *entry;
    int found;
    STRPTR bname;
    LONG n;
    char nbuf[64];
    LONG i;

    found = 0;
    lhx_wb_log_s((STRPTR)"LHA check", (STRPTR)"LockDosList DEVICES");
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    if (dl == NULL) {
        lhx_wb_log_l((STRPTR)"LockDosList fail IoErr", IoErr());
        return 0;
    }
    entry = FindDosEntry(dl, (STRPTR)"LHA", LDF_DEVICES);
    if (entry == NULL) {
        lhx_wb_log_s((STRPTR)"FindDosEntry LHA", (STRPTR)"not found");
    } else {
        bname = (STRPTR)BADDR(entry->dol_Name);
        nbuf[0] = '\0';
        if (bname != NULL) {
            n = (LONG)((UBYTE *)bname)[0];
            if (n > 60) {
                n = 60;
            }
            for (i = 0; i < n; i++) {
                nbuf[i] = bname[i + 1];
            }
            nbuf[n] = '\0';
        }
        lhx_wb_log_s((STRPTR)"FindDosEntry LHA name", (STRPTR)nbuf);
        lhx_wb_log_l((STRPTR)"dol_Type", (LONG)entry->dol_Type);
        lhx_wb_log_l((STRPTR)"dol_Task", (LONG)entry->dol_Task);
        if (entry->dol_Task != NULL) {
            found = 1;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    lhx_wb_log_l((STRPTR)"LHA device available", (LONG)found);
    return found;
}

/*
 * Build a full Amiga path from one WBArg (wa_Lock + wa_Name).
 * Does not UnLock wa_Lock - it belongs to Workbench.
 */
static int lhx_wbarg_to_path(struct WBArg *arg, STRPTR out, LONG outlen)
{
    if (arg == NULL || out == NULL || outlen <= 1) {
        return 0;
    }
    out[0] = '\0';

    lhx_wb_log_l((STRPTR)"wbarg wa_Lock", (LONG)arg->wa_Lock);
    lhx_wb_log_s((STRPTR)"wbarg wa_Name",
        arg->wa_Name != NULL ? arg->wa_Name : (STRPTR)"(null)");

    if (arg->wa_Lock == ZERO) {
        if (arg->wa_Name == NULL || arg->wa_Name[0] == '\0') {
            lhx_wb_log_s((STRPTR)"wbarg", (STRPTR)"no lock and no name");
            return 0;
        }
        lhx_copy_str(out, (const char *)arg->wa_Name, outlen);
        lhx_wb_log_s((STRPTR)"wbarg path (name only)", out);
        return out[0] != '\0';
    }

    if (!NameFromLock(arg->wa_Lock, out, outlen)) {
        lhx_wb_log_l((STRPTR)"NameFromLock fail IoErr", IoErr());
        return 0;
    }
    lhx_wb_log_s((STRPTR)"NameFromLock", out);

    /* Drawer/disk icon: lock alone is the path. */
    if (arg->wa_Name == NULL || arg->wa_Name[0] == '\0') {
        return 1;
    }

    /* NameFromLock + AddPart handles trailing ':' / '/' correctly. */
    if (!AddPart(out, arg->wa_Name, (ULONG)outlen)) {
        lhx_wb_log_l((STRPTR)"AddPart fail IoErr", IoErr());
        return 0;
    }
    lhx_wb_log_s((STRPTR)"wbarg full path", out);
    return 1;
}

/*
 * Map a real Amiga path onto the LHA: overlay.
 *   Work:t/foo.lha  -> LHA:Work/t/foo.lha
 *   RAM:bar.lha     -> LHA:RAM/bar.lha
 * Paths that are already LHA:... are copied unchanged.
 */
int lhx_real_to_lha_path(STRPTR real, STRPTR out, LONG outlen)
{
    LONG i;
    LONG j;

    if (real == NULL || out == NULL || outlen < 6) {
        return 0;
    }
    if (real[0] == '\0') {
        return 0;
    }
    if (lhx_path_is_lha(real)) {
        lhx_copy_str(out, (const char *)real, outlen);
        return 1;
    }

    for (i = 0; real[i] != '\0' && real[i] != ':'; i++) {
        ;
    }
    if (real[i] != ':' || i == 0) {
        return 0;
    }

    if (4 + i + 1 >= outlen) {
        return 0;
    }
    out[0] = 'L';
    out[1] = 'H';
    out[2] = 'A';
    out[3] = ':';
    for (j = 0; j < i; j++) {
        out[4 + j] = real[j];
    }
    j = 4 + i;

    i++;
    if (real[i] != '\0') {
        if (j + 1 >= outlen) {
            return 0;
        }
        out[j] = '/';
        j++;
        while (real[i] != '\0') {
            if (real[i] == ':') {
                return 0;
            }
            if (j + 1 >= outlen) {
                return 0;
            }
            out[j] = real[i];
            j++;
            i++;
        }
    }
    out[j] = '\0';
    return 1;
}

/*
 * Strip trailing .lha / .lzh / .lzs (case-insensitive) into out.
 */
static void lhx_archive_stem(STRPTR path, STRPTR out, LONG outlen)
{
    STRPTR base;
    LONG n;
    LONG i;

    out[0] = '\0';
    if (path == NULL || outlen < 2) {
        return;
    }
    base = FilePart(path);
    if (base == NULL || base[0] == '\0') {
        base = path;
    }
    lhx_copy_str(out, (const char *)base, outlen);
    n = 0;
    while (out[n] != '\0') {
        n++;
    }
    if (n >= 4) {
        i = n - 4;
        if (out[i] == '.'
            && (out[i + 1] == 'l' || out[i + 1] == 'L')
            && (out[i + 2] == 'h' || out[i + 2] == 'H'
                || out[i + 2] == 'z' || out[i + 2] == 'Z')
            && (out[i + 3] == 'a' || out[i + 3] == 'A'
                || out[i + 3] == 'h' || out[i + 3] == 'H'
                || out[i + 3] == 's' || out[i + 3] == 'S')) {
            out[i] = '\0';
        }
    }
    if (out[0] == '\0') {
        lhx_copy_str(out, "archive", outlen);
    }
}

static LONG lhx_wb_open_lha_path(STRPTR lha_path)
{
    LONG ok;

    if (lha_path == NULL || lha_path[0] == '\0') {
        return RETURN_FAIL;
    }

    ok = OpenWorkbenchObject(lha_path,
        WBOPENA_Show, (ULONG)DDFLAGS_SHOWALL,
        TAG_DONE);
    if (!ok) {
        lhx_dbg_s((STRPTR)"OpenWorkbenchObject fail", lha_path);
        lhx_dbg_l((STRPTR)"IoErr", IoErr());
        return RETURN_WARN;
    }
    lhx_dbg_s((STRPTR)"opened", lha_path);
    return RETURN_OK;
}

static int lhx_fib_is_file(struct FileInfoBlock *fib)
{
    if (fib == NULL) {
        return 0;
    }
    if (fib->fib_DirEntryType > 0) {
        return 0;
    }
    return 1;
}

static int lhx_wb_gauge_hit(struct Gauge *gauge)
{
    LONG stop;

    stop = 0;
    if (gauge == NULL) {
        return 0;
    }
    GetGauge(gauge, GAUGE_Hit, &stop, TAG_DONE);
    return stop != 0;
}

static void lhx_wb_gauge_fill(struct Gauge *gauge, LONG done, LONG total,
    STRPTR entry_name)
{
    LONG pct;
    LONG stop;
    char title[96];
    LONG n;
    LONG i;
    STRPTR leaf;

    if (gauge == NULL) {
        return;
    }
    if (total < 1) {
        total = 1;
    }
    pct = (done * 100L) / total;
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }

    /* Update title with current member so a fast fill still looks alive. */
    title[0] = '\0';
    if (entry_name != NULL && entry_name[0] != '\0') {
        leaf = FilePart(entry_name);
        if (leaf == NULL || leaf[0] == '\0') {
            leaf = entry_name;
        }
        n = 0;
        lhx_wb_append_str(title, &n, (LONG)sizeof(title), "Extracting \"");
        for (i = 0; leaf[i] != '\0' && n + 2 < (LONG)sizeof(title); i++) {
            title[n++] = leaf[i];
        }
        if (n + 1 < (LONG)sizeof(title)) {
            title[n++] = '"';
        }
        title[n] = '\0';
        SetGauge(gauge,
            GAUGE_Title, (ULONG)title,
            GAUGE_Fill, pct,
            TAG_DONE);
    } else {
        SetGauge(gauge, GAUGE_Fill, pct, TAG_DONE);
    }

    /* Drain Stop hits / refresh; brief yield so the bar can paint. */
    stop = 0;
    GetGauge(gauge, GAUGE_Hit, &stop, TAG_DONE);
    Delay(1L);
}

/*
 * Recursively count regular file entries for the progress denominator.
 */
static LONG lhx_wb_count_dir(struct LhArchive *arc, STRPTR dirpath)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG count;
    LONG ok;
    char childpath[512];
    LONG dlen;
    LONG nlen;

    count = 0;
    lock = LhLock(arc, dirpath != NULL ? dirpath : (STRPTR)"");
    if (lock == ZERO) {
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhUnLock(lock);
        return 0;
    }
    if (!LhExamine(lock, fib)) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhUnLock(lock);
        return 0;
    }
    ok = LhExNext(lock, fib);
    while (ok) {
        dlen = 0;
        if (dirpath != NULL && dirpath[0] != '\0') {
            while (dirpath[dlen] != '\0' && dlen < 500) {
                childpath[dlen] = dirpath[dlen];
                dlen++;
            }
            childpath[dlen++] = '/';
        }
        nlen = 0;
        while (fib->fib_FileName[nlen] != '\0' && dlen + nlen < 511) {
            childpath[dlen + nlen] = fib->fib_FileName[nlen];
            nlen++;
        }
        childpath[dlen + nlen] = '\0';

        if (!lhx_fib_is_file(fib)) {
            count += lhx_wb_count_dir(arc, (STRPTR)childpath);
        } else {
            count++;
        }
        ok = LhExNext(lock, fib);
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhUnLock(lock);
    return count;
}

struct lhx_wb_extract_ctx {
    struct Gauge *gauge;
    STRPTR dest;
    LONG total;
    LONG done;
    LONG stop;
    LONG fail;
};

/*
 * Extract one file entry under dest, update Gauge, honour Stop.
 */
static LONG lhx_wb_extract_one(struct LhArchive *arc, STRPTR entry_name,
    struct lhx_wb_extract_ctx *ctx)
{
    char outpath[LHX_PATH_LEN];

    if (ctx->stop) {
        return RETURN_WARN;
    }
    if (lhx_wb_gauge_hit(ctx->gauge)) {
        ctx->stop = 1;
        return RETURN_WARN;
    }
    if (!lhx_path_join((STRPTR)outpath, (LONG)sizeof(outpath),
            ctx->dest, entry_name)) {
        ctx->fail++;
        return RETURN_OK;
    }
    if (!lhx_ensure_parent_dir((STRPTR)outpath)) {
        ctx->fail++;
        return RETURN_OK;
    }
    if (!LhExtractEntry(arc, entry_name, (STRPTR)outpath)) {
        lhx_dbg_s((STRPTR)"extract fail", entry_name);
        ctx->fail++;
        return RETURN_OK;
    }
    ctx->done++;
    lhx_wb_gauge_fill(ctx->gauge, ctx->done, ctx->total, entry_name);
    if (lhx_wb_gauge_hit(ctx->gauge)) {
        ctx->stop = 1;
        return RETURN_WARN;
    }
    return RETURN_OK;
}

static LONG lhx_wb_extract_dir(struct LhArchive *arc, STRPTR dirpath,
    struct lhx_wb_extract_ctx *ctx)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG walkrc;
    LONG rc;
    char childpath[512];
    LONG dlen;
    LONG nlen;

    if (ctx->stop) {
        return RETURN_WARN;
    }
    lock = LhLock(arc, dirpath != NULL ? dirpath : (STRPTR)"");
    if (lock == ZERO) {
        return RETURN_FAIL;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhUnLock(lock);
        return RETURN_FAIL;
    }
    if (!LhExamine(lock, fib)) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhUnLock(lock);
        return RETURN_OK;
    }
    rc = RETURN_OK;
    ok = LhExNext(lock, fib);
    while (ok) {
        dlen = 0;
        if (dirpath != NULL && dirpath[0] != '\0') {
            while (dirpath[dlen] != '\0' && dlen < 500) {
                childpath[dlen] = dirpath[dlen];
                dlen++;
            }
            childpath[dlen++] = '/';
        }
        nlen = 0;
        while (fib->fib_FileName[nlen] != '\0' && dlen + nlen < 511) {
            childpath[dlen + nlen] = fib->fib_FileName[nlen];
            nlen++;
        }
        childpath[dlen + nlen] = '\0';

        if (!lhx_fib_is_file(fib)) {
            walkrc = lhx_wb_extract_dir(arc, (STRPTR)childpath, ctx);
            if (walkrc == RETURN_WARN) {
                rc = RETURN_WARN;
                break;
            }
            if (walkrc != RETURN_OK) {
                rc = RETURN_ERROR;
            }
        } else {
            walkrc = lhx_wb_extract_one(arc, (STRPTR)childpath, ctx);
            if (walkrc == RETURN_WARN) {
                rc = RETURN_WARN;
                break;
            }
        }
        if (lhx_check_break()) {
            ctx->stop = 1;
            rc = RETURN_WARN;
            break;
        }
        ok = LhExNext(lock, fib);
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhUnLock(lock);
    return rc;
}

/*
 * Recursively delete a file or drawer (AmigaDOS DeleteFile needs empty dirs).
 * Used to scrub T:LhX/<stem>/ after a failed or aborted unpack.
 */
static void lhx_wb_rmtree(STRPTR path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    char child[LHX_PATH_LEN];
    LONG ok;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    lock = Lock(path, ACCESS_READ);
    if (lock == ZERO) {
        DeleteFile(path);
        return;
    }

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        DeleteFile(path);
        return;
    }

    if (!Examine(lock, fib)) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        UnLock(lock);
        DeleteFile(path);
        return;
    }

    /* Directory: remove children first. */
    if (fib->fib_DirEntryType > 0) {
        ok = ExNext(lock, fib);
        while (ok) {
            if (lhx_path_join((STRPTR)child, (LONG)sizeof(child), path,
                    fib->fib_FileName)) {
                lhx_wb_rmtree((STRPTR)child);
            }
            ok = ExNext(lock, fib);
        }
    }

    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    DeleteFile(path);
}

/*
 * Extract archive into T:LhX/<stem>/ with a Gauge progress requester.
 * On success dest_out is the drawer path (caller opens it in Workbench).
 * On failure or Stop, the drawer is removed, dest_out is cleared, and
 * an EasyRequest explains why (Gauge has no error text area).
 */
static LONG lhx_wb_extract_to_temp(STRPTR real_path, STRPTR dest_out,
    LONG dest_len)
{
    struct Library *lh_opened;
    struct IntuitionBase *intuition_opened;
    struct GfxBase *gfx_opened;
    struct Gauge *gauge;
    struct LhArchive *arc;
    BPTR lock;
    char stem[128];
    char title[96];
    char leaf[80];
    char dest_path[LHX_PATH_LEN];
    LONG total;
    LONG rc;
    LONG dest_ready;
    LONG why;
    LONG errcode;
    struct lhx_wb_extract_ctx ctx;
    LONG i;
    LONG n;
    STRPTR fp;

    if (dest_out == NULL || dest_len < 8) {
        return RETURN_FAIL;
    }
    dest_out[0] = '\0';
    dest_path[0] = '\0';

    lhx_wb_log_s((STRPTR)"extract begin", real_path);

    lh_opened = NULL;
    intuition_opened = NULL;
    gfx_opened = NULL;
    gauge = NULL;
    arc = NULL;
    dest_ready = 0;
    why = LHX_WB_ERR_LIBS;
    errcode = 0;
    rc = RETURN_FAIL;

    if (LhBase == NULL) {
        lh_opened = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
        LhBase = lh_opened;
        lhx_wb_log_l((STRPTR)"OpenLibrary lh.library", (LONG)LhBase);
        if (LhBase == NULL) {
            lhx_wb_log_l((STRPTR)"lh.library IoErr", IoErr());
            if (IntuitionBase == NULL) {
                intuition_opened = (struct IntuitionBase *)OpenLibrary(
                    (STRPTR)"intuition.library", 37L);
                IntuitionBase = intuition_opened;
            }
            errcode = IoErr();
            lhx_wb_alert(LHX_WB_ERR_LIBS, real_path, errcode);
            if (intuition_opened != NULL) {
                CloseLibrary((struct Library *)intuition_opened);
                IntuitionBase = NULL;
            }
            return RETURN_FAIL;
        }
    } else {
        lhx_wb_log_l((STRPTR)"LhBase already open", (LONG)LhBase);
    }

    if (IntuitionBase == NULL) {
        intuition_opened = (struct IntuitionBase *)OpenLibrary(
            (STRPTR)"intuition.library", 37L);
        IntuitionBase = intuition_opened;
    }
    if (GfxBase == NULL) {
        gfx_opened = (struct GfxBase *)OpenLibrary(
            (STRPTR)"graphics.library", 37L);
        GfxBase = gfx_opened;
    }
    lhx_wb_log_ll((STRPTR)"Intuition/Gfx", (LONG)IntuitionBase, (LONG)GfxBase);
    lhx_wb_log_l((STRPTR)"UtilityBase", (LONG)UtilityBase);
    if (IntuitionBase == NULL || GfxBase == NULL || UtilityBase == NULL) {
        why = LHX_WB_ERR_LIBS;
        goto close_libs;
    }

    lhx_archive_stem(real_path, (STRPTR)stem, (LONG)sizeof(stem));
    lhx_wb_log_s((STRPTR)"stem", (STRPTR)stem);
    if (!lhx_path_join((STRPTR)dest_path, (LONG)sizeof(dest_path),
            (STRPTR)"T:LhX", (STRPTR)stem)) {
        why = LHX_WB_ERR_DEST;
        lhx_wb_log_s((STRPTR)"path_join dest fail", (STRPTR)stem);
        goto close_libs;
    }
    lhx_wb_log_s((STRPTR)"dest_path", (STRPTR)dest_path);
    lock = CreateDir((STRPTR)"T:LhX");
    if (lock != ZERO) {
        UnLock(lock);
    }
    if (!lhx_ensure_parent_dir((STRPTR)dest_path)) {
        why = LHX_WB_ERR_DEST;
        errcode = IoErr();
        lhx_wb_log_l((STRPTR)"ensure_parent fail IoErr", errcode);
        goto close_libs;
    }
    lock = CreateDir((STRPTR)dest_path);
    if (lock != ZERO) {
        UnLock(lock);
    }
    dest_ready = 1;

    /*
     * Open the archive before the Gauge so a bad path fails cleanly.
     * With CD set to the project drawer by the caller, also try the leaf
     * name if the absolute path does not open.
     */
    {
        BPTR probe;

        probe = Lock(real_path, ACCESS_READ);
        lhx_wb_log_l((STRPTR)"Lock(real_path)", (LONG)probe);
        if (probe == ZERO) {
            lhx_wb_log_l((STRPTR)"Lock real IoErr", IoErr());
        } else {
            UnLock(probe);
        }
    }

    arc = LhOpenArchive(real_path, LHARC_MODE_READ);
    lhx_wb_log_l((STRPTR)"LhOpenArchive(real)", (LONG)arc);
    if (arc == NULL) {
        lhx_wb_log_l((STRPTR)"LhErr after real", LhErr());
        lhx_wb_log_l((STRPTR)"IoErr after real", IoErr());
        fp = FilePart(real_path);
        lhx_wb_log_s((STRPTR)"retry leaf",
            fp != NULL ? fp : (STRPTR)"(null)");
        if (fp != NULL && fp[0] != '\0' && fp != real_path) {
            {
                BPTR probe2;

                probe2 = Lock(fp, ACCESS_READ);
                lhx_wb_log_l((STRPTR)"Lock(leaf)", (LONG)probe2);
                if (probe2 == ZERO) {
                    lhx_wb_log_l((STRPTR)"Lock leaf IoErr", IoErr());
                } else {
                    UnLock(probe2);
                }
            }
            arc = LhOpenArchive(fp, LHARC_MODE_READ);
            lhx_wb_log_l((STRPTR)"LhOpenArchive(leaf)", (LONG)arc);
            if (arc == NULL) {
                lhx_wb_log_l((STRPTR)"LhErr after leaf", LhErr());
                lhx_wb_log_l((STRPTR)"IoErr after leaf", IoErr());
            }
        }
    }
    if (arc == NULL) {
        why = LHX_WB_ERR_OPEN;
        errcode = LhErr();
        if (errcode == 0) {
            errcode = IoErr();
        }
        goto close_libs;
    }

    lhx_wb_log_s((STRPTR)"archive open ok", real_path);

    /* Style Guide title: Extracting "name.lha" */
    leaf[0] = '\0';
    fp = FilePart(real_path);
    if (fp != NULL && fp[0] != '\0') {
        lhx_copy_str((STRPTR)leaf, (const char *)fp, (LONG)sizeof(leaf));
    } else {
        lhx_copy_str((STRPTR)leaf, (const char *)stem, (LONG)sizeof(leaf));
    }
    n = 0;
    lhx_copy_str((STRPTR)title, "Extracting \"", (LONG)sizeof(title));
    while (title[n] != '\0') {
        n++;
    }
    for (i = 0; leaf[i] != '\0' && n + 2 < (LONG)sizeof(title); i++) {
        title[n++] = leaf[i];
    }
    if (n + 2 < (LONG)sizeof(title)) {
        title[n++] = '"';
        title[n] = '\0';
    }

    gauge = NewGauge(
        GAUGE_Title, (ULONG)title,
        GAUGE_Fill, 0L,
        TAG_DONE);
    lhx_wb_log_l((STRPTR)"NewGauge", (LONG)gauge);
    if (gauge == NULL) {
        why = LHX_WB_ERR_GAUGE;
        LhCloseArchive(arc);
        arc = NULL;
        goto close_libs;
    }

    total = lhx_wb_count_dir(arc, (STRPTR)"");
    lhx_wb_log_l((STRPTR)"file count", total);
    if (total < 1) {
        total = 1;
    }
    ctx.gauge = gauge;
    ctx.dest = (STRPTR)dest_path;
    ctx.total = total;
    ctx.done = 0;
    ctx.stop = 0;
    ctx.fail = 0;

    lhx_wb_gauge_fill(gauge, 0, total, NULL);
    Delay(TICKS_PER_SECOND / 10);
    rc = lhx_wb_extract_dir(arc, (STRPTR)"", &ctx);
    LhCloseArchive(arc);
    arc = NULL;

    if (ctx.stop) {
        rc = RETURN_WARN;
        why = LHX_WB_ERR_STOP;
    } else if (ctx.fail > 0 || rc != RETURN_OK) {
        rc = RETURN_WARN;
        why = LHX_WB_ERR_EXTRACT;
        errcode = LhErr();
    } else {
        lhx_wb_gauge_fill(gauge, total, total, NULL);
        Delay(TICKS_PER_SECOND / 5);
        why = LHX_WB_OK;
        rc = RETURN_OK;
    }
    lhx_wb_log_ll((STRPTR)"extract done why/rc", why, rc);
    lhx_wb_log_ll((STRPTR)"extract done done/fail", ctx.done, ctx.fail);

dispose_gauge:
    if (gauge != NULL) {
        DisposeGauge(gauge);
        gauge = NULL;
    }

close_libs:
    lhx_wb_log_ll((STRPTR)"extract exit why/rc", why, rc);
    if (rc == RETURN_OK && dest_ready) {
        lhx_copy_str(dest_out, (const char *)dest_path, dest_len);
        lhx_wb_log_s((STRPTR)"extract dest_out", dest_out);
    } else if (dest_ready) {
        /* Failed or aborted: scrub the partial T:LhX/<stem>/ tree. */
        lhx_wb_log_s((STRPTR)"cleanup T:", (STRPTR)dest_path);
        lhx_dbg_s((STRPTR)"cleanup T:", (STRPTR)dest_path);
        lhx_wb_rmtree((STRPTR)dest_path);
        dest_out[0] = '\0';
    } else {
        lhx_wb_log_s((STRPTR)"extract dest_out", (STRPTR)"(none)");
    }

    if (why != LHX_WB_OK) {
        lhx_wb_log_l((STRPTR)"EasyRequest why", why);
        lhx_wb_log_l((STRPTR)"EasyRequest errcode", errcode);
        lhx_wb_alert(why, real_path, errcode);
    }

    if (gfx_opened != NULL) {
        CloseLibrary((struct Library *)gfx_opened);
        GfxBase = NULL;
    }
    if (intuition_opened != NULL) {
        CloseLibrary((struct Library *)intuition_opened);
        IntuitionBase = NULL;
    }
    if (lh_opened != NULL) {
        CloseLibrary(lh_opened);
        LhBase = NULL;
    }
    return rc;
}

/*
 * Workbench entry: sm_ArgList[0] is LhX; following args are projects.
 */
LONG lhx_wb_startup(struct WBStartup *wb)
{
    struct WBArg *arg;
    LONG i;
    LONG n;
    LONG opened;
    LONG rc;
    LONG one;
    int use_lha;
    static char real_path[LHX_PATH_LEN];
    static char lha_path[LHX_PATH_LEN];
    static char dest_path[LHX_PATH_LEN];

    lhx_wb_log_open();

    if (wb == NULL || wb->sm_ArgList == NULL || wb->sm_NumArgs < 1) {
        lhx_wb_log_s((STRPTR)"startup", (STRPTR)"bad WBStartup");
        lhx_wb_log_close();
        return RETURN_FAIL;
    }

    lhx_wb_log_l((STRPTR)"sm_NumArgs", (LONG)wb->sm_NumArgs);

    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 44L);
    if (WorkbenchBase == NULL) {
        lhx_wb_log_s((STRPTR)"startup", (STRPTR)"no workbench.library");
        lhx_wb_log_close();
        return RETURN_FAIL;
    }

    use_lha = lhx_lha_device_available();
    lhx_dbg_l((STRPTR)"LHA: available", (LONG)use_lha);
    lhx_wb_log_l((STRPTR)"use_lha", (LONG)use_lha);

    opened = 0;
    rc = RETURN_OK;
    n = wb->sm_NumArgs;
    arg = wb->sm_ArgList;

    for (i = 1; i < n; i++) {
        lhx_wb_log_l((STRPTR)"--- arg index", i);
        if (!lhx_wbarg_to_path(&arg[i], (STRPTR)real_path, LHX_PATH_LEN)) {
            lhx_wb_log_s((STRPTR)"arg", (STRPTR)"path build failed, skip");
            continue;
        }
        if (arg[i].wa_Name == NULL || arg[i].wa_Name[0] == '\0') {
            lhx_wb_log_s((STRPTR)"arg", (STRPTR)"drawer/disk, skip");
            continue;
        }

        one = RETURN_FAIL;
        if (use_lha) {
            if (!lhx_real_to_lha_path((STRPTR)real_path, (STRPTR)lha_path,
                    LHX_PATH_LEN)) {
                lhx_wb_log_s((STRPTR)"real_to_lha", (STRPTR)"failed");
                rc = RETURN_FAIL;
                continue;
            }
            lhx_dbg_s((STRPTR)"wb real", (STRPTR)real_path);
            lhx_dbg_s((STRPTR)"wb lha", (STRPTR)lha_path);
            lhx_wb_log_s((STRPTR)"OpenWorkbenchObject LHA", (STRPTR)lha_path);
            one = lhx_wb_open_lha_path((STRPTR)lha_path);
            lhx_wb_log_l((STRPTR)"OWO LHA rc", one);
        }

        /*
         * No live LHA: device, or overlay open failed: unpack to T: and
         * open that drawer.  CD into the project drawer so relative opens
         * still work if NameFromLock was incomplete.
         */
        if (one != RETURN_OK) {
            BPTR oldcd;
            BPTR dlock;

            oldcd = ZERO;
            dlock = ZERO;
            if (arg[i].wa_Lock != ZERO) {
                dlock = DupLock(arg[i].wa_Lock);
                if (dlock != ZERO) {
                    oldcd = CurrentDir(dlock);
                }
            }
            lhx_wb_log_l((STRPTR)"CurrentDir project", (LONG)dlock);

            lhx_dbg_s((STRPTR)"wb extract", (STRPTR)real_path);
            lhx_wb_log_s((STRPTR)"calling extract_to_temp", (STRPTR)real_path);
            one = lhx_wb_extract_to_temp((STRPTR)real_path,
                (STRPTR)dest_path, LHX_PATH_LEN);
            lhx_wb_log_l((STRPTR)"extract_to_temp rc", one);
            lhx_wb_log_s((STRPTR)"dest_path after", (STRPTR)dest_path);

            if (dlock != ZERO) {
                dlock = CurrentDir(oldcd);
                if (dlock != ZERO) {
                    UnLock(dlock);
                }
            }

            if (one == RETURN_OK && dest_path[0] != '\0') {
                if (!OpenWorkbenchObject(dest_path,
                        WBOPENA_Show, (ULONG)DDFLAGS_SHOWALL,
                        TAG_DONE)) {
                    lhx_dbg_s((STRPTR)"open T: drawer fail", dest_path);
                    lhx_dbg_l((STRPTR)"IoErr", IoErr());
                    lhx_wb_log_l((STRPTR)"OWO T: fail IoErr", IoErr());
                    one = RETURN_WARN;
                } else {
                    lhx_dbg_s((STRPTR)"opened T:", dest_path);
                    lhx_wb_log_s((STRPTR)"OWO T: ok", dest_path);
                }
            }
        }

        if (one == RETURN_OK) {
            opened++;
        } else if (rc == RETURN_OK) {
            rc = one;
        }
    }

    CloseLibrary(WorkbenchBase);
    WorkbenchBase = NULL;

    lhx_wb_log_ll((STRPTR)"startup opened/rc", opened, rc);
    lhx_wb_log_close();

    if (opened == 0) {
        if (n < 2) {
            return RETURN_WARN;
        }
        return rc != RETURN_OK ? rc : RETURN_FAIL;
    }
    return RETURN_OK;
}
