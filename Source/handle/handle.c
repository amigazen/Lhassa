/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle.c - Handle: create a Bin-profile LHA from a stage directory.
 *
 * LHA = Load / Handle / Archive.  FROM named SYS => system package mode;
 * otherwise application package mode.  Output name-version.lha from NAME,
 * PRIMARY ($VER), and optional VERSION= override.
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

#include "handle_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

struct Library *LhBase = NULL;

static const char *verstag = "$VER: Handle 1.1 (23.07.2026)\n";
static const char *stack_cookie = "$STACK: 40960\n";
const long oslibversion = 37L;

#define HANDLE_NARGS 8

static const char *TEMPLATE =
    "FROM/A,"
    "NAME/A,"
    "PRIMARY/A,"
    "VERSION/K,"
    "OUT/K,"
    "FORCE/S,"
    "QUIET/S,"
    "HELP/S";

static void handle_set_rc(LONG code)
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

static void handle_args_from_rd(struct HandleArgs *out, LONG *rd)
{
    out->from = (STRPTR)rd[0];
    out->name = (STRPTR)rd[1];
    out->primary = (STRPTR)rd[2];
    out->version = (STRPTR)rd[3];
    out->out = (STRPTR)rd[4];
    out->force = (ULONG)rd[5];
    out->quiet = (ULONG)rd[6];
    out->help = (ULONG)rd[7];
    out->from_path[0] = '\0';
    out->out_path[0] = '\0';
    out->name_buf[0] = '\0';
    out->primary_buf[0] = '\0';
    out->version_buf[0] = '\0';
    out->out_dir[0] = '\0';
    out->sys_mode = 0;
}

static void handle_stabilize_args(struct HandleArgs *args)
{
    if (args->from && args->from[0]) {
        Strncpy((STRPTR)args->from_path, args->from, HANDLE_PATH_LEN);
        args->from = (STRPTR)args->from_path;
        args->sys_mode = handle_detect_sys_mode(args->from);
    } else {
        args->from = NULL;
    }
    if (args->name && args->name[0]) {
        Strncpy((STRPTR)args->name_buf, args->name, HANDLE_VAL_LEN);
        args->name = (STRPTR)args->name_buf;
    } else {
        args->name = NULL;
    }
    if (args->primary && args->primary[0]) {
        Strncpy((STRPTR)args->primary_buf, args->primary, HANDLE_PATH_LEN);
        args->primary = (STRPTR)args->primary_buf;
    } else {
        args->primary = NULL;
    }
    if (args->version && args->version[0]) {
        Strncpy((STRPTR)args->version_buf, args->version,
            (LONG)sizeof(args->version_buf));
        args->version = (STRPTR)args->version_buf;
    } else {
        args->version = NULL;
    }
    if (args->out && args->out[0]) {
        Strncpy((STRPTR)args->out_dir, args->out, HANDLE_PATH_LEN);
        args->out = (STRPTR)args->out_dir;
    } else {
        args->out = NULL;
    }
}

static int handle_open_libs(void)
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
    struct HandleArgs args;
    LONG rdargs[HANDLE_NARGS];
    LONG rc;
    LONG i;

    (void)argc;
    (void)argv;
    (void)verstag;
    (void)stack_cookie;

    if (!handle_open_libs()) {
        if (DOSBase) {
            handle_print_error((STRPTR)"cannot open utility.library", 0);
        }
        return RETURN_FAIL;
    }

    for (i = 0; i < HANDLE_NARGS; i++) {
        rdargs[i] = 0;
    }

    rda = ReadArgs((STRPTR)TEMPLATE, rdargs, NULL);
    if (!rda) {
        LONG err;

        err = IoErr();
        if (err != 0) {
            PrintFault(err, (STRPTR)"Handle");
        } else {
            handle_print_usage();
        }
        handle_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    handle_args_from_rd(&args, rdargs);

    if (args.help) {
        handle_print_usage();
        FreeArgs(rda);
        handle_set_rc(RETURN_OK);
        return RETURN_OK;
    }

    if (!args.from || !args.from[0] || !args.name || !args.name[0]
        || !args.primary || !args.primary[0]) {
        handle_print_error((STRPTR)"FROM, NAME, and PRIMARY are required", 0);
        handle_print_usage();
        FreeArgs(rda);
        handle_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    handle_stabilize_args(&args);
    FreeArgs(rda);
    rda = NULL;

    LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
    if (!LhBase) {
        handle_print_error((STRPTR)"cannot open lh.library", 0);
        handle_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    rc = handle_run(&args);

    CloseLibrary(LhBase);
    LhBase = NULL;
    handle_set_rc(rc);
    return (int)rc;
}
