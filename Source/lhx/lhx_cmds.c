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
#include <utility/tagitem.h>
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

/*
 * Nested archive walk: LhExamine/LhExNext now yield immediate children.
 * Build full entry paths (test/test1/file) for list/extract/test/print.
 */
static LONG lhx_walk_entries(struct LhArchive *arc, STRPTR dirpath,
    struct LhxArgs *args,
    LONG (*fn)(struct LhxArgs *, struct LhArchive *, STRPTR,
        struct FileInfoBlock *, APTR),
    APTR ud)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG rc;
    char childpath[512];
    LONG dlen;
    LONG nlen;

    lock = LhLock(arc, dirpath ? dirpath : (STRPTR)"");
    if (!lock) {
        return RETURN_FAIL;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhUnLock(lock);
        return RETURN_FAIL;
    }
    /* Examine = directory self; skip.  Children come from ExNext. */
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
            /* Recurse into nested virtual directory. */
            if (lhx_walk_entries(arc, (STRPTR)childpath, args, fn, ud)
                != RETURN_OK) {
                rc = RETURN_ERROR;
            }
        } else if (fn != NULL) {
            if (fn(args, arc, (STRPTR)childpath, fib, ud) != RETURN_OK) {
                rc = RETURN_ERROR;
            }
        }
        ok = LhExNext(lock, fib);
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhUnLock(lock);
    return rc;
}

struct lhx_list_ud {
    LONG count;
    LONG blocks;
};

static LONG lhx_list_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_list_ud *st;

    (void)arc;
    st = (struct lhx_list_ud *)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    /* Show full nested path in the name column. */
    Strncpy(fib->fib_FileName, fullname, (LONG)sizeof(fib->fib_FileName));
    lhx_list_entry(args, fib);
    st->count++;
    st->blocks += fib->fib_NumBlocks;
    return RETURN_OK;
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
    struct lhx_list_ud st;

    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    st.count = 0;
    st.blocks = 0;
    lhx_walk_entries(arc, (STRPTR)"", args, lhx_list_one, (APTR)&st);
    LhCloseArchive(arc);
    if (!args->quiet) {
        if (st.count == 0 && args->files && args->files[0]) {
            Printf("No match found\n", 0);
        } else {
            lhx_list_total(st.count, st.blocks);
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

struct lhx_fail_ud {
    LONG fail;
    LONG with_paths;
};

static LONG lhx_extract_walk_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_fail_ud *st;

    st = (struct lhx_fail_ud *)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    if (!lhx_extract_one(args, arc, fullname, st->with_paths, fib->fib_Size)) {
        st->fail++;
    }
    return RETURN_OK;
}

LONG lhx_cmd_extract(struct LhxArgs *args, LONG with_paths)
{
    struct LhArchive *arc;
    struct lhx_fail_ud st;

    lhx_dbg_s((STRPTR)"cmd", (STRPTR)"EXTRACT");
    lhx_dbg_l((STRPTR)"with_paths", with_paths);
    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_dbg_l((STRPTR)"LhOpenArchive LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    st.fail = 0;
    st.with_paths = with_paths;
    lhx_walk_entries(arc, (STRPTR)"", args, lhx_extract_walk_one, (APTR)&st);
    LhCloseArchive(arc);
    return st.fail > 0 ? RETURN_WARN : RETURN_OK;
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
    BPTR flock;
    struct FileInfoBlock *fib;
    struct TagItem tags[5];
    ULONG n;

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
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
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

        n = 0;
        flock = Lock(args->files[i], ACCESS_READ);
        if (flock != (BPTR)NULL && Examine(flock, fib)) {
            tags[n].ti_Tag = LHADD_Attrs;
            tags[n].ti_Data = (ULONG)fib->fib_Protection;
            n++;
            tags[n].ti_Tag = LHADD_DateStamp;
            tags[n].ti_Data = (ULONG)&fib->fib_Date;
            n++;
            if (fib->fib_Comment[0] != '\0') {
                tags[n].ti_Tag = LHADD_Comment;
                tags[n].ti_Data = (ULONG)fib->fib_Comment;
                n++;
            }
        }
        if (flock != (BPTR)NULL) {
            UnLock(flock);
        }
        tags[n].ti_Tag = TAG_DONE;

        if (!LhAddEntryTagList(arc, (STRPTR)arcname, data, len, tags)) {
            lhx_print_error((STRPTR)"add failed", LhErr());
            if (data) {
                FreeMem(data, (ULONG)len);
            }
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            LhCloseArchive(arc);
            return RETURN_FAIL;
        }
        if (data) {
            FreeMem(data, (ULONG)len);
        }
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    return RETURN_OK;
}

struct lhx_test_ud {
    LONG fail;
};

static LONG lhx_test_walk_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_test_ud *st;
    LONG len;

    st = (struct lhx_test_ud *)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    len = LhTestEntry(arc, fullname);
    if (len < 0 || len != fib->fib_Size) {
        st->fail++;
        return RETURN_ERROR;
    }
    lhx_work_start(args, fullname);
    lhx_work_done(args, 1);
    return RETURN_OK;
}

LONG lhx_cmd_test(struct LhxArgs *args)
{
    struct LhArchive *arc;
    struct lhx_test_ud st;

    lhx_dbg_s((STRPTR)"cmd", (STRPTR)"TEST");
    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_dbg_l((STRPTR)"LhOpenArchive LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    st.fail = 0;
    if (lhx_walk_entries(arc, (STRPTR)"", args, lhx_test_walk_one, (APTR)&st)
        != RETURN_OK || st.fail > 0) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    LhCloseArchive(arc);
    return RETURN_OK;
}

static LONG lhx_print_walk_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    (void)fib;
    (void)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    if (!LhPrintEntry(arc, fullname)) {
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

LONG lhx_cmd_print(struct LhxArgs *args)
{
    struct LhArchive *arc;

    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    if (lhx_walk_entries(arc, (STRPTR)"", args, lhx_print_walk_one, NULL)
        != RETURN_OK) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    LhCloseArchive(arc);
    Flush(Output());
    return RETURN_OK;
}
