/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * libraries/lhlib.h - lh.library public types and constants
 * Keep LhBuffer layout identical to Holger Krekel & Olaf Barthel lh.library.
 */

#ifndef LIBRARIES_LHLIB_H
#define LIBRARIES_LHLIB_H

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif

#ifndef DOS_DOS_H
#include <dos/dos.h>
#endif

#ifndef DOS_EXALL_H
#include <dos/exall.h>
#endif

#define LH_NAME         "lh.library"
#define LH_VERSION      2
#define LH_MIN_VERSION  2
#define ENCODEEXTRA(n)  (((n) + 7) >> 3)

struct LhBuffer
{
    APTR    lh_Src;
    ULONG   lh_SrcSize;
    APTR    lh_Dst;
    ULONG   lh_DstSize;
    APTR    lh_Aux;
    ULONG   lh_AuxSize;
    ULONG   lh_Reserved;
};

struct LhArchive;

/*
 * DOS.library-style archive access.  LhOpenArchive mounts the archive as a
 * volume root; LhLock/LhExamine/LhExNext mirror dos Lock/Examine/ExNext on
 * that volume.  LhOpen and LhOpenFromLock mirror dos Open/OpenFromLock;
 * LhRead/LhWrite/LhSeek/LhClose use the same argument order as dos.library.
 * LhReadData/LhExtractEntry/LhTestEntry/LhPrintEntry avoid fragile handle I/O
 * across SAS/C #pragma libcall boundaries.
 */
#define LHARC_MODE_READ   MODE_OLDFILE
#define LHARC_MODE_WRITE  MODE_NEWFILE
#define LHARC_MODE_APPEND 1007L

#endif /* LIBRARIES_LHLIB_H */
