/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * LhTest.c - SDK example: classic buffer API + DOS-style archive traversal.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/lh.h>
#include <proto/lh.h>

struct Library *LhBase = NULL;

static void test_buffer(void)
{
    struct LhBuffer *buf;
    static unsigned char plain[] = "LhASsA lh.library buffer roundtrip.";
    static unsigned char comp[128];
    static unsigned char back[128];
    ULONG n;

    buf = CreateBuffer(FALSE);
    if (!buf) {
        printf("LhTest: CreateBuffer failed\n");
        return;
    }
    buf->lh_Src = plain;
    buf->lh_SrcSize = (ULONG)(sizeof(plain) - 1);
    buf->lh_Dst = comp;
    buf->lh_DstSize = (ULONG)sizeof(comp);

    n = LhEncode(buf);
    if (n == 0) {
        printf("LhTest: LhEncode failed\n");
        DeleteBuffer(buf);
        return;
    }

    buf->lh_Src = comp;
    buf->lh_SrcSize = n;
    buf->lh_Dst = back;
    buf->lh_DstSize = (ULONG)(sizeof(plain) - 1);
    n = LhDecode(buf);
    if (n != (ULONG)(sizeof(plain) - 1) || memcmp(back, plain, n) != 0) {
        printf("LhTest: LhDecode mismatch\n");
    } else {
        printf("LhTest: buffer roundtrip OK\n");
    }
    DeleteBuffer(buf);
}

static void test_list(const char *path)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;

    arc = LhOpenArchive((STRPTR)path, LHARC_MODE_READ);
    if (!arc) {
        printf("LhTest: cannot open %s (IoErr %ld)\n", path, LhErr());
        return;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (long)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (!lock) {
        printf("LhTest: root lock failed (IoErr %ld)\n", LhErr());
        FreeMem(fib, (long)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return;
    }
    ok = LhExamine(lock, fib);
    while (ok) {
        printf("  %8ld  %s\n", fib->fib_Size, fib->fib_FileName);
        ok = LhExNext(lock, fib);
    }
    LhUnLock(lock);
    FreeMem(fib, (long)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
}

static void test_read(const char *path, const char *entry)
{
    struct LhArchive *arc;
    APTR data;
    LONG len;
    LONG show;

    arc = LhOpenArchive((STRPTR)path, LHARC_MODE_READ);
    if (!arc) {
        return;
    }
    data = NULL;
    len = LhReadData(arc, (STRPTR)entry, &data);
    if (len < 0) {
        printf("LhTest: LhReadData %s failed (IoErr %ld)\n", entry, LhErr());
        LhCloseArchive(arc);
        return;
    }
    show = len;
    if (show > 255) {
        show = 255;
    }
    if (len > 0 && data) {
        char buf[256];

        memcpy(buf, data, (size_t)show);
        buf[show] = '\0';
        printf("LhTest: first %ld bytes of %s (%ld total):\n%s\n",
            show, entry, len, buf);
    }
    if (data) {
        FreeMem(data, (ULONG)len);
    }
    LhCloseArchive(arc);
}

int main(int argc, char **argv)
{
    LhBase = OpenLibrary(LH_NAME, (LONG)LH_MIN_VERSION);
    if (!LhBase) {
        printf("LhTest: cannot open %s\n", LH_NAME);
        return 1;
    }
    test_buffer();
    if (argc > 1) {
        test_list(argv[1]);
        if (argc > 2) {
            test_read(argv[1], argv[2]);
        }
    }
    CloseLibrary(LhBase);
    return 0;
}
