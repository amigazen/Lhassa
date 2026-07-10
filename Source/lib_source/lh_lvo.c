/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_lvo.c - lh.library LVO wrappers (classic LhLib API + DOS-style archive API)
 */

#define __USE_SYSBASE

#include "lh_native_guard.h"

#include <exec/types.h>
#include <string.h>

#include "compiler.h"
#include "/include/lh.h"
#include "lh/lhbase.h"
#include "lh_archive_dos.h"

#include <utility/tagitem.h>

extern struct LHBase *LhBase;

/* --- Original lh.library API (LhLib 1990) --- */

struct LhBuffer * __ASM__ __SAVE_DS__ CreateBuffer(__REG__(d0, LONG OnlyDecode))
{
    return lh_create_buffer(OnlyDecode ? 1 : 0);
}

void __ASM__ __SAVE_DS__ DeleteBuffer(__REG__(a0, struct LhBuffer *OldBuffer))
{
    lh_delete_buffer(OldBuffer);
}

ULONG __ASM__ __SAVE_DS__ LhEncode(__REG__(a0, struct LhBuffer *Buffer))
{
    return lh_encode(Buffer);
}

ULONG __ASM__ __SAVE_DS__ LhDecode(__REG__(a0, struct LhBuffer *Buffer))
{
    return lh_decode(Buffer);
}

ULONG __ASM__ __SAVE_DS__ LhCompress(
    __REG__(d0, LONG Method),
    __REG__(a0, struct LhBuffer *Buffer))
{
    size_t out_len;
    lh_status st;

    if (!Buffer || !Buffer->lh_Src || !Buffer->lh_Dst) {
        return 0;
    }
    out_len = (size_t)Buffer->lh_DstSize;
    st = lh_compress((lh_method)Method,
        (const unsigned char *)Buffer->lh_Src,
        (size_t)Buffer->lh_SrcSize,
        (unsigned char *)Buffer->lh_Dst,
        &out_len);
    if (st != LH_OK) {
        return 0;
    }
    Buffer->lh_DstSize = (unsigned long)out_len;
    return (ULONG)out_len;
}

ULONG __ASM__ __SAVE_DS__ LhDecompress(
    __REG__(d0, LONG Method),
    __REG__(a0, struct LhBuffer *Buffer))
{
    lh_status st;
    size_t expected;

    if (!Buffer || !Buffer->lh_Src || !Buffer->lh_Dst) {
        return 0;
    }
    expected = (size_t)Buffer->lh_DstSize;
    st = lh_decompress((lh_method)Method,
        (const unsigned char *)Buffer->lh_Src,
        (size_t)Buffer->lh_SrcSize,
        expected,
        (unsigned char *)Buffer->lh_Dst,
        expected);
    if (st != LH_OK) {
        return 0;
    }
    return Buffer->lh_DstSize;
}

/* --- DOS.library-style archive API --- */

struct LhArchive * __ASM__ __SAVE_DS__ LhOpenArchive(
    __REG__(d1, STRPTR Path),
    __REG__(d0, LONG Mode))
{
    return lh_arc_open(Path, Mode);
}

LONG __ASM__ __SAVE_DS__ LhCloseArchive(__REG__(a0, struct LhArchive *Archive))
{
    return lh_arc_close(Archive);
}

BPTR __ASM__ __SAVE_DS__ LhLock(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name))
{
    return lh_arc_lock(Archive, Name);
}

LONG __ASM__ __SAVE_DS__ LhUnLock(__REG__(a0, BPTR Lock))
{
    return lh_arc_unlock(Lock);
}

LONG __ASM__ __SAVE_DS__ LhExamine(
    __REG__(a0, BPTR Lock),
    __REG__(a1, struct FileInfoBlock *Fib))
{
    return lh_arc_examine(Lock, Fib);
}

LONG __ASM__ __SAVE_DS__ LhExNext(
    __REG__(a0, BPTR Lock),
    __REG__(a1, struct FileInfoBlock *Fib))
{
    return lh_arc_exnext(Lock, Fib);
}

LONG __ASM__ __SAVE_DS__ LhExAll(
    __REG__(a0, BPTR Lock),
    __REG__(d1, STRPTR Buffer),
    __REG__(d0, LONG Size),
    __REG__(d2, LONG Type),
    __REG__(a1, struct ExAllControl *Control))
{
    return lh_arc_exall(Lock, Buffer, Size, Type, Control);
}

LONG __ASM__ __SAVE_DS__ LhExAllEnd(__REG__(a0, BPTR Lock))
{
    return lh_arc_exall_end(Lock);
}

LONG __ASM__ __SAVE_DS__ LhInfo(
    __REG__(a0, BPTR Lock),
    __REG__(a1, struct InfoData *Info))
{
    return lh_arc_info(Lock, Info);
}

BPTR __ASM__ __SAVE_DS__ LhOpenFromLock(
    __REG__(a0, BPTR Lock),
    __REG__(d0, LONG Mode))
{
    return lh_arc_open_from_lock(Lock, Mode);
}

BPTR __ASM__ __SAVE_DS__ LhOpen(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(d0, LONG Mode))
{
    return lh_arc_open_entry(Archive, Name, Mode);
}

LONG __ASM__ __SAVE_DS__ LhRead(
    __REG__(d1, BPTR Fh),
    __REG__(a0, APTR Buffer),
    __REG__(d0, LONG Len))
{
    return lh_arc_read(Fh, Buffer, Len);
}

LONG __ASM__ __SAVE_DS__ LhWrite(
    __REG__(d1, BPTR Fh),
    __REG__(a0, APTR Buffer),
    __REG__(d0, LONG Len))
{
    return lh_arc_write(Fh, Buffer, Len);
}

LONG __ASM__ __SAVE_DS__ LhClose(__REG__(d1, BPTR Fh))
{
    return lh_arc_closefh(Fh);
}

LONG __ASM__ __SAVE_DS__ LhSeek(
    __REG__(d1, BPTR Fh),
    __REG__(d2, LONG Position),
    __REG__(d0, LONG Mode))
{
    return lh_arc_seek(Fh, Position, Mode);
}

LONG __ASM__ __SAVE_DS__ LhNameFromLock(
    __REG__(a0, BPTR Lock),
    __REG__(a1, STRPTR Buffer),
    __REG__(d0, LONG Len))
{
    return lh_arc_name_from_lock(Lock, Buffer, Len);
}

LONG __ASM__ __SAVE_DS__ LhAddEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, APTR Data),
    __REG__(d0, LONG DataLen))
{
    return lh_arc_add_entry(Archive, Name, Data, DataLen);
}

LONG __ASM__ __SAVE_DS__ LhAddEntryTagList(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, APTR Data),
    __REG__(d0, LONG DataLen),
    __REG__(a3, struct TagItem *TagList))
{
    return lh_arc_add_entry_taglist(Archive, Name, Data, DataLen, TagList);
}

LONG __ASM__ __SAVE_DS__ LhDeleteFile(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name))
{
    return lh_arc_delete_entry(Archive, Name);
}

LONG __ASM__ __SAVE_DS__ LhConcatArchive(
    __REG__(a0, struct LhArchive *Dest),
    __REG__(a1, STRPTR SourcePath))
{
    return lh_arc_concat(Dest, SourcePath);
}

LONG __ASM__ __SAVE_DS__ LhSetPassword(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Password))
{
    return lh_arc_set_password(Archive, Password);
}

LONG __ASM__ __SAVE_DS__ LhReadData(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, APTR *DataOut))
{
    return lh_arc_read_data(Archive, Name, DataOut);
}

LONG __ASM__ __SAVE_DS__ LhExtractEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, STRPTR DestPath))
{
    return lh_arc_extract_entry(Archive, Name, DestPath);
}

LONG __ASM__ __SAVE_DS__ LhTestEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name))
{
    return lh_arc_test_entry(Archive, Name);
}

LONG __ASM__ __SAVE_DS__ LhPrintEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name))
{
    return lh_arc_print_entry(Archive, Name);
}

LONG __ASM__ __SAVE_DS__ LhErr(void)
{
    if (LhBase) {
        return LhBase->lhb_Err;
    }
    return 0;
}
