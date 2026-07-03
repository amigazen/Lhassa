/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx_cmds.c - LhX commands via lh.library DOS-style archive API.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <libraries/lhlib.h>
#include <proto/lh.h>

#include "lhx_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

#define LHX_IOBUF 8192L

/* dos.doc: fib_DirEntryType positive for directories, negative for files. */
static int lhx_fib_is_file(struct FileInfoBlock *fib)
{
    if (!fib) {
        return 0;
    }
    if (fib->fib_DirEntryType > 0) {
        return 0;
    }
    return 1;
}

static struct LhArchive *lhx_open_archive(STRPTR path, LONG mode, STRPTR password)
{
    struct LhArchive *arc;

    arc = LhOpenArchive(path, mode);
    if (!arc) {
        return NULL;
    }
    if (password && password[0]) {
        LhSetPassword(arc, password);
    }
    return arc;
}

LONG lhx_cmd_list(struct LhxArgs *args)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;
    LONG blocks;

    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (!lock) {
        lhx_print_error((STRPTR)"cannot lock archive root", LhErr());
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    count = 0;
    blocks = 0;
    ok = LhExamine(lock, fib);
    while (ok) {
        if (lhx_any_selected(fib->fib_FileName, args->files)) {
            lhx_list_entry(args, fib);
            count++;
            blocks += fib->fib_NumBlocks;
        }
        ok = LhExNext(lock, fib);
    }
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    if (!args->quiet) {
        if (count == 0 && args->files && args->files[0]) {
            Printf("No match found\n", 0);
        } else {
            lhx_list_total(count, blocks);
        }
    }
    return RETURN_OK;
}

static LONG lhx_extract_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR entry_name, LONG with_paths, LONG expected_size)
{
    STRPTR outpath;
    char namebuf[512];
    char pathbuf[512];
    LONG len;

    lhx_dbg_s((STRPTR)"extract_one", entry_name);
    lhx_dbg_l((STRPTR)"with_paths", with_paths);
    lhx_dbg_l((STRPTR)"expected_size", expected_size);
    if (!entry_name || !entry_name[0]) {
        lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"skip empty name");
        return DOSTRUE;
    }
    if (with_paths) {
        Strncpy((STRPTR)namebuf, entry_name, (LONG)sizeof(namebuf));
    } else {
        if (!lhx_basename(entry_name, (STRPTR)namebuf, (LONG)sizeof(namebuf))) {
            lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"basename failed");
            return DOSFALSE;
        }
    }
    lhx_dbg_s((STRPTR)"out name", (STRPTR)namebuf);
    if (!lhx_path_join((STRPTR)pathbuf, (LONG)sizeof(pathbuf),
            args->destdir, (STRPTR)namebuf)) {
        lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"path_join failed");
        return DOSFALSE;
    }
    outpath = (STRPTR)pathbuf;
    lhx_dbg_s((STRPTR)"out path", outpath);
    if (args->force) {
        BPTR old;

        old = Lock(outpath, ACCESS_WRITE);
        if (old != (BPTR)NULL) {
            UnLock(old);
            DeleteFile(outpath);
        }
    }
    lhx_work_start(args, entry_name);
    if (!LhExtractEntry(arc, entry_name, outpath)) {
        lhx_dbg_l((STRPTR)"LhExtractEntry LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot extract archive entry", LhErr());
        return DOSFALSE;
    }
    len = expected_size;
    if (expected_size > 0) {
        BPTR outlock;
        struct FileInfoBlock *outfib;

        outlock = Lock(outpath, ACCESS_READ);
        if (outlock != (BPTR)NULL) {
            outfib = (struct FileInfoBlock *)AllocMem(
                (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
            if (outfib) {
                if (Examine(outlock, outfib)) {
                    len = outfib->fib_Size;
                }
                FreeMem(outfib, (ULONG)sizeof(struct FileInfoBlock));
            }
            UnLock(outlock);
        }
        lhx_dbg_l((STRPTR)"extracted size", len);
        if (len != expected_size) {
            lhx_dbg_ll((STRPTR)"size mismatch got want", len, expected_size);
            lhx_print_error((STRPTR)"archive entry size mismatch", 0);
            return DOSFALSE;
        }
    }
    lhx_work_done(args, 0);
    lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"ok");
    return DOSTRUE;
}

LONG lhx_cmd_extract(struct LhxArgs *args, LONG with_paths)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG fail;
    LONG idx;
    LONG tried;
    LONG skipped_dir;
    LONG skipped_pat;

    lhx_dbg_s((STRPTR)"cmd", (STRPTR)"EXTRACT");
    lhx_dbg_l((STRPTR)"with_paths", with_paths);
    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_dbg_l((STRPTR)"LhOpenArchive LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    lhx_dbg_l((STRPTR)"arc", (LONG)arc);
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (!lock) {
        lhx_dbg_l((STRPTR)"LhLock LhErr", LhErr());
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    lhx_dbg_l((STRPTR)"lock", (LONG)lock);
    fail = 0;
    idx = 0;
    tried = 0;
    skipped_dir = 0;
    skipped_pat = 0;
    ok = LhExamine(lock, fib);
    lhx_dbg_l((STRPTR)"LhExamine", ok);
    if (!ok) {
        lhx_dbg_l((STRPTR)"IoErr after Examine", IoErr());
    }
    while (ok) {
        lhx_dbg_l((STRPTR)"idx", idx);
        lhx_dbg_s((STRPTR)"entry", fib->fib_FileName);
        lhx_dbg_ll((STRPTR)"type size", fib->fib_DirEntryType, fib->fib_Size);
        if (!lhx_fib_is_file(fib)) {
            skipped_dir++;
            lhx_dbg_s((STRPTR)"skip", (STRPTR)"not a file (dir)");
        } else if (!lhx_any_selected(fib->fib_FileName, args->files)) {
            skipped_pat++;
            lhx_dbg_s((STRPTR)"skip", (STRPTR)"pattern mismatch");
        } else {
            tried++;
            if (!lhx_extract_one(args, arc, fib->fib_FileName, with_paths,
                    fib->fib_Size)) {
                fail++;
            }
        }
        idx++;
        ok = LhExNext(lock, fib);
    }
    lhx_dbg_l((STRPTR)"IoErr after loop", IoErr());
    lhx_dbg_ll((STRPTR)"summary tried fail", tried, fail);
    lhx_dbg_ll((STRPTR)"skipped dir pat", skipped_dir, skipped_pat);
    lhx_dbg_l((STRPTR)"entries seen", idx);
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    return fail > 0 ? RETURN_WARN : RETURN_OK;
}

LONG lhx_cmd_add(struct LhxArgs *args)
{
    struct LhArchive *arc;
    LONG i;
    APTR data;
    LONG len;
    char arcname[512];
    LONG mode;
    BPTR testlock;

    if (!args->files || !args->files[0]) {
        lhx_print_error((STRPTR)"ADD requires FILES", 0);
        return RETURN_FAIL;
    }
    testlock = Lock(args->archive, ACCESS_READ);
    if (testlock != (BPTR)NULL) {
        UnLock(testlock);
        mode = LHARC_MODE_APPEND;
    } else {
        mode = LHARC_MODE_WRITE;
    }
    arc = lhx_open_archive(args->archive, mode, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive for output", LhErr());
        return RETURN_FAIL;
    }
    for (i = 0; args->files[i] != NULL; i++) {
        data = NULL;
        len = 0;
        if (!lhx_read_file(args->files[i], &data, &len)) {
            lhx_print_error((STRPTR)"cannot read file", IoErr());
            continue;
        }
        if (!lhx_basename(args->files[i], (STRPTR)arcname, (LONG)sizeof(arcname))) {
            if (data) {
                FreeMem(data, (ULONG)len);
            }
            continue;
        }
        lhx_add_line(args, (STRPTR)arcname);
        if (!LhAddEntry(arc, (STRPTR)arcname, data, len)) {
            lhx_print_error((STRPTR)"add failed", LhErr());
            if (data) {
                FreeMem(data, (ULONG)len);
            }
            LhCloseArchive(arc);
            return RETURN_FAIL;
        }
        if (data) {
            FreeMem(data, (ULONG)len);
        }
    }
    LhCloseArchive(arc);
    return RETURN_OK;
}

LONG lhx_cmd_test(struct LhxArgs *args)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG len;
    LONG idx;
    LONG tried;
    LONG skipped_dir;
    LONG skipped_pat;

    lhx_dbg_s((STRPTR)"cmd", (STRPTR)"TEST");
    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_dbg_l((STRPTR)"LhOpenArchive LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (!lock) {
        lhx_dbg_l((STRPTR)"LhLock LhErr", LhErr());
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    idx = 0;
    tried = 0;
    skipped_dir = 0;
    skipped_pat = 0;
    ok = LhExamine(lock, fib);
    lhx_dbg_l((STRPTR)"LhExamine", ok);
    if (!ok) {
        lhx_dbg_l((STRPTR)"IoErr after Examine", IoErr());
    }
    while (ok) {
        lhx_dbg_l((STRPTR)"idx", idx);
        lhx_dbg_s((STRPTR)"entry", fib->fib_FileName);
        lhx_dbg_ll((STRPTR)"type size", fib->fib_DirEntryType, fib->fib_Size);
        if (!lhx_fib_is_file(fib)) {
            skipped_dir++;
            lhx_dbg_s((STRPTR)"skip", (STRPTR)"not a file (dir)");
        } else if (!lhx_any_selected(fib->fib_FileName, args->files)) {
            skipped_pat++;
            lhx_dbg_s((STRPTR)"skip", (STRPTR)"pattern mismatch");
        } else {
            tried++;
            len = LhTestEntry(arc, fib->fib_FileName);
            if (len < 0) {
                lhx_dbg_l((STRPTR)"LhTestEntry LhErr", LhErr());
                LhUnLock(lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                LhCloseArchive(arc);
                return RETURN_FAIL;
            }
            lhx_dbg_l((STRPTR)"LhTestEntry len", len);
            if (len != fib->fib_Size) {
                lhx_dbg_ll((STRPTR)"size mismatch got want", len, fib->fib_Size);
                LhUnLock(lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                LhCloseArchive(arc);
                return RETURN_FAIL;
            }
            lhx_work_start(args, fib->fib_FileName);
            lhx_work_done(args, 1);
        }
        idx++;
        ok = LhExNext(lock, fib);
    }
    lhx_dbg_l((STRPTR)"IoErr after loop", IoErr());
    lhx_dbg_ll((STRPTR)"summary tried", tried, idx);
    lhx_dbg_ll((STRPTR)"skipped dir pat", skipped_dir, skipped_pat);
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    return RETURN_OK;
}

LONG lhx_cmd_print(struct LhxArgs *args)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;

    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (!lock) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    ok = LhExamine(lock, fib);
    while (ok) {
        if (lhx_fib_is_file(fib)
            && lhx_any_selected(fib->fib_FileName, args->files)) {
            if (!LhPrintEntry(arc, fib->fib_FileName)) {
                LhUnLock(lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                LhCloseArchive(arc);
                return RETURN_FAIL;
            }
        }
        ok = LhExNext(lock, fib);
    }
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    Flush(Output());
    return RETURN_OK;
}
