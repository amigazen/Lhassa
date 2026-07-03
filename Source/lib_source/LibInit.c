/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * LibInit.c - ROMTag, DataTab, and pooled allocator init for lh.library
 */

#define __USE_SYSBASE

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "lh/lhbase.h"
#include "private/lh_build.h"
#include "compiler.h"

#define LHNAME "lh"
#define LHVER  " 2.0 (02.07.2026)"

const char LH_LibName[] = LHNAME ".library";
const char LH_LibID[]   = LHNAME LHVER;
const char LH_VerString[] = "\0$VER: " LHNAME LHVER;

extern struct ExecBase *SysBase;
extern struct LHBase *LhBase;

struct DosLibrary *DOSBase;

extern int lh_malloc_init(void);
extern void lh_malloc_exit(void);

ULONG __SAVE_DS__
L_OpenLibs(struct LHBase *base)
{
    (void)base;
    SysBase = *((struct ExecBase **)4);
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37L);
    if (!DOSBase) {
        return 1;
    }
    if (lh_malloc_init() == 0) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 1;
    }
    return 0;
}

void __SAVE_DS__
L_CloseLibs(void)
{
    lh_malloc_exit();
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

extern struct InitTable InitTab;
extern APTR EndResident;

struct Resident ROMTag = {
    RTC_MATCHWORD,
    &ROMTag,
    &EndResident,
    RTF_AUTOINIT,
    LH_LIB_VERSION,
    NT_LIBRARY,
    0,
    (APTR)LH_LibName,
    (APTR)LH_LibID,
    (APTR)&InitTab
};

APTR EndResident;

struct MyDataInit DataTab = {
    0xE000, 8,  NT_LIBRARY,
    0x80,   10, (ULONG)LH_LibName,
    0xE000, 14, LIBF_SUMUSED | LIBF_CHANGED,
    0xE000, 20, LH_LIB_VERSION,
    0xE000, 22, LH_LIB_REVISION,
    0x80,   24, (ULONG)LH_LibID,
    (ULONG)0
};

#ifdef __SASC
void __regargs __chkabort(void) { }
void __regargs _CXBRK(void)     { }
void __XCEXIT(void)            { }
#endif
