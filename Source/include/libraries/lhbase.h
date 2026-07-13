/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * libraries/lhbase.h - lh.library base structure (positive size fields)
 */

#ifndef LIBRARIES_LHBASE_H
#define LIBRARIES_LHBASE_H

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif

#include <dos/dos.h>

struct LHBase
{
    struct Library lhb_LibNode;
    BPTR lhb_SegList;
    ULONG lhb_Pad;
    LONG lhb_Err;
    /*
     * Set AllocVec'd archive bytes then call LhOpenArchive; consumed as a
     * memory-backed stream (no dos WaitPkt during subsequent reads).
     */
    APTR lhb_PendingMem;
    ULONG lhb_PendingMemLen;
};

#endif /* LIBRARIES_LHBASE_H */
