/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx_wb.c - Workbench startup: open the project archive via LHA:.
 *
 * When LhX is the default tool of a .lha project icon, WB passes the
 * archive as WBArg[1].  Remap Vol:path/file.lha -> LHA:Vol/path/file.lha
 * and OpenWorkbenchObject() so the overlay presents it as a drawer.
 * One OpenWorkbenchObject call per project (the LHA: archive path only).
 * See workbench.library/OpenWorkbenchObjectA.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/wb.h>

#include "lhx_internal.h"

#ifndef ZERO
#define ZERO ((BPTR)0L)
#endif

/* SAS/C workbench stubs expect this name. */
struct Library *WorkbenchBase = NULL;

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
 * Build a full Amiga path from one WBArg (wa_Lock + wa_Name).
 * Does not UnLock wa_Lock - it belongs to Workbench.
 */
static int lhx_wbarg_to_path(struct WBArg *arg, STRPTR out, LONG outlen)
{
    LONG n;
    LONG i;

    if (arg == NULL || out == NULL || outlen <= 1) {
        return 0;
    }
    out[0] = '\0';

    if (arg->wa_Lock == ZERO) {
        if (arg->wa_Name == NULL || arg->wa_Name[0] == '\0') {
            return 0;
        }
        lhx_copy_str(out, (const char *)arg->wa_Name, outlen);
        return out[0] != '\0';
    }

    if (!NameFromLock(arg->wa_Lock, out, outlen)) {
        return 0;
    }

    /* Drawer/disk icon: lock alone is the path. */
    if (arg->wa_Name == NULL || arg->wa_Name[0] == '\0') {
        return 1;
    }

    n = 0;
    while (out[n] != '\0') {
        n++;
    }
    if (n > 0 && out[n - 1] != ':' && out[n - 1] != '/') {
        if (n + 1 >= outlen) {
            return 0;
        }
        out[n] = '/';
        n++;
        out[n] = '\0';
    }
    for (i = 0; arg->wa_Name[i] != '\0'; i++) {
        if (n + 1 >= outlen) {
            return 0;
        }
        out[n] = arg->wa_Name[i];
        n++;
    }
    out[n] = '\0';
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

    /* Find volume colon. */
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

    /* Skip ':' then append '/' + remainder when non-empty. */
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

#ifndef WBOPENA_Show
#define WBOPENA_Show (TAG_USER + 0x4B)
#endif
#ifndef DDFLAGS_SHOWALL
#define DDFLAGS_SHOWALL 2
#endif

/*
 * Open one LHA: archive path in Workbench - a single OpenWorkbenchObject
 * on the final path (not LHA: root / parents).
 * Always request Show All so iconless archive members are visible
 * (WBOPENA_Show / DDFLAGS_SHOWALL, V45+).
 */
static LONG lhx_wb_open_lha_path(STRPTR lha_path, STRPTR real_path)
{
    LONG ok;

    (void)real_path;
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

/*
 * Workbench entry: sm_ArgList[0] is LhX; following args are projects.
 * Remap each file project to LHA: and open it as a drawer.
 */
LONG lhx_wb_startup(struct WBStartup *wb)
{
    struct WBArg *arg;
    LONG i;
    LONG n;
    LONG opened;
    LONG rc;
    LONG one;
    static char real_path[LHX_PATH_LEN];
    static char lha_path[LHX_PATH_LEN];

    if (wb == NULL || wb->sm_ArgList == NULL || wb->sm_NumArgs < 1) {
        return RETURN_FAIL;
    }

    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 44L);
    if (WorkbenchBase == NULL) {
        /* No console when started from WB; fail quietly. */
        return RETURN_FAIL;
    }

    opened = 0;
    rc = RETURN_OK;
    n = wb->sm_NumArgs;
    arg = wb->sm_ArgList;

    /*
     * Index 0 is the tool.  Projects follow (default-tool launch or
     * extended select).  Skip drawer/disk args with NULL wa_Name.
     */
    for (i = 1; i < n; i++) {
        if (!lhx_wbarg_to_path(&arg[i], (STRPTR)real_path, LHX_PATH_LEN)) {
            continue;
        }
        if (arg[i].wa_Name == NULL || arg[i].wa_Name[0] == '\0') {
            continue;
        }
        if (!lhx_real_to_lha_path((STRPTR)real_path, (STRPTR)lha_path,
            LHX_PATH_LEN)) {
            rc = RETURN_FAIL;
            continue;
        }
        lhx_dbg_s((STRPTR)"wb real", (STRPTR)real_path);
        lhx_dbg_s((STRPTR)"wb lha", (STRPTR)lha_path);
        one = lhx_wb_open_lha_path((STRPTR)lha_path, (STRPTR)real_path);
        if (one == RETURN_OK) {
            opened++;
        } else if (rc == RETURN_OK) {
            rc = one;
        }
    }

    CloseLibrary(WorkbenchBase);
    WorkbenchBase = NULL;

    if (opened == 0) {
        if (n < 2) {
            /* Tool icon alone - nothing to open. */
            return RETURN_WARN;
        }
        return rc != RETURN_OK ? rc : RETURN_FAIL;
    }
    return RETURN_OK;
}
