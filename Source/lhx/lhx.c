/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx.c - LhX: lean native Amiga LHA archiver (lh.library client).
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *UtilityBase = NULL;

#include <proto/utility.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "lhx_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

struct Library *LhBase = NULL;

static const char *verstag = "$VER: LhX 1.0 (02.07.2026)\n";
static const char *stack_cookie = "$STACK: 8096\n";
const long oslibversion = 37L;

#define LHX_NARGS 14

static const char *TEMPLATE =
    "L=LIST/S,"
    "X=EXTRACT/S,"
    "EXTRACTFLAT/S,"
    "A=ADD/S,"
    "T=TEST/S,"
    "PRINT/S,"
    "HELP/S,"
    "ARCHIVE/A,"
    "FILES/M,"
    "TO/K,"
    "QUIET/S,"
    "FORCE/S,"
    "NOPATHS/S,"
    "PASSWORD/K";

static void lhx_set_rc(LONG code)
{
    char buf[16];
    LONG n;
    LONG v;
    LONG i;
    char tmp[16];

    v = code;
    if (v < 0) {
        v = -v;
    }
    n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (LONG)sizeof(tmp) - 1) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';
    SetVar((STRPTR)"RC", (STRPTR)buf, -1, GVF_GLOBAL_ONLY);
}

static LONG lhx_count_ops(struct LhxArgs *args)
{
    LONG n;

    n = 0;
    if (args->list) {
        n++;
    }
    if (args->extract) {
        n++;
    }
    if (args->extractflat) {
        n++;
    }
    if (args->add) {
        n++;
    }
    if (args->test) {
        n++;
    }
    if (args->print) {
        n++;
    }
    return n;
}

static void lhx_args_from_rd(struct LhxArgs *out, LONG *rd)
{
    out->list = (ULONG)rd[0];
    out->extract = (ULONG)rd[1];
    out->extractflat = (ULONG)rd[2];
    out->add = (ULONG)rd[3];
    out->test = (ULONG)rd[4];
    out->print = (ULONG)rd[5];
    out->help = (ULONG)rd[6];
    out->archive = (STRPTR)rd[7];
    out->files = (STRPTR *)rd[8];
    out->destdir = (STRPTR)rd[9];
    out->quiet = (ULONG)rd[10];
    out->force = (ULONG)rd[11];
    out->nopaths = (ULONG)rd[12];
    out->password = (STRPTR)rd[13];
    out->archive_path[0] = '\0';
    out->destdir_path[0] = '\0';
    out->password_buf[0] = '\0';
}

/*
 * Copy ReadArgs-owned strings into LhxArgs storage.  The RDArgs pool must
 * not be used after FreeArgs(), so call this then FreeArgs() before lh.library.
 */
static void lhx_stabilize_args(struct LhxArgs *args)
{
    static char pattern_bufs[LHX_MAX_PATTERNS][256];
    static STRPTR pattern_ptrs[LHX_MAX_PATTERNS + 1];
    LONG i;

    args->archive_path[0] = '\0';
    args->destdir_path[0] = '\0';
    args->password_buf[0] = '\0';

    if (args->archive && args->archive[0]) {
        Strncpy((STRPTR)args->archive_path, args->archive, LHX_PATH_LEN);
        args->archive = (STRPTR)args->archive_path;
    }

    if (args->destdir && args->destdir[0]) {
        Strncpy((STRPTR)args->destdir_path, args->destdir, LHX_PATH_LEN);
        args->destdir = (STRPTR)args->destdir_path;
    }

    if (args->password && args->password[0]) {
        Strncpy((STRPTR)args->password_buf, args->password, LHX_PASS_LEN);
        args->password = (STRPTR)args->password_buf;
    }

    if (args->files) {
        for (i = 0; i < LHX_MAX_PATTERNS && args->files[i] != NULL; i++) {
            Strncpy(pattern_bufs[i], args->files[i], 256);
            pattern_ptrs[i] = (STRPTR)pattern_bufs[i];
        }
        pattern_ptrs[i] = NULL;
        args->files = pattern_ptrs;
    } else {
        args->files = NULL;
    }

    /*
     * ReadArgs can leave a lone ARCHIVE path in FILES/M; that is not a member
     * pattern and would match nothing inside the archive.
     */
    if (args->files && args->files[0] && args->files[1] == NULL) {
        if (Stricmp(args->files[0], args->archive) == 0) {
            lhx_dbg_s((STRPTR)"stabilize", (STRPTR)"FILES==ARCHIVE, clear filter");
            args->files = NULL;
        }
    }
    lhx_dbg_dump_args(args);
}

static int lhx_open_libs(void)
{
    if (!DOSBase) {
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37L);
        if (!DOSBase) {
            return 0;
        }
    }
    if (!UtilityBase) {
        UtilityBase = OpenLibrary((STRPTR)"utility.library", 37L);
        if (!UtilityBase) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[])
{
    struct RDArgs *rda;
    struct LhxArgs args;
    LONG rdargs[LHX_NARGS];
    LONG ops;
    LONG rc;
    LONG with_paths;
    LONG i;

    /* dos.library and utility.library before ReadArgs or any other API use. */
    if (!lhx_open_libs()) {
        if (DOSBase) {
            lhx_print_error((STRPTR)"cannot open utility.library", 0);
        }
        return RETURN_FAIL;
    }

    /*
     * Workbench: argc == 0 and argv is WBStartup *.  Default-tool launch
     * for a .lha project remaps the file to LHA: volume, assuming Lh-handler is mounted and calls OpenWorkbenchObject().
     */
    if (argc == 0) {
        rc = lhx_wb_startup((struct WBStartup *)argv);
        lhx_set_rc(rc);
        return (int)rc;
    }

    for (i = 0; i < LHX_NARGS; i++) {
        rdargs[i] = 0;
    }

    rda = ReadArgs(TEMPLATE, rdargs, NULL);
    if (!rda) {
        LONG err;

        err = IoErr();
        if (err != 0) {
            PrintFault(err, (STRPTR)"LhX");
        } else {
            lhx_print_usage();
        }
        lhx_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    lhx_args_from_rd(&args, rdargs);

    if (args.help) {
        lhx_print_usage();
        FreeArgs(rda);
        lhx_set_rc(RETURN_OK);
        return RETURN_OK;
    }

    if (!args.archive || !args.archive[0]) {
        lhx_print_error((STRPTR)"ARCHIVE path required", 0);
        lhx_print_usage();
        FreeArgs(rda);
        lhx_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    ops = lhx_count_ops(&args);
    if (ops != 1) {
        lhx_print_error((STRPTR)"specify exactly one operation", 0);
        lhx_print_usage();
        FreeArgs(rda);
        lhx_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    lhx_stabilize_args(&args);
    FreeArgs(rda);
    rda = NULL;

    LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
    if (!LhBase) {
        lhx_print_error((STRPTR)"cannot open lh.library", 0);
        lhx_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    rc = RETURN_OK;
    with_paths = 1;
    if (args.nopaths) {
        with_paths = 0;
    }
    if (args.extractflat) {
        with_paths = 0;
    }
    lhx_dbg_l((STRPTR)"with_paths", with_paths);

    if (args.list) {
        rc = lhx_cmd_list(&args);
    } else if (args.extract || args.extractflat) {
        rc = lhx_cmd_extract(&args, with_paths);
    } else if (args.add) {
        rc = lhx_cmd_add(&args);
    } else if (args.test) {
        rc = lhx_cmd_test(&args);
    } else if (args.print) {
        rc = lhx_cmd_print(&args);
    }

    CloseLibrary(LhBase);
    LhBase = NULL;
    lhx_set_rc(rc);
    return (int)rc;
}
