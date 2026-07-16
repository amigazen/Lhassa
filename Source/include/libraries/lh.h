/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * libraries/lh.h - lh.library public types and constants
 * Keep LhBuffer layout identical to Holger Krekel & Olaf Barthel lh.library.
 */

#ifndef LIBRARIES_LH_H
#define LIBRARIES_LH_H

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif

#ifndef DOS_DOS_H
#include <dos/dos.h>
#endif

#ifndef DOS_EXALL_H
#include <dos/exall.h>
#endif

#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
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

/*
 * LhAddEntryA / LhAddEntry tags.  Defaults without tags: LH0/store,
 * LH_ATTR_DEFAULT (0 = ----rwed), current DateStamp, no comment.  Nested
 * names use header level 2 with path extension type 2.  LhAddEntry is the
 * ==varargs alias of LhAddEntryA (same LVO).  Use LHADD_Method for
 * lh5/lh6/lh7 when those codecs are reliable.
 */
#define LHADD_Method     (TAG_USER + 1)  /* ti_Data = compression level (0/5/6/7/11=dir) */
#define LHADD_Attrs      (TAG_USER + 2)  /* ti_Data = Amiga protection bits */
#define LHADD_DateStamp  (TAG_USER + 3)  /* ti_Data = struct DateStamp * */
#define LHADD_Comment    (TAG_USER + 4)  /* ti_Data = STRPTR filenote */
#define LHADD_Directory  (TAG_USER + 5)  /* ti_Data = nonzero -> -lhd- directory */

/*
 * Bin-profile LHA (Handle / Load): reserved LH0 stored member with a
 * canonical $VER: line for the package.  Not listed in Manifest File:
 * lines; tools read it via LhReadData(arc, LH_BIN_PACKAGE_VER, ...).
 */
#define LH_BIN_PACKAGE_VER "-package.ver"

/*
 * Bin-profile autoshow text (LhA .displayme convention).  Shown on plain
 * extract; Load does not install this member (not in Manifest File: lines).
 */
#define LH_BIN_AUTOSHOW "Load.displayme"

#endif /* LIBRARIES_LH_H */
