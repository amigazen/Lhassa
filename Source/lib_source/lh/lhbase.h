/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh/lhbase.h - lh.library base structure
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
};

#endif /* LIBRARIES_LHBASE_H */
