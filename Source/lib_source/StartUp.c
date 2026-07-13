/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * StartUp.c - LVO trap, function vector table, and library lifecycle
 */

#define __USE_SYSBASE

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#include <proto/exec.h>

#include "lh/lhbase.h"
#include "private/lh_build.h"
#include "compiler.h"
#include "lh_funcs.h"

extern ULONG L_OpenLibs(struct LHBase *base);
extern void L_CloseLibs(void);

extern struct Resident ROMTag;
extern const char LH_LibName[];
extern const char LH_LibID[];
extern struct MyDataInit DataTab;

struct LHBase *LhBase = NULL;

extern struct ExecBase *SysBase;

LONG __ASM__ LibStart(void);
struct LHBase * __ASM__ __SAVE_DS__ InitLib(
    __REG__(a6, struct ExecBase *sysbase),
    __REG__(a0, BPTR seglist),
    __REG__(d0, struct LHBase *base));
struct LHBase * __ASM__ __SAVE_DS__ OpenLib(
    __REG__(a6, struct LHBase *base));
BPTR __ASM__ __SAVE_DS__ CloseLib(
    __REG__(a6, struct LHBase *base));
BPTR __ASM__ __SAVE_DS__ ExpungeLib(
    __REG__(a6, struct LHBase *base));
ULONG __ASM__ ExtFuncLib(void);

APTR FuncTab[];

/*
 * FuncTab[] order MUST match SDK/SFD/lh_lib.sfd and SDK/Include_h/pragmas/lh_pragmas.h
 * (bias 0x1e, +6 per LVO).  Index 0-3 are Open/Close/Expunge/Reserved; index 4
 * is CreateBuffer (libcall 0x1e).
 */

struct InitTable InitTab = {
    (ULONG)sizeof(struct LHBase),
    (APTR *)FuncTab,
    (APTR)&DataTab,
    (APTR)InitLib
};

APTR FuncTab[] = {
    (APTR)OpenLib,
    (APTR)CloseLib,
    (APTR)ExpungeLib,
    (APTR)ExtFuncLib,
    /* Classic lh.library API (LhLib 1990) - LVO 0x1e..0x30 */
    (APTR)CreateBuffer,
    (APTR)DeleteBuffer,
    (APTR)LhEncode,
    (APTR)LhDecode,
    /* Extended codec */
    (APTR)LhCompress,
    (APTR)LhDecompress,
    /* DOS.library-style archive API */
    (APTR)LhOpenArchive,
    (APTR)LhCloseArchive,
    (APTR)LhLock,
    (APTR)LhUnLock,
    (APTR)LhExamine,
    (APTR)LhExNext,
    (APTR)LhExAll,
    (APTR)LhExAllEnd,
    (APTR)LhInfo,
    (APTR)LhOpenFromLock,
    (APTR)LhOpen,
    (APTR)LhRead,
    (APTR)LhWrite,
    (APTR)LhClose,
    (APTR)LhSeek,
    (APTR)LhNameFromLock,
    (APTR)LhAddEntryA,
    (APTR)LhDeleteFile,
    (APTR)LhConcatArchive,
    (APTR)LhSetPassword,
    (APTR)LhReadData,
    (APTR)LhExtractEntry,
    (APTR)LhTestEntry,
    (APTR)LhPrintEntry,
    (APTR)LhErr,
    (APTR)((LONG)-1)
};

LONG
__ASM__ LibStart(void)
{
    return -1;
}

struct LHBase *
__ASM__ __SAVE_DS__ InitLib(
    __REG__(a6, struct ExecBase *sysbase),
    __REG__(a0, BPTR seglist),
    __REG__(d0, struct LHBase *base))
{
    LhBase = base;

    base->lhb_LibNode.lib_Node.ln_Type = NT_LIBRARY;
    base->lhb_LibNode.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lhb_LibNode.lib_Version = LH_LIB_VERSION;
    base->lhb_LibNode.lib_Revision = LH_LIB_REVISION;
    base->lhb_LibNode.lib_IdString = (STRPTR)LH_LibID;

    base->lhb_SegList = seglist;
    base->lhb_Pad = 0;
    base->lhb_Err = 0;
    base->lhb_PendingMem = NULL;
    base->lhb_PendingMemLen = 0;

    if (L_OpenLibs(base) != 0) {
        return (struct LHBase *)NULL;
    }

    return base;
}

struct LHBase *
__ASM__ __SAVE_DS__ OpenLib(__REG__(a6, struct LHBase *base))
{
    base->lhb_LibNode.lib_OpenCnt++;
    base->lhb_LibNode.lib_Flags &= ~LIBF_DELEXP;
    return base;
}

BPTR
__ASM__ __SAVE_DS__ CloseLib(__REG__(a6, struct LHBase *base))
{
    base->lhb_LibNode.lib_OpenCnt--;

    if (base->lhb_LibNode.lib_OpenCnt == 0) {
        if (base->lhb_LibNode.lib_Flags & LIBF_DELEXP) {
            return ExpungeLib(base);
        }
    }

    return 0;
}

BPTR
__ASM__ __SAVE_DS__ ExpungeLib(__REG__(a6, struct LHBase *base))
{
    BPTR seg;

    if (base->lhb_LibNode.lib_OpenCnt != 0) {
        base->lhb_LibNode.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    seg = base->lhb_SegList;

    L_CloseLibs();

    Remove(&base->lhb_LibNode.lib_Node);
    FreeMem((APTR)((BYTE *)base - base->lhb_LibNode.lib_NegSize),
        base->lhb_LibNode.lib_NegSize + base->lhb_LibNode.lib_PosSize);

    LhBase = NULL;

    return seg;
}

ULONG
__ASM__ ExtFuncLib(void)
{
    return 0;
}
