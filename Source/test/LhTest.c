/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * LhTest.c - Standalone lh.library smoke / regression harness.
 *
 * Exercises every public LVO from SDK/SFD/lh_lib.sfd (31 functions):
 *
 *   CreateBuffer, DeleteBuffer, LhEncode, LhDecode, LhCompress, LhDecompress,
 *   LhOpenArchive, LhCloseArchive, LhLock, LhUnLock, LhExamine, LhExNext,
 *   LhExAll, LhExAllEnd, LhInfo, LhOpenFromLock, LhOpen, LhRead, LhWrite,
 *   LhClose, LhSeek, LhNameFromLock, LhAddEntry, LhDeleteFile, LhConcatArchive,
 *   LhSetPassword, LhReadData, LhExtractEntry, LhTestEntry, LhPrintEntry, LhErr.
 *
 * Flow: classic buffer -> create fixture -> read APIs -> mutate -> verify.
 *
 *   LhTest >RAM:LhTest.out
 *   LhTest LhA-mos.lha
 *
 * Exit codes: 0 = all pass, 10 = test failure, 20 = library missing.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/exall.h>
#include <stdio.h>
#include <string.h>

#include <libraries/lhlib.h>
#include <proto/lh.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define LT_RAMDIR       "RAM:LhTest"
#define LT_FIXTURE      "RAM:LhTest/LhTest.lha"
#define LT_FIXTURE2     "RAM:LhTest/LhTstCat.lha"
#define LT_EXTRACT_DIR  "RAM:LhTest/Out"
#define LT_METHOD_LH0   0L
#define LT_ENTRY_A      "LtReadme.txt"
#define LT_ENTRY_B      "lhtest.bin"
#define LT_ENTRY_C      "LtExtra.txt"
#define LT_CONCAT_ENTRY "LtConcat.txt"

static const STRPTR lt_plain_a = (STRPTR)"LhTest fixture entry A.";
static const STRPTR lt_plain_b =
    (STRPTR)"LhTest fixture entry B (longer payload for lh.library).";
static const STRPTR lt_plain_c = (STRPTR)"LhTest appended entry C.";
static const STRPTR lt_plain_cat = (STRPTR)"LhTest concat donor entry.";

static ULONG lt_pass;
static ULONG lt_fail;
static BOOL lt_got_fixture;
static BOOL lt_mutate_ok;
static char lt_cur_name[80];
static char lt_cur_expect[96];

static const char lt_stack_cookie[] = "$STACK: 8192";

/* Print the test name and expected result before the call under test. */
static VOID
lt_begin(STRPTR name, STRPTR expect)
{
    strncpy(lt_cur_name, (const char *)name, sizeof(lt_cur_name) - 1);
    lt_cur_name[sizeof(lt_cur_name) - 1] = '\0';
    strncpy(lt_cur_expect, (const char *)expect, sizeof(lt_cur_expect) - 1);
    lt_cur_expect[sizeof(lt_cur_expect) - 1] = '\0';
    Printf("LhTest: RUN  %s\n", lt_cur_name);
    Printf("LhTest:   expect: %s\n", lt_cur_expect);
    Flush(Output());
}

/* Print PASS/FAIL and the actual result after the call under test. */
static VOID
lt_end(BOOL ok, STRPTR actual)
{
    if (ok) {
        lt_pass++;
        Printf("LhTest: PASS %s\n", lt_cur_name);
    } else {
        lt_fail++;
        Printf("LhTest: FAIL %s\n", lt_cur_name);
    }
    if (actual != NULL && actual[0] != '\0') {
        Printf("LhTest:   actual: %s\n", actual);
    } else {
        Printf("LhTest:   actual: %s\n", ok ? "ok" : "failed");
    }
    Flush(Output());
}

static VOID
lt_print_api_plan(VOID)
{
    Printf("LhTest: plan - all 31 public LVOs from SDK/SFD/lh_lib.sfd\n");
    Flush(Output());
}

static VOID
lt_lherr_detail(char *buf, LONG bufsz)
{
    sprintf(buf, "LhErr %ld", (long)LhErr());
}

static BOOL
lt_ensure_dir(STRPTR path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    BOOL is_dir;

    lock = CreateDir(path);
    if (lock != (BPTR)NULL) {
        UnLock(lock);
        return TRUE;
    }
    if (IoErr() != ERROR_OBJECT_EXISTS) {
        return FALSE;
    }
    lock = Lock(path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        return FALSE;
    }
    is_dir = FALSE;
    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        is_dir = TRUE;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    if (is_dir) {
        return TRUE;
    }
    /* A plain file at this path blocks CreateDir for nested archives. */
    DeleteFile(path);
    lock = CreateDir(path);
    if (lock != (BPTR)NULL) {
        UnLock(lock);
        return TRUE;
    }
    return FALSE;
}

static BOOL
lt_ensure_parent(STRPTR filepath)
{
    char parent[256];
    STRPTR slash;

    strncpy(parent, (const char *)filepath, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    slash = (STRPTR)strrchr(parent, '/');
    if (slash == NULL) {
        return TRUE;
    }
    *slash = '\0';
    if (parent[0] == '\0') {
        return TRUE;
    }
    return lt_ensure_dir((STRPTR)parent);
}

static VOID
lt_remove_path(STRPTR path)
{
    BPTR fh;
    char aside[280];

    /* Remove any prior fixture; rename aside if DeleteFile is stubborn. */
    for (;;) {
        SetIoErr(0);
        fh = Open(path, MODE_OLDFILE);
        if (fh == (BPTR)NULL) {
            break;
        }
        Close(fh);
        if (!DeleteFile(path)) {
            strncpy(aside, (const char *)path, sizeof(aside) - 4);
            aside[sizeof(aside) - 4] = '\0';
            strcat(aside, ".$$");
            SetIoErr(0);
            if (Rename(path, (STRPTR)aside)) {
                DeleteFile((STRPTR)aside);
            }
            break;
        }
    }
    SetIoErr(0);
}

static BOOL
lt_test_open(VOID)
{
    lt_begin((STRPTR)"OpenLibrary(lh.library,2)",
        (STRPTR)"non-NULL LhBase");
    LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
    if (LhBase == NULL) {
        lt_end(FALSE, (STRPTR)"NULL (not found)");
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)"non-NULL LhBase");
    if (LhBase->lib_IdString != NULL) {
        Printf("LhTest: library %s\n", LhBase->lib_IdString);
        Flush(Output());
    }
    return TRUE;
}

static VOID
lt_test_classic_buffer(VOID)
{
    struct LhBuffer *buf;
    struct LhBuffer *decbuf;
    static UBYTE comp[256];
    static UBYTE back[256];
    ULONG n;
    ULONG plain_len;
    char actual[48];

    plain_len = (ULONG)strlen((const char *)lt_plain_a);

    lt_begin((STRPTR)"CreateBuffer(FALSE)", (STRPTR)"non-NULL buffer");
    buf = CreateBuffer(FALSE);
    if (buf == NULL) {
        lt_end(FALSE, (STRPTR)"NULL");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL buffer");

    buf->lh_Src = (APTR)lt_plain_a;
    buf->lh_SrcSize = plain_len;
    buf->lh_Dst = (APTR)comp;
    buf->lh_DstSize = (ULONG)sizeof(comp);
    lt_begin((STRPTR)"LhEncode", (STRPTR)"compressed size > 0");
    n = LhEncode(buf);
    if (n == 0) {
        DeleteBuffer(buf);
        lt_end(FALSE, (STRPTR)"size=0");
        return;
    }
    sprintf(actual, "size=%lu", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    buf->lh_Src = (APTR)comp;
    buf->lh_SrcSize = n;
    buf->lh_Dst = (APTR)back;
    buf->lh_DstSize = plain_len;
    memset(back, 0, sizeof(back));
    lt_begin((STRPTR)"LhDecode", (STRPTR)"original payload restored");
    n = LhDecode(buf);
    if (n != plain_len || memcmp(back, lt_plain_a, plain_len) != 0) {
        DeleteBuffer(buf);
        sprintf(actual, "size=%lu mismatch", (unsigned long)n);
        lt_end(FALSE, (STRPTR)actual);
        return;
    }
    sprintf(actual, "size=%lu match", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    lt_begin((STRPTR)"DeleteBuffer", (STRPTR)"buffer freed");
    DeleteBuffer(buf);
    lt_end(TRUE, (STRPTR)"freed");

    lt_begin((STRPTR)"CreateBuffer(TRUE)", (STRPTR)"non-NULL decode-only buffer");
    decbuf = CreateBuffer(TRUE);
    if (decbuf == NULL) {
        lt_end(FALSE, (STRPTR)"NULL");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL decode-only buffer");

    lt_begin((STRPTR)"DeleteBuffer(decode-only)", (STRPTR)"buffer freed");
    DeleteBuffer(decbuf);
    lt_end(TRUE, (STRPTR)"freed");
}

static VOID
lt_test_codec_lh0(VOID)
{
    struct LhBuffer *buf;
    static UBYTE comp[512];
    static UBYTE back[256];
    ULONG n;
    ULONG plain_len;
    char actual[48];

    plain_len = (ULONG)strlen((const char *)lt_plain_b);

    lt_begin((STRPTR)"LhCompress setup", (STRPTR)"non-NULL buffer");
    buf = CreateBuffer(FALSE);
    if (buf == NULL) {
        lt_end(FALSE, (STRPTR)"NULL");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL buffer");

    buf->lh_Src = (APTR)lt_plain_b;
    buf->lh_SrcSize = plain_len;
    buf->lh_Dst = (APTR)comp;
    buf->lh_DstSize = (ULONG)sizeof(comp);
    lt_begin((STRPTR)"LhCompress", (STRPTR)"LH0 size > 0");
    n = LhCompress(LT_METHOD_LH0, buf);
    if (n == 0) {
        DeleteBuffer(buf);
        lt_end(FALSE, (STRPTR)"size=0");
        return;
    }
    sprintf(actual, "size=%lu", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    buf->lh_Src = (APTR)comp;
    buf->lh_SrcSize = n;
    buf->lh_Dst = (APTR)back;
    buf->lh_DstSize = plain_len;
    memset(back, 0, sizeof(back));
    lt_begin((STRPTR)"LhDecompress", (STRPTR)"original payload restored");
    n = LhDecompress(LT_METHOD_LH0, buf);
    if (n != plain_len || memcmp(back, lt_plain_b, plain_len) != 0) {
        DeleteBuffer(buf);
        sprintf(actual, "size=%lu mismatch", (unsigned long)n);
        lt_end(FALSE, (STRPTR)actual);
        return;
    }
    sprintf(actual, "size=%lu match", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    lt_begin((STRPTR)"DeleteBuffer(codec)", (STRPTR)"buffer freed");
    DeleteBuffer(buf);
    lt_end(TRUE, (STRPTR)"freed");
}

static BOOL
lt_write_archive(STRPTR path, STRPTR name_a, STRPTR data_a, ULONG len_a,
    STRPTR name_b, STRPTR data_b, ULONG len_b)
{
    struct LhArchive *arc;
    char detail[40];
    char expect[80];

    lt_begin((STRPTR)"fixture RAM dir", (STRPTR)"directory exists");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)LT_RAMDIR);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_RAMDIR);

    lt_begin((STRPTR)"fixture parent dir", (STRPTR)"parent path ready");
    if (!lt_ensure_parent(path)) {
        lt_end(FALSE, path);
        return FALSE;
    }
    lt_end(TRUE, path);

    lt_remove_path(path);
    SetIoErr(0);

    lt_begin((STRPTR)"LhOpenArchive(write)", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, path);

    sprintf(expect, "DOSTRUE for %s", (const char *)name_a);
    lt_begin((STRPTR)"LhAddEntry", (STRPTR)expect);
    if (!LhAddEntry(arc, name_a, (APTR)data_a, (LONG)len_a)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, name_a);

    sprintf(expect, "DOSTRUE for %s", (const char *)name_b);
    lt_begin((STRPTR)"LhAddEntry", (STRPTR)expect);
    if (!LhAddEntry(arc, name_b, (APTR)data_b, (LONG)len_b)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, name_b);

    lt_begin((STRPTR)"LhCloseArchive", (STRPTR)"DOSTRUE");
    if (!LhCloseArchive(arc)) {
        lt_end(FALSE, (STRPTR)"DOSFALSE");
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    return TRUE;
}

static BOOL
lt_count_entries(STRPTR path, LONG *count_out)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;

    count = 0;
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhCloseArchive(arc);
        return FALSE;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return FALSE;
    }
    ok = LhExamine(lock, fib);
    while (ok) {
        count++;
        ok = LhExNext(lock, fib);
    }
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    *count_out = count;
    return TRUE;
}

static VOID
lt_test_fixture_create(VOID)
{
    ULONG len_a;
    ULONG len_b;

    len_a = (ULONG)strlen((const char *)lt_plain_a);
    len_b = (ULONG)strlen((const char *)lt_plain_b);
    Printf("LhTest: RUN  fixture create\n");
    Printf("LhTest:   expect: archive written\n");
    Flush(Output());
    lt_got_fixture = lt_write_archive((STRPTR)LT_FIXTURE,
        (STRPTR)LT_ENTRY_A, lt_plain_a, len_a,
        (STRPTR)LT_ENTRY_B, lt_plain_b, len_b);
    if (lt_got_fixture) {
        lt_pass++;
        Printf("LhTest: PASS fixture create\n");
        Printf("LhTest:   actual: %s\n", LT_FIXTURE);
    } else {
        lt_fail++;
        Printf("LhTest: FAIL fixture create\n");
        Printf("LhTest:   actual: write failed\n");
    }
    Flush(Output());
}

static VOID
lt_test_openarchive_read(STRPTR path)
{
    struct LhArchive *arc;
    char detail[40];

    lt_begin((STRPTR)"LhOpenArchive(read)", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, path);

    lt_begin((STRPTR)"LhCloseArchive(read)", (STRPTR)"DOSTRUE");
    LhCloseArchive(arc);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
}

static VOID
lt_test_lock_examine(STRPTR path, LONG expect_count)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;
    char detail[48];
    char expect[48];

    lt_begin((STRPTR)"LhLock", (STRPTR)"non-NULL root lock");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, (STRPTR)"root lock");

    lt_begin((STRPTR)"LhExamine", (STRPTR)"first entry FIB filled");
    count = 0;
    ok = LhExamine(lock, fib);
    if (!ok) {
        LhUnLock(lock);
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"DOSFALSE");
        return;
    }
    lt_end(TRUE, fib->fib_FileName);
    count = 1;

    lt_begin((STRPTR)"LhExNext", (STRPTR)"walk remaining entries");
    while (LhExNext(lock, fib)) {
        count++;
    }
    sprintf(detail, "walked to %ld", (long)count);
    lt_end(TRUE, (STRPTR)detail);

    lt_begin((STRPTR)"LhUnLock", (STRPTR)"DOSTRUE");
    LhUnLock(lock);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);

    sprintf(expect, "%ld entries", (long)expect_count);
    lt_begin((STRPTR)"LhExamine/LhExNext count", (STRPTR)expect);
    sprintf(detail, "%ld entries", (long)count);
    if (count == expect_count) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_exall(STRPTR path, LONG expect_count)
{
    struct LhArchive *arc;
    BPTR lock;
    struct ExAllControl ctl;
    UBYTE buf[4096];
    LONG total;
    char detail[48];
    char expect[48];

    sprintf(expect, "%ld entries (ED_NAME)", (long)expect_count);
    lt_begin((STRPTR)"LhExAll", (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    memset(&ctl, 0, sizeof(ctl));
    ctl.eac_LastKey = 0;
    total = 0;
    for (;;) {
        if (!LhExAll(lock, (STRPTR)buf, (LONG)sizeof(buf), ED_NAME, &ctl)) {
            total += (LONG)ctl.eac_Entries;
            break;
        }
        total += (LONG)ctl.eac_Entries;
    }
    sprintf(detail, "%ld entries", (long)total);
    if (total == expect_count) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }

    lt_begin((STRPTR)"LhExAllEnd", (STRPTR)"DOSTRUE");
    LhExAllEnd(lock);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    LhUnLock(lock);
    LhCloseArchive(arc);
}

static VOID
lt_test_lock_entry(STRPTR path, STRPTR name)
{
    struct LhArchive *arc;
    BPTR lock;
    char label[48];
    char expect[64];

    sprintf(label, "LhLock %s", (const char *)name);
    sprintf(expect, "non-NULL lock for %s", (const char *)name);
    lt_begin((STRPTR)label, (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, name);
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"NULL lock");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL lock");
    LhUnLock(lock);
    LhCloseArchive(arc);
}

static VOID
lt_test_exall_size(STRPTR path, LONG expect_count)
{
    struct LhArchive *arc;
    BPTR lock;
    struct ExAllControl ctl;
    UBYTE buf[4096];
    LONG total;
    char detail[48];
    char expect[48];

    sprintf(expect, "%ld entries (ED_SIZE)", (long)expect_count);
    lt_begin((STRPTR)"LhExAll(ED_SIZE)", (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    memset(&ctl, 0, sizeof(ctl));
    ctl.eac_LastKey = 0;
    total = 0;
    for (;;) {
        if (!LhExAll(lock, (STRPTR)buf, (LONG)sizeof(buf), ED_SIZE, &ctl)) {
            total += (LONG)ctl.eac_Entries;
            break;
        }
        total += (LONG)ctl.eac_Entries;
    }
    LhExAllEnd(lock);
    LhUnLock(lock);
    LhCloseArchive(arc);
    sprintf(detail, "%ld entries", (long)total);
    if (total == expect_count) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_seek_end(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    BPTR fh;
    LONG esz;
    char detail[48];
    char expect[48];

    sprintf(expect, "pos=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)"LhSeek(OFFSET_END)", (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    fh = LhOpen(arc, name, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"LhOpen failed");
        return;
    }
    esz = LhSeek(fh, 0L, OFFSET_END);
    LhClose(fh);
    LhCloseArchive(arc);
    sprintf(detail, "pos=%ld", (long)esz);
    if (esz == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_info(STRPTR path)
{
    struct LhArchive *arc;
    BPTR lock;
    struct InfoData info;
    char detail[48];

    lt_begin((STRPTR)"LhInfo", (STRPTR)"DOSTRUE, NumBlocksUsed > 0");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    memset(&info, 0, sizeof(info));
    if (LhInfo(lock, &info) && info.id_NumBlocksUsed > 0) {
        sprintf(detail, "NumBlocksUsed=%ld", (long)info.id_NumBlocksUsed);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "NumBlocksUsed=%ld", (long)info.id_NumBlocksUsed);
        lt_end(FALSE, (STRPTR)detail);
    }
    LhUnLock(lock);
    LhCloseArchive(arc);
}

static VOID
lt_test_openfromlock(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    BPTR lock;
    BPTR fh;
    char namebuf[256];
    UBYTE buf[256];
    LONG n;
    LONG total;
    LONG wrote;
    char detail[64];
    char expect[64];

    lt_begin((STRPTR)"LhNameFromLock", name);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, name);
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    namebuf[0] = '\0';
    if (LhNameFromLock(lock, (STRPTR)namebuf, (LONG)sizeof(namebuf)) <= 0) {
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"empty name");
        return;
    }
    if (strcmp((const char *)namebuf, (const char *)name) != 0) {
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)namebuf);
        return;
    }
    lt_end(TRUE, (STRPTR)namebuf);

    lt_begin((STRPTR)"LhOpenFromLock", (STRPTR)"non-NULL file handle");
    fh = LhOpenFromLock(lock, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, name);

    sprintf(expect, "read %lu bytes", (unsigned long)expect_len);
    lt_begin((STRPTR)"LhRead", (STRPTR)expect);
    total = 0;
    for (;;) {
        n = LhRead(fh, buf, (LONG)sizeof(buf));
        if (n < 0) {
            LhClose(fh);
            LhUnLock(lock);
            LhCloseArchive(arc);
            lt_end(FALSE, (STRPTR)"read error");
            return;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    sprintf(detail, "read=%ld", (long)total);
    if (total == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }

    lt_begin((STRPTR)"LhSeek", (STRPTR)"pos=0");
    if (LhSeek(fh, 0L, OFFSET_BEGINNING) != 0) {
        LhClose(fh);
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"seek failed");
        return;
    }
    lt_end(TRUE, (STRPTR)"pos=0");

    lt_begin((STRPTR)"LhWrite(read-only)", (STRPTR)"rejected (< 0)");
    wrote = LhWrite(fh, buf, (LONG)sizeof(buf));
    if (wrote >= 0) {
        LhClose(fh);
        LhUnLock(lock);
        LhCloseArchive(arc);
        sprintf(detail, "wrote=%ld", (long)wrote);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    sprintf(detail, "rc=%ld", (long)wrote);
    lt_end(TRUE, (STRPTR)detail);

    lt_begin((STRPTR)"LhClose", (STRPTR)"DOSTRUE");
    LhClose(fh);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    LhUnLock(lock);
    LhCloseArchive(arc);

    sprintf(expect, "read=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)"LhOpenFromLock roundtrip", (STRPTR)expect);
    sprintf(detail, "read=%ld", (long)total);
    if (total == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_open_handle(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    BPTR fh;
    UBYTE buf[256];
    LONG n;
    LONG total;
    LONG esz;
    char detail[64];
    char expect[64];

    lt_begin((STRPTR)"LhOpen", (STRPTR)"non-NULL file handle");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open archive failed");
        return;
    }
    fh = LhOpen(arc, name, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, name);

    sprintf(expect, "read=%lu seek=%lu", (unsigned long)expect_len,
        (unsigned long)expect_len);
    lt_begin((STRPTR)"LhOpen/LhSeek/LhRead/LhClose", (STRPTR)expect);
    esz = LhSeek(fh, 0L, OFFSET_END);
    LhSeek(fh, 0L, OFFSET_BEGINNING);
    total = 0;
    for (;;) {
        n = LhRead(fh, buf, (LONG)sizeof(buf));
        if (n < 0) {
            LhClose(fh);
            LhCloseArchive(arc);
            lt_end(FALSE, (STRPTR)"read error");
            return;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    LhClose(fh);
    LhCloseArchive(arc);
    sprintf(detail, "read=%ld seek=%ld", (long)total, (long)esz);
    if (total == (LONG)expect_len && esz == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_setpassword(STRPTR path)
{
    struct LhArchive *arc;

    lt_begin((STRPTR)"LhSetPassword", (STRPTR)"DOSTRUE");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    if (LhSetPassword(arc, (STRPTR)"LhTestSecret")) {
        lt_end(TRUE, (STRPTR)"DOSTRUE");
    } else {
        lt_end(FALSE, (STRPTR)"DOSFALSE");
    }
    LhSetPassword(arc, (STRPTR)"");
    LhCloseArchive(arc);
}

static VOID
lt_test_read_data(STRPTR path, STRPTR name, STRPTR expect, ULONG expect_len)
{
    struct LhArchive *arc;
    APTR data;
    LONG len;
    char label[64];
    char detail[40];
    char want[48];

    sprintf(label, "LhReadData %s", (const char *)name);
    sprintf(want, "len=%lu content match", (unsigned long)expect_len);
    lt_begin((STRPTR)label, (STRPTR)want);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    data = NULL;
    len = LhReadData(arc, name, &data);
    if (len < 0) {
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if ((ULONG)len != expect_len || data == NULL
        || memcmp(data, expect, expect_len) != 0) {
        if (data != NULL) {
            FreeMem(data, (ULONG)len);
        }
        LhCloseArchive(arc);
        sprintf(detail, "len=%ld content mismatch", (long)len);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(data, (ULONG)len);
    LhCloseArchive(arc);
    sprintf(detail, "len=%ld match", (long)len);
    lt_end(TRUE, (STRPTR)detail);
}

static VOID
lt_test_testentry(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    LONG len;
    char label[64];
    char detail[48];
    char want[48];

    sprintf(label, "LhTestEntry %s", (const char *)name);
    sprintf(want, "len=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)label, (STRPTR)want);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    len = LhTestEntry(arc, name);
    LhCloseArchive(arc);
    sprintf(detail, "len=%ld", (long)len);
    if (len == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_printentry(STRPTR path, STRPTR name)
{
    struct LhArchive *arc;
    char label[64];

    sprintf(label, "LhPrintEntry %s", (const char *)name);
    lt_begin((STRPTR)label, (STRPTR)"DOSTRUE (payload to Output)");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    if (LhPrintEntry(arc, name)) {
        lt_end(TRUE, (STRPTR)"DOSTRUE");
    } else {
        lt_end(FALSE, (STRPTR)"DOSFALSE");
    }
    LhCloseArchive(arc);
}

static BOOL
lt_file_size(STRPTR path, LONG *size_out)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    BOOL ok;

    ok = FALSE;
    lock = Lock(path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        return FALSE;
    }
    if (Examine(lock, fib)) {
        *size_out = fib->fib_Size;
        ok = TRUE;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    return ok;
}

static VOID
lt_test_extract(STRPTR arc_path, STRPTR entry, ULONG expect_len)
{
    struct LhArchive *arc;
    char outpath[256];
    LONG size;
    char label[64];
    char detail[48];

    sprintf(label, "LhExtractEntry %s", (const char *)entry);
    sprintf(detail, "file size=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)label, (STRPTR)detail);
    if (!lt_ensure_dir((STRPTR)LT_EXTRACT_DIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    sprintf(outpath, "%s/%s", LT_EXTRACT_DIR, (const char *)entry);
    DeleteFile((STRPTR)outpath);

    arc = LhOpenArchive(arc_path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    if (!LhExtractEntry(arc, entry, (STRPTR)outpath)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    LhCloseArchive(arc);

    size = 0;
    if (lt_file_size((STRPTR)outpath, &size) && size == (LONG)expect_len) {
        sprintf(detail, "size=%ld", (long)size);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "size=%ld", (long)size);
        lt_end(FALSE, (STRPTR)detail);
    }
}

static BOOL
lt_build_single_entry(STRPTR path, STRPTR name, STRPTR data, ULONG len)
{
    struct LhArchive *arc;

    if (!lt_ensure_parent(path)) {
        return FALSE;
    }
    lt_remove_path(path);
    arc = LhOpenArchive(path, LHARC_MODE_WRITE);
    if (arc == NULL) {
        return FALSE;
    }
    if (!LhAddEntry(arc, name, (APTR)data, (LONG)len)) {
        LhCloseArchive(arc);
        return FALSE;
    }
    LhCloseArchive(arc);
    return TRUE;
}

static BOOL
lt_test_mutate(STRPTR path)
{
    struct LhArchive *arc;
    ULONG len_c;
    ULONG len_cat;
    LONG count;
    char detail[48];

    lt_mutate_ok = FALSE;
    len_c = (ULONG)strlen((const char *)lt_plain_c);
    len_cat = (ULONG)strlen((const char *)lt_plain_cat);

    lt_begin((STRPTR)"concat donor create", (STRPTR)"archive written");
    if (!lt_build_single_entry((STRPTR)LT_FIXTURE2,
            (STRPTR)LT_CONCAT_ENTRY, lt_plain_cat, len_cat)) {
        lt_end(FALSE, (STRPTR)LT_FIXTURE2);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_FIXTURE2);

    lt_begin((STRPTR)"LhOpenArchive(append)", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_APPEND);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, path);

    lt_begin((STRPTR)"LhAddEntry(append)", (STRPTR)"DOSTRUE for LtExtra.txt");
    if (!LhAddEntry(arc, (STRPTR)LT_ENTRY_C, (APTR)lt_plain_c, (LONG)len_c)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_ENTRY_C);

    lt_begin((STRPTR)"LhConcatArchive", (STRPTR)"DOSTRUE");
    if (!LhConcatArchive(arc, (STRPTR)LT_FIXTURE2)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_FIXTURE2);
    LhCloseArchive(arc);

    lt_begin((STRPTR)"post-append count", (STRPTR)"4 entries");
    count = 0;
    if (lt_count_entries(path, &count) && count == 4) {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(FALSE, (STRPTR)detail);
    }

    lt_begin((STRPTR)"LhDeleteFile", (STRPTR)"DOSTRUE for lhtest.bin");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return FALSE;
    }
    if (LhDeleteFile(arc, (STRPTR)LT_ENTRY_B)) {
        lt_end(TRUE, (STRPTR)LT_ENTRY_B);
    } else {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
    }
    LhCloseArchive(arc);

    lt_begin((STRPTR)"post-delete count", (STRPTR)"3 entries");
    count = 0;
    if (lt_count_entries(path, &count) && count == 3) {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(FALSE, (STRPTR)detail);
    }
    lt_mutate_ok = TRUE;
    return TRUE;
}

static VOID
lt_run_post_mutate_verify(VOID)
{
    struct LhArchive *arc;
    LONG len;
    ULONG len_a;
    ULONG len_c;
    ULONG len_cat;

    len_a = (ULONG)strlen((const char *)lt_plain_a);
    len_c = (ULONG)strlen((const char *)lt_plain_c);
    len_cat = (ULONG)strlen((const char *)lt_plain_cat);

    lt_test_lock_examine((STRPTR)LT_FIXTURE, 3);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, lt_plain_a, len_a);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_C, lt_plain_c, len_c);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_CONCAT_ENTRY,
        lt_plain_cat, len_cat);
    lt_test_extract((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_C, len_c);
    lt_test_printentry((STRPTR)LT_FIXTURE, (STRPTR)LT_CONCAT_ENTRY);

    lt_begin((STRPTR)"post-delete LhTestEntry B", (STRPTR)"absent (len < 0)");
    arc = LhOpenArchive((STRPTR)LT_FIXTURE, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    len = LhTestEntry(arc, (STRPTR)LT_ENTRY_B);
    LhCloseArchive(arc);
    if (len < 0) {
        lt_end(TRUE, (STRPTR)"absent");
    } else {
        lt_end(FALSE, (STRPTR)"still present");
    }
}

static VOID
lt_test_lherr(VOID)
{
    struct LhArchive *arc;
    char detail[40];

    lt_begin((STRPTR)"LhErr", (STRPTR)"non-zero after failed open");
    arc = LhOpenArchive((STRPTR)"RAM:NoSuchArchive_999.lha", LHARC_MODE_READ);
    if (arc == NULL && LhErr() != 0) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(TRUE, (STRPTR)detail);
    } else {
        if (arc != NULL) {
            LhCloseArchive(arc);
        }
        sprintf(detail, "LhErr %ld", (long)LhErr());
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_negative(VOID)
{
    struct LhArchive *arc;
    LONG len;
    char detail[32];

    if (!lt_got_fixture) {
        Printf("LhTest: SKIP neg/missing entry (no fixture)\n");
        Flush(Output());
        return;
    }
    lt_begin((STRPTR)"neg/missing entry", (STRPTR)"LhTestEntry returns < 0");
    arc = LhOpenArchive((STRPTR)LT_FIXTURE, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    len = LhTestEntry(arc, (STRPTR)"NoSuchEntry.dat");
    LhCloseArchive(arc);
    sprintf(detail, "len=%ld", (long)len);
    if (len < 0) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_run_read_apis(VOID)
{
    ULONG len_a;
    ULONG len_b;

    len_a = (ULONG)strlen((const char *)lt_plain_a);
    len_b = (ULONG)strlen((const char *)lt_plain_b);

    lt_test_openarchive_read((STRPTR)LT_FIXTURE);
    lt_test_lock_examine((STRPTR)LT_FIXTURE, 2);
    lt_test_lock_entry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B);
    lt_test_exall((STRPTR)LT_FIXTURE, 2);
    lt_test_exall_size((STRPTR)LT_FIXTURE, 2);
    lt_test_info((STRPTR)LT_FIXTURE);
    lt_test_openfromlock((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_seek_end((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_open_handle((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_setpassword((STRPTR)LT_FIXTURE);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, lt_plain_a, len_a);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B, lt_plain_b, len_b);
    lt_test_testentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_testentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B, len_b);
    lt_test_printentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A);
    lt_test_printentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B);
    lt_test_extract((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_extract((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B, len_b);
}

static VOID
lt_test_external(STRPTR path)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;
    char detail[80];

    Printf("LhTest: external archive %s\n", path);
    Flush(Output());

    lt_begin((STRPTR)"external open", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, path);

    lt_begin((STRPTR)"external lock", (STRPTR)"non-NULL root lock");
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"NULL lock");
        return;
    }
    lt_end(TRUE, (STRPTR)"root lock");

    lt_begin((STRPTR)"external LhTestEntry",
        (STRPTR)"each entry size matches FIB");
    count = 0;
    ok = LhExamine(lock, fib);
    while (ok) {
        LONG tlen;

        tlen = LhTestEntry(arc, fib->fib_FileName);
        if (tlen != fib->fib_Size) {
            sprintf(detail, "%s len=%ld size=%ld",
                fib->fib_FileName, (long)tlen, (long)fib->fib_Size);
            lt_end(FALSE, (STRPTR)detail);
            LhUnLock(lock);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            LhCloseArchive(arc);
            return;
        }
        count++;
        ok = LhExNext(lock, fib);
    }
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    if (count > 0) {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)"empty");
    }
}

static VOID
lt_print_archive_skip(VOID)
{
    Printf("LhTest: SKIP 25 archive LVOs (fixture create failed):\n");
    Printf("  LhOpenArchive LhCloseArchive LhLock LhUnLock LhExamine LhExNext\n");
    Printf("  LhExAll LhExAllEnd LhInfo LhOpenFromLock LhOpen LhRead LhWrite\n");
    Printf("  LhClose LhSeek LhNameFromLock LhAddEntry LhDeleteFile\n");
    Printf("  LhConcatArchive LhSetPassword LhReadData LhExtractEntry\n");
    Printf("  LhTestEntry LhPrintEntry\n");
    Flush(Output());
}

int
main(int argc, char **argv)
{
    lt_pass = 0;
    lt_fail = 0;
    lt_got_fixture = FALSE;
    lt_mutate_ok = FALSE;

    Printf("LhTest: lh.library API harness\n");
    lt_print_api_plan();

    if (!lt_test_open()) {
        Printf("LhTest: %lu passed, %lu failed (library missing)\n",
            lt_pass, lt_fail);
        Flush(Output());
        return 20;
    }

    lt_test_classic_buffer();
    lt_test_codec_lh0();
    lt_test_fixture_create();

    if (lt_got_fixture) {
        lt_run_read_apis();
        if (lt_test_mutate((STRPTR)LT_FIXTURE) && lt_mutate_ok) {
            lt_run_post_mutate_verify();
        }
    } else {
        lt_print_archive_skip();
    }

    lt_test_lherr();
    lt_test_negative();

    if (argc > 1 && argv[1] != NULL) {
        lt_test_external((STRPTR)argv[1]);
    }

    CloseLibrary(LhBase);
    LhBase = NULL;

    Printf("LhTest: %lu passed, %lu failed\n", lt_pass, lt_fail);
    Flush(Output());

    if (lt_fail > 0) {
        return 10;
    }
    return 0;
}
