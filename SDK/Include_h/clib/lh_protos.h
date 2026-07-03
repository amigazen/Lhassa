/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * clib/lh_protos.h - lh.library C function prototypes
 * Keep in sync with SDK/SFD/lh_lib.sfd (LibDescConverter output).
 */

#ifndef CLIB_LH_PROTOS_H
#define CLIB_LH_PROTOS_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif
#ifndef DOS_DOS_H
#include <dos/dos.h>
#endif
#ifndef DOS_EXALL_H
#include <dos/exall.h>
#endif
#ifndef LIBRARIES_LHLIB_H
#include <libraries/lhlib.h>
#endif

/* Classic LhLib API (Holger Krekel 1990) + extended codec */

struct LhBuffer *CreateBuffer(LONG OnlyDecode);
void DeleteBuffer(struct LhBuffer *OldBuffer);
ULONG LhEncode(struct LhBuffer *Buffer);
ULONG LhDecode(struct LhBuffer *Buffer);
ULONG LhCompress(LONG Method, struct LhBuffer *Buffer);
ULONG LhDecompress(LONG Method, struct LhBuffer *Buffer);

/* DOS.library-style archive access (LhArchive volume root) */

struct LhArchive *LhOpenArchive(STRPTR Path, LONG Mode);
LONG LhCloseArchive(struct LhArchive *Archive);
BPTR LhLock(struct LhArchive *Archive, STRPTR Name);
LONG LhUnLock(BPTR Lock);
LONG LhExamine(BPTR Lock, struct FileInfoBlock *Fib);
LONG LhExNext(BPTR Lock, struct FileInfoBlock *Fib);
LONG LhExAll(BPTR Lock, STRPTR Buffer, LONG Size, LONG Type,
    struct ExAllControl *Control);
LONG LhExAllEnd(BPTR Lock);
LONG LhInfo(BPTR Lock, struct InfoData *Info);
BPTR LhOpenFromLock(BPTR Lock, LONG Mode);
BPTR LhOpen(struct LhArchive *Archive, STRPTR Name, LONG Mode);
LONG LhRead(BPTR Fh, APTR Buffer, LONG Len);
LONG LhWrite(BPTR Fh, APTR Buffer, LONG Len);
LONG LhClose(BPTR Fh);
LONG LhSeek(BPTR Fh, LONG Position, LONG Mode);
LONG LhNameFromLock(BPTR Lock, STRPTR Buffer, LONG Len);
LONG LhAddEntry(struct LhArchive *Archive, STRPTR Name, APTR Data, LONG DataLen);
LONG LhDeleteFile(struct LhArchive *Archive, STRPTR Name);
LONG LhConcatArchive(struct LhArchive *Dest, STRPTR SourcePath);
LONG LhSetPassword(struct LhArchive *Archive, STRPTR Password);
LONG LhReadData(struct LhArchive *Archive, STRPTR Name, APTR *DataOut);
LONG LhExtractEntry(struct LhArchive *Archive, STRPTR Name, STRPTR DestPath);
LONG LhTestEntry(struct LhArchive *Archive, STRPTR Name);
LONG LhPrintEntry(struct LhArchive *Archive, STRPTR Name);
LONG LhErr(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIB_LH_PROTOS_H */
