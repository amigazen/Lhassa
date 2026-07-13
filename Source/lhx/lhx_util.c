/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx_util.c - AmigaDOS / utility.library helpers for LhX.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <string.h>

#include "lhx_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

#define LHX_PATTERN_BUF 512
#define LHX_EXPAND_MAX  128
#define LHX_EXPAND_PATH 160
#ifndef ERROR_BREAK
#define ERROR_BREAK 304
#endif

static BPTR lhx_dbg_out(void)
{
    BPTR fh;

    fh = ErrorOutput();
    if (fh == (BPTR)NULL) {
        fh = Output();
    }
    return fh;
}

void lhx_dbg_s(STRPTR label, STRPTR value)
{
#if LHX_DEBUG
    BPTR fh;

    fh = lhx_dbg_out();
    Printf("LhX DBG: %s=", (LONG)label);
    if (value) {
        Printf("%s\n", (LONG)value);
    } else {
        Printf("(null)\n", 0);
    }
    Flush(fh);
#else
    (void)label;
    (void)value;
#endif
}

void lhx_dbg_l(STRPTR label, LONG value)
{
#if LHX_DEBUG
    BPTR fh;

    fh = lhx_dbg_out();
    Printf("LhX DBG: %s=%ld\n", (LONG)label, value);
    Flush(fh);
#else
    (void)label;
    (void)value;
#endif
}

void lhx_dbg_ll(STRPTR label, LONG a, LONG b)
{
#if LHX_DEBUG
    BPTR fh;

    fh = lhx_dbg_out();
    Printf("LhX DBG: %s %ld %ld\n", (LONG)label, a, b);
    Flush(fh);
#else
    (void)label;
    (void)a;
    (void)b;
#endif
}

void lhx_dbg_dump_args(struct LhxArgs *args)
{
#if LHX_DEBUG
    LONG i;

    if (!args) {
        lhx_dbg_s((STRPTR)"args", (STRPTR)"(null)");
        return;
    }
    lhx_dbg_s((STRPTR)"dump", (STRPTR)"--- args ---");
    lhx_dbg_s((STRPTR)"archive", args->archive);
    lhx_dbg_s((STRPTR)"destdir", args->destdir);
    lhx_dbg_l((STRPTR)"list", (LONG)args->list);
    lhx_dbg_l((STRPTR)"extract", (LONG)args->extract);
    lhx_dbg_l((STRPTR)"extractflat", (LONG)args->extractflat);
    lhx_dbg_l((STRPTR)"test", (LONG)args->test);
    lhx_dbg_l((STRPTR)"print", (LONG)args->print);
    lhx_dbg_l((STRPTR)"add", (LONG)args->add);
    lhx_dbg_l((STRPTR)"quiet", (LONG)args->quiet);
    lhx_dbg_l((STRPTR)"force", (LONG)args->force);
    lhx_dbg_l((STRPTR)"nopaths", (LONG)args->nopaths);
    if (!args->files) {
        lhx_dbg_s((STRPTR)"files", (STRPTR)"(null, match all)");
    } else if (!args->files[0]) {
        lhx_dbg_s((STRPTR)"files", (STRPTR)"(empty, match all)");
    } else {
        for (i = 0; args->files[i] != NULL; i++) {
            lhx_dbg_s((STRPTR)"files[]", args->files[i]);
        }
    }
    lhx_dbg_s((STRPTR)"dump", (STRPTR)"--- end ---");
#else
    (void)args;
#endif
}

void lhx_print_error(STRPTR msg, LONG code)
{
    if (!msg) {
        return;
    }
    if (code) {
        Printf("LhX: %s (IoErr %ld)\n", (LONG)msg, code);
    } else {
        Printf("LhX: %s\n", (LONG)msg);
    }
    Flush(Output());
}

void lhx_print_usage(void)
{
    Printf("LhX LIST|EXTRACT|EXTRACTFLAT|ADD|TEST|PRINT ARCHIVE [FILES...] [TO dir] [QUIET] [FORCE] [PASSWORD pass]\n", 0);
    Printf("  FILES may use Amiga wildcards (#? *). Ctrl-C aborts between files.\n", 0);
    Flush(Output());
}

static void lhx_format_prot(LONG prot, char *out)
{
    static const char tpl[9] = "hsparwed";
    LONG i;

    for (i = 0; i < 8; i++) {
        out[i] = tpl[i];
    }
    out[8] = '\0';
    for (i = 0; i < 8; i++) {
        if ((prot & (1L << (7 - i))) ==
            ((i < 4) ? 0L : (1L << (7 - i)))) {
            out[i] = '-';
        }
    }
}

void lhx_list_entry(struct LhxArgs *args, struct FileInfoBlock *fib)
{
    char datebuf[16];
    char timebuf[16];
    char prot[9];
    struct DateTime dt;

    if (!fib) {
        return;
    }
    if (args && args->quiet) {
        Printf("%s\n", (LONG)fib->fib_FileName);
        return;
    }
    lhx_format_prot(fib->fib_Protection, prot);
    datebuf[0] = '\0';
    timebuf[0] = '\0';
    dt.dat_Stamp = fib->fib_Date;
    dt.dat_Format = FORMAT_DOS;
    dt.dat_Flags = 0;
    dt.dat_StrDay = NULL;
    dt.dat_StrDate = datebuf;
    dt.dat_StrTime = timebuf;
    if (!DateToStr(&dt)) {
        Strncpy((STRPTR)datebuf, (STRPTR)"<invalid>", (LONG)sizeof(datebuf));
        Strncpy((STRPTR)timebuf, (STRPTR)"<invalid>", (LONG)sizeof(timebuf));
    }
    if (fib->fib_DirEntryType > 0) {
        Printf("%-24s %7s %s %-9s %s",
            (LONG)fib->fib_FileName, (LONG)"Dir", (LONG)prot,
            (LONG)datebuf, (LONG)timebuf);
    } else if (fib->fib_Size == 0) {
        Printf("%-24s %7s %s %-9s %s",
            (LONG)fib->fib_FileName, (LONG)"empty", (LONG)prot,
            (LONG)datebuf, (LONG)timebuf);
    } else {
        Printf("%-24s %7ld %s %-9s %s",
            (LONG)fib->fib_FileName, fib->fib_Size, (LONG)prot,
            (LONG)datebuf, (LONG)timebuf);
    }
    if (fib->fib_Comment[0]) {
        Printf("\n: %s", (LONG)fib->fib_Comment);
    }
    Printf("\n", 0);
}

void lhx_list_total(LONG files, LONG blocks)
{
    Printf("\nTOTAL: %ld ", files);
    if (files == 1) {
        Printf("file - ", 0);
    } else {
        Printf("files - ", 0);
    }
    Printf("%ld ", blocks);
    if (blocks == 1) {
        Printf("block used\n", 0);
    } else {
        Printf("blocks used\n", 0);
    }
    Flush(Output());
}

void lhx_work_start(struct LhxArgs *args, STRPTR name)
{
    if (!args || args->quiet || !name) {
        return;
    }
    Printf("%s\t- ", (LONG)name);
    Flush(Output());
}

void lhx_work_done(struct LhxArgs *args, LONG test_mode)
{
    if (!args || args->quiet) {
        return;
    }
    if (test_mode) {
        Printf("Tested  \n", 0);
    } else {
        Printf("Melted  \n", 0);
    }
    Flush(Output());
}

void lhx_add_line(struct LhxArgs *args, STRPTR name)
{
    if (!args || args->quiet || !name) {
        return;
    }
    Printf("ADD %s\n", (LONG)name);
    Flush(Output());
}

int lhx_name_matches(STRPTR pattern, STRPTR name)
{
    UBYTE parsed[LHX_PATTERN_BUF];
    LONG wild;
    LONG matched;
    STRPTR leaf;

    if (!name) {
#if LHX_DEBUG
        lhx_dbg_s((STRPTR)"match", (STRPTR)"name is null");
#endif
        return 0;
    }
    if (!pattern || !pattern[0]) {
#if LHX_DEBUG
        lhx_dbg_s((STRPTR)"match", (STRPTR)"no pattern, accept");
#endif
        return 1;
    }
    /* Match-all patterns; ParsePatternNoCase treats '*' differently by OS rev. */
    if ((pattern[0] == '*' && pattern[1] == '\0')
        || (pattern[0] == '#' && pattern[1] == '?' && pattern[2] == '\0')) {
#if LHX_DEBUG
        lhx_dbg_s((STRPTR)"match-all", pattern);
#endif
        return 1;
    }
    if (Stricmp(pattern, name) == 0) {
#if LHX_DEBUG
        lhx_dbg_s((STRPTR)"match Stricmp", name);
#endif
        return 1;
    }
    leaf = FilePart(name);
    if (leaf != NULL && leaf != name && Stricmp(pattern, leaf) == 0) {
        return 1;
    }
    wild = ParsePatternNoCase(pattern, parsed, (LONG)sizeof(parsed));
    if (wild == -1) {
#if LHX_DEBUG
        lhx_dbg_s((STRPTR)"ParsePatternNoCase fail pat", pattern);
        lhx_dbg_s((STRPTR)"ParsePatternNoCase fail name", name);
#endif
        return 0;
    }
    matched = MatchPatternNoCase(parsed, name) ? 1 : 0;
    if (!matched && leaf != NULL && leaf != name) {
        matched = MatchPatternNoCase(parsed, leaf) ? 1 : 0;
    }
#if LHX_DEBUG
    if (!matched) {
        lhx_dbg_s((STRPTR)"no match pat", pattern);
        lhx_dbg_s((STRPTR)"no match name", name);
    } else {
        lhx_dbg_s((STRPTR)"matched", name);
    }
#endif
    return matched;
}

int lhx_any_selected(STRPTR name, STRPTR *patterns)
{
    LONG i;

    if (!patterns || !patterns[0]) {
        return 1;
    }
    for (i = 0; patterns[i] != NULL; i++) {
        if (lhx_name_matches(patterns[i], name)) {
            return 1;
        }
    }
#if LHX_DEBUG
    lhx_dbg_s((STRPTR)"any_selected reject", name);
#endif
    return 0;
}

int lhx_check_break(void)
{
    if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        return 1;
    }
    return 0;
}

/*
 * Expand FILES args with AmigaDOS wildcards (MatchFirst/MatchNext).
 * Non-wild paths are kept as given.  Returns a static NULL-terminated
 * list, or NULL on failure (*err set).  Caller must not FreeMem the list.
 */
STRPTR *lhx_expand_file_args(STRPTR *patterns, LONG *err)
{
    static char expand_bufs[LHX_EXPAND_MAX][LHX_EXPAND_PATH];
    static STRPTR expand_ptrs[LHX_EXPAND_MAX + 1];
    LONG n;
    LONG i;
    LONG wild;
    LONG merr;
    UBYTE parsed[LHX_PATTERN_BUF];
    struct AnchorPath *ap;
    ULONG apsize;

    if (err) {
        *err = 0;
    }
    n = 0;
    if (!patterns) {
        expand_ptrs[0] = NULL;
        return expand_ptrs;
    }

    apsize = (ULONG)sizeof(struct AnchorPath) + (ULONG)LHX_EXPAND_PATH;
    ap = (struct AnchorPath *)AllocVec(apsize, MEMF_CLEAR);
    if (ap == NULL) {
        if (err) {
            *err = ERROR_NO_FREE_STORE;
        }
        return NULL;
    }
    ap->ap_BreakBits = SIGBREAKF_CTRL_C;
    ap->ap_Strlen = (WORD)LHX_EXPAND_PATH;

    for (i = 0; patterns[i] != NULL; i++) {
        if (lhx_check_break()) {
            FreeVec(ap);
            if (err) {
                *err = ERROR_BREAK;
            }
            Printf("LhX: *** Break\n", 0);
            Flush(Output());
            return NULL;
        }

        wild = ParsePatternNoCase(patterns[i], parsed, (LONG)sizeof(parsed));
        if (wild <= 0) {
            /* Literal path (or parse error treated as literal). */
            if (n >= LHX_EXPAND_MAX) {
                FreeVec(ap);
                if (err) {
                    *err = ERROR_NO_FREE_STORE;
                }
                lhx_print_error((STRPTR)"too many files after wildcard expand",
                    0);
                return NULL;
            }
            Strncpy(expand_bufs[n], patterns[i], LHX_EXPAND_PATH);
            expand_ptrs[n] = (STRPTR)expand_bufs[n];
            n++;
            continue;
        }

        merr = MatchFirst(patterns[i], ap);
        while (merr == 0) {
            if (lhx_check_break()) {
                MatchEnd(ap);
                FreeVec(ap);
                if (err) {
                    *err = ERROR_BREAK;
                }
                Printf("LhX: *** Break\n", 0);
                Flush(Output());
                return NULL;
            }
            /* Files only; skip drawers. */
            if (ap->ap_Info.fib_DirEntryType < 0) {
                if (n >= LHX_EXPAND_MAX) {
                    MatchEnd(ap);
                    FreeVec(ap);
                    if (err) {
                        *err = ERROR_NO_FREE_STORE;
                    }
                    lhx_print_error(
                        (STRPTR)"too many files after wildcard expand", 0);
                    return NULL;
                }
                if (ap->ap_Buf[0] != '\0') {
                    Strncpy(expand_bufs[n], (STRPTR)ap->ap_Buf,
                        LHX_EXPAND_PATH);
                } else {
                    Strncpy(expand_bufs[n],
                        (STRPTR)ap->ap_Info.fib_FileName, LHX_EXPAND_PATH);
                }
                expand_ptrs[n] = (STRPTR)expand_bufs[n];
                n++;
            }
            merr = MatchNext(ap);
        }
        MatchEnd(ap);
        if (merr != ERROR_NO_MORE_ENTRIES && merr != 0) {
            /*
             * No match for this pattern is a warning for ADD; keep going
             * so other patterns still expand.
             */
            if (merr != ERROR_OBJECT_NOT_FOUND) {
                lhx_print_error(patterns[i], merr);
            }
        }
        /* Reset anchor for next pattern. */
        memset(ap, 0, (size_t)apsize);
        ap->ap_BreakBits = SIGBREAKF_CTRL_C;
        ap->ap_Strlen = (WORD)LHX_EXPAND_PATH;
    }

    FreeVec(ap);
    expand_ptrs[n] = NULL;
    if (n == 0) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        lhx_print_error((STRPTR)"no files matched", 0);
        return NULL;
    }
    return expand_ptrs;
}

int lhx_basename(STRPTR path, STRPTR out, LONG outlen)
{
    STRPTR fp;

    if (!path || !out || outlen <= 0) {
        return 0;
    }
    fp = FilePart(path);
    if (!fp) {
        return 0;
    }
    Strncpy(out, fp, outlen);
    return 1;
}

int lhx_path_join(STRPTR out, LONG outlen, STRPTR dir, STRPTR name)
{
    if (!out || outlen <= 0 || !name) {
        return 0;
    }
    if (!dir || !dir[0]) {
        Strncpy(out, name, outlen);
        return 1;
    }
    Strncpy(out, dir, outlen);
    if (!AddPart(out, name, (ULONG)outlen)) {
        return 0;
    }
    return 1;
}

static int lhx_mkdir_chain(STRPTR dirpath)
{
    char buf[512];
    char *p;
    BPTR lock;

    if (!dirpath || !dirpath[0]) {
        return 1;
    }
    Strncpy((STRPTR)buf, dirpath, (LONG)sizeof(buf));
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

int lhx_ensure_parent_dir(STRPTR path)
{
    char buf[512];
    STRPTR pp;
    STRPTR p;

    if (!path || !path[0]) {
        return 0;
    }
    Strncpy((STRPTR)buf, path, (LONG)sizeof(buf));
    pp = PathPart((STRPTR)buf);
    if (!pp || pp == (STRPTR)buf) {
        return 1;
    }
    *pp = '\0';
    if (buf[0] == '\0') {
        return 1;
    }
    /*
     * PathPart("vol:file") leaves parent "vol:"; the volume root already
     * exists and CreateDir() on it fails (see dos.doc PathPart example).
     */
    p = (STRPTR)buf;
    while (*p) {
        p++;
    }
    if (p > (STRPTR)buf && *(p - 1) == ':') {
        return 1;
    }
    return lhx_mkdir_chain((STRPTR)buf);
}

LONG lhx_read_file(STRPTR path, APTR *data, LONG *len)
{
    BPTR fh;
    LONG size;
    LONG got;
    APTR buf;

    if (!path || !data || !len) {
        return DOSFALSE;
    }
    *data = NULL;
    *len = 0;
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return DOSFALSE;
    }
    /*
     * AmigaDOS Seek returns the previous position, not the new one.
     * Seek to end, then Seek to start — the second call's return is the size.
     */
    if (Seek(fh, 0L, OFFSET_END) < 0) {
        Close(fh);
        return DOSFALSE;
    }
    size = Seek(fh, 0L, OFFSET_BEGINNING);
    if (size < 0) {
        Close(fh);
        return DOSFALSE;
    }
    if (size == 0) {
        Close(fh);
        *data = NULL;
        *len = 0;
        return DOSTRUE;
    }
    buf = AllocMem((ULONG)size, MEMF_ANY);
    if (!buf) {
        Close(fh);
        return DOSFALSE;
    }
    got = Read(fh, buf, size);
    Close(fh);
    if (got != size) {
        FreeMem(buf, (ULONG)size);
        return DOSFALSE;
    }
    *data = buf;
    *len = size;
    return DOSTRUE;
}
