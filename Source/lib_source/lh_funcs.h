/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_funcs.h - LVO forward declarations for FuncTab (must match lh_lvo.c)
 */

#ifndef LH_FUNCS_H
#define LH_FUNCS_H

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/exall.h>

#include "compiler.h"

struct LhBuffer;
struct LhArchive;

/* Classic lh.library (LhLib 1990) */
struct LhBuffer * __ASM__ __SAVE_DS__ CreateBuffer(__REG__(d0, LONG OnlyDecode));
void __ASM__ __SAVE_DS__ DeleteBuffer(__REG__(a0, struct LhBuffer *OldBuffer));
ULONG __ASM__ __SAVE_DS__ LhEncode(__REG__(a0, struct LhBuffer *Buffer));
ULONG __ASM__ __SAVE_DS__ LhDecode(__REG__(a0, struct LhBuffer *Buffer));

/* Extended codec */
ULONG __ASM__ __SAVE_DS__ LhCompress(
    __REG__(d0, LONG Method),
    __REG__(a0, struct LhBuffer *Buffer));
ULONG __ASM__ __SAVE_DS__ LhDecompress(
    __REG__(d0, LONG Method),
    __REG__(a0, struct LhBuffer *Buffer));

/* DOS.library-style archive API */
struct LhArchive * __ASM__ __SAVE_DS__ LhOpenArchive(
    __REG__(d1, STRPTR Path),
    __REG__(d0, LONG Mode));
LONG __ASM__ __SAVE_DS__ LhCloseArchive(__REG__(a0, struct LhArchive *Archive));
BPTR __ASM__ __SAVE_DS__ LhLock(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name));
LONG __ASM__ __SAVE_DS__ LhUnLock(__REG__(a0, BPTR Lock));
LONG __ASM__ __SAVE_DS__ LhExamine(
    __REG__(a0, BPTR Lock),
    __REG__(a1, struct FileInfoBlock *Fib));
LONG __ASM__ __SAVE_DS__ LhExNext(
    __REG__(a0, BPTR Lock),
    __REG__(a1, struct FileInfoBlock *Fib));
LONG __ASM__ __SAVE_DS__ LhExAll(
    __REG__(a0, BPTR Lock),
    __REG__(d1, STRPTR Buffer),
    __REG__(d0, LONG Size),
    __REG__(d2, LONG Type),
    __REG__(a1, struct ExAllControl *Control));
LONG __ASM__ __SAVE_DS__ LhExAllEnd(__REG__(a0, BPTR Lock));
LONG __ASM__ __SAVE_DS__ LhInfo(
    __REG__(a0, BPTR Lock),
    __REG__(a1, struct InfoData *Info));
BPTR __ASM__ __SAVE_DS__ LhOpenFromLock(
    __REG__(a0, BPTR Lock),
    __REG__(d0, LONG Mode));
BPTR __ASM__ __SAVE_DS__ LhOpen(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(d0, LONG Mode));
LONG __ASM__ __SAVE_DS__ LhRead(
    __REG__(d1, BPTR Fh),
    __REG__(a0, APTR Buffer),
    __REG__(d0, LONG Len));
LONG __ASM__ __SAVE_DS__ LhWrite(
    __REG__(d1, BPTR Fh),
    __REG__(a0, APTR Buffer),
    __REG__(d0, LONG Len));
LONG __ASM__ __SAVE_DS__ LhClose(__REG__(d1, BPTR Fh));
LONG __ASM__ __SAVE_DS__ LhSeek(
    __REG__(d1, BPTR Fh),
    __REG__(d2, LONG Position),
    __REG__(d0, LONG Mode));
LONG __ASM__ __SAVE_DS__ LhNameFromLock(
    __REG__(a0, BPTR Lock),
    __REG__(a1, STRPTR Buffer),
    __REG__(d0, LONG Len));
LONG __ASM__ __SAVE_DS__ LhAddEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, APTR Data),
    __REG__(d0, LONG DataLen));
LONG __ASM__ __SAVE_DS__ LhDeleteFile(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name));
LONG __ASM__ __SAVE_DS__ LhConcatArchive(
    __REG__(a0, struct LhArchive *Dest),
    __REG__(a1, STRPTR SourcePath));
LONG __ASM__ __SAVE_DS__ LhSetPassword(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Password));
LONG __ASM__ __SAVE_DS__ LhReadData(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, APTR *DataOut));
LONG __ASM__ __SAVE_DS__ LhExtractEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name),
    __REG__(a2, STRPTR DestPath));
LONG __ASM__ __SAVE_DS__ LhTestEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name));
LONG __ASM__ __SAVE_DS__ LhPrintEntry(
    __REG__(a0, struct LhArchive *Archive),
    __REG__(a1, STRPTR Name));
LONG __ASM__ __SAVE_DS__ LhErr(void);

#endif /* LH_FUNCS_H */
